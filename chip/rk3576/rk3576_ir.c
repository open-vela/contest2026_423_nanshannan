/****************************************************************************
 * arch/arm64/src/rk3576/rk3576_ir.c
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

/* 红外接收（NEC），注册为 LIRC 扫描码设备 /dev/lircN。
 *
 * ★ 硬件：板上红外接收头的输出接 PWM0_CH0_M0（GPIO0_C4，功能 12），
 *   出处是 K7_V2.1 原理图 "IR Receiver" 一页与厂商 rk3576-kickpi-ir.dtsi。
 *   PWM0 是 v4 版 PWM（VERSION_ID 高字节 0x04），寄存器布局与
 *   RK3399/RK3568 的老 PWM 完全不同，照老驱动写不报错、只是收不到。
 *
 * ★ 解码方式照厂商 drivers/input/remotectl/rockchip_pwm_remotectl.c：
 *   PWM 工作在捕获模式，电平每翻转一次来一个中断，中断里读出上一段
 *   电平的持续时间，软件按 NEC 时序判位。
 *
 *     LPC 中断 -> 读 HPC = 刚结束的高电平（接收头空闲为高，即"间隔"）
 *     HPC 中断 -> 读 LPC = 刚结束的低电平（载波段，即"脉冲"）
 *
 *   NEC 的信息全在间隔长度里（引导 4.5ms、重复 2.25ms、位 1 约
 *   1.69ms、位 0 约 0.56ms），所以和厂商一样只用间隔判位，脉冲只
 *   记进原始事件环里供 `ir raw` 诊断。
 *
 * ★ 计数时钟选 clk_pwm_osc（24MHz 晶振）而不是 clk_pwm（PLL 派生、
 *   默认 100MHz）：后者的分频由 PMU1CRU 的 mux 决定，那个 mux 在 AMP
 *   下 Linux 也可能改。晶振固定，24MHz / (2 * 12) = 1MHz，计数值
 *   直接就是微秒。
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>
#include <nuttx/rc/lirc_dev.h>

#include <arch/irq.h>

#include "arm64_internal.h"
#include "rk3576_cru.h"
#include "rk3576_ir.h"
#include "rk3576_pinmux.h"
#include "hardware/rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define IR_BASE                 RK3576_PWM0_ADDR    /* PWM0 通道 0 */
#define IR_IRQ                  RK3576_IRQ_SPI(100)

/* PWM v4 寄存器（TRM Part 1 第 34 章） */

#define PWM_VERSION_ID          0x0000
#define PWM_ENABLE              0x0004
#define PWM_CLK_CTRL            0x0008
#define PWM_CTRL                0x000c
#define PWM_HPC                 0x002c
#define PWM_LPC                 0x0030
#define PWM_INTSTS              0x0070
#define PWM_INT_EN              0x0074

/* 高 16 位是写使能掩码，与 CRU/GRF 同一套约定 */

#define PWM_HIWORD(val, mask)   (((uint32_t)(mask) << 16) | (val))

#define PWM_ENABLE_CLK_EN       (1u << 0)
#define PWM_ENABLE_EN           (1u << 1)
#define PWM_ENABLE_CTRL_UPDATE  (1u << 2)           /* W1T，自清 */

#define PWM_CLK_SRC_OSC         (1u << 13)          /* clk_src_sel = 01 */
#define PWM_CLK_SCALE(n)        ((uint32_t)(n) << 4) /* 分频 2*n */
#define PWM_CLK_CTRL_MASK       0xfff7              /* bit3 保留 */

#define PWM_CTRL_MODE_CAPTURE   0x2
#define PWM_CTRL_MASK           0x01ff              /* in_sel..mode */

#define PWM_INT_CAP_LPC         (1u << 0)
#define PWM_INT_CAP_HPC         (1u << 1)
#define PWM_INT_ALL             0x03ff

#define PWM_VERSION_MAJOR(v)    (((v) >> 24) & 0xff)

/* 时钟（clk-rk3576.c，均在 PMU1CRU 的 CLKGATE_CON4）：
 *   GATE(PCLK_PMU1PWM,    ..., RK3576_PMU_CLKGATE_CON(4), 11, ...)
 *   COMPOSITE_NODIV(CLK_PMU1PWM, ..., RK3576_PMU_CLKGATE_CON(4), 12, ...)
 *   GATE(CLK_PMU1PWM_OSC, ..., RK3576_PMU_CLKGATE_CON(4), 13, ...)
 */

#define IR_GATE_CON             4
#define IR_GATE_PCLK            11
#define IR_GATE_CLK             12
#define IR_GATE_OSC             13

/* 引脚：pwm0m0_ch0 = <0 RK_PC4 12 &pcfg_pull_none_drv_level_2> */

