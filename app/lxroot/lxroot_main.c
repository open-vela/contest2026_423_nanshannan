/****************************************************************************
 * app/lxroot/lxroot_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A72 上 Linux 的根文件系统分区维护命令。分区是 eMMC 出厂 GPT 的 backup
 * （384MB），由板级代码注册成 /dev/lxroot（kickpi_k7_appinit.c，名字和
 * 范围都核对过才注册）。
 *
 *   lxroot scan    全分区读一遍，报告有多少扇区非零（确认是空的）
 *   lxroot mark    在首、尾两个扇区写标记（两处原本必须是全零或旧标记）
 *   lxroot check   读回两处标记，判断是否完好
 *   lxroot write <url> <字节数> <sha256>
 *                  经 HTTP 下载镜像写进分区，回读校验后才写头部
 *   lxroot info    看头部
 *   lxroot verify  按头部回读本体重算 sha256
 *
 * 镜像格式见 amp/include/k7_lxroot.h。主机侧用 scripts/lxroot-push.py。
 *
 * mark / check 用来回答一个问题：U-Boot 每次启动会不会改写这个分区？
 * amp/FLASH.md 记过一次 —— 借来当中转站的区域被 U-Boot 写进了 bootctrl。
 * 做法是 mark 之后重启两次再 check。
 *
 * ★ 只通过 /dev/lxroot 访问，不碰 /dev/mmcsd0：本端 off_t 是 32 位，对整盘
 *   定位到 4GB 以外会静默回绕（2026-10-31 实测）。分区设备的偏移从分区
 *   起点算，384MB 不会溢出。
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

#include "k7_lxroot.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define LXROOT_DEV      "/dev/lxroot"
#define LXROOT_SECTOR   512
#define LXROOT_CHUNK    (64 * 1024)
#define LXROOT_MAGIC    "K7LXMARK"
#define LXROOT_NET_BUF  4096
#define LXROOT_NET_TMO  30         /* 秒；webclient 每次收发的超时 */
#define LXROOT_REPORT   (8u << 20) /* 每 8MB 报一次进度 */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static long elapsed_ms(const struct timespec *t0)
{
  struct timespec t1;

  clock_gettime(CLOCK_MONOTONIC, &t1);
  return (long)((t1.tv_sec - t0->tv_sec) * 1000 +
                (t1.tv_nsec - t0->tv_nsec) / 1000000);
}

/* 打开分区并取扇区数。分区设备经 BCH 以字符设备访问，SEEK_END 就是大小 */

static int lxroot_open(int flags, FAR uint32_t *nsect)
{
  off_t end;
  int fd;

  fd = open(LXROOT_DEV, flags);
  if (fd < 0)
    {
      printf("打不开 %s: %d（GPT 的 backup 分区没注册？看启动日志）\n",
             LXROOT_DEV, -errno);
      return -1;
    }

  end = lseek(fd, 0, SEEK_END);
  if (end <= 0 || end % LXROOT_SECTOR != 0)
    {
      printf("%s 大小不对: %jd\n", LXROOT_DEV, (intmax_t)end);
      close(fd);
      return -1;
    }

  /* 取完大小回到开头：scan 是从当前位置顺序读的 */

  if (lseek(fd, 0, SEEK_SET) != 0)
    {
      printf("%s 回不到开头: %d\n", LXROOT_DEV, -errno);
      close(fd);
      return -1;
    }

  *nsect = (uint32_t)(end / LXROOT_SECTOR);
  return fd;
}

static int rw_sector(int fd, uint32_t sect, FAR uint8_t *buf, bool wr)
{
  ssize_t n;

  if (lseek(fd, (off_t)sect * LXROOT_SECTOR, SEEK_SET) < 0)
    {
      return -errno;
    }

  n = wr ? write(fd, buf, LXROOT_SECTOR) : read(fd, buf, LXROOT_SECTOR);
  return n == LXROOT_SECTOR ? 0 : (n < 0 ? -errno : -EIO);
}

/* 标记内容只由扇区号决定：check 不需要额外保存什么就能逐字节比 */

static void make_mark(uint32_t sect, FAR uint8_t *buf)
{
  int i;

  memcpy(buf, LXROOT_MAGIC, 8);
  memcpy(buf + 8, &sect, 4);
  for (i = 12; i < LXROOT_SECTOR; i++)
    {
      buf[i] = (uint8_t)(i ^ (sect * 131) ^ 0x5a);
    }
}

