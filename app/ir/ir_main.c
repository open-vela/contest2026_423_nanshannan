/****************************************************************************
 * app/ir/ir_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 红外接收 bring-up 命令：
 *
 *   ir init      注册 /dev/lirc0（手动触发，不在启动路径）
 *   ir st        中断/帧计数与寄存器快照
 *   ir raw       最近 64 段电平时长（P=载波低电平，S=间隔高电平，us）
 *   ir rx [秒]   读扫描码，默认 10 秒，到点即返回
 *
 * ★ 判读：对着接收头按遥控器后看 ir st
 *
 *     irqs 不涨           -> 中断没到本核（amp-irqs 漏了 132 / Linux 还占着）
 *                            或引脚复用不对
 *     irqs 涨、frames 不涨 -> 时钟换算或电平极性不对，看 ir raw 里的时长
 *     frames 涨            -> 解码通了，ir rx 能看到扫描码
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <nuttx/lirc.h>
#include <arch/board/board.h>

#define IR_DEV          "/dev/lirc0"
#define IR_RX_DEFAULT_S 10
#define IR_RX_MAX_S     600

static const char *ir_proto_name(unsigned int proto)
{
  switch (proto)
    {
      case RC_PROTO_NEC:
        return "NEC";
      case RC_PROTO_NECX:
        return "NECX";
      case RC_PROTO_NEC32:
        return "NEC32";
      default:
        return "?";
    }
}

static uint64_t now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 有界地读扫描码：整体截止时间到了就返回，不管收到多少 */

static int ir_rx(int seconds)
{
  struct lirc_scancode sc;
  struct pollfd pfd;
  uint64_t deadline;
  unsigned int mode = LIRC_MODE_SCANCODE;
  int count = 0;
  int fd;
  int ret;

  fd = open(IR_DEV, O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    {
      printf("打开 %s 失败: %d（先 ir init）\n", IR_DEV, errno);
      return 1;
    }

  ioctl(fd, LIRC_SET_REC_MODE, (unsigned long)&mode);

  printf("对着接收头按遥控器，%d 秒后返回\n", seconds);
  deadline = now_ms() + (uint64_t)seconds * 1000;

  for (; ; )
    {
      uint64_t now = now_ms();

      if (now >= deadline)
        {
          break;
        }

      pfd.fd      = fd;
      pfd.events  = POLLIN;
      pfd.revents = 0;

      ret = poll(&pfd, 1, (int)(deadline - now));
      if (ret <= 0)
        {
          continue;   /* 超时则下一轮检查截止时间后退出 */
        }

      while (read(fd, &sc, sizeof(sc)) == sizeof(sc))
        {
          printf("%-5s scancode=0x%06" PRIx64 "%s\n",
                 ir_proto_name(sc.rc_proto), sc.scancode,
                 (sc.flags & LIRC_SCANCODE_FLAG_REPEAT) ? " (repeat)" : "");
          count++;
        }
    }

  close(fd);
  printf("共收到 %d 个扫描码\n", count);
  return 0;
}

int main(int argc, FAR char *argv[])
{
  int ret;

  if (argc >= 2 && strcmp(argv[1], "init") == 0)
    {
      ret = kickpi_k7_ir_initialize();
      printf("IR init: %d\n", ret);
      return ret < 0 ? 1 : 0;
    }

  if (argc >= 2 &&
      (strcmp(argv[1], "st") == 0 || strcmp(argv[1], "raw") == 0))
    {
      ret = kickpi_k7_ir_dump(strcmp(argv[1], "raw") == 0);
      if (ret < 0)
        {
          printf("IR 未初始化，先 ir init\n");
          return 1;
        }

      return 0;
    }

  if (argc >= 2 && strcmp(argv[1], "rx") == 0)
    {
      int seconds = argc >= 3 ? atoi(argv[2]) : IR_RX_DEFAULT_S;

      if (seconds <= 0 || seconds > IR_RX_MAX_S)
        {
          seconds = IR_RX_DEFAULT_S;
        }

      return ir_rx(seconds);
    }

  printf("用法: ir init | st | raw | rx [秒]\n");
  return 1;
}