#define IR_PIN_BANK             0
#define IR_PIN_NUM              20                  /* PC4 = 2 * 8 + 4 */
#define IR_PIN_FUNC             12

/* 24MHz / (2 * 12) = 1MHz，一个计数 = 1us */

#define IR_CLK_SCALE            12

/* NEC 间隔时长窗口（us），与厂商驱动 rockchip_pwm_remotectl.h 一致 */

#define NEC_LEADER_MIN          4000
#define NEC_LEADER_MAX          5000
#define NEC_REPEAT_MIN          2000
#define NEC_REPEAT_MAX          2500
#define NEC_BIT0_MIN            390
#define NEC_BIT0_MAX            730
#define NEC_BIT1_MIN            1300
#define NEC_BIT1_MAX            2000

/* 重复码只在上一帧之后这么久之内有效。NEC 帧周期 108ms，
 * 给一点余量；超时的重复码说明丢了首帧，不能凭空补一个按键。
 */

#define NEC_REPEAT_WINDOW_NS    (150ull * 1000 * 1000)

#define IR_RAW_DEPTH            64      /* 原始事件环，2 的幂 */
#define IR_SCANCODE_DEPTH       16      /* 上半部每个句柄的缓冲帧数 */

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum nec_state_e
{
  NEC_IDLE = 0,
  NEC_DATA,
};

struct rk3576_ir_s
{
  struct lirc_lowerhalf_s lower;  /* 必须在首位 */
  spinlock_t              lock;
  bool                    registered;

  /* NEC 解码状态（只在中断里改） */

  enum nec_state_e        state;
  uint32_t                bits;
  int                     nbits;
  uint64_t                last_frame_ns;
  struct lirc_scancode    last;   /* 最近一次有效帧，供重复码复用 */
  bool                    have_last;

  /* 诊断 */

