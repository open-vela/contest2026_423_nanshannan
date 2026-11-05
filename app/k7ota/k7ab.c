/****************************************************************************
 * app/k7ota/k7ab.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A/B 控制块读写。规则与 U-Boot 侧（amp/uboot/0014）对应：
 *   - 两份，seq 为偶数写 /dev/abctl 的扇区 0，奇数写扇区 1；
 *   - 读取取 crc 正确且 seq 较大的那份；
 *   - crc32 是 zlib 的那一种（U-Boot 的 crc32(0, ...)）。
 *
 * 上游 frameworks/system/ota 的 bootctl 把同样的几个标志存在 KVDB 里，
 * U-Boot 读不了，所以换成一个扇区的二进制结构；三个动作的语义不变：
 *   update  开始写目标槽之前：目标槽清成不可启动
 *   done    写完校验过：目标槽 active + bootable，另一个 active 清掉
 *   success 启动成功：本次启动的槽 successful，清 try
 * done 比上游多清一次 try：上游在"试启动失败、回退"之后 try 一直留着，
 * 下一次升级的新槽会被 bootloader 直接判为失败（U-Boot 侧也一并处理）。
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <nuttx/crc32.h>

#include "k7ab.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* zlib crc32 = 初值全 1、结果取反；NuttX 的 crc32part 两头都不做 */

static uint32_t ab_crc(FAR const struct k7ab_ctrl_s *c)
{
  return ~crc32part((FAR const uint8_t *)c,
                    offsetof(struct k7ab_ctrl_s, crc32), 0xffffffff);
}

static bool ab_valid(FAR const struct k7ab_ctrl_s *c)
{
  return memcmp(c->magic, K7AB_MAGIC, 8) == 0 &&
         c->version == K7AB_VERSION && c->crc32 == ab_crc(c);
}

static int ab_rw(int idx, FAR uint8_t *buf, bool wr)
{
  ssize_t n;
  int fd;
  int ret = 0;

  /* 每次重新打开：读到的是 eMMC 上的内容，不是 BCH 的扇区缓存 */

  fd = open(K7AB_CTRL_DEV, wr ? O_RDWR : O_RDONLY);
  if (fd < 0)
    {
      return -errno;
    }

  if (lseek(fd, (off_t)idx * K7AB_SECTOR, SEEK_SET) < 0)
    {
      ret = -errno;
    }
  else
    {
      n = wr ? write(fd, buf, K7AB_SECTOR) : read(fd, buf, K7AB_SECTOR);
      if (n != K7AB_SECTOR)
        {
          ret = n < 0 ? -errno : -EIO;
        }
    }

  close(fd);
  return ret;
}

