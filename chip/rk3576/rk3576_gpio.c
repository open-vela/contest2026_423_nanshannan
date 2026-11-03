/****************************************************************************
 * arch/arm64/src/rk3576/rk3576_gpio.c
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

/* RK3576 GPIO —— Rockchip "GPIO v2" 控制器。
 *
 * ★ 本文件不配置引脚复用（pinmux）。
 *
 *   RK3576 的复用寄存器分散在多个 IOC 块里（GPIO0 属于 PMU 域，用的是
 *   另一套 IOC），一次做全需要完整的引脚表，工作量与收益不成比例。
 *   M2 阶段依赖启动链路上游（U-Boot）已经配好的复用状态：板子 dts 里
 *   声明为 gpio-leds / gpio-fan 的引脚，U-Boot 阶段就已复用成 GPIO。
 *
 *   代价是只能操作 U-Boot 已经配成 GPIO 的那些引脚。要动别的引脚时
 *   再补 pinmux，届时应放到独立的 rk3576_pinmux.c 里。
 *
 *   自检手段：rk3576_gpio_verid() 读 VER_ID。这一个读操作同时验证了
 *   基址、PCLK 时钟、MMU 映射三件事，比逐项猜测可靠。
 *
 * ★ v2 的写使能掩码使得所有写操作天然原子：高 16 位 mask、低 16 位数据，
 *   硬件只更新 mask 中置 1 的位。因此这里不需要读改写，也就不需要加锁。
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <debug.h>
#include <errno.h>
#include <inttypes.h>
#include <syslog.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/irq.h>
#include <nuttx/arch.h>

#include "arm64_internal.h"
#include "rk3576_cru.h"
#include "rk3576_gpio.h"
#include "hardware/rk3576_gpio.h"
#include "hardware/rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 单个引脚允许的累计中断次数上限，超过即判定为失控并屏蔽。
 * 取值远高于任何真实用途：10kHz 方波也要 10 秒才触到。
 */

#define RK3576_GPIO_IRQ_STORM_LIMIT  100000

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* GPIO0 与 GPIO1-4 不在同一地址段，必须查表，不能用基址加步长算。 */

static const uintptr_t g_gpio_base[RK3576_GPIO_NBANKS] =
{
  RK3576_GPIO0_ADDR,      /* 0x27320000 */
  RK3576_GPIO1_ADDR,      /* 0x2ae10000 */
  RK3576_GPIO2_ADDR,      /* 0x2ae20000 */
  RK3576_GPIO3_ADDR,      /* 0x2ae30000 */
  RK3576_GPIO4_ADDR       /* 0x2ae40000 */
};

/* 前置声明：bank 级 ISR 在 rk3576_gpio_wr16 之前用到它 */

static void rk3576_gpio_wr16(uintptr_t base, uint32_t offset_l,
                             int pin, bool value);

static const int g_gpio_irq[RK3576_GPIO_NBANKS] =
{
  RK3576_GPIO0_IRQ, RK3576_GPIO1_IRQ, RK3576_GPIO2_IRQ,
  RK3576_GPIO3_IRQ, RK3576_GPIO4_IRQ
};

/* 每引脚的中断回调。
 *
 * ★ 五个 bank 每个只有一个 GIC 中断号，bank 内 32 个引脚共享它。
 *   因此这里维护一张表，由 bank 级 ISR 读 INT_STATUS 后分发到具体引脚。
 *   上层拿到的是"某个引脚的中断"，不必关心共享这件事。
 */

struct rk3576_gpio_isr_s
{
  xcpt_t    isr;
  void     *arg;
  uint32_t  count;   /* 失控保护用的计数，见 rk3576_gpio_interrupt */
};

static struct rk3576_gpio_isr_s
  g_gpio_isr[RK3576_GPIO_NBANKS][RK3576_GPIO_NPINS];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_gpio_interrupt
 *
 * Description:
 *   bank 级中断入口。读 INT_STATUS 找出是哪些引脚，逐个分发，
 *   然后写 EOI 清除 —— 清除必须在调用处理函数之后，否则电平触发的
 *   中断会在处理函数尚未消除中断源时被重复触发。
 *
 ****************************************************************************/

