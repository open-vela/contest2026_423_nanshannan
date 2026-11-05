/****************************************************************************
 * chip/rk3576/rk3576_timer.c
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

/* RK3576 独立 TIMER —— 给 /dev/oneshot 用的 oneshot 下半部。
 *
 * ★ 为什么非要用独立定时器，不能复用 ARM 通用定时器
 *
 *   板级最初是直接把 arm64_oneshot_initialize() 的返回值注册成
 *   /dev/oneshot 的。那个下半部**是调度器正在用的那一个**：
 *
 *     arm64_arch_timer.c
 *       static struct oneshot_lowerhalf_s g_arm64_oneshot_lowerhalf;
 *       void up_timer_initialize(void)
 *       { up_alarm_set_lowerhalf(arm64_oneshot_initialize()); }
 *
 *   而 struct oneshot_lowerhalf_s 只有**一对** callback/arg，谁后设谁赢。
 *   注册 /dev/oneshot 就把调度器的定时回调顶掉了，系统时基当场死亡：
 *   sleep 永不返回、延时 work_queue 永不触发、看门狗 automonitor 从不
 *   喂狗于是每 95 秒硬件复位一次 —— 而串口和 nsh 一切正常（靠 UART
 *   中断唤醒），up_mdelay() 也正常（忙等读计数器），所以这个故障可以
 *   长期潜伏。详见 notes/DEBUG-CASES.md 案例 16。
 *
 *   本文件给 /dev/oneshot 一套自己的硬件，从根上不再和调度器共享任何
 *   东西。
 *
 * ★ 两个通道，各司其职
 *
 *   TIMER_NS_0-CH0  用户定义 + 向下计数 + 开中断  -> 一次性闹钟
 *   TIMER_NS_0-CH1  自由运行 + 向上计数 + 不开中断 -> current() 的读数
 *
 *   CH1 不需要中断，也就不需要知道它的 GIC 中断号 —— 设备树只给了 CH0
 *   的（GIC_SPI 45），CH1..CH5 的中断号没有依据。用一个不需要中断的
 *   通道当计数源，就绕开了这个没有出处的假设。
 *
 *   出处（RK3576 TRM Part1 第 14 章）：
 *     通道基址间距 0x1000：TIMER_NS_0-CH0 = 0x2ACC0000、CH1 = 0x2ACC1000
 *     CONTROL(0x10) bit0 使能、bit1 模式、bit2 中断、bit3 计数方向
 *     「free-running + count-up 从 0 计到 LOAD_COUNT 后自动重载」
 *     「user-defined + count-down 从 LOAD_COUNT 减到 0 后停住，不重载」
 *
 *   ★ bit3 是 RK3576 才有的。Linux 里匹配的 rockchip,rk3288-timer 驱动
 *     完全没用这一位（老芯片只能向下数），照抄那个驱动就会漏掉向上计数，
 *     只好去读一个递减的计数器再取反。TRM 比驱动多给了这条。
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/timers/oneshot.h>

#include "arm64_internal.h"
#include "rk3576_cru.h"
#include "rk3576_timer.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 通道基址。TRM Part1 14.4.1 内部地址映射。 */

#define TIMER_CH_BASE(n)      (0x2acc0000ul + (uintptr_t)(n) * 0x1000)

#define TIMER_CH_ALARM        0        /* 闹钟：它的中断号设备树里有   */
#define TIMER_CH_COUNTER      1        /* 计数：自由运行，不用中断     */

/* 通道内寄存器偏移 */

#define TIMER_LOAD_COUNT0     0x00
#define TIMER_LOAD_COUNT1     0x04
#define TIMER_CURRENT_VALUE0  0x08
#define TIMER_CURRENT_VALUE1  0x0c
#define TIMER_CONTROL         0x10
#define TIMER_INTSTATUS       0x18

#define TIMER_CTRL_EN         (1u << 0)
#define TIMER_CTRL_USERDEF    (1u << 1)  /* 0 自由运行 1 用户定义      */
#define TIMER_CTRL_INT_EN     (1u << 2)
#define TIMER_CTRL_COUNT_DOWN (1u << 3)  /* 0 向上 1 向下              */

#define TIMER_INT_PD          (1u << 0)  /* 写 1 清                     */

