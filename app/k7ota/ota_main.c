/****************************************************************************
 * app/k7ota/ota_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * openvela 镜像的在线升级：把新的 AMP FIT 经 HTTP 下载进**没在运行的**
 * 那个槽，回读校验通过后才设为下次启动。
 *
 *   ota install <url> <字节数> <sha256>
 *   ota verify [A|B]     按控制块记录回读槽内镜像重算 sha256
 *   ota info             同 bootctl info
 *
 * 主机侧用 scripts/ota-push.py（起 HTTP 服务、经 telnet 发命令）。
 *
 * ★ 顺序就是安全性
 *
 *   1. bootctl update：目标槽先标成不可启动；
 *   2. 下载、边写边算 sha256；第一块不是 FIT 头就立刻停；
 *   3. 关设备、重新打开、从 eMMC 回读重算 sha256；
 *   4. 一致才 bootctl done（目标槽 active + bootable，try 清零）。
 *   任何一步失败，目标槽停在"不可启动"，当前槽不受影响，重启还是现在的系统。
 *
 *   重启后 U-Boot 先校验槽里 FIT 的哈希再试启动；这次启动要被确认
 *   （bootctl，界面起来后自动做），否则下次进 U-Boot 就回到原来的槽。
 *
 * ★ 只通过 /dev/abslot 访问（recovery 分区，96MB），不碰 /dev/mmcsd0：
 *   本端 off_t 是 32 位，整盘偏移过 4GB 会静默回绕。
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mbedtls/sha256.h>
#include <netutils/webclient.h>

#include "k7ab.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define OTA_CHUNK     (64 * 1024)
#define OTA_NET_BUF   4096
#define OTA_NET_TMO   30          /* 秒；webclient 每次收发的超时 */
#define OTA_REPORT    (1u << 20)  /* 每 1MB 报一次进度 */
#define OTA_SLOT_MAX  ((uint32_t)K7AB_SLOT_SECTORS * K7AB_SECTOR)

static const uint8_t g_fit_magic[4] =
{
  0xd0, 0x0d, 0xfe, 0xed
};

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ota_dl_s
{
  int                    fd;
  FAR uint8_t           *chunk;
  uint32_t               fill;
  uint32_t               got;
  uint32_t               want;
  uint32_t               report;
  int                    err;
  bool                   checked;   /* FIT 头已检查 */
  struct timespec        t0;
  mbedtls_sha256_context sha;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static long elapsed_ms(FAR const struct timespec *t0)
{
  struct timespec t1;

  clock_gettime(CLOCK_MONOTONIC, &t1);
  return (long)((t1.tv_sec - t0->tv_sec) * 1000 +
                (t1.tv_nsec - t0->tv_nsec) / 1000000);
}

static void sha_hex(FAR const uint8_t *d, FAR char *out)
{
  int i;

  for (i = 0; i < 32; i++)
    {
      sprintf(out + i * 2, "%02x", d[i]);
    }
}

static int parse_sha(FAR const char *hex, FAR uint8_t *out)
{
  int i;

  if (strlen(hex) != 64)
    {
      return -EINVAL;
    }

  for (i = 0; i < 32; i++)
    {
      unsigned int v;

      if (sscanf(hex + i * 2, "%2x", &v) != 1)
        {
          return -EINVAL;
        }

      out[i] = (uint8_t)v;
    }

  return 0;
}

static int slot_open(int slot, int flags)
{
  int fd;

  fd = open(K7AB_SLOT_DEV, flags);
  if (fd < 0)
    {
      printf("打不开 %s: %d（GPT 的 recovery 分区没注册？看启动日志）\n",
             K7AB_SLOT_DEV, -errno);
      return -1;
    }

  if (lseek(fd, (off_t)k7ab_slot_sector(slot) * K7AB_SECTOR, SEEK_SET) < 0)
    {
      printf("%s 定位到槽 %c 失败: %d\n", K7AB_SLOT_DEV,
             k7ab_slot_name(slot), -errno);
      close(fd);
      return -1;
    }

  return fd;
}

static int dl_flush(FAR struct ota_dl_s *dl)
{
  ssize_t n;

  if (dl->fill == 0)
    {
      return 0;
    }

  n = write(dl->fd, dl->chunk, dl->fill);
  if (n != (ssize_t)dl->fill)
    {
      return n < 0 ? -errno : -EIO;
    }

  dl->fill = 0;
  return 0;
}

static int dl_sink(FAR char **buffer, int offset, int datend,
                   FAR int *buflen, FAR void *arg)
{
  FAR struct ota_dl_s *dl = arg;
  FAR const uint8_t *p = (FAR const uint8_t *)*buffer + offset;
  uint32_t len = (uint32_t)(datend - offset);

  (void)buflen;

  if (len > dl->want - dl->got)
    {
      dl->err = -EFBIG;
      return -EFBIG;
    }

  mbedtls_sha256_update(&dl->sha, p, len);
  dl->got += len;

  while (len > 0)
    {
      uint32_t n = OTA_CHUNK - dl->fill;

      if (n > len)
        {
          n = len;
        }

      memcpy(dl->chunk + dl->fill, p, n);
      dl->fill += n;
      p += n;
      len -= n;

      /* 一攒够 4 字节就看是不是 FIT：传错文件不要等下完才发现。
       * 第一块写出去之前 chunk 开头就是文件开头。
       */

      if (!dl->checked && dl->fill >= sizeof(g_fit_magic))
        {
          dl->checked = true;
          if (memcmp(dl->chunk, g_fit_magic, sizeof(g_fit_magic)) != 0)
            {
              dl->err = -ENOEXEC;
              return -ENOEXEC;
            }
        }

      if (dl->fill == OTA_CHUNK)
        {
          dl->err = dl_flush(dl);
          if (dl->err < 0)
            {
              return dl->err;
            }
        }
    }

  if (dl->got >= dl->report)
    {
      printf("  已收 %" PRIu32 " KB，%ld s\n", dl->got >> 10,
             elapsed_ms(&dl->t0) / 1000);
      dl->report += OTA_REPORT;
    }

  return 0;
}

/* 重新打开设备，从 eMMC 回读槽内前 length 字节算 sha256 */

static int readback_sha(int slot, uint32_t length, FAR uint8_t *out)
{
  mbedtls_sha256_context sha;
  FAR uint8_t *buf;
  uint32_t left = length;
  int ret = 0;
  int fd;

  fd = slot_open(slot, O_RDONLY);
  if (fd < 0)
    {
      return -ENODEV;
    }

  buf = malloc(OTA_CHUNK);
  if (buf == NULL)
    {
      close(fd);
      return -ENOMEM;
    }

  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);

  while (left > 0)
    {
      uint32_t n = left > OTA_CHUNK ? OTA_CHUNK : left;
      ssize_t r = read(fd, buf, n);

      if (r != (ssize_t)n)
        {
          ret = r < 0 ? -errno : -EIO;
          break;
        }

      mbedtls_sha256_update(&sha, buf, n);
      left -= n;
    }

  mbedtls_sha256_finish(&sha, out);
  mbedtls_sha256_free(&sha);
  free(buf);
  close(fd);
  return ret;
}