/* ★ bank 级 ISR 的进入次数与最近一次的 INT_STATUS。
 *
 *   排查"中断到底有没有到 CPU"时，这是唯一不依赖串口时序的证据：
 *   在中断上下文里打日志会改变时序、也会被 1.5M 无流控的串口丢掉，
 *   而计数器可以事后从容读。
 */

static volatile uint32_t g_gpio_isr_count[RK3576_GPIO_NBANKS];
static volatile uint32_t g_gpio_isr_last[RK3576_GPIO_NBANKS];

/* 读某个 bank 的中断原始状态（未经屏蔽）。诊断用。 */

uint32_t rk3576_gpio_rawstatus(int bank)
{
  if (bank < 0 || bank >= RK3576_GPIO_NBANKS)
    {
      return 0;
    }

  return getreg32(g_gpio_base[bank] + RK3576_GPIO_INT_RAWSTATUS);
}

/* 引脚的中断此刻是否被屏蔽（INT_MASK 位）。半寄存器读回来低 16 位是值。 */

bool rk3576_gpio_irq_masked(int bank, int pin)
{
  if (bank < 0 || bank >= RK3576_GPIO_NBANKS ||
      pin  < 0 || pin  >= RK3576_GPIO_NPINS)
    {
      return false;
    }

  return (getreg32(g_gpio_base[bank] + RK3576_GPIO_INT_MASK_L +
                   (pin >= 16 ? 4 : 0)) >> (pin & 15)) & 1;
}

uint32_t rk3576_gpio_irq_count(int bank, uint32_t *last_status)
{
  if (bank < 0 || bank >= RK3576_GPIO_NBANKS)
    {
      return 0;
    }

  if (last_status != NULL)
    {
      *last_status = g_gpio_isr_last[bank];
    }

  return g_gpio_isr_count[bank];
}