static bool all_zero(FAR const uint8_t *buf, size_t len)
{
  while (len-- > 0)
    {
      if (*buf++ != 0)
        {
          return false;
        }
    }

  return true;
}

static int lxroot_scan(void)
{
  struct timespec t0;
  FAR uint8_t *buf;
  uint32_t nsect;
  uint32_t sect;
  uint32_t nz = 0;
  uint32_t first = 0;
  uint32_t last = 0;
  int fd;

  fd = lxroot_open(O_RDONLY, &nsect);
  if (fd < 0)
    {
      return -ENODEV;
    }

  buf = malloc(LXROOT_CHUNK);
  if (buf == NULL)
    {
      close(fd);
      return -ENOMEM;
    }

  printf("扫描 %s：%" PRIu32 " 扇区（%" PRIu32 " MB）\n", LXROOT_DEV, nsect,
         nsect / 2048);
  clock_gettime(CLOCK_MONOTONIC, &t0);

  for (sect = 0; sect < nsect; )
    {
      uint32_t cnt = LXROOT_CHUNK / LXROOT_SECTOR;
      ssize_t n;
      uint32_t i;

      if (cnt > nsect - sect)
        {
          cnt = nsect - sect;
        }

      n = read(fd, buf, cnt * LXROOT_SECTOR);
      if (n != (ssize_t)(cnt * LXROOT_SECTOR))
        {
          printf("读扇区 %" PRIu32 " 失败: %zd（errno %d）\n", sect, n,
                 errno);
          break;
        }

      for (i = 0; i < cnt; i++)
        {
          if (!all_zero(buf + i * LXROOT_SECTOR, LXROOT_SECTOR))
            {
              if (nz++ == 0)
                {
                  first = sect + i;
                }

              last = sect + i;
            }
        }

      sect += cnt;
      if (sect % (64 * 2048) == 0)
        {
          printf("  %" PRIu32 " MB，非零扇区 %" PRIu32 "\n", sect / 2048, nz);
        }
    }

  printf("扫完 %" PRIu32 "/%" PRIu32 " 扇区，用时 %ld ms\n", sect, nsect,
         elapsed_ms(&t0));

  /* 没扫完就不下结论：前面那段全零不代表整个分区是空的 */

  if (sect != nsect)
    {
      printf("结论：扫描中断，不下结论（已读部分非零扇区 %" PRIu32 "）\n",
             nz);
    }
  else if (nz == 0)
    {
      printf("结论：全零\n");
    }
  else
    {
      printf("结论：%" PRIu32 " 个扇区非零，第一个 %" PRIu32 "，最后一个 %"
             PRIu32 "（分区内扇区号）\n", nz, first, last);
    }

  free(buf);
  close(fd);
  return sect == nsect ? 0 : -EIO;
}

static int lxroot_mark(void)
{
  uint8_t buf[LXROOT_SECTOR];
  uint8_t mark[LXROOT_SECTOR];
  uint32_t where[2];
  uint32_t nsect;
  int ret;
  int fd;
  int i;

  fd = lxroot_open(O_RDWR, &nsect);
  if (fd < 0)
    {
      return -ENODEV;
    }

  where[0] = 0;
  where[1] = nsect - 1;

  /* 先全部检查再写：两处都得是全零或者本命令留下的旧标记 */

  for (i = 0; i < 2; i++)
    {
      make_mark(where[i], mark);
      ret = rw_sector(fd, where[i], buf, false);
      if (ret < 0)
        {
          printf("读扇区 %" PRIu32 " 失败: %d\n", where[i], ret);
          close(fd);
          return ret;
        }

      if (!all_zero(buf, sizeof(buf)) && memcmp(buf, mark, sizeof(buf)) != 0)
        {
          printf("扇区 %" PRIu32 " 既不是全零也不是旧标记，不写\n",
                 where[i]);
          close(fd);
          return -EEXIST;
        }
    }

  for (i = 0; i < 2; i++)
    {
      make_mark(where[i], mark);
      ret = rw_sector(fd, where[i], mark, true);
      if (ret < 0)
        {
          printf("写扇区 %" PRIu32 " 失败: %d\n", where[i], ret);
          close(fd);
          return ret;
        }
    }

  close(fd);
  printf("已在扇区 %" PRIu32 " 和 %" PRIu32 " 写入标记。"
         "重启后跑 lxroot check\n", where[0], where[1]);
  return 0;
}