/* 时钟。出处 clk-rk3576.c：
 *   GATE(PCLK_BUSTIMER0, ... CLKGATE_CON(17), 3)
 *   COMPOSITE_NODIV(CLK_TIMER0_ROOT, ... mux_100m_24m_p,
 *                   CLKSEL_CON(71), 14, 1, CLKGATE_CON(17), 5)
 *   GATE(CLK_TIMER0, ... CLKGATE_CON(17), 6)
 *   GATE(CLK_TIMER1, ... CLKGATE_CON(17), 7)
 *
 * mux_100m_24m_p = { "clk_cpll_div10", "xin24m" }，所以选 1 是 24MHz。
 * 选 24MHz 而不是 100MHz：24MHz 直接来自晶振，不依赖 CPLL 的分频链，
 * 少一层可能被别处改动的东西；这个用途也不需要那么高的分辨率。
 */

#define TIMER_GATE_CON        17
#define TIMER_GATE_PCLK       3
#define TIMER_GATE_ROOT       5
#define TIMER_GATE_CH0        6
#define TIMER_GATE_CH1        7

#define TIMER_MUX_CON         71
#define TIMER_MUX_SHIFT       14
#define TIMER_MUX_WIDTH       1
#define TIMER_MUX_24M         1

#define TIMER_FREQ_HZ         24000000u

/* 设备树：rktimer: timer@2acc0000 { interrupts = <GIC_SPI 45 ...>; }
 * 这是 CH0 的中断（该节点的 reg 只覆盖 CH0 的 0x20 字节）。
 *
 * ★ 各通道的中断号是连续的，但这一条设备树里没有 —— 它只声明了 CH0。
 *
 *   交叉验证来源：另一支队伍在同一块板上的移植
 *   github.com/open-vela/contest2026_062_PharosTech
 *   chips/rk3576/include/irq.h:
 *     RK3576_IRQ_TIMER_NS_0_CH0 = 77   （= SPI 45 + 32，与本文件一致）
 *     RK3576_IRQ_TIMER_NS_0_CH1 = 78
 *     RK3576_IRQ_TIMER_NS_1_CH0 = 83
 *
 *   本文件只用 CH0 的中断，CH1 当自由运行计数器、不接中断，所以并不
 *   依赖这张表；记在这里是为了将来要用第二个闹钟通道时不必重新去找。
 */

#define TIMER_ALARM_IRQ       RK3576_IRQ_SPI(45)

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct oneshot_lowerhalf_s g_rk3576_oneshot;

/* ISR 进来的次数。自检用：它和 INTSTATUS 两个观测量合起来才能把
 * "定时器没产生中断"和"中断号接错了"分开 —— 只看其中一个都分不出来。
 */

static volatile uint32_t g_rk3576_timer_hits;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline void timer_putreg(int ch, uint32_t off, uint32_t val)
{
  putreg32(val, TIMER_CH_BASE(ch) + off);
}

static inline uint32_t timer_getreg(int ch, uint32_t off)
{
  return getreg32(TIMER_CH_BASE(ch) + off);
}

/****************************************************************************
 * Name: timer_read64
 *
 * Description:
 *   读 64 位的当前计数值。
 *
 *   ★ 高低两半是分两次读的，中间计数器还在走。
 *
 *     低半溢出的那一瞬间读到的可能是"旧的高半 + 新的低半"，凑出来的值
 *     会往回跳一大截 —— 对一个被当作单调时钟用的读数来说，往回跳比读到
 *     稍旧的值危险得多，上层会算出一个巨大的负延时。
 *
 *     读高、读低、再读高，两次高半一致才采信，否则重读。
 *
 ****************************************************************************/

static uint64_t timer_read64(int ch)
{
  uint32_t hi1;
  uint32_t lo;
  uint32_t hi2;

  do
    {
      hi1 = timer_getreg(ch, TIMER_CURRENT_VALUE1);
      lo  = timer_getreg(ch, TIMER_CURRENT_VALUE0);
      hi2 = timer_getreg(ch, TIMER_CURRENT_VALUE1);
    }
  while (hi1 != hi2);

  return ((uint64_t)hi2 << 32) | lo;
}

static clkcnt_t rk3576_oneshot_current(struct oneshot_lowerhalf_s *lower)
{
  UNUSED(lower);
  return (clkcnt_t)timer_read64(TIMER_CH_COUNTER);
}

static void rk3576_oneshot_cancel(struct oneshot_lowerhalf_s *lower)
{
  UNUSED(lower);

  timer_putreg(TIMER_CH_ALARM, TIMER_CONTROL, 0);
  timer_putreg(TIMER_CH_ALARM, TIMER_INTSTATUS, TIMER_INT_PD);
}