static int rk3576_gpio_interrupt(int irq, void *context, void *arg)
{
  int bank = (int)(intptr_t)arg;
  uintptr_t base;
  uint32_t status;
  int pin;

  UNUSED(irq);

  base   = g_gpio_base[bank];
  status = getreg32(base + RK3576_GPIO_INT_STATUS);

  g_gpio_isr_count[bank]++;
  g_gpio_isr_last[bank] = status;

  /* ★ 状态为 0 的伪中断也必须清一次，否则直接返回会让中断线一直有效，
   * 立刻重入 —— 形成中断风暴，现象是整块板子失去响应而不是报错。
   */

  if (status == 0)
    {
      for (pin = 0; pin < RK3576_GPIO_NPINS; pin++)
        {
          rk3576_gpio_wr16(base, RK3576_GPIO_PORT_EOI_L, pin, true);
        }

      return OK;
    }

  for (pin = 0; pin < RK3576_GPIO_NPINS; pin++)
    {
      if ((status & (1u << pin)) == 0)
        {
          continue;
        }

      if (g_gpio_isr[bank][pin].isr != NULL)
        {
          g_gpio_isr[bank][pin].isr(irq, context,
                                    g_gpio_isr[bank][pin].arg);
        }

      /* 写 1 清。EOI 也是 _L/_H 两个半寄存器，用同一套写使能掩码。 */

      rk3576_gpio_wr16(base, RK3576_GPIO_PORT_EOI_L, pin, true);

      /* ★ 失控保护
       *
       *   有两种情况会让同一个引脚无限重入，而且都不会报错，只会让整块
       *   板子停止响应 —— 连一行日志都打不出来，事后无从判断是哪个引脚：
       *
       *     - 电平触发：EOI 清不掉电平型中断，只要电平还在，ISR 一返回
       *       就立刻再次进入。这是 DW GPIO 的固有行为，不是配置错误。
       *     - 悬空引脚：排针上没接东西的引脚会拾取噪声，产生真实但无意义
       *       的边沿，速率可以远超轮询能处理的范围。
       *
       *   与其让板子死掉，不如在计数越过阈值时把这一个引脚屏蔽掉并留下
       *   记录。屏蔽是针对单个引脚的，同 bank 的其它引脚不受影响；系统
       *   继续运行，下次排查时能直接从日志读到是哪个 bank 哪个 pin。
       *
       *   阈值取得比任何真实用途都高：即便 10kHz 的方波也要 10 秒才会
       *   触到，而风暴在毫秒内就会撞上。
       */

      if (++g_gpio_isr[bank][pin].count > RK3576_GPIO_IRQ_STORM_LIMIT)
        {
          rk3576_gpio_wr16(base, RK3576_GPIO_INT_EN_L, pin, false);
          rk3576_gpio_wr16(base, RK3576_GPIO_INT_MASK_L, pin, true);

          /* ★ 用 syslog 而不是 gpioerr。
           *
           *   gpioerr 在未开 GPIO 调试时是空宏 —— 真发生风暴时引脚被
           *   静默屏蔽，一行记录都没有，事后只能看到"某个中断突然不
           *   工作了"，根本联想不到这里。保护措施如果不留证据，等于
           *   把一个响亮的故障换成一个安静的故障，更难查。
           */

          syslog(LOG_ERR,
                 "GPIO%d_%d 中断失控（已进入 %" PRIu32 " 次），"
                 "已屏蔽该引脚\n",
                 bank, pin, g_gpio_isr[bank][pin].count);
        }
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_gpio_wr16
 *
 * Description:
 *   往 v2 的"半寄存器"写一位。
 *
 *   pin 0-15 落在 offset_l，pin 16-31 落在 offset_l + 4，位号折算回 0-15。
 *   写入格式：高 16 位写使能掩码，低 16 位数据。
 *
 ****************************************************************************/

static void rk3576_gpio_wr16(uintptr_t base, uint32_t offset_l,
                             int pin, bool value)
{
  uint32_t regval;

  if (pin >= 16)
    {
      offset_l += 4;      /* 切到 _H 寄存器 */
      pin      -= 16;
    }

  regval = 1u << (pin + 16);        /* 写使能：只允许改这一位 */

  if (value)
    {
      regval |= 1u << pin;
    }

  putreg32(regval, base + offset_l);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int rk3576_gpio_setdir(int bank, int pin, bool output)
{
  if (bank < 0 || bank >= RK3576_GPIO_NBANKS ||
      pin  < 0 || pin  >= RK3576_GPIO_NPINS)
    {
      return -EINVAL;
    }

  rk3576_gpio_wr16(g_gpio_base[bank], RK3576_GPIO_SWPORT_DDR_L,
                   pin, output);
  return OK;
}

int rk3576_gpio_write(int bank, int pin, bool value)
{
  if (bank < 0 || bank >= RK3576_GPIO_NBANKS ||
      pin  < 0 || pin  >= RK3576_GPIO_NPINS)
    {
      return -EINVAL;
    }

  rk3576_gpio_wr16(g_gpio_base[bank], RK3576_GPIO_SWPORT_DR_L,
                   pin, value);
  return OK;
}

int rk3576_gpio_read_output(int bank, int pin)
{
  uint32_t regval;
  uint32_t off;

  if (bank < 0 || bank >= RK3576_GPIO_NBANKS ||
      pin  < 0 || pin  >= RK3576_GPIO_NPINS)
    {
      return -EINVAL;
    }

  /* ★ 读的是输出数据寄存器，不是 EXT_PORT。
   *
   *   配成输出的脚，EXT_PORT 读回来不一定等于驱动值 —— 本板 GPIO4_A4
   *   实测写 1 之后 EXT_PORT 仍读 0（引脚确实被驱动：同一根杜邦线上的
   *   中断子项能收到边沿），说明该脚做输出时输入缓冲不工作。
   *   xTS 1.3.6 的 drivertest_gpio_rw 写完立刻读回并要求相等，
   *   对输出脚就该回答"我驱动的是什么"，这个值只有 DR 寄存器有。
   *
   *   DR 是带高 16 位写使能的寄存器，但读回来是普通值；pin >= 16 在 _H。
   */

  off = pin < 16 ? RK3576_GPIO_SWPORT_DR_L : RK3576_GPIO_SWPORT_DR_H;
  regval = getreg32(g_gpio_base[bank] + off);
  return (regval >> (pin & 15)) & 1;
}

int rk3576_gpio_read(int bank, int pin)
{
  uint32_t regval;

  if (bank < 0 || bank >= RK3576_GPIO_NBANKS ||
      pin  < 0 || pin  >= RK3576_GPIO_NPINS)
    {
      return -EINVAL;
    }

  /* EXT_PORT 是普通只读寄存器，32 位一次读完，不分 _L/_H。 */

  regval = getreg32(g_gpio_base[bank] + RK3576_GPIO_EXT_PORT);
  return (regval >> pin) & 1;
}


/****************************************************************************
 * Name: rk3576_gpio_clk_enable
 *
 * Description:
 *   打开某个 bank 的 pclk 与 **dbclk**。
 *
 * ★ 少开 dbclk 的后果：寄存器全对，但中断永远不来
 *
 *   GPIO 有两个时钟（厂商 dtsi 写得很清楚）：
 *     clocks = <&cru PCLK_GPIO0>, <&cru DBCLK_GPIO0>;
 *
 *   pclk 管寄存器访问，dbclk 管**电平/边沿的采样**。U-Boot 把 pclk 留着
 *   开，所以读写寄存器一切正常 —— 方向、中断类型、极性、使能位写进去
 *   再读出来都对得上。但 dbclk 关着，检测逻辑根本没跑。
 *
 *   板上实测把这个失效模式摆得很清楚（k7diag tp，60 秒采样）：
 *
 *     起始: INT=1  INT_EN_H=0x00000020  RAWSTATUS=0x00000000
 *     结果: 采样 12000 次，被拉低 1714 次，跳变 1889 次
 *     结束: INT=0  RAWSTATUS=0x00000000  STATUS=0x00000000
 *
 *   引脚确实被按下去拉低了（1714 次），配置是电平触发+低有效+已使能，
 *   而**中断原始状态位始终是 0**。写得对、读得到、就是不触发。
 *
 *   这种"寄存器层面全对、功能不工作"的组合极难从代码上看出来 —— 因为
 *   代码确实没写错。只能靠把引脚电平和 RAWSTATUS 放在一起看：两者矛盾
 *   时，问题一定在两者之间的那段逻辑，而它唯一的依赖就是 dbclk。
 *
 ****************************************************************************/

static void rk3576_gpio_clk_enable(int bank)
{
  static bool done[RK3576_GPIO_NBANKS];

  if (bank < 0 || bank >= RK3576_GPIO_NBANKS || done[bank])
    {
      return;
    }

  /* 门控位出处：kernel-6.1 drivers/clk/rockchip/clk-rk3576.c
   *   GPIO0 在常开域，归 PMU CRU 管；GPIO1~4 在主 CRU。
   */

  switch (bank)
    {
      case 0:
        rk3576_pmu_clk_gate(7, 6, true);    /* PCLK_GPIO0  */
        rk3576_pmu_clk_gate(7, 7, true);    /* DBCLK_GPIO0 */
        break;

      case 1:
        rk3576_clk_gate(17, 15, true);      /* PCLK_GPIO1  */
        rk3576_clk_gate(18, 0,  true);      /* DBCLK_GPIO1 */
        break;

      case 2:
        rk3576_clk_gate(18, 1, true);       /* PCLK_GPIO2  */
        rk3576_clk_gate(18, 2, true);       /* DBCLK_GPIO2 */
        break;

      case 3:
        rk3576_clk_gate(18, 3, true);       /* PCLK_GPIO3  */
        rk3576_clk_gate(18, 4, true);       /* DBCLK_GPIO3 */
        break;

      case 4:
        rk3576_clk_gate(18, 5, true);       /* PCLK_GPIO4  */
        rk3576_clk_gate(18, 6, true);       /* DBCLK_GPIO4 */
        break;

      default:
        return;
    }

  done[bank] = true;
}

int rk3576_gpio_irq_config(int bank, int pin, bool rising, bool level)
{
  uintptr_t base;

  if (bank < 0 || bank >= RK3576_GPIO_NBANKS ||
      pin  < 0 || pin  >= RK3576_GPIO_NPINS)
    {
      return -EINVAL;
    }

  /* ★ 先开时钟。dbclk 不开，下面所有配置写进去也检测不到电平。 */

  rk3576_gpio_clk_enable(bank);

  base = g_gpio_base[bank];

  /* 中断引脚必须是输入 */

  rk3576_gpio_wr16(base, RK3576_GPIO_SWPORT_DDR_L, pin, false);

  /* INT_TYPE: 0=电平 1=边沿；INT_POLARITY: 0=低/下降 1=高/上升 */

  rk3576_gpio_wr16(base, RK3576_GPIO_INT_TYPE_L, pin, !level);
  rk3576_gpio_wr16(base, RK3576_GPIO_INT_POLARITY_L, pin, rising);
  rk3576_gpio_wr16(base, RK3576_GPIO_INT_BOTHEDGE_L, pin, false);

  /* 先屏蔽，等 attach 之后再放开 */

  rk3576_gpio_wr16(base, RK3576_GPIO_INT_MASK_L, pin, true);
  rk3576_gpio_wr16(base, RK3576_GPIO_INT_EN_L, pin, false);
  return OK;
}

int rk3576_gpio_irq_attach(int bank, int pin, xcpt_t isr, void *arg)
{
  static bool attached[RK3576_GPIO_NBANKS];
  int ret;

  if (bank < 0 || bank >= RK3576_GPIO_NBANKS ||
      pin  < 0 || pin  >= RK3576_GPIO_NPINS)
    {
      return -EINVAL;
    }

  g_gpio_isr[bank][pin].isr = isr;
  g_gpio_isr[bank][pin].arg = arg;

  /* bank 级中断只挂一次 */

  if (!attached[bank])
    {
      ret = irq_attach(g_gpio_irq[bank], rk3576_gpio_interrupt,
                       (void *)(intptr_t)bank);
      if (ret < 0)
        {
          return ret;
        }

      up_enable_irq(g_gpio_irq[bank]);
      attached[bank] = true;
    }

  return OK;
}

int rk3576_gpio_irq_enable(int bank, int pin, bool enable)
{
  uintptr_t base;

  if (bank < 0 || bank >= RK3576_GPIO_NBANKS ||
      pin  < 0 || pin  >= RK3576_GPIO_NPINS)
    {
      return -EINVAL;
    }

  base = g_gpio_base[bank];

  /* 顺序有讲究：使能前先清掉可能残留的挂起状态，否则一放开就会
   * 立刻进一次不属于本次的中断。
   */

  if (enable)
    {
      rk3576_gpio_wr16(base, RK3576_GPIO_PORT_EOI_L, pin, true);
    }

  rk3576_gpio_wr16(base, RK3576_GPIO_INT_EN_L, pin, enable);
  rk3576_gpio_wr16(base, RK3576_GPIO_INT_MASK_L, pin, !enable);
  return OK;
}

uint32_t rk3576_gpio_verid(int bank)
{
  if (bank < 0 || bank >= RK3576_GPIO_NBANKS)
    {
      return 0;
    }

  return getreg32(g_gpio_base[bank] + RK3576_GPIO_VER_ID);
}