  struct rk3576_ir_stats_s stats;
  uint32_t                raw[IR_RAW_DEPTH];  /* bit31 = 脉冲，低 31 位 us */
  unsigned int            raw_head;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int  rk3576_ir_open(FAR struct lirc_lowerhalf_s *lower);
static void rk3576_ir_close(FAR struct lirc_lowerhalf_s *lower);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct lirc_ops_s g_ir_ops =
{
  .driver_type = LIRC_DRIVER_SCANCODE,
  .open        = rk3576_ir_open,
  .close       = rk3576_ir_close,
};

static struct rk3576_ir_s g_ir;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t ir_getreg(uint32_t off)
{
  return getreg32(IR_BASE + off);
}

static inline void ir_putreg(uint32_t off, uint32_t val)
{
  putreg32(val, IR_BASE + off);
}

static int rk3576_ir_open(FAR struct lirc_lowerhalf_s *lower)
{
  /* 中断在初始化时就开着（诊断计数不依赖有没有人打开设备），
   * 这里无事可做。
   */

  return OK;
}

static void rk3576_ir_close(FAR struct lirc_lowerhalf_s *lower)
{
}

/****************************************************************************
 * Name: ir_raw_push
 *
 * Description:
 *   记一条原始电平事件，环满了覆盖最旧的。
 *
 ****************************************************************************/

static void ir_raw_push(FAR struct rk3576_ir_s *priv, bool pulse,
                        uint32_t us)
{
  if (us > 0x7fffffff)
    {
      us = 0x7fffffff;
    }

  priv->raw[priv->raw_head & (IR_RAW_DEPTH - 1)] =
    (pulse ? 0x80000000u : 0) | us;
  priv->raw_head++;
}

/****************************************************************************
 * Name: nec_emit
 *
 * Description:
 *   32 位收齐后转换成 LIRC 扫描码。编码约定与 Linux rc-core 的
 *   ir_nec_bytes_to_scancode() 一致，便于直接对照现成的键值表：
 *
 *     地址与地址反码互补 -> NEC，   scancode = addr << 8 | cmd
 *     只有命令互补       -> NECX，  scancode = addr << 16 | ~addr << 8 | cmd
 *     命令也不互补       -> NEC32， scancode = 原始 32 位
 *
 *   厂商键值表里的 usercode（如 0x4040）是"反码字节 << 8 | 地址字节"，
 *   对应这里 NECX 扫描码的高 16 位。
 *
 ****************************************************************************/

static void nec_emit(FAR struct rk3576_ir_s *priv, uint64_t now)
{
  struct lirc_scancode sc;
  uint8_t addr     = priv->bits & 0xff;
  uint8_t addr_inv = (priv->bits >> 8) & 0xff;
  uint8_t cmd      = (priv->bits >> 16) & 0xff;
  uint8_t cmd_inv  = (priv->bits >> 24) & 0xff;

  memset(&sc, 0, sizeof(sc));

  if ((uint8_t)(cmd ^ cmd_inv) != 0xff)
    {
      sc.rc_proto = RC_PROTO_NEC32;
      sc.scancode = priv->bits;
    }
  else if ((uint8_t)(addr ^ addr_inv) == 0xff)
    {
      sc.rc_proto = RC_PROTO_NEC;
      sc.scancode = ((uint32_t)addr << 8) | cmd;
    }
  else
    {
      sc.rc_proto = RC_PROTO_NECX;
      sc.scancode = ((uint32_t)addr << 16) | ((uint32_t)addr_inv << 8) |
                    cmd;
    }

  priv->last          = sc;
  priv->have_last     = true;
  priv->last_frame_ns = now;
  priv->stats.frames++;
  priv->stats.last_scancode = sc.scancode;
  priv->stats.last_proto    = sc.rc_proto;

  if (priv->registered)
    {
      lirc_scancode_event(&priv->lower, &sc);
    }
}

/****************************************************************************
 * Name: nec_space
 *
 * Description:
 *   NEC 状态机，输入一段间隔（高电平）的时长。
 *
 ****************************************************************************/

static void nec_space(FAR struct rk3576_ir_s *priv, uint32_t us)
{
  uint64_t now;

  if (priv->state == NEC_DATA)
    {
      if (us >= NEC_BIT1_MIN && us <= NEC_BIT1_MAX)
        {
          priv->bits |= 1u << priv->nbits;
        }
      else if (us < NEC_BIT0_MIN || us > NEC_BIT0_MAX)
        {
          /* 帧中途出现非法间隔：丢弃，并把这一段当成可能的新引导
           * 继续往下判，免得紧跟着的下一帧也被吃掉。
           */

          priv->stats.errors++;
          priv->state = NEC_IDLE;
          goto idle;
        }

      if (++priv->nbits == 32)
        {
          priv->state = NEC_IDLE;
          nec_emit(priv, lirc_get_timestamp());
        }

      return;
    }

idle:
  if (us >= NEC_LEADER_MIN && us <= NEC_LEADER_MAX)
    {
      priv->state = NEC_DATA;
      priv->bits  = 0;
      priv->nbits = 0;
    }
  else if (us >= NEC_REPEAT_MIN && us <= NEC_REPEAT_MAX)
    {
      now = lirc_get_timestamp();
      if (priv->have_last &&
          now - priv->last_frame_ns < NEC_REPEAT_WINDOW_NS)
        {
          struct lirc_scancode sc = priv->last;

          sc.flags = LIRC_SCANCODE_FLAG_REPEAT;
          priv->last_frame_ns = now;    /* 重复码每 108ms 一个，顺延 */
          priv->stats.repeats++;

          if (priv->registered)
            {
              lirc_scancode_event(&priv->lower, &sc);
            }
        }
    }
}

static int rk3576_ir_interrupt(int irq, FAR void *context, FAR void *arg)
{
  FAR struct rk3576_ir_s *priv = arg;
  irqstate_t flags;
  uint32_t status;
  uint32_t us;

  status = ir_getreg(PWM_INTSTS);
  ir_putreg(PWM_INTSTS, status & PWM_INT_ALL);   /* 写 1 清 */

  flags = spin_lock_irqsave(&priv->lock);
  priv->stats.irqs++;

  if (status & PWM_INT_CAP_HPC)
    {
      us = ir_getreg(PWM_LPC);
      ir_raw_push(priv, true, us);
    }

  if (status & PWM_INT_CAP_LPC)
    {
      us = ir_getreg(PWM_HPC);
      ir_raw_push(priv, false, us);
      nec_space(priv, us);
    }

  spin_unlock_irqrestore(&priv->lock, flags);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int rk3576_ir_initialize(int devno)
{
  FAR struct rk3576_ir_s *priv = &g_ir;
  uint32_t version;
  int ret;

  if (priv->registered)
    {
      return -EEXIST;
    }

  /* 1) 先开 APB 时钟再碰寄存器 —— 门控关着时访问会挂总线 */

  rk3576_pmu_clk_gate(IR_GATE_CON, IR_GATE_PCLK, true);
  rk3576_pmu_clk_gate(IR_GATE_CON, IR_GATE_CLK, true);
  rk3576_pmu_clk_gate(IR_GATE_CON, IR_GATE_OSC, true);

  version = ir_getreg(PWM_VERSION_ID);
  if (PWM_VERSION_MAJOR(version) != 0x04)
    {
      syslog(LOG_ERR, "IR: PWM0 版本 0x%08" PRIx32 " 不是 v4\n", version);
      return -ENODEV;
    }

  /* 2) 引脚复用并读回核对（写错寄存器时写入不报错） */

  rk3576_pinmux_set(IR_PIN_BANK, IR_PIN_NUM, IR_PIN_FUNC);
  rk3576_pinmux_setpull(IR_PIN_BANK, IR_PIN_NUM, RK3576_PULL_NONE);
  ret = rk3576_pinmux_get(IR_PIN_BANK, IR_PIN_NUM);
  if (ret != IR_PIN_FUNC)
    {
      syslog(LOG_ERR, "IR: GPIO0_C4 复用读回 %d，期望 %d\n",
             ret, IR_PIN_FUNC);
      return -EIO;
    }

  /* 3) 停下通道，配成捕获模式，计数时钟 1MHz */

  ir_putreg(PWM_ENABLE,
            PWM_HIWORD(0, PWM_ENABLE_CLK_EN | PWM_ENABLE_EN));
  ir_putreg(PWM_INT_EN, PWM_HIWORD(0, PWM_INT_ALL));
  ir_putreg(PWM_CLK_CTRL,
            PWM_HIWORD(PWM_CLK_SRC_OSC | PWM_CLK_SCALE(IR_CLK_SCALE),
                       PWM_CLK_CTRL_MASK));
  ir_putreg(PWM_CTRL, PWM_HIWORD(PWM_CTRL_MODE_CAPTURE, PWM_CTRL_MASK));
  ir_putreg(PWM_INTSTS, PWM_INT_ALL);

  spin_lock_init(&priv->lock);
  priv->state     = NEC_IDLE;
  priv->have_last = false;
  memset(&priv->stats, 0, sizeof(priv->stats));
  priv->raw_head  = 0;

  /* 4) 先挂中断再开通道，不会漏掉第一个沿 */

  ret = irq_attach(IR_IRQ, rk3576_ir_interrupt, priv);
  if (ret < 0)
    {
      return ret;
    }

  ir_putreg(PWM_INT_EN,
            PWM_HIWORD(PWM_INT_CAP_LPC | PWM_INT_CAP_HPC, PWM_INT_ALL));
  up_enable_irq(IR_IRQ);

  ir_putreg(PWM_ENABLE,
            PWM_HIWORD(PWM_ENABLE_CLK_EN | PWM_ENABLE_EN |
                       PWM_ENABLE_CTRL_UPDATE,
                       PWM_ENABLE_CLK_EN | PWM_ENABLE_EN |
                       PWM_ENABLE_CTRL_UPDATE));

  /* 5) 注册 LIRC 上半部 */

  priv->lower.ops           = &g_ir_ops;
  priv->lower.buffer_bytes  = IR_SCANCODE_DEPTH *
                              sizeof(struct lirc_scancode);
  priv->lower.rx_resolution = 1;

  priv->registered = true;    /* 中断里据此决定是否上报 */
  ret = lirc_register(&priv->lower, devno);
  if (ret < 0)
    {
      priv->registered = false;
      up_disable_irq(IR_IRQ);
      ir_putreg(PWM_INT_EN, PWM_HIWORD(0, PWM_INT_ALL));
      ir_putreg(PWM_ENABLE,
                PWM_HIWORD(0, PWM_ENABLE_CLK_EN | PWM_ENABLE_EN));
      irq_detach(IR_IRQ);
      syslog(LOG_ERR, "IR: lirc_register 失败 %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO, "IR: /dev/lirc%d 就绪（PWM0 v%08" PRIx32 "）\n",
         devno, version);
  return OK;
}

bool rk3576_ir_running(void)
{
  return g_ir.registered;
}

int rk3576_ir_getstats(FAR struct rk3576_ir_stats_s *stats,
                       FAR uint32_t *raw, unsigned int nraw)
{
  FAR struct rk3576_ir_s *priv = &g_ir;
  irqstate_t flags;
  unsigned int avail;
  unsigned int i;

  if (!priv->registered)
    {
      return -ENODEV;
    }

  flags = spin_lock_irqsave(&priv->lock);

  *stats = priv->stats;
  stats->intsts = ir_getreg(PWM_INTSTS);
  stats->enable = ir_getreg(PWM_ENABLE);
  stats->ctrl   = ir_getreg(PWM_CTRL);
  stats->clkctl = ir_getreg(PWM_CLK_CTRL);
  stats->level_hpc = ir_getreg(PWM_HPC);
  stats->level_lpc = ir_getreg(PWM_LPC);

  avail = priv->raw_head < IR_RAW_DEPTH ? priv->raw_head : IR_RAW_DEPTH;
  if (nraw > avail)
    {
      nraw = avail;
    }

  /* 按时间顺序给出最近 nraw 条 */

  for (i = 0; i < nraw; i++)
    {
      raw[i] = priv->raw[(priv->raw_head - nraw + i) & (IR_RAW_DEPTH - 1)];
    }

  spin_unlock_irqrestore(&priv->lock, flags);
  return (int)nraw;
}