static int lxroot_check(void)
{
  uint8_t buf[LXROOT_SECTOR];
  uint8_t mark[LXROOT_SECTOR];
  uint32_t where[2];
  uint32_t nsect;
  int bad = 0;
  int ret;
  int fd;
  int i;

  /* 重新打开一次：读到的是 eMMC 上的内容，而不是 BCH 的扇区缓存 */

  fd = lxroot_open(O_RDONLY, &nsect);
  if (fd < 0)
    {
      return -ENODEV;
    }

  where[0] = 0;
  where[1] = nsect - 1;

  for (i = 0; i < 2; i++)
    {
      int j;

      make_mark(where[i], mark);
      ret = rw_sector(fd, where[i], buf, false);
      if (ret < 0)
        {
          printf("读扇区 %" PRIu32 " 失败: %d\n", where[i], ret);
          bad++;
          continue;
        }

      if (memcmp(buf, mark, sizeof(buf)) == 0)
        {
          printf("扇区 %" PRIu32 ": 标记完好\n", where[i]);
          continue;
        }

      bad++;
      if (all_zero(buf, sizeof(buf)))
        {
          printf("扇区 %" PRIu32 ": 全零（没写过标记，或被清掉了）\n",
                 where[i]);
          continue;
        }

      for (j = 0; j < LXROOT_SECTOR && buf[j] == mark[j]; j++)
        {
        }

      printf("扇区 %" PRIu32 ": 被改动，第一处在字节 %d（期望 0x%02x 实际 "
             "0x%02x）\n", where[i], j, mark[j], buf[j]);
    }

  close(fd);
  return bad == 0 ? 0 : -EIO;
}

/* ---- 镜像写入 ---- */

struct lxroot_dl_s
{
  int                    fd;
  FAR uint8_t           *chunk;     /* 攒满 LXROOT_CHUNK 再写，减少小写 */
  uint32_t               fill;
  uint32_t               got;
  uint32_t               want;
  uint32_t               report;
  int                    err;
  long                   write_ms;  /* 花在写 eMMC 上的时间，其余是等网络 */
  struct timespec        t0;
  mbedtls_sha256_context sha;
};

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

static int dl_flush(FAR struct lxroot_dl_s *dl)
{
  struct timespec t0;
  ssize_t n;

  if (dl->fill == 0)
    {
      return 0;
    }

  clock_gettime(CLOCK_MONOTONIC, &t0);
  n = write(dl->fd, dl->chunk, dl->fill);
  dl->write_ms += elapsed_ms(&t0);
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
  FAR struct lxroot_dl_s *dl = arg;
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
      uint32_t n = LXROOT_CHUNK - dl->fill;

      if (n > len)
        {
          n = len;
        }

      memcpy(dl->chunk + dl->fill, p, n);
      dl->fill += n;
      p += n;
      len -= n;

      if (dl->fill == LXROOT_CHUNK)
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
      printf("  已收 %" PRIu32 " MB，%ld s\n", dl->got >> 20,
             elapsed_ms(&dl->t0) / 1000);
      dl->report += LXROOT_REPORT;
    }

  return 0;
}

/* 从分区回读本体并算 sha256。重新打开设备，读的是 eMMC 上的内容 */