static void ab_init(FAR struct k7ab_ctrl_s *c)
{
  memset(c, 0, sizeof(*c));
  memcpy(c->magic, K7AB_MAGIC, 8);
  c->version = K7AB_VERSION;
  c->booted = K7AB_BOOTED_NONE;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int k7ab_load(FAR struct k7ab_ctrl_s *c)
{
  uint8_t buf[K7AB_CTRL_COPIES][K7AB_SECTOR];
  FAR struct k7ab_ctrl_s *p0 = (FAR struct k7ab_ctrl_s *)buf[0];
  FAR struct k7ab_ctrl_s *p1 = (FAR struct k7ab_ctrl_s *)buf[1];
  bool v0;
  bool v1;
  int ret;

  ret = ab_rw(0, buf[0], false);
  if (ret == 0)
    {
      ret = ab_rw(1, buf[1], false);
    }

  if (ret < 0)
    {
      return ret;
    }

  v0 = ab_valid(p0);
  v1 = ab_valid(p1);
  if (!v0 && !v1)
    {
      ab_init(c);
      return -ENOENT;
    }

  /* seq 用差值比较，回绕也对 */

  memcpy(c, v0 && (!v1 || (int32_t)(p0->seq - p1->seq) > 0) ? p0 : p1,
         sizeof(*c));
  return 0;
}

int k7ab_store(FAR struct k7ab_ctrl_s *c)
{
  uint8_t buf[K7AB_SECTOR];
  uint8_t back[K7AB_SECTOR];
  int idx;
  int ret;

  c->seq++;
  c->crc32 = ab_crc(c);
  idx = c->seq & 1;

  memset(buf, 0, sizeof(buf));
  memcpy(buf, c, sizeof(*c));

  ret = ab_rw(idx, buf, true);
  if (ret == 0)
    {
      ret = ab_rw(idx, back, false);
    }

  if (ret == 0 && memcmp(buf, back, sizeof(buf)) != 0)
    {
      ret = -EIO;
    }

  if (ret < 0)
    {
      printf("控制块写入第 %d 份失败: %d\n", idx, ret);
    }

  return ret;
}

int k7ab_clear(void)
{
  uint8_t zero[K7AB_SECTOR];
  int ret;

  memset(zero, 0, sizeof(zero));
  ret = ab_rw(0, zero, true);
  if (ret == 0)
    {
      ret = ab_rw(1, zero, true);
    }

  return ret;
}

int k7ab_target(FAR const struct k7ab_ctrl_s *c)
{
  int i;

  if (c->booted < K7AB_NSLOTS)
    {
      return !c->booted;
    }

  for (i = 0; i < K7AB_NSLOTS; i++)
    {
      if (!(c->slot[i].active && c->slot[i].successful))
        {
          return i;
        }
    }

  return 0;
}

int k7ab_update(void)
{
  struct k7ab_ctrl_s c;
  int ret;
  int t;

  ret = k7ab_load(&c);
  if (ret < 0 && ret != -ENOENT)
    {
      return ret;
    }

  t = k7ab_target(&c);
  memset(&c.slot[t], 0, sizeof(c.slot[t]));
  return k7ab_store(&c);
}

int k7ab_done(uint32_t length, FAR const uint8_t *sha256)
{
  struct k7ab_ctrl_s c;
  int ret;
  int t;

  ret = k7ab_load(&c);
  if (ret < 0 && ret != -ENOENT)
    {
      return ret;
    }

  t = k7ab_target(&c);
  c.slot[t].active = 1;
  c.slot[t].bootable = 1;
  c.slot[t].successful = 0;
  c.slot[t].length = length;
  memcpy(c.slot[t].sha256, sha256, 32);
  c.slot[!t].active = 0;
  c.try_boot = 0;
  return k7ab_store(&c);
}

int k7ab_success(void)
{
  struct k7ab_ctrl_s c;
  int ret;
  int b;

  ret = k7ab_load(&c);
  if (ret < 0)
    {
      return ret == -ENOENT ? 0 : ret;
    }

  b = c.booted;
  if (b >= K7AB_NSLOTS || c.slot[b].successful)
    {
      return 0;
    }

  c.slot[b].successful = 1;
  c.slot[b].active = 1;
  c.try_boot = 0;
  ret = k7ab_store(&c);
  return ret < 0 ? ret : 1;
}

uint32_t k7ab_slot_sector(int slot)
{
  return slot ? K7AB_SLOT_B : K7AB_SLOT_A;
}

char k7ab_slot_name(int slot)
{
  return slot < K7AB_NSLOTS ? (char)('A' + slot) : 'R';
}

void k7ab_print(FAR const struct k7ab_ctrl_s *c)
{
  int i;
  int j;

  printf("seq %" PRIu32 "，本次启动：%s，try=%u，试启动累计 %u 次\n",
         c->seq, c->booted == 0 ? "槽 A" : c->booted == 1 ? "槽 B" :
         "救援 FIT", c->try_boot, c->boot_count);

  for (i = 0; i < K7AB_NSLOTS; i++)
    {
      FAR const struct k7ab_slot_s *s = &c->slot[i];

      printf("  槽 %c  active=%u bootable=%u successful=%u  %" PRIu32
             " 字节  sha256 ", k7ab_slot_name(i), s->active, s->bootable,
             s->successful, s->length);
      for (j = 0; j < 8; j++)
        {
          printf("%02x", s->sha256[j]);
        }

      printf("…\n");
    }
}
