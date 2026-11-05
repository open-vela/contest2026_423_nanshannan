/****************************************************************************
 * chip/rk3576/rk3576_mailbox.c
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
 * RK3576 Mailbox 门铃驱动 —— AMP 里两个 OS 之间唯一的硬件通知通道。
 *
 * ★ 这个外设有多简单
 *
 *   一个 group 就是四个寄存器的单向信箱：INTEN / STATUS / CMD / DATA。
 *   写方按 CMD、DATA 的顺序写两个 32 位字，硬件在写完 DATA 的那一拍给收方
 *   拉一根中断线；收方读走两个字，往 STATUS 写 1 清掉。没有 FIFO，没有
 *   深度，**第二条消息会覆盖第一条**。所以它只能当门铃用：真正的数据放
 *   共享内存（vring），门铃只说"你去看一眼"。上层 rptun 正是这么用的。
 *
 * ★ 方向与命名（最容易搞反的地方）
 *
 *   TRM 把两端叫 AP 和 BB，但 17.3 明说这只是叫法，谁站哪端由软件定。
 *   实际约束来自 Linux：drivers/mailbox/rockchip-mailbox.c 固定
 *   **发 A2B、收 B2A**。所以 Linux 是 AP，openvela 只能站 BB：
 *
 *     openvela 发 → 写 B2A_CMD/DATA → Linux 的 irq_mailbox_apN
 *     openvela 收 ← 读 A2B_CMD/DATA ← 中断 irq_mailbox_bbN
 *
 *   把这个记反的症状是"两边都在发，谁也收不到"，而寄存器读回来全是对的。
 *
 * ★ 为什么 initialize() 里不开中断
 *
 *   Linux 那边先起来的时候会立刻 kick 一次（rockchip_rpmsg_mbox.c 的
 *   first_notify）。如果我们在回调还没装好时就 enable，这一次 kick 会被
 *   当成"没人要的中断"吞掉，而它恰恰是握手的第一步，丢了就再也等不到。
 *   所以这里只 irq_attach + 清状态，enable 推迟到 register_callback()。
 *
 * 出处：TRM Part1 V1.2 第 17 章；
 *       Linux 6.1 drivers/mailbox/rockchip-mailbox.c。
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <syslog.h>
#include <unistd.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>

#include "arm64_internal.h"
#include "rk3576_cru.h"
#include "rk3576_mailbox.h"
#include "hardware/rk3576_mailbox.h"

#ifdef CONFIG_RK3576_MAILBOX

/****************************************************************************
 * Private Data
 ****************************************************************************/

static rk3576_mailbox_callback_t g_mbox_callback;
static void                    *g_mbox_arg;
static uintptr_t                g_mbox_rx_base;
static int                      g_mbox_rx_irq = -1;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uintptr_t mbox_base(unsigned int group)
{
  if (group >= RK3576_MAILBOX_COUNT)
    {
      return 0;
    }

  return RK3576_MAILBOX_BASE + group * RK3576_MAILBOX_STRIDE;
}

static inline uint32_t mbox_getreg(uintptr_t base, unsigned int off)
{
  return getreg32(base + off);
}

static inline void mbox_putreg(uint32_t val, uintptr_t base,
                               unsigned int off)
{
  putreg32(val, base + off);
}

/****************************************************************************
 * Name: rk3576_mailbox_interrupt
 *
 * Description:
 *   A2B 门铃（对端 → 我们）。读走 CMD/DATA，W1C 清状态，再转给上层。
 *   清状态放在回调之前：回调里可能会立刻回敬一次 kick，对端也可能马上再
 *   按一次门铃，先清掉才不会把那一次当成本次的重复而丢掉。
 *
 ****************************************************************************/