static int ota_install(FAR const char *url, FAR const char *lenstr,
                       FAR const char *hex)
{
  struct webclient_context ctx;
  struct k7ab_ctrl_s c;
  struct ota_dl_s dl;
  uint8_t want_sha[32];
  uint8_t got_sha[32];
  char hexbuf[65];
  FAR char *netbuf = NULL;
  uint32_t length;
  long ms;
  int target;
  int ret;

  length = (uint32_t)strtoul(lenstr, NULL, 0);
  if (parse_sha(hex, want_sha) < 0)
    {
      printf("sha256 要 64 个十六进制字符\n");
      return -EINVAL;
    }

  if (length < K7AB_SECTOR || length > OTA_SLOT_MAX)
    {
      printf("长度 %" PRIu32 " 不对：一个槽最多 %" PRIu32 " 字节\n",
             length, OTA_SLOT_MAX);
      return -EINVAL;
    }

  ret = k7ab_load(&c);
  if (ret < 0 && ret != -ENOENT)
    {
      printf("读控制块失败: %d\n", ret);
      return ret;
    }

  target = k7ab_target(&c);
  printf("本次启动：%s；写入槽 %c\n",
         c.booted < K7AB_NSLOTS ? (c.booted ? "槽 B" : "槽 A") : "救援 FIT",
         k7ab_slot_name(target));

  /* 1. 目标槽先作废 */

  ret = k7ab_update();
  if (ret < 0)
    {
      printf("bootctl update 失败: %d\n", ret);
      return ret;
    }

  /* 2. 边下边写边算 */

  memset(&dl, 0, sizeof(dl));
  dl.fd = slot_open(target, O_RDWR);
  if (dl.fd < 0)
    {
      return -ENODEV;
    }

  dl.chunk = malloc(OTA_CHUNK);
  netbuf = malloc(OTA_NET_BUF);
  if (dl.chunk == NULL || netbuf == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  dl.want = length;
  dl.report = OTA_REPORT;
  mbedtls_sha256_init(&dl.sha);
  mbedtls_sha256_starts(&dl.sha, 0);
  clock_gettime(CLOCK_MONOTONIC, &dl.t0);

  printf("下载 %s（%" PRIu32 " 字节）-> 槽 %c\n", url, length,
         k7ab_slot_name(target));

  webclient_set_defaults(&ctx);
  ctx.method = "GET";
  ctx.url = url;
  ctx.buffer = netbuf;
  ctx.buflen = OTA_NET_BUF;
  ctx.sink_callback = dl_sink;
  ctx.sink_callback_arg = &dl;
  ctx.timeout_sec = OTA_NET_TMO;

  ret = webclient_perform(&ctx);
  if (ret == 0 && dl.err == 0)
    {
      ret = dl_flush(&dl);
    }

  mbedtls_sha256_finish(&dl.sha, got_sha);
  mbedtls_sha256_free(&dl.sha);
  ms = elapsed_ms(&dl.t0);

  if (dl.err == -ENOEXEC)
    {
      printf("下载的不是 FIT（头 4 字节不是 d00dfeed），停止\n");
      ret = -ENOEXEC;
      goto out;
    }

  if (ret < 0 || dl.err < 0)
    {
      printf("下载/写入失败: %d（sink %d，HTTP %u，已收 %" PRIu32 "）\n",
             ret, dl.err, ctx.http_status, dl.got);
      ret = ret < 0 ? ret : dl.err;
      goto out;
    }

  if (ctx.http_status != 200 || dl.got != length)
    {
      printf("HTTP %u，收到 %" PRIu32 " 字节，期望 %" PRIu32 "\n",
             ctx.http_status, dl.got, length);
      ret = -EIO;
      goto out;
    }

  printf("收完 %" PRIu32 " 字节，%ld ms（%ld KB/s）\n", dl.got, ms,
         ms > 0 ? (long)((uint64_t)dl.got * 1000 / 1024 / ms) : 0);

  if (memcmp(got_sha, want_sha, 32) != 0)
    {
      sha_hex(got_sha, hexbuf);
      printf("收到的数据 sha256 不对: %s，槽 %c 保持不可启动\n", hexbuf,
             k7ab_slot_name(target));
      ret = -EBADMSG;
      goto out;
    }

  /* 3. 从 eMMC 回读重算 */

  close(dl.fd);
  dl.fd = -1;

  ret = readback_sha(target, length, got_sha);
  if (ret < 0 || memcmp(got_sha, want_sha, 32) != 0)
    {
      sha_hex(got_sha, hexbuf);
      printf("回读校验失败（%d）: %s，槽 %c 保持不可启动\n", ret, hexbuf,
             k7ab_slot_name(target));
      ret = ret < 0 ? ret : -EBADMSG;
      goto out;
    }

  printf("回读 sha256 一致\n");

  /* 4. 设为下次启动 */

  ret = k7ab_done(length, want_sha);
  if (ret < 0)
    {
      printf("bootctl done 失败: %d\n", ret);
      goto out;
    }

  printf("完成：槽 %c 已写入并校验，下次启动试运行；"
         "启动成功后自动确认，未确认则回退\n", k7ab_slot_name(target));

out:
  if (dl.fd >= 0)
    {
      close(dl.fd);
    }

  free(dl.chunk);
  free(netbuf);
  return ret;
}

static int ota_verify(FAR const char *which)
{
  struct k7ab_ctrl_s c;
  struct timespec t0;
  uint8_t sha[32];
  char hexbuf[65];
  int slot;
  int ret;

  ret = k7ab_load(&c);
  if (ret < 0)
    {
      printf("没有有效控制块（%d）\n", ret);
      return ret;
    }

  slot = which != NULL ? (which[0] == 'B' || which[0] == 'b') :
         (c.booted < K7AB_NSLOTS ? c.booted : k7ab_target(&c));
  if (c.slot[slot].length == 0)
    {
      printf("槽 %c 没有记录镜像\n", k7ab_slot_name(slot));
      return -ENOENT;
    }

  clock_gettime(CLOCK_MONOTONIC, &t0);
  ret = readback_sha(slot, c.slot[slot].length, sha);
  if (ret < 0)
    {
      printf("回读失败: %d\n", ret);
      return ret;
    }

  if (memcmp(sha, c.slot[slot].sha256, 32) != 0)
    {
      sha_hex(sha, hexbuf);
      printf("槽 %c 校验不一致，实际 %s\n", k7ab_slot_name(slot), hexbuf);
      return -EBADMSG;
    }

  sha_hex(sha, hexbuf);
  printf("槽 %c：%" PRIu32 " 字节，sha256 %s 一致（%ld ms）\n",
         k7ab_slot_name(slot), c.slot[slot].length, hexbuf,
         elapsed_ms(&t0));
  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct k7ab_ctrl_s c;
  int ret;

  if (argc == 5 && strcmp(argv[1], "install") == 0)
    {
      ret = ota_install(argv[2], argv[3], argv[4]);
    }
  else if ((argc == 2 || argc == 3) && strcmp(argv[1], "verify") == 0)
    {
      ret = ota_verify(argc == 3 ? argv[2] : NULL);
    }
  else if (argc == 2 && strcmp(argv[1], "info") == 0)
    {
      ret = k7ab_load(&c);
      if (ret == 0)
        {
          k7ab_print(&c);
        }
      else
        {
          printf("没有控制块（%d）：启动的是救援 FIT\n", ret);
          ret = ret == -ENOENT ? 0 : ret;
        }
    }
  else
    {
      printf("用法: ota <子命令>\n"
             "  install <url> <字节数> <sha256>\n"
             "          下载 FIT 写进没在运行的槽，回读校验一致才设为下次启动\n"
             "  verify [A|B]  回读槽内镜像重算 sha256\n"
             "  info          看控制块\n");
      return EXIT_FAILURE;
    }

  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