static void rk3576_oneshot_start(struct oneshot_lowerhalf_s *lower,
                                 clkcnt_t delay)
{
  UNUSED(lower);

  /* 0 会让"数到 0"这个条件在装载的一瞬间就成立，硬件行为没有依据；
   * 给最小的 1 拍，让它走正常路径产生一次中断。
   */

  if (delay == 0)
    {
      delay = 1;
    }

  /* ★ 必须先关再配。
   *
   *   TRM 讲用户定义模式时明确写着「不会自动重载，需要先 disable 再按
   *   编程序列重新启动」。带着使能位改 LOAD_COUNT，硬件用的是新值还是
   *   旧值没有定义。
   *
   *   关掉之后，模式位与使能位可以在**同一次**写里给出，实测有效
   *   （1ms 闹钟稳定在 ~10x100us 触发，cmocka_driver_oneshot 通过）。
   *   我一度以为必须拆成"先配置、后使能"两次写，那个结论是错的：
   *   当时同一轮改了两处，又把串口丢字造成的 ISR 计数缺失误读成
   *   "没有产生中断"。拆写与否单独对比过，两种写法都能触发。
   */

  timer_putreg(TIMER_CH_ALARM, TIMER_CONTROL, 0);
  timer_putreg(TIMER_CH_ALARM, TIMER_INTSTATUS, TIMER_INT_PD);

  timer_putreg(TIMER_CH_ALARM, TIMER_LOAD_COUNT0,
               (uint32_t)(delay & 0xffffffffu));
  timer_putreg(TIMER_CH_ALARM, TIMER_LOAD_COUNT1,
               (uint32_t)(delay >> 32));

  timer_putreg(TIMER_CH_ALARM, TIMER_CONTROL,
               TIMER_CTRL_EN | TIMER_CTRL_USERDEF |
               TIMER_CTRL_INT_EN | TIMER_CTRL_COUNT_DOWN);
}

static void rk3576_oneshot_start_absolute(struct oneshot_lowerhalf_s *lower,
                                          clkcnt_t cnt)
{
  clkcnt_t now = rk3576_oneshot_current(lower);

  /* 目标时刻已经过去就立刻触发，而不是让 cnt - now 回绕成一个天文数字
   * 的延时 —— 那种情况下闹钟等于永远不响，现象是"偶尔有一次定时丢了"，
   * 极难复现。
   */

  rk3576_oneshot_start(lower, (clkcnt_t)(cnt - now) > 0 ? cnt - now : 1);
}

static clkcnt_t rk3576_oneshot_max_delay(struct oneshot_lowerhalf_s *lower)
{
  UNUSED(lower);
  return UINT64_MAX;
}

static int rk3576_oneshot_isr(int irq, void *context, void *arg)
{
  struct oneshot_lowerhalf_s *lower = arg;

  UNUSED(irq);
  UNUSED(context);

  /* 先关闹钟再清中断标志，最后才回调。
   *
   * 回调里上层通常会立刻设下一个闹钟；如果这时本通道还使能着、标志也
   * 还挂着，新设的那次会被当场"补发"一个中断，表现为定时提前。
   */

  timer_putreg(TIMER_CH_ALARM, TIMER_CONTROL, 0);
  timer_putreg(TIMER_CH_ALARM, TIMER_INTSTATUS, TIMER_INT_PD);

  g_rk3576_timer_hits++;

  /* 自检期间上层还没接管，callback 是空的，这时不能回调。 */

  if (lower->callback != NULL)
    {
      oneshot_process_callback(lower);
    }

  return OK;
}

