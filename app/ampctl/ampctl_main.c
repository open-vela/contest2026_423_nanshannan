/****************************************************************************
 * apps/packages/demos/contest2026_423_ampctl/ampctl_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * AMP 排查工具。
 *
 * 这个命令存在的理由和 spi_selftest 一样：**AMP 出问题时，"没反应"有太多
 * 种原因**——对端没起来、门铃走错 group、vring 地址对不上、握手没完成、
 * 端点没绑定。一条 "ping 超时" 区分不了这五种，而它们的修法完全不同。
 *
 * 所以这里把链路拆成三层，每层单独有输出：
 *
 *   status  只看本端：mailbox 寄存器 + rptun 计数。**不需要对端在线**，
 *           因此永远有输出，是"是不是我这边就没配对"的判据。
 *   ping    建端点 → 发一帧 → 等回应，有超时。测的是端到端。
 *   listen  只收不发，用来看对端主动发了什么。
 *
 * 每一层都有界：没有任何一条路径会无限等待，最长就是 -t 给的毫秒数。
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <semaphore.h>
#include <pthread.h>

#include <nuttx/rpmsg/rpmsg.h>

#include <arch/chip/amp.h>

#ifdef CONFIG_RK3576_FRAME_SHM
#  include <nuttx/cache.h>
#  include <time.h>
/* jmorecfg.h 自己定义 EXTERN，和 NuttX 头文件里的同名宏冲突；后面不再
 * 用到 NuttX 的那个。
 */
#  undef EXTERN
#  include <jpeglib.h>
#  include "k7_frame.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define AMPCTL_EPT_NAME   "amp-echo"
#define AMPCTL_DEF_TMO_MS 3000

/* exec 用的端点名。Linux 的 rpmsg_char 只认这一个名字（id_table），
 * 认出来就生成 /dev/rpmsgN，k7d 在那头等着。
 */

#define AMPCTL_EXEC_EPT   "rpmsg-raw"

/* k7d 自己 60s 杀超时的命令；这里多等一点，让它的 'E' 帧有机会回来。 */

#define AMPCTL_EXEC_TMO_MS  70000

/* 收包环。rptun 线程往里写、nsh 任务往外打印。串口 1.5M 约 150KB/s，
 * Linux 那边吐得比这快，所以环满时回调会有界地等一会儿（见 exec_cb）。
 */

#define AMPCTL_RING_SIZE  16384

/* 自检用的 group。必须是 rptun 没占用的 —— 见 rk3576_mailbox_selftest()
 * 的说明：在 rptun 的 group 上放假门铃会把它推进没有对端的非法状态。
 */

#define AMPCTL_SELFTEST_GROUP 5

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ampctl_ctx_s
{
  struct rpmsg_endpoint ept;
  sem_t                 sem;      /* 收到回应时 post                 */
  bool                  bound;    /* 端点已经建起来                  */
  char                  rxbuf[256];
  size_t                rxlen;
  const char           *cpuname;
  const char           *eptname;
  rpmsg_ept_cb          cb;
};

/* exec 的状态。base 必须是第一个成员：回调里的 priv 就是 base 的地址。 */