static int rk3576_mailbox_interrupt(int irq, void *context, void *arg)
{
  uint32_t status;
  uint32_t cmd;
  uint32_t data;

  UNUSED(irq);
  UNUSED(context);
  UNUSED(arg);

  status = mbox_getreg(g_mbox_rx_base, RK3576_MBOX_A2B_STATUS);
  if ((status & RK3576_MBOX_INT_MASK) == 0)
    {
      return OK;
    }

  cmd  = mbox_getreg(g_mbox_rx_base, RK3576_MBOX_A2B_CMD);
  data = mbox_getreg(g_mbox_rx_base, RK3576_MBOX_A2B_DATA);

  mbox_putreg(RK3576_MBOX_INT_MASK, g_mbox_rx_base, RK3576_MBOX_A2B_STATUS);

  if (g_mbox_callback != NULL)
    {
      g_mbox_callback(g_mbox_arg, cmd, data);
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int rk3576_mailbox_initialize(unsigned int rx_group)
{
  uintptr_t base = mbox_base(rx_group);
  bool pending;
  int ret;

  if (base == 0)
    {
      return -EINVAL;
    }

  g_mbox_rx_base = base;
  g_mbox_rx_irq  = RK3576_IRQ_MAILBOX_BB(rx_group);

  /* pclk_mailbox0 一个门控管全部 14 个 group。我们可能比 Linux 的时钟
   * 驱动先跑，不能指望对方已经开好。
   */

  rk3576_clk_gate(RK3576_CRU_MAILBOX_GATE_CON,
                  RK3576_CRU_MAILBOX_GATE_BIT, true);

  /* ★ 既不关中断，也不清状态 —— 两件事都会丢掉握手。
   *
   *   不清状态的理由（原来就有）：
   *
   *     STATUS 是 W1C 且电平有效。Linux 的 first_notify **只发一次**，
   *     清掉就再也等不到第二次；留着的话，register_callback() 一使能
   *     GIC 上的那条 SPI 就会立刻进一次 ISR，握手不丢。
   *     代价是万一真有上次启动的残留会多进一次 ISR —— rptun 的回调对
   *     重复 kick 是幂等的（只是把两个 vring 再扫一遍），无害。
   *
   *   不关使能的理由（这一条是后来在板上补的）：
   *
   *     这里原来会先写 INT_UPDATE 把 inten 关掉，注释里写着"关闭使能
   *     不清 STATUS"。**那个假设是错的。** 板上实测：Linux 已经把门铃
   *     写进来了（A2B_CMD=0x00000003 A2B_DATA=0x524d5347，就是 "RMSG"），
   *     而 A2B_STATUS 是 0，握手永远完不成。
   *
   *     已知的硬件事实是「inten 关着时状态位不锁存」（TRM 17.3，也是
   *     0004 补丁里用寄存器 dump 查出来的）。从现场看，关掉 inten 同时
   *     会让已经锁存的那一位消失 —— 也就是说这个开关不只是"以后不锁存"，
   *     它把当前值也一并带走了。
   *
   *     关一下再开在这块硬件上就是一个**丢门铃的窗口**，而我们恰恰只有
   *     一次门铃可丢。所以改成防御性地**确保它是开的**。
   *
   *     这样做不会有"attach 之前就来中断"的风险：GIC 上那条 SPI 要到
   *     register_callback() 里 up_enable_irq() 才使能，在那之前中断到
   *     不了 CPU。
   */

  mbox_putreg(RK3576_MBOX_INT_UPDATE | RK3576_MBOX_INT_MASK, base,
              RK3576_MBOX_A2B_INTEN);

  pending = (mbox_getreg(base, RK3576_MBOX_A2B_STATUS) &
             RK3576_MBOX_INT_MASK) != 0;

  ret = irq_attach(g_mbox_rx_irq, rk3576_mailbox_interrupt, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "mailbox: irq_attach(%d) 失败: %d\n",
             g_mbox_rx_irq, ret);
      return ret;
    }

  syslog(LOG_INFO, "mailbox: group%u @%08lx, 收 IRQ %d%s\n",
         rx_group, (unsigned long)base, g_mbox_rx_irq,
         pending ? "，已有对端门铃在等（引导器留下的）" : "");
  return OK;
}

int rk3576_mailbox_send(unsigned int group, uint32_t cmd, uint32_t data)
{
  uintptr_t base = mbox_base(group);

  if (base == 0)
    {
      return -EINVAL;
    }

  /* 触发模式选"写完 DATA 才触发"。只动 bit8，不碰 bit0 —— bit0 是对端的
   * 接收使能，它归对端管。
   */

  mbox_putreg(RK3576_MBOX_TRIGGER_UPDATE | RK3576_MBOX_TRIGGER_MASK,
              base, RK3576_MBOX_B2A_INTEN);

  /* 上一条还没被对端清掉。信箱只有一格，现在写进去就是覆盖。 */

  if ((mbox_getreg(base, RK3576_MBOX_B2A_STATUS) &
       RK3576_MBOX_INT_MASK) != 0)
    {
      return -EBUSY;
    }

  mbox_putreg(cmd, base, RK3576_MBOX_B2A_CMD);
  UP_DMB();
  mbox_putreg(data, base, RK3576_MBOX_B2A_DATA);
  return OK;
}

void rk3576_mailbox_register_callback(rk3576_mailbox_callback_t callback,
                                      void *arg)
{
  if (g_mbox_rx_irq < 0)
    {
      return;
    }

  g_mbox_callback = callback;
  g_mbox_arg      = arg;

  if (callback != NULL)
    {
      /* 回调先可见再开中断，否则开中断那一拍进来的 kick 会看到空回调。 */

      UP_DMB();
      mbox_putreg(RK3576_MBOX_INT_UPDATE | RK3576_MBOX_INT_MASK,
                  g_mbox_rx_base, RK3576_MBOX_A2B_INTEN);
      up_enable_irq(g_mbox_rx_irq);
    }
  else
    {
      up_disable_irq(g_mbox_rx_irq);
      mbox_putreg(RK3576_MBOX_INT_UPDATE, g_mbox_rx_base,
                  RK3576_MBOX_A2B_INTEN);
    }
}

/****************************************************************************
 * Name: rk3576_mailbox_selftest
 *
 * Description:
 *   自己给自己按门铃。
 *
 * ★ 为什么需要它
 *
 *   AMP 的接收路径要等对端 Linux 起来才有输入，而"一直收不到"能来自五
 *   个互不相干的地方：pclk 没开、IRQ 号填错、INTEN 的 bit0 没置、状态位
 *   没清所以只响一次、回调没派发。没有对端的时候，这五个全都表现为
 *   "ampctl status 里收到门铃 = 0"，一条信息区分不了五种原因。
 *
 *   但这条路其实**不需要对端**就能走通：A2B_CMD/DATA 只是两个普通的可写
 *   寄存器，硬件不关心是谁写的，写完照样拉 irq_mailbox_bbN。所以本端自
 *   己写一次，就能把上面五项全部验证掉。剩下唯一没验证的是"对端会不会
 *   写"——而那正是我们想留给真实联调的那个变量。
 *
 * ★ 为什么必须用另一个 group
 *
 *   在 rptun 用的那个 group 上自检，收到的假门铃会被 rptun 当成"对端
 *   DRIVER_OK 了"，于是它会拿着一片全是垃圾的 vring 往下走。自检本身
 *   成功了，却把系统推进了一个没有对端的非法状态 —— 这种"测试把被测
 *   对象弄坏"的坑不能踩。所以调用方要传一个空闲 group。
 *
 * Input Parameters:
 *   group   - 用哪个 group，必须不是 rptun 占用的那两个
 *   cmd     - 写进 A2B_CMD 的值
 *   data    - 写进 A2B_DATA 的值（写它才触发中断）
 *   rx_cmd  - 出参：中断里读回的 cmd
 *   rx_data - 出参：中断里读回的 data
 *
 * Returned Value:
 *   OK          门铃响了，且读回的两个字与写入一致
 *   -ETIMEDOUT  等了 100ms 没进中断
 *   -EIO        进了中断但读回的值对不上
 *   -EBUSY      group 正被 rptun 使用，或另一次自检还没结束
 *
 ****************************************************************************/

static volatile bool     g_st_fired;
static volatile uint32_t g_st_cmd;
static volatile uint32_t g_st_data;
static uintptr_t         g_st_base;

static int rk3576_mailbox_st_isr(int irq, void *context, void *arg)
{
  UNUSED(irq);
  UNUSED(context);
  UNUSED(arg);

  if ((mbox_getreg(g_st_base, RK3576_MBOX_A2B_STATUS) &
       RK3576_MBOX_INT_MASK) != 0)
    {
      g_st_cmd  = mbox_getreg(g_st_base, RK3576_MBOX_A2B_CMD);
      g_st_data = mbox_getreg(g_st_base, RK3576_MBOX_A2B_DATA);
      mbox_putreg(RK3576_MBOX_INT_MASK, g_st_base, RK3576_MBOX_A2B_STATUS);
      g_st_fired = true;
    }

  return OK;
}

static int mbox_selftest_run(unsigned int group, bool trigger,
                             uint32_t cmd, uint32_t data,
                             uint32_t *rx_cmd, uint32_t *rx_data)
{
  uintptr_t base = mbox_base(group);
  int irq;
  int ret;
  int i;

  if (base == 0)
    {
      return -EINVAL;
    }

  if (base == g_mbox_rx_base || g_st_base != 0)
    {
      return -EBUSY;
    }

  g_st_base  = base;
  g_st_fired = false;
  g_st_cmd   = 0;
  g_st_data  = 0;
  irq        = RK3576_IRQ_MAILBOX_BB(group);

  rk3576_clk_gate(RK3576_CRU_MAILBOX_GATE_CON,
                  RK3576_CRU_MAILBOX_GATE_BIT, true);

  ret = irq_attach(irq, rk3576_mailbox_st_isr, NULL);
  if (ret < 0)
    {
      g_st_base = 0;
      return ret;
    }

  /* 清残留 → 开中断 → 按 CMD、DATA 的顺序写（写 DATA 才触发）。 */

  mbox_putreg(RK3576_MBOX_INT_MASK, base, RK3576_MBOX_A2B_STATUS);
  mbox_putreg(RK3576_MBOX_INT_UPDATE | RK3576_MBOX_INT_MASK |
              RK3576_MBOX_TRIGGER_UPDATE | RK3576_MBOX_TRIGGER_MASK,
              base, RK3576_MBOX_A2B_INTEN);
  up_enable_irq(irq);

  /* 反例（trigger = false）只写 CMD 不写 DATA。硬件规定"写完 DATA 才
   * 触发"，所以这一路**必须**超时；它验证的是自检确实在测中断，而不是
   * 在测某个恰好为真的东西。
   */

  mbox_putreg(cmd, base, RK3576_MBOX_A2B_CMD);
  UP_DMB();
  if (trigger)
    {
      mbox_putreg(data, base, RK3576_MBOX_A2B_DATA);
    }

  /* 有界等待：100 × 1ms。中断是立刻到的，这里只是给调度留余量。 */

  for (i = 0; i < 100 && !g_st_fired; i++)
    {
      usleep(1000);
    }

  up_disable_irq(irq);
  mbox_putreg(RK3576_MBOX_INT_UPDATE, base, RK3576_MBOX_A2B_INTEN);
  irq_detach(irq);

  if (rx_cmd != NULL)
    {
      *rx_cmd = g_st_cmd;
    }

  if (rx_data != NULL)
    {
      *rx_data = g_st_data;
    }

  ret = !g_st_fired ? -ETIMEDOUT :
        !trigger          ? -EIO :   /* 没触发却响了 —— 仪器不可信 */
        (g_st_cmd != cmd || g_st_data != data) ? -EIO : OK;

  g_st_base = 0;
  return ret;
}

int rk3576_mailbox_selftest(unsigned int group, uint32_t cmd, uint32_t data,
                            uint32_t *rx_cmd, uint32_t *rx_data)
{
  return mbox_selftest_run(group, true, cmd, data, rx_cmd, rx_data);
}

int rk3576_mailbox_selftest_notrigger(unsigned int group,
                                      uint32_t *rx_cmd, uint32_t *rx_data)
{
  /* 只写 CMD，不写 DATA。期望返回 -ETIMEDOUT。 */

  return mbox_selftest_run(group, false, 0xdeadbeef, 0, rx_cmd, rx_data);
}

int rk3576_mailbox_dump(unsigned int group, uint32_t regs[8])
{
  uintptr_t base = mbox_base(group);
  int i;

  if (base == 0)
    {
      return -EINVAL;
    }

  for (i = 0; i < 8; i++)
    {
      regs[i] = mbox_getreg(base, i * 4);
    }

  return OK;
}

#endif /* CONFIG_RK3576_MAILBOX */