static const struct oneshot_operations_s g_rk3576_oneshot_ops =
{
  .current        = rk3576_oneshot_current,
  .start          = rk3576_oneshot_start,
  .start_absolute = rk3576_oneshot_start_absolute,
  .cancel         = rk3576_oneshot_cancel,
  .max_delay      = rk3576_oneshot_max_delay,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

struct oneshot_lowerhalf_s *rk3576_oneshot_initialize(void)
{
  struct oneshot_lowerhalf_s *lower = &g_rk3576_oneshot;
  uint64_t a;
  uint64_t b;
  uint32_t hits0;
  uint32_t st;
  int i;

  /* 时钟：pclk、根时钟、两个通道各自的门控，以及把根时钟选到 24MHz */

  rk3576_clk_gate(TIMER_GATE_CON, TIMER_GATE_PCLK, true);
  rk3576_clk_gate(TIMER_GATE_CON, TIMER_GATE_ROOT, true);
  rk3576_clk_gate(TIMER_GATE_CON, TIMER_GATE_CH0,  true);
  rk3576_clk_gate(TIMER_GATE_CON, TIMER_GATE_CH1,  true);

  rk3576_clk_setmux(TIMER_MUX_CON, TIMER_MUX_SHIFT, TIMER_MUX_WIDTH,
                    TIMER_MUX_24M);

  /* 计数通道：自由运行 + 向上计数，装满 1 让它一路数到 2^64-1 才回绕。
   * 向上计数时 LOAD_COUNT 是"数到哪里重来"，不是起点（起点固定为 0）。
   */

  timer_putreg(TIMER_CH_COUNTER, TIMER_CONTROL, 0);
  timer_putreg(TIMER_CH_COUNTER, TIMER_LOAD_COUNT0, 0xffffffffu);
  timer_putreg(TIMER_CH_COUNTER, TIMER_LOAD_COUNT1, 0xffffffffu);
  timer_putreg(TIMER_CH_COUNTER, TIMER_CONTROL, TIMER_CTRL_EN);

  /* 闹钟通道：先关干净，等上层来设 */

  timer_putreg(TIMER_CH_ALARM, TIMER_CONTROL, 0);
  timer_putreg(TIMER_CH_ALARM, TIMER_INTSTATUS, TIMER_INT_PD);

  /* ★ 自检：计数器到底有没有在走。
   *
   *   时钟门控、时钟源、基地址三样里任何一样错了，读回来都是一个纹丝
   *   不动的常数 —— 而寄存器写入全部"成功"，不报任何错。上层拿到一个
   *   停住的时钟，现象是所有定时都不到期，与"根本没注册"难以区分。
   *   在这里花两次读的代价把它变成一条明确的错误。
   */

  a = timer_read64(TIMER_CH_COUNTER);
  up_udelay(100);
  b = timer_read64(TIMER_CH_COUNTER);

  if (b == a)
    {
      syslog(LOG_ERR,
             "ERROR: TIMER 计数器不走（读两次都是 %" PRIu64 "）—— "
             "查 CLKGATE_CON(%d) 与基地址 0x%08lx\n",
             a, TIMER_GATE_CON, (unsigned long)TIMER_CH_BASE(1));
      return NULL;
    }

  lower->ops = &g_rk3576_oneshot_ops;
  oneshot_count_init(lower, TIMER_FREQ_HZ);

  irq_attach(TIMER_ALARM_IRQ, rk3576_oneshot_isr, lower);
  up_enable_irq(TIMER_ALARM_IRQ);

  /* ★ 闹钟自检：设一个 1ms 的闹钟，同时看两个观测量。
   *
   *   中断走不通有两种原因，改法完全不同：
   *     INTSTATUS 置位、ISR 没进  -> 中断号接错了（GIC 那一侧）
   *     INTSTATUS 也没置位        -> 定时器本身没产生中断（配置那一侧）
   *
   *   只看"上层收不到信号"这一个现象是分不开的。这里花 10ms 把它分开，
   *   而且是在启动日志里就分开 —— 不用等到跑 xTS 用例才发现。
   */

  hits0 = g_rk3576_timer_hits;
  st = 0;

  uint64_t c0;

  rk3576_oneshot_start(lower, TIMER_FREQ_HZ / 1000);   /* 1ms */
  c0 = timer_read64(TIMER_CH_ALARM);

  for (i = 0; i < 100; i++)
    {
      st = timer_getreg(TIMER_CH_ALARM, TIMER_INTSTATUS);
      if (st != 0 || g_rk3576_timer_hits != hits0)
        {
          break;
        }

      up_udelay(100);
    }

  syslog(LOG_INFO,
         "TIMER: 闹钟自检 INTSTATUS=0x%08" PRIx32 " ISR %" PRIu32
         " 次 等了 %dx100us CTRL回读=0x%08" PRIx32
         " CH0计数 %" PRIu64 "->%" PRIu64 "\n",
         st, g_rk3576_timer_hits - hits0, i,
         timer_getreg(TIMER_CH_ALARM, TIMER_CONTROL),
         c0, timer_read64(TIMER_CH_ALARM));

  rk3576_oneshot_cancel(lower);

  syslog(LOG_INFO,
         "TIMER: /dev/oneshot 用 CH0(闹钟,IRQ %d)+CH1(计数) @%uHz，"
         "100us 内计数 %" PRIu64 " 拍\n",
         TIMER_ALARM_IRQ, TIMER_FREQ_HZ, b - a);

  return lower;
}