struct ampctl_exec_s
{
  struct ampctl_ctx_s   base;
  pthread_mutex_t       lock;
  char                  ring[AMPCTL_RING_SIZE];
  size_t                head;     /* 写位置（回调推进）              */
  size_t                tail;     /* 读位置（打印推进）              */
  size_t                dropped;  /* 等了也没腾出地方而丢掉的字节    */
  bool                  hello;    /* 收到 'H'：知道对端地址了        */
  bool                  done;     /* 收到 'E'                        */
  int                   status;   /* 'E' 带回的退出码                */
  char                  hellomsg[128];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char * const g_mbox_regname[8] =
{
  "A2B_INTEN", "A2B_STATUS", "A2B_CMD", "A2B_DATA",
  "B2A_INTEN", "B2A_STATUS", "B2A_CMD", "B2A_DATA"
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void ampctl_usage(void)
{
  printf("用法: ampctl <子命令>\n"
         "  status            本端链路状态（不需要对端在线）\n"
         "  ping [-t ms] [文本]  发一帧并等回应，默认超时 %d ms\n"
         "  listen [-t ms]    只收不发，打印对端发来的帧\n"
         "  selftest          自己给自己按门铃，验证接收路径（不需要对端）\n"
         "  exec <命令...>     让 A72 上的 Linux 执行 shell 命令，打印输出\n"
         "                    例: ampctl exec cat /proc/cpuinfo\n"
#ifdef CONFIG_RK3576_FRAME_SHM
         "  frame             读帧共享区的最新一帧；测试图案逐像素核对\n"
         "                    （先 ampctl exec k7d frame 让 Linux 写一帧）\n"
         "  cam start|stop    让 Linux 持续把摄像头画面写进帧共享区（30fps）\n"
         "  snap <宽> <高> <文件.jpg>\n"
         "                    拍一张存成 JPEG（参数同 v4l2cap；不放大）\n"
#endif
         , AMPCTL_DEF_TMO_MS);
}

/****************************************************************************
 * Name: ampctl_status
 *
 * Description:
 *   只读本端状态。三组信息各自回答一个问题：
 *
 *     rptun 计数  —— 门铃通没通？握手成没成？
 *     mailbox 寄存器 —— 硬件层面有没有卡住的 pending 位？
 *     /dev/rpmsg  —— rpmsg 设备有没有真的注册出来？
 *
 ****************************************************************************/

static int ampctl_status(void)
{
  struct rk3576_rptun_stat_s st;
  uint32_t regs[8];
  int i;
  int ret;

  ret = rk3576_rptun_getstat(&st);
  if (ret < 0)
    {
      printf("读 rptun 状态失败: %d\n", ret);
      return ret;
    }

  printf("== rptun ==\n");
  printf("  已注册    : %s\n", st.registered ? "是" : "否");
  printf("  握手完成  : %s%s\n", st.driver_ok ? "是" : "否",
         st.driver_ok ? "" : "  ← 还没收到对端的第一次门铃");
  printf("  收到门铃  : %" PRIu32 "\n", st.kicks_rx);
  printf("  发出门铃  : %" PRIu32 "\n", st.kicks_tx);
  printf("  发被挡回  : %" PRIu32 "%s\n", st.tx_busy,
         st.tx_busy > 0 ? "  ← 对端没在清信箱" : "");
  printf("  最后收到  : cmd=%08" PRIx32 " data=%08" PRIx32 "%s\n",
         st.last_cmd, st.last_data,
         (st.kicks_rx == 0 && st.last_data != 0) ?
           "  ← 收到了但不是本链路的" : "");

  printf("== 共享内存 ==\n");
  printf("  vring0(发) : %08x\n", RK3576_RPTUN_VRING0_DA);
  printf("  vring1(收) : %08x\n", RK3576_RPTUN_VRING1_DA);
  printf("  缓冲池     : %08x + %dK\n",
         RK3576_RPTUN_POOL_DA, RK3576_RPTUN_POOL_LEN / 1024);

  printf("== mailbox group%d（收）==\n", CONFIG_RK3576_RPTUN_RX_GROUP);
  ret = rk3576_mailbox_dump(CONFIG_RK3576_RPTUN_RX_GROUP, regs);
  if (ret >= 0)
    {
      for (i = 0; i < 8; i++)
        {
          printf("  %-10s %08" PRIx32 "\n", g_mbox_regname[i], regs[i]);
        }
    }

  printf("== mailbox group%d（发）==\n", CONFIG_RK3576_RPTUN_TX_GROUP);
  ret = rk3576_mailbox_dump(CONFIG_RK3576_RPTUN_TX_GROUP, regs);
  if (ret >= 0)
    {
      for (i = 0; i < 8; i++)
        {
          printf("  %-10s %08" PRIx32 "\n", g_mbox_regname[i], regs[i]);
        }
    }

  /* rpmsg 设备节点存在 == rptun 已经跑完 dev_start。 */

  printf("== rpmsg 设备 ==\n");
  if (access("/dev/rpmsg/" CONFIG_RK3576_RPTUN_CPUNAME, F_OK) == 0)
    {
      printf("  /dev/rpmsg/%s 存在\n", CONFIG_RK3576_RPTUN_CPUNAME);
    }
  else
    {
      printf("  /dev/rpmsg/%s 不存在 —— rptun 还卡在 -EAGAIN\n",
             CONFIG_RK3576_RPTUN_CPUNAME);
    }

  return OK;
}

/****************************************************************************
 * Name: ampctl_selftest
 *
 * Description:
 *   门铃接收路径自检。分两步，**第二步同样重要**：
 *
 *     正例：写一次 → 应该进中断，且读回的两个字与写入一致
 *     反例：不写   → 应该超时
 *
 *   只跑正例是不够的。一个永远返回"成功"的自检比没有自检更坏 —— 本项目
 *   已经被四个这样的假仪器坑过（RXFIFOLR、GPIO 探针、计数器没使能的
 *   RX_DATA_CNT、参数未初始化的回环）。每一个都给出了**自信的错误答案**，
 *   而不是"未知"。所以这里必须证明它会失败。
 *
 ****************************************************************************/

static int ampctl_selftest(void)
{
  uint32_t rx_cmd = 0;
  uint32_t rx_data = 0;
  unsigned int group = AMPCTL_SELFTEST_GROUP;
  int ret;

  if (group == CONFIG_RK3576_RPTUN_RX_GROUP ||
      group == CONFIG_RK3576_RPTUN_TX_GROUP)
    {
      printf("自检用的 group%u 和 rptun 撞了，换一个\n", group);
      return -EBUSY;
    }

  printf("在空闲的 mailbox group%u 上自检（不影响 rptun）\n", group);

  /* 正例 */

  ret = rk3576_mailbox_selftest(group, 0xa5a50003, 0x524d5347,
                                &rx_cmd, &rx_data);
  if (ret == OK)
    {
      printf("  正例 ✓ 中断到达，读回 cmd=%08" PRIx32 " data=%08" PRIx32 "\n",
             rx_cmd, rx_data);
    }
  else
    {
      printf("  正例 ✗ %d（%s）\n", ret,
             ret == -ETIMEDOUT ? "没进中断：时钟/IRQ 号/INTEN 三者之一不对" :
             ret == -EIO       ? "进了中断但读回的值对不上" : "参数或占用");
      return ret;
    }

  /* 反例：同一条路径，但不触发。必须超时 —— 否则说明它根本不在测中断。 */

  rx_cmd = rx_data = 0;
  ret = rk3576_mailbox_selftest_notrigger(group, &rx_cmd, &rx_data);
  if (ret == -ETIMEDOUT)
    {
      printf("  反例 ✓ 不写 DATA 时如期超时 —— 说明正例测的确实是中断\n");
      return OK;
    }

  printf("  反例 ✗ 不触发也返回了 %d —— 这个自检不可信，别用它下结论\n", ret);
  return -EIO;
}

/****************************************************************************
 * Name: ampctl_ept_cb
 *
 * Description:
 *   端点收到数据。拷走并唤醒等待方 —— data 在回调返回后就不再有效。
 *
 ****************************************************************************/

static int ampctl_ept_cb(struct rpmsg_endpoint *ept, void *data, size_t len,
                         uint32_t src, void *priv)
{
  struct ampctl_ctx_s *ctx = priv;

  UNUSED(src);

  ctx->rxlen = len < sizeof(ctx->rxbuf) - 1 ? len : sizeof(ctx->rxbuf) - 1;
  memcpy(ctx->rxbuf, data, ctx->rxlen);
  ctx->rxbuf[ctx->rxlen] = '\0';

  rpmsg_post(ept, &ctx->sem);
  return 0;
}

/****************************************************************************
 * Name: ampctl_device_created
 *
 * Description:
 *   rpmsg 设备起来了。只有名字对得上的那一个才建端点 —— 板上可能不止
 *   一条 rpmsg 链路。
 *
 ****************************************************************************/

static void ampctl_device_created(struct rpmsg_device *rdev, void *priv)
{
  struct ampctl_ctx_s *ctx = priv;

  if (strcmp(rpmsg_get_cpuname(rdev), ctx->cpuname) != 0)
    {
      return;
    }

  if (rpmsg_create_ept(&ctx->ept, rdev, ctx->eptname,
                       RPMSG_ADDR_ANY, RPMSG_ADDR_ANY,
                       ctx->cb, NULL) == 0)
    {
      ctx->ept.priv = ctx;
      ctx->bound    = true;
    }
}

static void ampctl_device_destroy(struct rpmsg_device *rdev, void *priv)
{
  struct ampctl_ctx_s *ctx = priv;

  if (strcmp(rpmsg_get_cpuname(rdev), ctx->cpuname) == 0 && ctx->bound)
    {
      rpmsg_destroy_ept(&ctx->ept);
      ctx->bound = false;
    }
}

/****************************************************************************
 * Name: ampctl_open
 *
 * Description:
 *   注册回调并等端点建起来。rpmsg_register_callback() 对**已经存在**的
 *   设备会立刻回调，所以不会漏掉"注册之前设备就好了"的情况。
 *
 ****************************************************************************/

static int ampctl_open(struct ampctl_ctx_s *ctx, int timeout_ms,
                       const char *eptname, rpmsg_ept_cb cb)
{
  int waited = 0;

  memset(ctx, 0, sizeof(*ctx));
  ctx->cpuname = CONFIG_RK3576_RPTUN_CPUNAME;
  ctx->eptname = eptname;
  ctx->cb      = cb;
  sem_init(&ctx->sem, 0, 0);

  rpmsg_register_callback(ctx, ampctl_device_created,
                          ampctl_device_destroy, NULL, NULL);

  /* 有界等待：每 20ms 看一次，最多等 timeout_ms。 */

  while (!ctx->bound && waited < timeout_ms)
    {
      usleep(20 * 1000);
      waited += 20;
    }

  if (!ctx->bound)
    {
      rpmsg_unregister_callback(ctx, ampctl_device_created,
                                ampctl_device_destroy, NULL, NULL);
      sem_destroy(&ctx->sem);
      printf("端点没建起来（等了 %d ms）。先跑 ampctl status 看握手完成没有。\n",
             timeout_ms);
      return -ETIMEDOUT;
    }

  return OK;
}

static void ampctl_close(struct ampctl_ctx_s *ctx)
{
  rpmsg_unregister_callback(ctx, ampctl_device_created,
                            ampctl_device_destroy, NULL, NULL);
  if (ctx->bound)
    {
      rpmsg_destroy_ept(&ctx->ept);
      ctx->bound = false;
    }

  sem_destroy(&ctx->sem);
}

static int ampctl_ping(int timeout_ms, const char *text)
{
  struct ampctl_ctx_s ctx;
  int ret;

  ret = ampctl_open(&ctx, timeout_ms, AMPCTL_EPT_NAME, ampctl_ept_cb);
  if (ret < 0)
    {
      return ret;
    }

  printf("端点已绑定，发送 \"%s\"\n", text);

  ret = rpmsg_send(&ctx.ept, text, strlen(text) + 1);
  if (ret < 0)
    {
      printf("发送失败: %d\n", ret);
      goto out;
    }

  ret = rpmsg_tickwait(&ctx.ept, &ctx.sem, MSEC2TICK(timeout_ms));
  if (ret < 0)
    {
      printf("等回应超时（%d ms）。对端收到了吗？看 ampctl status 的"
             "「发出门铃」有没有涨。\n", timeout_ms);
    }
  else
    {
      printf("回应 %zu 字节: \"%s\"\n", ctx.rxlen, ctx.rxbuf);
      ret = OK;
    }

out:
  ampctl_close(&ctx);
  return ret;
}

static int ampctl_listen(int timeout_ms)
{
  struct ampctl_ctx_s ctx;
  int ret;

  ret = ampctl_open(&ctx, timeout_ms, AMPCTL_EPT_NAME, ampctl_ept_cb);
  if (ret < 0)
    {
      return ret;
    }

  printf("监听中，最长 %d ms…\n", timeout_ms);

  ret = rpmsg_tickwait(&ctx.ept, &ctx.sem, MSEC2TICK(timeout_ms));
  if (ret < 0)
    {
      printf("这段时间内对端没发东西。\n");
    }
  else
    {
      printf("收到 %zu 字节: \"%s\"\n", ctx.rxlen, ctx.rxbuf);
      ret = OK;
    }

  ampctl_close(&ctx);
  return ret;
}

/****************************************************************************
 * Name: ampctl_exec_cb
 *
 * Description:
 *   跑在 rptun 线程里。帧格式见 amp/linux/rootfs/k7d.c 开头：首字节是
 *   类型，'H' 握手、'O' 输出、'E' 结束。
 *
 *   'O' 进环；环满时**有界地**等打印那边腾地方（最多约 1s）。等是为了
 *   不丢字 —— Linux 吐得比串口快；有界是因为 rptun 线程被卡住期间，这条
 *   链路上别的端点也收不到东西。真等不到就丢，并记下丢了多少。
 *
 ****************************************************************************/

static int ampctl_exec_cb(struct rpmsg_endpoint *ept, void *data, size_t len,
                          uint32_t src, void *priv)
{
  struct ampctl_exec_s *ex = priv;
  const char *p = data;
  int waited = 0;
  size_t i;

  UNUSED(src);

  if (len < 1)
    {
      return 0;
    }

  pthread_mutex_lock(&ex->lock);

  switch (p[0])
    {
      case 'H':
        len = len - 1 < sizeof(ex->hellomsg) - 1 ?
              len - 1 : sizeof(ex->hellomsg) - 1;
        memcpy(ex->hellomsg, p + 1, len);
        ex->hellomsg[len] = '\0';
        ex->hello = true;
        break;

      case 'O':
        for (i = 1; i < len; i++)
          {
            while (ex->head - ex->tail >= AMPCTL_RING_SIZE && waited < 1000)
              {
                pthread_mutex_unlock(&ex->lock);
                rpmsg_post(ept, &ex->base.sem);
                usleep(10 * 1000);
                waited += 10;
                pthread_mutex_lock(&ex->lock);
              }

            if (ex->head - ex->tail >= AMPCTL_RING_SIZE)
              {
                ex->dropped += len - i;
                break;
              }

            ex->ring[ex->head++ % AMPCTL_RING_SIZE] = p[i];
          }
        break;

      case 'E':
        {
          char num[12];

          len = len - 1 < sizeof(num) - 1 ? len - 1 : sizeof(num) - 1;
          memcpy(num, p + 1, len);
          num[len] = '\0';
          ex->status = atoi(num);
          ex->done   = true;
        }
        break;

      default:
        break;
    }

  pthread_mutex_unlock(&ex->lock);
  rpmsg_post(ept, &ex->base.sem);
  return 0;
}

/* 把环里现有的字节打印出来。返回 true 表示已经收到 'E'、而且环已经空了。 */

static bool ampctl_exec_drain(struct ampctl_exec_s *ex)
{
  char buf[256];
  bool done;
  size_t n;

  for (; ; )
    {
      pthread_mutex_lock(&ex->lock);
      for (n = 0; n < sizeof(buf) && ex->tail != ex->head; n++)
        {
          buf[n] = ex->ring[ex->tail++ % AMPCTL_RING_SIZE];
        }

      done = ex->done && ex->tail == ex->head;
      pthread_mutex_unlock(&ex->lock);

      if (n == 0)
        {
          return done;
        }

      fwrite(buf, 1, n, stdout);
    }
}

/****************************************************************************
 * Name: ampctl_exec
 *
 * Description:
 *   让 A72 簇上的 Linux 执行一条 shell 命令，把输出打出来。
 *
 *   三段，每段都有界：
 *     1. 建 "rpmsg-raw" 端点，等 k7d 发来 'H'（它轮询 /dev/rpmsgN，
 *        50ms 一次）。收到之前不能发 —— 还不知道对端地址。
 *     2. 发 'X' + 命令。
 *     3. 收 'O' 打印，直到 'E'。
 *
 *   返回 Linux 那边的退出码（>= 0），或链路错误（< 0）。
 *
 ****************************************************************************/

static int ampctl_exec(int timeout_ms, const char *cmd)
{
  struct ampctl_exec_s *ex;
  char frame[480];
  clock_t deadline;
  size_t len;
  int ret;

  len = strlen(cmd);
  if (len == 0 || len > sizeof(frame) - 2)
    {
      printf("命令为空或超过 %zu 字节\n", sizeof(frame) - 2);
      return -EINVAL;
    }

  /* 16KB 的环放栈上太大，放堆上 */

  ex = calloc(1, sizeof(*ex));
  if (ex == NULL)
    {
      return -ENOMEM;
    }

  pthread_mutex_init(&ex->lock, NULL);

  ret = ampctl_open(&ex->base, timeout_ms, AMPCTL_EXEC_EPT, ampctl_exec_cb);
  if (ret < 0)
    {
      goto out_free;
    }

  deadline = clock() + MSEC2TICK(timeout_ms);
  while (!ex->hello && clock() < deadline)
    {
      rpmsg_tickwait(&ex->base.ept, &ex->base.sem, MSEC2TICK(100));
    }

  if (!ex->hello)
    {
      printf("Linux 没有应答（等了 %d ms）。\n"
             "  ampctl status 握手完成=否  → Linux 内核没起来\n"
             "  握手完成=是                → 用户态 k7d 没在跑"
             "（Image 里没带 initramfs？）\n", timeout_ms);
      ret = -ETIMEDOUT;
      goto out_close;
    }

  printf("[%s]\n", ex->hellomsg);

  frame[0] = 'X';
  memcpy(frame + 1, cmd, len);
  ret = rpmsg_send(&ex->base.ept, frame, len + 1);
  if (ret < 0)
    {
      printf("发送失败: %d\n", ret);
      goto out_close;
    }

  deadline = clock() + MSEC2TICK(AMPCTL_EXEC_TMO_MS);
  while (!ampctl_exec_drain(ex))
    {
      if (clock() >= deadline)
        {
          printf("\n等 Linux 结束超时（%d ms）\n", AMPCTL_EXEC_TMO_MS);
          ret = -ETIMEDOUT;
          goto out_close;
        }

      rpmsg_tickwait(&ex->base.ept, &ex->base.sem, MSEC2TICK(100));
    }

  fflush(stdout);
  if (ex->dropped > 0)
    {
      printf("\n（输出太快，丢了 %zu 字节）\n", ex->dropped);
    }

  /* NSH 的 $? 只分成功/失败，具体的退出码只能在这里打出来 */

  ret = ex->status;
  if (ret != 0)
    {
      printf("[退出码 %d%s]\n", ret, ret == 124 ? "：Linux 侧 60s 超时被杀" : "");
    }

out_close:
  ampctl_close(&ex->base);
out_free:
  pthread_mutex_destroy(&ex->lock);
  free(ex);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#ifdef CONFIG_RK3576_FRAME_SHM
/****************************************************************************
 * Name: ampctl_frame
 *
 * Description:
 *   读帧共享区（amp/include/k7_frame.h）的最新一帧。来源是测试图案时，
 *   按同一个公式逐像素核对，报告不一致的个数和第一处的位置与数值。
 *
 *   ★ 读之前 invalidate：本端把共享区映成 cacheable（rk3576_boot.c），
 *     不 invalidate 就可能读到上一轮留在 cache 里的旧帧，而且不报错。
 *
 *   帧头里的每个数都先做范围检查再用：它是对端写的，写坏了不能让本端
 *   越界去读。
 *
 ****************************************************************************/

static int ampctl_frame(void)
{
  uintptr_t base = CONFIG_RK3576_FRAME_SHM_BASE;
  FAR volatile struct k7f_hdr_s *vh = (FAR volatile void *)base;
  struct k7f_hdr_s h;
  struct timespec t0;
  struct timespec t1;
  FAR const uint32_t *px;
  uint32_t seq2;
  uint32_t off;
  uint32_t bad = 0;
  uint32_t bx = 0;
  uint32_t by = 0;
  uint32_t bexp = 0;
  uint32_t bgot = 0;
  uint32_t x;
  uint32_t y;

  up_invalidate_dcache(base, base + K7F_HDR_SIZE);
  memcpy(&h, (FAR const void *)vh, sizeof(h));

  if (h.magic != K7F_MAGIC || h.version != K7F_VERSION)
    {
      printf("帧共享区未初始化: magic=0x%08" PRIx32 " version=%" PRIu32
             "（期望 0x%08x / %d）\n",
             h.magic, h.version, K7F_MAGIC, K7F_VERSION);
      return -ENODATA;
    }

  if (h.seq == 0)
    {
      printf("还没有帧（seq=0）\n");
      return -ENODATA;
    }

  off = k7f_slot_offset(&h, h.slot);
  if (h.slot >= K7F_SLOTS || h.slot_size == 0 ||
      (uint64_t)K7F_HDR_SIZE + (uint64_t)h.slot_size * K7F_SLOTS >
      CONFIG_RK3576_FRAME_SHM_SIZE ||
      h.width == 0 || h.height == 0 || h.stride < h.width * 4 ||
      h.bytes != h.stride * h.height || h.bytes > h.slot_size)
    {
      printf("帧头不合理: 槽=%" PRIu32 "/%d 槽大小=%" PRIu32 " %" PRIu32
             "x%" PRIu32 " stride=%" PRIu32 " bytes=%" PRIu32 "\n",
             h.slot, K7F_SLOTS, h.slot_size, h.width, h.height, h.stride,
             h.bytes);
      return -EINVAL;
    }

  printf("seq=%" PRIu32 " 槽=%" PRIu32 " %" PRIu32 "x%" PRIu32
         " stride=%" PRIu32 " 格式=%.4s 来源=%" PRIu32 " seed=%" PRIu32 "\n",
         h.seq, h.slot, h.width, h.height, h.stride,
         (FAR const char *)&h.format, h.source, h.seed);

  if (h.source != K7F_SRC_PATTERN || h.format != K7F_FMT_XRGB8888)
    {
      printf("不是测试图案，不核对\n");
      return 0;
    }

  clock_gettime(CLOCK_MONOTONIC, &t0);
  up_invalidate_dcache(base + off, base + off + h.bytes);

  for (y = 0; y < h.height; y++)
    {
      px = (FAR const uint32_t *)(base + off + y * h.stride);
      for (x = 0; x < h.width; x++)
        {
          uint32_t exp = k7f_pattern(x, y, h.width, h.height, h.seed);

          if (px[x] != exp)
            {
              if (bad++ == 0)
                {
                  bx = x;
                  by = y;
                  bexp = exp;
                  bgot = px[x];
                }
            }
        }
    }

  clock_gettime(CLOCK_MONOTONIC, &t1);

  /* 核对期间对端又发了几帧？绕回本槽（差 >= 槽数-1）才可能读到半帧 */

  up_invalidate_dcache(base, base + K7F_HDR_SIZE);
  seq2 = vh->seq;
  if (seq2 - h.seq >= K7F_SLOTS - 1)
    {
      printf("核对期间对端发布到 seq=%" PRIu32 "，本槽可能已被覆盖，"
             "结果作废\n", seq2);
      return -EAGAIN;
    }

  if (bad == 0)
    {
      printf("核对一致: %" PRIu32 " 个像素全部相等，用时 %ld ms\n",
             h.width * h.height,
             (long)((t1.tv_sec - t0.tv_sec) * 1000 +
                    (t1.tv_nsec - t0.tv_nsec) / 1000000));
      return 0;
    }

  printf("核对不一致: %" PRIu32 " 个像素不同；第一处 (%" PRIu32 ",%" PRIu32
         ") 期望 0x%08" PRIx32 " 实际 0x%08" PRIx32 "\n",
         bad, bx, by, bexp, bgot);
  return 1;
}
#endif

#ifdef CONFIG_RK3576_FRAME_SHM
/****************************************************************************
 * 摄像头（归 Linux）：cam start / cam stop / snap
 *
 * 摄像头整条链在 A72 的 Linux 上（amp/OWNERSHIP.md），openvela 这边没有
 * /dev/video0。画面经帧共享区过来：Linux 的 k7cam 写，这里读。
 *
 *   cam start  后台起 k7cam：持续开流，每帧发布（30fps），最长
 *              10 分钟；已经在跑就先让旧的停下。先走 ISP（live-isp，
 *              rkaiq 管曝光白平衡，没在跑就先 k7aiq start）；rkaiq 起不来
 *              或 ISP 一帧都取不到，退回 RAW 直出的 live。输出在 Linux 的
 *              /tmp/k7live.log，`ampctl exec cat /tmp/k7live.log` 看。
 *   cam stop   建停止文件，k7cam 半秒内看到就退。
 *   snap       live 在跑就直接拿最新一帧；没在跑就让 Linux 拍一帧。
 *              参数和 v4l2cap 一样，界面和视觉问答换个程序名就能用。
 *
 * 曝光 2242 行（一帧满）、增益 120 是 RAW 退路用的：没有 ISP 的 AE，
 * k7cam 按 99% 分位拉伸亮度，这组值在室内灯光下不过曝也不全黑。
 ****************************************************************************/

#define CAM_ARGS  "2242 120"

/* 等帧共享区出现比 seq0 新的帧，最多 ms 毫秒；返回新 seq，超时返回 0 */

static uint32_t cam_wait_seq(uint32_t seq0, int ms)
{
  uintptr_t base = CONFIG_RK3576_FRAME_SHM_BASE;
  FAR volatile struct k7f_hdr_s *vh = (FAR volatile void *)base;
  int waited;

  for (waited = 0; waited <= ms; waited += 20)
    {
      up_invalidate_dcache(base, base + K7F_HDR_SIZE);
      if (vh->magic == K7F_MAGIC && vh->seq != seq0)
        {
          return vh->seq;
        }

      usleep(20 * 1000);
    }

  return 0;
}

static uint32_t cam_cur_seq(void)
{
  uintptr_t base = CONFIG_RK3576_FRAME_SHM_BASE;

  up_invalidate_dcache(base, base + K7F_HDR_SIZE);
  return ((FAR volatile struct k7f_hdr_s *)base)->seq;
}

static int ampctl_cam(const char *what)
{
  if (strcmp(what, "start") == 0)
    {
      return ampctl_exec(AMPCTL_DEF_TMO_MS,
        "if pidof k7cam >/dev/null; then touch /tmp/k7live.stop; sleep 1; "
        "fi; (pidof rkaiq_3A_server >/dev/null || k7aiq start; "
        "pidof rkaiq_3A_server >/dev/null && k7cam live-isp 600 || "
        "k7cam live 600 " CAM_ARGS ") </dev/null >/tmp/k7live.log 2>&1 &");
    }

  if (strcmp(what, "stop") == 0)
    {
      return ampctl_exec(AMPCTL_DEF_TMO_MS, "touch /tmp/k7live.stop");
    }

  ampctl_usage();
  return -EINVAL;
}

/* 把最新一帧缩到不超过 w x h（不放大、取整数步长的最近点）存成 JPEG */

static int ampctl_snap(int w, int h, const char *path)
{
  uintptr_t base = CONFIG_RK3576_FRAME_SHM_BASE;
  FAR const struct k7f_hdr_s *vh = (FAR const void *)base;
  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr jerr;
  struct k7f_hdr_s hd;
  FAR unsigned char *row;
  FAR uint32_t *pix;
  uint32_t seq0;
  uint32_t ow;
  uint32_t oh;
  uint32_t x;
  uint32_t y;
  FILE *fp;
  int ret;

  /* live 在跑：300ms 内一定有新帧（30fps），直接用 */

  seq0 = cam_cur_seq();
  if (cam_wait_seq(seq0, 300) == 0)
    {
      ret = ampctl_exec(AMPCTL_DEF_TMO_MS,
                        "k7cam cap 1 show " CAM_ARGS " >/dev/null 2>&1");
      if (ret != 0 || cam_wait_seq(seq0, 2000) == 0)
        {
          printf("snap: Linux 没有拍到新帧（%d）\n", ret);
          return -EIO;
        }
    }

  up_invalidate_dcache(base, base + K7F_HDR_SIZE);
  memcpy(&hd, vh, sizeof(hd));
  if (hd.magic != K7F_MAGIC || hd.slot >= K7F_SLOTS ||
      hd.format != K7F_FMT_XRGB8888 || hd.width == 0 || hd.height == 0 ||
      hd.width > 4096 || hd.height > 4096 || hd.stride < hd.width * 4 ||
      hd.bytes != hd.stride * hd.height || hd.bytes > hd.slot_size ||
      (uint64_t)K7F_HDR_SIZE + (uint64_t)hd.slot_size * K7F_SLOTS >
      CONFIG_RK3576_FRAME_SHM_SIZE)
    {
      printf("snap: 帧头不合理\n");
      return -EINVAL;
    }

  /* 先整帧拷出来再编码：编码要几十毫秒，live 期间槽会被覆盖 */

  pix = malloc(hd.bytes);
  row = malloc(hd.width * 3);
  if (pix == NULL || row == NULL)
    {
      free(pix);
      free(row);
      return -ENOMEM;
    }

  up_invalidate_dcache(base + k7f_slot_offset(&hd, hd.slot),
                       base + k7f_slot_offset(&hd, hd.slot) + hd.bytes);
  memcpy(pix, (FAR const void *)(base + k7f_slot_offset(&hd, hd.slot)),
         hd.bytes);
  if (cam_cur_seq() - hd.seq >= K7F_SLOTS - 1)
    {
      printf("snap: 拷贝期间槽被覆盖，重试一次\n");
      free(pix);
      free(row);
      return -EAGAIN;
    }

  ow = w > 0 && (uint32_t)w < hd.width ? (uint32_t)w : hd.width;
  oh = h > 0 && (uint32_t)h < hd.height ? (uint32_t)h : hd.height;

  fp = fopen(path, "wb");
  if (fp == NULL)
    {
      printf("snap: 打不开 %s\n", path);
      free(pix);
      free(row);
      return -errno;
    }

  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);
  jpeg_stdio_dest(&cinfo, fp);
  cinfo.image_width = ow;
  cinfo.image_height = oh;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, 85, TRUE);
  jpeg_start_compress(&cinfo, TRUE);

  for (y = 0; y < oh; y++)
    {
      FAR const uint32_t *src = (FAR const uint32_t *)
        ((FAR const uint8_t *)pix + (y * hd.height / oh) * hd.stride);

      for (x = 0; x < ow; x++)
        {
          uint32_t v = src[x * hd.width / ow];

          row[3 * x]     = v >> 16;
          row[3 * x + 1] = v >> 8;
          row[3 * x + 2] = v;
        }

      jpeg_write_scanlines(&cinfo, &row, 1);
    }

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  fclose(fp);
  free(pix);
  free(row);
  printf("snap: seq=%" PRIu32 " %" PRIu32 "x%" PRIu32 " -> %s\n",
         hd.seq, ow, oh, path);
  return 0;
}
#endif

int main(int argc, FAR char *argv[])
{
  int timeout = AMPCTL_DEF_TMO_MS;
  const char *text = "hello from openvela";
  int ret;
  int i;

  if (argc < 2)
    {
      ampctl_usage();
      return EXIT_FAILURE;
    }

  /* exec 后面的全部参数拼回一条命令行，交给 Linux 的 sh -c。
   * 带管道、引号的写法要整体加引号：ampctl exec "ps | grep k7d"
   */

  if (strcmp(argv[1], "exec") == 0)
    {
      char cmd[478];
      size_t off = 0;

      cmd[0] = '\0';
      for (i = 2; i < argc; i++)
        {
          int n = snprintf(cmd + off, sizeof(cmd) - off, "%s%s",
                           i > 2 ? " " : "", argv[i]);
          if (n < 0 || (size_t)n >= sizeof(cmd) - off)
            {
              printf("命令太长\n");
              return EXIT_FAILURE;
            }

          off += n;
        }

      ret = ampctl_exec(timeout, cmd);
      return ret < 0 ? EXIT_FAILURE : ret;
    }

#ifdef CONFIG_RK3576_FRAME_SHM
  if (strcmp(argv[1], "cam") == 0 && argc >= 3)
    {
      return ampctl_cam(argv[2]) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

  if (strcmp(argv[1], "snap") == 0 && argc >= 5)
    {
      ret = ampctl_snap(atoi(argv[2]), atoi(argv[3]), argv[4]);
      if (ret == -EAGAIN)
        {
          ret = ampctl_snap(atoi(argv[2]), atoi(argv[3]), argv[4]);
        }

      return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
#endif

  for (i = 2; i < argc; i++)
    {
      if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
        {
          timeout = atoi(argv[++i]);
        }
      else
        {
          text = argv[i];
        }
    }

  if (strcmp(argv[1], "status") == 0)
    {
      return ampctl_status() < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }
  else if (strcmp(argv[1], "ping") == 0)
    {
      return ampctl_ping(timeout, text) < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }
  else if (strcmp(argv[1], "selftest") == 0)
    {
      return ampctl_selftest() < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }
  else if (strcmp(argv[1], "listen") == 0)
    {
      return ampctl_listen(timeout) < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }
#ifdef CONFIG_RK3576_FRAME_SHM
  else if (strcmp(argv[1], "frame") == 0)
    {
      return ampctl_frame() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
#endif

  ampctl_usage();
  return EXIT_FAILURE;
}