static int readback_sha(uint32_t data_sector, uint32_t length,
                        FAR uint8_t *out)
{
  mbedtls_sha256_context sha;
  FAR uint8_t *buf;
  uint32_t left = length;
  uint32_t nsect;
  int ret = 0;
  int fd;

  fd = lxroot_open(O_RDONLY, &nsect);
  if (fd < 0)
    {
      return -ENODEV;
    }

  buf = malloc(LXROOT_CHUNK);
  if (buf == NULL)
    {
      close(fd);
      return -ENOMEM;
    }

  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);

  if (lseek(fd, (off_t)data_sector * LXROOT_SECTOR, SEEK_SET) < 0)
    {
      ret = -errno;
    }

  while (ret == 0 && left > 0)
    {
      uint32_t n = left > LXROOT_CHUNK ? LXROOT_CHUNK : left;
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

static int read_hdr(FAR struct k7lx_hdr_s *h, FAR uint32_t *nsect)
{
  uint8_t buf[LXROOT_SECTOR];
  int ret;
  int fd;

  fd = lxroot_open(O_RDONLY, nsect);
  if (fd < 0)
    {
      return -ENODEV;
    }

  ret = rw_sector(fd, 0, buf, false);
  close(fd);
  if (ret < 0)
    {
      return ret;
    }

  memcpy(h, buf, sizeof(*h));
  return 0;
}

/* 头部是否可信：魔数、版本、范围都要对 */

static bool hdr_valid(FAR const struct k7lx_hdr_s *h, uint32_t nsect)
{
  return memcmp(h->magic, K7LX_MAGIC, 8) == 0 &&
         h->version == K7LX_VERSION &&
         h->data_sector >= 1 && h->data_sector < nsect &&
         h->length > 0 &&
         h->length <= (uint64_t)(nsect - h->data_sector) * LXROOT_SECTOR;
}

static int lxroot_write(FAR const char *url, FAR const char *lenstr,
                        FAR const char *hex)
{
  struct webclient_context ctx;
  struct lxroot_dl_s dl;
  struct k7lx_hdr_s hdr;
  uint8_t want_sha[32];
  uint8_t got_sha[32];
  uint8_t sector[LXROOT_SECTOR];
  char hexbuf[65];
  FAR char *netbuf = NULL;
  uint32_t nsect;
  uint32_t length;
  long ms;
  int ret;

  length = (uint32_t)strtoul(lenstr, NULL, 0);
  if (parse_sha(hex, want_sha) < 0)
    {
      printf("sha256 要 64 个十六进制字符\n");
      return -EINVAL;
    }

  memset(&dl, 0, sizeof(dl));
  dl.fd = lxroot_open(O_RDWR, &nsect);
  if (dl.fd < 0)
    {
      return -ENODEV;
    }

  if (length == 0 ||
      length > (uint64_t)(nsect - K7LX_DATA_SECTOR) * LXROOT_SECTOR)
    {
      printf("长度 %" PRIu32 " 不对：分区最多放 %" PRIu32 " 字节\n", length,
             (nsect - K7LX_DATA_SECTOR) * LXROOT_SECTOR);
      close(dl.fd);
      return -EINVAL;
    }

  /* 1. 先让旧镜像失效：头部清零 */

  memset(sector, 0, sizeof(sector));
  ret = rw_sector(dl.fd, 0, sector, true);
  if (ret < 0)
    {
      printf("清头部失败: %d\n", ret);
      close(dl.fd);
      return ret;
    }

  /* 2. 边下边写边算 */

  dl.chunk = malloc(LXROOT_CHUNK);
  netbuf = malloc(LXROOT_NET_BUF);
  if (dl.chunk == NULL || netbuf == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  if (lseek(dl.fd, (off_t)K7LX_DATA_SECTOR * LXROOT_SECTOR, SEEK_SET) < 0)
    {
      ret = -errno;
      goto out;
    }

  dl.want = length;
  dl.report = LXROOT_REPORT;
  mbedtls_sha256_init(&dl.sha);
  mbedtls_sha256_starts(&dl.sha, 0);
  clock_gettime(CLOCK_MONOTONIC, &dl.t0);

  printf("下载 %s（%" PRIu32 " 字节）-> %s\n", url, length, LXROOT_DEV);

  webclient_set_defaults(&ctx);
  ctx.method = "GET";
  ctx.url = url;
  ctx.buffer = netbuf;
  ctx.buflen = LXROOT_NET_BUF;
  ctx.sink_callback = dl_sink;
  ctx.sink_callback_arg = &dl;
  ctx.timeout_sec = LXROOT_NET_TMO;

  ret = webclient_perform(&ctx);
  if (ret == 0 && dl.err == 0)
    {
      ret = dl_flush(&dl);
    }

  mbedtls_sha256_finish(&dl.sha, got_sha);
  mbedtls_sha256_free(&dl.sha);
  ms = elapsed_ms(&dl.t0);

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

  printf("收完 %" PRIu32 " 字节，%ld ms（%ld KB/s）；其中写 eMMC %ld ms\n",
         dl.got, ms, ms > 0 ? (long)((uint64_t)dl.got * 1000 / 1024 / ms) : 0,
         dl.write_ms);

  if (memcmp(got_sha, want_sha, 32) != 0)
    {
      sha_hex(got_sha, hexbuf);
      printf("收到的数据 sha256 不对: %s，不写头部\n", hexbuf);
      ret = -EBADMSG;
      goto out;
    }

  /* 3. 关掉再打开，从 eMMC 回读重算 */

  close(dl.fd);
  dl.fd = -1;

  ret = readback_sha(K7LX_DATA_SECTOR, length, got_sha);
  if (ret < 0 || memcmp(got_sha, want_sha, 32) != 0)
    {
      sha_hex(got_sha, hexbuf);
      printf("回读校验失败（%d）: %s，不写头部\n", ret, hexbuf);
      ret = ret < 0 ? ret : -EBADMSG;
      goto out;
    }

  printf("回读 sha256 一致\n");

  /* 4. 最后写头部，并读回比对 */

  memset(sector, 0, sizeof(sector));
  memset(&hdr, 0, sizeof(hdr));
  memcpy(hdr.magic, K7LX_MAGIC, 8);
  hdr.version = K7LX_VERSION;
  hdr.data_sector = K7LX_DATA_SECTOR;
  hdr.length = length;
  memcpy(hdr.sha256, want_sha, 32);
  memcpy(sector, &hdr, sizeof(hdr));

  dl.fd = lxroot_open(O_RDWR, &nsect);
  if (dl.fd < 0)
    {
      ret = -ENODEV;
      goto out;
    }

  ret = rw_sector(dl.fd, 0, sector, true);
  close(dl.fd);
  dl.fd = -1;
  if (ret < 0)
    {
      printf("写头部失败: %d\n", ret);
      goto out;
    }

  ret = read_hdr(&hdr, &nsect);
  if (ret < 0 || memcmp(&hdr, sector, sizeof(hdr)) != 0)
    {
      printf("头部回读不一致（%d）\n", ret);
      ret = ret < 0 ? ret : -EIO;
      goto out;
    }

  printf("完成：镜像 %" PRIu32 " 字节已写入并校验，头部有效\n", length);

out:
  if (dl.fd >= 0)
    {
      close(dl.fd);
    }

  free(dl.chunk);
  free(netbuf);
  return ret;
}

static int lxroot_info(bool verify)
{
  struct k7lx_hdr_s h;
  uint8_t sha[32];
  char hexbuf[65];
  uint32_t nsect;
  struct timespec t0;
  int ret;

  ret = read_hdr(&h, &nsect);
  if (ret < 0)
    {
      printf("读头部失败: %d\n", ret);
      return ret;
    }

  if (!hdr_valid(&h, nsect))
    {
      printf("没有有效镜像（头部无效）\n");
      return -ENOENT;
    }

  sha_hex(h.sha256, hexbuf);
  printf("镜像: 版本 %" PRIu32 "，起点扇区 %" PRIu32 "，%" PRIu32
         " 字节\nsha256 %s\n", h.version, h.data_sector, h.length, hexbuf);

  if (!verify)
    {
      return 0;
    }

  clock_gettime(CLOCK_MONOTONIC, &t0);
  ret = readback_sha(h.data_sector, h.length, sha);
  if (ret < 0)
    {
      printf("回读失败: %d\n", ret);
      return ret;
    }

  if (memcmp(sha, h.sha256, 32) != 0)
    {
      sha_hex(sha, hexbuf);
      printf("校验不一致，实际 %s\n", hexbuf);
      return -EBADMSG;
    }

  printf("校验一致，用时 %ld ms\n", elapsed_ms(&t0));
  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  int ret;

  if (argc == 2 && strcmp(argv[1], "scan") == 0)
    {
      ret = lxroot_scan();
    }
  else if (argc == 2 && strcmp(argv[1], "mark") == 0)
    {
      ret = lxroot_mark();
    }
  else if (argc == 2 && strcmp(argv[1], "check") == 0)
    {
      ret = lxroot_check();
    }
  else if (argc == 5 && strcmp(argv[1], "write") == 0)
    {
      ret = lxroot_write(argv[2], argv[3], argv[4]);
    }
  else if (argc == 2 && strcmp(argv[1], "info") == 0)
    {
      ret = lxroot_info(false);
    }
  else if (argc == 2 && strcmp(argv[1], "verify") == 0)
    {
      ret = lxroot_info(true);
    }
  else
    {
      printf("用法: lxroot <子命令>\n"
             "  scan    全分区读一遍，报告非零扇区\n"
             "  mark    在首尾两个扇区写标记\n"
             "  check   读回标记（重启后用，判断 U-Boot 有没有改写）\n"
             "  write <url> <字节数> <sha256>\n"
             "          下载镜像写进分区，回读校验一致才写头部\n"
             "  info    看头部\n"
             "  verify  按头部回读本体重算 sha256\n");
      return EXIT_FAILURE;
    }

  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
