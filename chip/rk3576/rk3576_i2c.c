/****************************************************************************
 * chip/rk3576/rk3576_i2c.c
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

/* RK3576 I2C 主机驱动（轮询式）。
 *
 * ★ 为什么用轮询而不是中断
 *
 *   I2C 是低速总线，一次事务通常几十微秒到几百微秒，轮询的开销可以
 *   接受；换来的是不依赖 GIC 的 SPI 挂接，少一个可能出错的环节。
 *   M2 阶段先把总线跑通，中断留到确有性能需求时再加（IRQ 号已记录在
 *   rk3576_memorymap.h，dtb 出处见 scripts/dtb-query.py）。
 *
 * ★ 不配置引脚复用和时钟
 *
 *   与 rk3576_gpio.c 同样的取舍：依赖 U-Boot 已经配好的复用与时钟。
 *   代价是只能用 U-Boot 已经初始化过的总线。
 *
 *   自检手段：往 CLKDIV 写一个值再读回。读回一致说明寄存器块活着
 *   （PCLK 已使能、基址正确、MMU 已映射）；读回 0 或读不回说明这一
 *   环没成立，此时继续操作只会得到静默失败。
 *
 * ★ SCL 频率的取值依据
 *
 *   分频公式需要输入时钟频率，而我们没有 CRU 驱动，拿不到准确值。
 *   这里按"宁慢勿快"处理：假定输入时钟不超过 200MHz，据此算出的分频
 *   使 SCL 不超过 100kHz。若实际时钟更低（如 100MHz），SCL 会变成
 *   50kHz —— 依然可用。
 *
 *   这个方向是安全的：I2C 从机都规定了 SCL 上限（HYM8563 是 400kHz），
 *   低于上限一律工作，高于上限才会失败。等有了 CRU 驱动再算准。
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/nuttx.h>
#include <nuttx/arch.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/mutex.h>

#include "arm64_internal.h"
#include "rk3576_i2c.h"
#include "hardware/rk3576_i2c.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_cru.h"
#include "rk3576_pinmux.h"
#include "rk3576_gpio.h"

#ifdef CONFIG_RK3576_I2C

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 目标 SCL 频率。HYM8563 与 HUSB311 都支持到 400kHz，取 100kHz 留足余量。 */

#define I2C_TARGET_SCL_HZ         100000

/* CLK_I2Cn 的可选时钟源，索引即 CLKSEL 位域的取值。
 * 出处：clk-rk3576.c 的 mux_200m_100m_50m_24m_p。
 */

static const uint32_t g_i2c_src_hz[4] =
{
  200000000, 100000000, 50000000, 24000000
};

/* 轮询超时。分两档，因为两件事的时间尺度差三个数量级。
 *
 * ★ 原来两档都用 100ms，代价在启动时间上暴露了出来
 *
 *   板上带时间戳抓启动日志（主机侧记录每个字节到达的时刻，不用改板子）
 *   看到：openvela 从显示就绪(4.0s)到摄像头探测(35.8s)之间的 **32 秒**
 *   全部花在 I2C3 上，一百三十多次事务，每次稳定 0.234s。
 *
 *   而 0.234s ≈ 两次 100ms 超时。日志里那一百多行
 *   `I2C3 写-起始条件 ...` 只在 rk3576_i2c_start() **失败**时才打 ——
 *   也就是说每一次都是"等起始条件超时"。
 *
 *   起始条件在 100kHz 下约 10us 就发完了，用 100ms 等它是一万倍的余量，
 *   等不到时就白白烧掉 100ms。数据相位才需要按字节数给余量。
 *
 *   分开之后，就算 I2C3 仍然有毛病（ES8388 那条总线的确还没修好，见
 *   docs/ 里音频采集全零那一条），代价也从 32 秒降到 0.7 秒。
 *
 *   这不是把问题藏起来：失败照样报错、照样有诊断行（只是限流了），
 *   只是**超时值终于和它所等待的事件同一个量级**。
 */

#define I2C_START_TIMEOUT_US      5000      /* 起始/停止：~10us 的 500 倍 */
#define I2C_XFER_TIMEOUT_US       50000     /* 数据相位：32 字节 @50kHz 约 6ms */

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* 一条总线所需的全部平台参数。时钟与复用的取值都能追到出处，
 * 见各字段旁的注释。
 */

struct rk3576_i2c_config_s
{
  uint8_t   port;
  uintptr_t base;
  uint8_t   gate_con;      /* CLKGATE_CON 序号（PCLK 与 CLK 同属一个） */
  uint8_t   gate_pclk;     /* PCLK_I2Cn 的位号 */
  uint8_t   gate_clk;      /* CLK_I2Cn 的位号  */
  uint8_t   sel_con;       /* CLKSEL_CON 序号  */
  uint8_t   sel_shift;     /* 时钟源位域起始位 */
  int8_t    scl_bank;      /* SCL 引脚，-1 表示不配复用 */
  int8_t    scl_pin;
  int8_t    sda_bank;
  int8_t    sda_pin;
  uint8_t   pin_func;      /* 两个引脚的复用功能号相同 */
  bool      pmu;           /* true = 时钟在 PMU 域 CRU（仅 I2C0） */
};

struct rk3576_i2c_priv_s
{
  const struct i2c_ops_s          *ops;   /* 必须是第一个成员 */
  const struct rk3576_i2c_config_s *cfg;
  uint8_t                          port;
  uintptr_t                        base;
  uint32_t                         src_hz; /* 实测的输入时钟频率 */
  mutex_t                          lock;
  bool                             initialized;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int rk3576_i2c_transfer(struct i2c_master_s *dev,
                               struct i2c_msg_s *msgs, int count);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 最近一次 rk3576_i2c_wait() 读到的完整 IPD。
 * 传输失败时由 rk3576_i2c_diag() 打出来 —— 只看清位后的 IPD 会丢掉
 * 命中的那几位，而它们正是判断失败原因的关键。
 */

static uint32_t g_last_hit;

static const struct i2c_ops_s g_rk3576_i2c_ops =
{
  .transfer = rk3576_i2c_transfer,
};

/* 支持的总线。只列出板上实际用到的两条：
 *
 *   I2C1  PMIC RK806 @0x23 —— U-Boot 启动时必然与它通过信，
 *                             因此这条总线的功能时钟一定是开的。
 *                             用作驱动逻辑的对照组。
 *   I2C2  RTC HYM8563 @0x51、Type-C PD HUSB311 @0x4e
 */

static const struct rk3576_i2c_config_s g_i2c_config[] =
{
  /* I2C0 —— 5 寸 MIPI 屏（F050008M01）转接板上的触摸控制器。
   *
   * 出处是原理图 K7_V1.1 第 27 页 "Single-MIPI LCM"：30pin FPC 的
   * Pin23/24 是 TP_I2C_SCL/SDA，对应网络 I2C0_SCL_M1_TP /
   * I2C0_SDA_M1_TP。此前扫遍 I2C1~I2C9 都没找到触摸，就是因为它在
   * I2C0 上，而 I2C0 被跳过了 —— 它的时钟不在主 CRU。
   *
   * ★ I2C0 基址 0x27300000 落在常开域，时钟归 PMU CRU 管：
   *     GATE(PCLK_I2C0, ... RK3576_PMU_CLKGATE_CON(5), 1)
   *     COMPOSITE_NODIV(CLK_I2C0, ... RK3576_PMU_CLKSEL_CON(6), 7, 2,
   *                                  RK3576_PMU_CLKGATE_CON(5), 2)
   *   用主 CRU 的偏移去开会写到别的寄存器，不报错但总线不动。
   *   选源父时钟表与主域相同：{200M, 100M, 50M, 24M}。
   *
   * 引脚：原厂 dtb 的 i2c0m1-xfer = <0 17 9 ...>, <0 18 9 ...>
   *   即 GPIO0_C1 / GPIO0_C2，功能号 9（与原理图一致）。
   *
   * ★ 触摸芯片由 VCC3V3_LCD_S0 供电，而该电源轨受 LCD_PWREN_H
   *   (GPIO0_C6) 控制。不先把它拉高，扫描必然全空 —— 芯片没电。
   */

  {
    .port = 0, .base = RK3576_I2C0_ADDR,
    .gate_con = 5, .gate_pclk = 1, .gate_clk = 2,
    .sel_con = 6, .sel_shift = 7,
    .scl_bank = 0, .scl_pin = 17, .sda_bank = 0, .sda_pin = 18,
    .pin_func = 9,
    .pmu = true,
  },

  /* I2C1 —— PMIC RK806。U-Boot 启动时必然访问过，用作对照组。
   * 时钟：clk-rk3576.c
   *   GATE(PCLK_I2C1, ... CLKGATE_CON(12), 0)
   *   COMPOSITE_NODIV(CLK_I2C1, ... CLKSEL_CON(57), 0, 2, ...
   *                   CLKGATE_CON(12), 12)
   * 引脚复用交给 U-Boot（bank/pin 填 -1）—— 这条总线它已经配好了，
   * 我们不去动它，正好保留一个"未经我们改动"的对照。
   */

  {
    .port = 1, .base = RK3576_I2C1_ADDR,
    .gate_con = 12, .gate_pclk = 0, .gate_clk = 12,
    .sel_con = 57, .sel_shift = 0,
    .scl_bank = -1, .scl_pin = -1, .sda_bank = -1, .sda_pin = -1,
    .pin_func = 0,
  },

  /* I2C2 —— RTC HYM8563 @0x51、Type-C PD HUSB311 @0x4e。
   * 时钟：clk-rk3576.c
   *   GATE(PCLK_I2C2, ... CLKGATE_CON(12), 1)
   *   COMPOSITE_NODIV(CLK_I2C2, ... CLKSEL_CON(57), 2, 2, ...
   *                   CLKGATE_CON(12), 13)
   * 引脚：原厂 dtb 的 i2c2m0-xfer = <0 15 9 &pcfg...>, <0 16 9 ...>
   *   即 GPIO0_B7 / GPIO0_C0，功能号 9。
   *   （用 scripts/dtb-query.py 从原厂 boot.img 查得）
   */

  {
    .port = 2, .base = RK3576_I2C2_ADDR,
    .gate_con = 12, .gate_pclk = 1, .gate_clk = 13,
    .sel_con = 57, .sel_shift = 2,
    .scl_bank = 0, .scl_pin = 15, .sda_bank = 0, .sda_pin = 16,
    .pin_func = 9,
  },

  /* I2C3 —— 音频 codec ES8388 @0x10（喇叭与咪头走它）。
   * 时钟：clk-rk3576.c
   *   GATE(PCLK_I2C3, ... CLKGATE_CON(12), 2)
   *   COMPOSITE_NODIV(CLK_I2C3, ... CLKSEL_CON(57), 4, 2, ...
   *                   CLKGATE_CON(12), 14)
   * 引脚：原厂 dtb 的 i2c3m0-xfer = <4 13 11 ...>, <4 12 11 ...>
   *   即 GPIO4_B5 / GPIO4_B4，功能号 11。
   */

  {
    .port = 3, .base = RK3576_I2C3_ADDR,
    .gate_con = 12, .gate_pclk = 2, .gate_clk = 14,
    .sel_con = 57, .sel_shift = 4,
    .scl_bank = 4, .scl_pin = 13, .sda_bank = 4, .sda_pin = 12,
    .pin_func = 11,
  },

  /* I2C4/5/7/8 —— 加入是为了找触摸控制器。
   *
   * 屏接上后扫描 I2C1/2/3 均未发现 GT9xx（0x5d 或 0x14），说明触摸
   * 挂在别的总线上。原厂 dtb 里这四条状态为 okay，逐条扫排除。
   *
   * 时钟位延续同一规律（clk-rk3576.c）：
   *   PCLK_I2Cn = CLKGATE_CON(12) 的 bit (n-1)
   *   CLK_I2C1..4 = CON(12) bit 12..15；CLK_I2C5..9 = CON(13) bit 0..4
   *   选源 CLKSEL_CON(57) 的 bit[2n-1:2n-2]，I2C9 在 CON(58)
   * 引脚取自原厂 dtb 的 i2cNmX-xfer。
   */

  {
    .port = 4, .base = RK3576_I2C4_ADDR,
    .gate_con = 12, .gate_pclk = 3, .gate_clk = 15,
    .sel_con = 57, .sel_shift = 6,
    .scl_bank = 3, .scl_pin = 16, .sda_bank = 3, .sda_pin = 15,
    .pin_func = 11,
  },
  {
    .port = 5, .base = RK3576_I2C5_ADDR,
    .gate_con = 13, .gate_pclk = 4, .gate_clk = 0,
    .sel_con = 57, .sel_shift = 8,
    .scl_bank = 3, .scl_pin = 20, .sda_bank = 3, .sda_pin = 17,
    .pin_func = 11,
  },
  {
    .port = 7, .base = RK3576_I2C7_ADDR,
    .gate_con = 13, .gate_pclk = 6, .gate_clk = 2,
    .sel_con = 57, .sel_shift = 12,
    .scl_bank = 3, .scl_pin = 0, .sda_bank = 3, .sda_pin = 1,
    .pin_func = 11,
  },

  /* I2C6/I2C9 —— 原厂 dtb 里状态为 disabled，但触摸所在的总线本就
   * 应由 LCD overlay 启用，因此 disabled 不代表物理上没有器件。
   * 加入继续排除。
   */

  {
    .port = 6, .base = RK3576_I2C6_ADDR,
    .gate_con = 13, .gate_pclk = 5, .gate_clk = 1,
    .sel_con = 57, .sel_shift = 10,
    .scl_bank = 0, .scl_pin = 2, .sda_bank = 0, .sda_pin = 5,
    .pin_func = 11,
  },
  {
    .port = 9, .base = RK3576_I2C9_ADDR,
    .gate_con = 13, .gate_pclk = 8, .gate_clk = 4,
    .sel_con = 58, .sel_shift = 0,
    .scl_bank = 1, .scl_pin = 5, .sda_bank = 1, .sda_pin = 6,
    .pin_func = 10,
  },
  {
    .port = 8, .base = RK3576_I2C8_ADDR,
    .gate_con = 13, .gate_pclk = 7, .gate_clk = 3,
    .sel_con = 57, .sel_shift = 14,
    .scl_bank = 2, .scl_pin = 14, .sda_bank = 2, .sda_pin = 15,
    .pin_func = 11,
  },
};

#define RK3576_I2C_NBUSES (sizeof(g_i2c_config) / sizeof(g_i2c_config[0]))

/* ★ 这里的行数必须与 g_i2c_config 的条目数一致。C 不会为多出来的元素
 * 补 .ops，漏一行的表现是最后一条总线的 ops 为 NULL，调用即崩。
 */

static struct rk3576_i2c_priv_s g_i2c_priv[RK3576_I2C_NBUSES] =
{
  { .ops = &g_rk3576_i2c_ops, .lock = NXMUTEX_INITIALIZER },
  { .ops = &g_rk3576_i2c_ops, .lock = NXMUTEX_INITIALIZER },
  { .ops = &g_rk3576_i2c_ops, .lock = NXMUTEX_INITIALIZER },
  { .ops = &g_rk3576_i2c_ops, .lock = NXMUTEX_INITIALIZER },
  { .ops = &g_rk3576_i2c_ops, .lock = NXMUTEX_INITIALIZER },
  { .ops = &g_rk3576_i2c_ops, .lock = NXMUTEX_INITIALIZER },
  { .ops = &g_rk3576_i2c_ops, .lock = NXMUTEX_INITIALIZER },
  { .ops = &g_rk3576_i2c_ops, .lock = NXMUTEX_INITIALIZER },
  { .ops = &g_rk3576_i2c_ops, .lock = NXMUTEX_INITIALIZER },
  { .ops = &g_rk3576_i2c_ops, .lock = NXMUTEX_INITIALIZER },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t i2c_getreg(struct rk3576_i2c_priv_s *priv,
                                  uint32_t off)
{
  return getreg32(priv->base + off);
}

static inline void i2c_putreg(struct rk3576_i2c_priv_s *priv,
                              uint32_t off, uint32_t val)
{
  putreg32(val, priv->base + off);
}

/****************************************************************************
 * Name: rk3576_i2c_wait
 *
 * Description:
 *   轮询等待 IPD 中出现 mask 里的任一位。
 *
 * Returned Value:
 *   命中的 IPD 位；超时返回 0。命中的位会被清掉（IPD 写 1 清）。
 *
 ****************************************************************************/

static uint32_t rk3576_i2c_wait(struct rk3576_i2c_priv_s *priv,
                                uint32_t mask, int timeout_us)
{
  uint32_t ipd;
  int us;

  for (us = 0; us < timeout_us; us++)
    {
      ipd = i2c_getreg(priv, RK3576_I2C_IPD);
      if ((ipd & mask) != 0)
        {
          i2c_putreg(priv, RK3576_I2C_IPD, ipd & mask);   /* 写 1 清 */
          g_last_hit = ipd;                               /* 留给 diag 用 */
          return ipd & mask;
        }

      up_udelay(1);
    }

  return 0;
}

/****************************************************************************
 * Name: rk3576_i2c_start
 *
 * Description:
 *   发起(重复)起始条件，等它真正发出，然后清掉 CON 里的 START 位。
 *
 *   ★ 清 START 这一步不能省。控制器要求起始条件完成后先撤掉 START，
 *     再装载 MTXCNT/MRXCNT 启动数据相位；START 一直挂着时数据不会发出，
 *     表现为总线上只有起始条件、所有地址都扫不到。
 *     依据：Linux i2c-rk3x.c 的 rk3x_i2c_handle_start()。
 *
 ****************************************************************************/

static int rk3576_i2c_start(struct rk3576_i2c_priv_s *priv, uint32_t mode)
{
  uint32_t con;

  i2c_putreg(priv, RK3576_I2C_IPD, I2C_INT_ALL);

  con = I2C_CON_EN | mode | I2C_CON_START | I2C_CON_ACT2NAK;
  i2c_putreg(priv, RK3576_I2C_CON, con);

  if (rk3576_i2c_wait(priv, I2C_INT_START, I2C_START_TIMEOUT_US) == 0)
    {
      return -ETIMEDOUT;
    }

  i2c_putreg(priv, RK3576_I2C_CON, con & ~I2C_CON_START);
  return OK;
}

/****************************************************************************
 * Name: rk3576_i2c_diag
 *
 * Description:
 *   传输失败时把控制器状态打出来。三个寄存器合起来能区分几种成因：
 *     FCNT == 0        总线上一个字节都没发出去 —— 功能时钟或引脚复用
 *     IPD 有 NAKRCV    时序正常，只是从机没应答
 *     全 0 且 FCNT==0  控制器没动作
 *
 ****************************************************************************/

static void rk3576_i2c_diag(struct rk3576_i2c_priv_s *priv,
                            const char *what, uint8_t addr)
{
  /* ★ 限流。
   *
   *   一条坏掉的总线会把同一行刷几百遍（实测 ES8388 初始化一次就有
   *   一百三十多行），既淹掉别的日志，也让人误以为"在忙"。
   *   每个端口只完整打前 3 次，之后每 64 次打一行汇总。
   */

  static uint32_t fails[nitems(g_i2c_config)];
  uint32_t n;

  if (priv->port >= nitems(g_i2c_config))
    {
      return;
    }

  n = ++fails[priv->port];

  if (n > 3 && (n % 64) != 0)
    {
      return;
    }

  syslog(LOG_ERR,
         "I2C%u %s a=%02x hit=%02" PRIx32 " ipd=%02" PRIx32
         " fcnt=%" PRIu32 "%s\n",
         priv->port, what, addr, g_last_hit,
         i2c_getreg(priv, RK3576_I2C_IPD),
         i2c_getreg(priv, RK3576_I2C_FCNT),
         n > 3 ? "（同类失败已限流）" : "");
}

/****************************************************************************
 * Name: rk3576_i2c_stop
 *
 * Description:
 *   发停止条件并关闭控制器。无论前面成功与否都要走这一步，否则总线
 *   会一直被占着，后续事务全部失败。
 *
 ****************************************************************************/

static void rk3576_i2c_stop(struct rk3576_i2c_priv_s *priv)
{
  i2c_putreg(priv, RK3576_I2C_IPD, I2C_INT_ALL);
  i2c_putreg(priv, RK3576_I2C_CON, I2C_CON_EN | I2C_CON_STOP);
  rk3576_i2c_wait(priv, I2C_INT_STOP, I2C_START_TIMEOUT_US);
  i2c_putreg(priv, RK3576_I2C_CON, 0);
}

/****************************************************************************
 * Name: rk3576_i2c_write_msg
 *
 * Description:
 *   发送一条写消息。从机地址由本函数拼进数据流的第一个字节 ——
 *   MOD_TX 模式下控制器不单独提供地址寄存器。
 *
 ****************************************************************************/

static int rk3576_i2c_write_msg(struct rk3576_i2c_priv_s *priv,
                                struct i2c_msg_s *msg)
{
  uint8_t buf[RK3576_I2C_FIFO_BYTES];
  uint32_t word;
  uint32_t hit;
  int total;
  int ret;
  int i;

  total = msg->length + 1;                    /* +1 是地址字节 */
  if (total > RK3576_I2C_FIFO_BYTES)
    {
      return -EINVAL;                         /* 超过 FIFO，暂不支持分片 */
    }

  buf[0] = (uint8_t)(msg->addr << 1);         /* 7 位地址 + 写方向 */
  memcpy(&buf[1], msg->buffer, msg->length);

  /* 按小端打包进 32 位窗口 */

  for (i = 0; i < total; i += 4)
    {
      int n = (total - i) > 4 ? 4 : (total - i);
      int b;

      word = 0;
      for (b = 0; b < n; b++)
        {
          word |= (uint32_t)buf[i + b] << (8 * b);
        }

      i2c_putreg(priv, RK3576_I2C_TXDATA_BASE + i, word);
    }

  ret = rk3576_i2c_start(priv, I2C_CON_MOD_TX);
  if (ret < 0)
    {
      rk3576_i2c_diag(priv, "写-起始条件", msg->addr);
      return ret;
    }

  i2c_putreg(priv, RK3576_I2C_MTXCNT, total);

  hit = rk3576_i2c_wait(priv, I2C_INT_MBTF | I2C_INT_NAKRCV,
                        I2C_XFER_TIMEOUT_US);
  if (hit == 0)
    {
      rk3576_i2c_diag(priv, "写-数据相位", msg->addr);
      return -ETIMEDOUT;
    }

  if ((hit & I2C_INT_NAKRCV) != 0)
    {
      /* 从机没应答。总线扫描时这是正常结果，不打日志。 */

      return -ENXIO;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_i2c_read_msg
 *
 * Description:
 *   接收一条读消息。MOD_RX 模式下从机地址放在 MRXADDR，且必须置
 *   VALID 位说明地址占了几个字节 —— 漏置 VALID 是这个控制器上最
 *   容易犯的错，表现为总线上根本没有地址相位。
 *
 ****************************************************************************/

static int rk3576_i2c_read_msg(struct rk3576_i2c_priv_s *priv,
                               struct i2c_msg_s *msg)
{
  uint32_t word;
  uint32_t hit;
  int ret;
  int i;

  if (msg->length > RK3576_I2C_FIFO_BYTES)
    {
      return -EINVAL;
    }

  i2c_putreg(priv, RK3576_I2C_MRXADDR,
             ((uint32_t)(msg->addr << 1) | 1) | I2C_MRXADDR_VALID(0));
  i2c_putreg(priv, RK3576_I2C_MRXRADDR, 0);

  /* ★ 一次读的首个数据块必须用 MOD_REGISTER_TX，不是 MOD_RX。
   *
   *   MOD_RX 下控制器不发地址相位 —— 它假定地址已经发过了，直接开始
   *   收数据。后果是永远不产生 NAKRCV（没有应答位可判），而 FCNT 照常
   *   计数，表现为"每个从机地址都有响应"的假象。
   *   MOD_RX 只用于读第二块及以后（本驱动单次不超过 32 字节，用不到）。
   *
   *   依据：Linux i2c-rk3x.c
   *     rk3x_i2c_setup():        读操作设
   *                              i2c->mode = REG_CON_MOD_REGISTER_TX
   *     rk3x_i2c_prepare_read(): 仅当 processed != 0 才切到 REG_CON_MOD_RX
   */

  ret = rk3576_i2c_start(priv, I2C_CON_MOD_REGISTER_TX | I2C_CON_LASTACK);
  if (ret < 0)
    {
      rk3576_i2c_diag(priv, "读-起始条件", msg->addr);
      return ret;
    }

  i2c_putreg(priv, RK3576_I2C_MRXCNT, msg->length);

  hit = rk3576_i2c_wait(priv, I2C_INT_MBRF | I2C_INT_NAKRCV,
                        I2C_XFER_TIMEOUT_US);
  if (hit == 0)
    {
      rk3576_i2c_diag(priv, "读-数据相位", msg->addr);
      return -ETIMEDOUT;
    }

  if ((hit & I2C_INT_NAKRCV) != 0)
    {
      return -ENXIO;
    }

  for (i = 0; i < msg->length; i += 4)
    {
      int n = (msg->length - i) > 4 ? 4 : (msg->length - i);
      int b;

      word = i2c_getreg(priv, RK3576_I2C_RXDATA_BASE + i);
      for (b = 0; b < n; b++)
        {
          msg->buffer[i + b] = (uint8_t)(word >> (8 * b));
        }
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_i2c_regread
 *
 * Description:
 *   "写寄存器地址 + 读数据" 的组合事务，由硬件一次完成。
 *
 *   ★ 这个控制器不支持软件自己发两次事务来拼重复起始条件：写完之后
 *     再发 START 不会产生 INT_START，事务卡死。必须把从机地址放进
 *     MRXADDR、寄存器地址放进 MRXRADDR，硬件会自动完成
 *     START-W(addr)-W(reg)-RESTART-R(data)-STOP 的完整时序。
 *
 *   MRXRADDR 最多容纳 3 个寄存器地址字节，每个字节要在 MRXADDR 里
 *   对应置一个 VALID 位说明用了几字节。
 *
 *   依据：Linux i2c-rk3x.c 的 rk3x_i2c_setup()，条件
 *         num >= 2 && msgs[0].len < 4 && !RD && msgs[1] 为 RD。
 *
 ****************************************************************************/

static int rk3576_i2c_regread(struct rk3576_i2c_priv_s *priv,
                              struct i2c_msg_s *wmsg,
                              struct i2c_msg_s *rmsg)
{
  uint32_t reg_addr = 0;
  uint32_t word;
  uint32_t hit;
  int ret;
  int i;

  if (rmsg->length > RK3576_I2C_FIFO_BYTES)
    {
      return -EINVAL;
    }

  for (i = 0; i < wmsg->length; i++)
    {
      reg_addr |= (uint32_t)wmsg->buffer[i] << (i * 8);
      reg_addr |= I2C_MRXADDR_VALID(i);
    }

  /* 组合模式下 MRXADDR 里放的是"写方向"的从机地址，读方向由硬件在
   * 重复起始后自己补上 —— 与单独读时要手工置 bit0 不同。
   */

  i2c_putreg(priv, RK3576_I2C_MRXADDR,
             ((uint32_t)(wmsg->addr << 1)) | I2C_MRXADDR_VALID(0));
  i2c_putreg(priv, RK3576_I2C_MRXRADDR, reg_addr);

  ret = rk3576_i2c_start(priv, I2C_CON_MOD_REGISTER_TX | I2C_CON_LASTACK);
  if (ret < 0)
    {
      rk3576_i2c_diag(priv, "组合读-起始条件", wmsg->addr);
      return ret;
    }

  i2c_putreg(priv, RK3576_I2C_MRXCNT, rmsg->length);

  hit = rk3576_i2c_wait(priv, I2C_INT_MBRF | I2C_INT_NAKRCV,
                        I2C_XFER_TIMEOUT_US);
  if (hit == 0)
    {
      rk3576_i2c_diag(priv, "组合读-数据相位", wmsg->addr);
      return -ETIMEDOUT;
    }

  if ((hit & I2C_INT_NAKRCV) != 0)
    {
      return -ENXIO;
    }

  for (i = 0; i < rmsg->length; i += 4)
    {
      int n = (rmsg->length - i) > 4 ? 4 : (rmsg->length - i);
      int b;

      word = i2c_getreg(priv, RK3576_I2C_RXDATA_BASE + i);
      for (b = 0; b < n; b++)
        {
          rmsg->buffer[i + b] = (uint8_t)(word >> (8 * b));
        }
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_i2c_transfer
 *
 * Description:
 *   NuttX i2c_master_s 的传输入口。
 *
 *   每条消息各自发起一次起始条件（第二条起就是重复起始），整串消息
 *   结束后统一发停止条件。"写寄存器地址再读数据"因此自然表现为
 *   START-W-RESTART-R-STOP，不依赖控制器的组合模式（那个模式在部分
 *   版本上有缺陷）。
 *
 ****************************************************************************/

static int rk3576_i2c_transfer(struct i2c_master_s *dev,
                               struct i2c_msg_s *msgs, int count)
{
  struct rk3576_i2c_priv_s *priv = (struct rk3576_i2c_priv_s *)dev;
  int ret = OK;
  int i;

  DEBUGASSERT(priv != NULL && msgs != NULL && count > 0);

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  i = 0;
  while (i < count)
    {
      /* "短写 + 读" 交给硬件的组合模式一次完成，见 rk3576_i2c_regread。 */

      if (i + 1 < count &&
          (msgs[i].flags & I2C_M_READ) == 0 &&
          (msgs[i + 1].flags & I2C_M_READ) != 0 &&
          msgs[i].length > 0 && msgs[i].length < 4 &&
          msgs[i].addr == msgs[i + 1].addr)
        {
          ret = rk3576_i2c_regread(priv, &msgs[i], &msgs[i + 1]);
          i += 2;
        }
      else if ((msgs[i].flags & I2C_M_READ) != 0)
        {
          ret = rk3576_i2c_read_msg(priv, &msgs[i]);
          i++;
        }
      else
        {
          ret = rk3576_i2c_write_msg(priv, &msgs[i]);
          i++;
        }

      if (ret < 0)
        {
          break;
        }
    }

  /* 无论成败都要发停止条件，否则总线保持被占用状态。 */

  rk3576_i2c_stop(priv);
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_i2cbus_initialize
 *
 * Description:
 *   初始化并返回一条 I2C 总线。
 *
 * Input Parameters:
 *   port - 总线号。当前只支持 2（板上 RTC 与 Type-C PD 挂在这条上）。
 *
 * Returned Value:
 *   成功返回总线句柄；端口不支持或寄存器块无响应返回 NULL。
 *
 ****************************************************************************/

struct i2c_master_s *rk3576_i2cbus_initialize(int port)
{
  const struct rk3576_i2c_config_s *cfg = NULL;
  struct rk3576_i2c_priv_s *priv = NULL;
  uint32_t clkdiv;
  uint32_t readback;
  uint32_t div_half;
  unsigned int sel;
  int i;

  for (i = 0; i < RK3576_I2C_NBUSES; i++)
    {
      if (g_i2c_config[i].port == port)
        {
          cfg  = &g_i2c_config[i];
          priv = &g_i2c_priv[i];
          break;
        }
    }

  if (cfg == NULL)
    {
      syslog(LOG_ERR, "ERROR: 不支持的 I2C 端口 %d\n", port);
      return NULL;
    }

  if (priv->initialized)
    {
      return (struct i2c_master_s *)priv;
    }

  priv->cfg  = cfg;
  priv->port = cfg->port;
  priv->base = cfg->base;

  /* 1) 开时钟。PCLK 供寄存器访问，CLK 驱动 SCL —— 两者独立，
   *    只开 PCLK 的话寄存器读写正常但总线不动，这正是本端口踩过的坑。
   */

  if (cfg->pmu)
    {
      rk3576_pmu_clk_gate(cfg->gate_con, cfg->gate_pclk, true);
      rk3576_pmu_clk_gate(cfg->gate_con, cfg->gate_clk, true);
    }
  else
    {
      rk3576_clk_gate(cfg->gate_con, cfg->gate_pclk, true);
      rk3576_clk_gate(cfg->gate_con, cfg->gate_clk, true);
    }

  /* 2) 配引脚复用。cfg 里填 -1 表示沿用引导器留下的配置。 */

  if (cfg->scl_bank >= 0)
    {
      /* 切成 GPIO 输入读一次空闲电平，再配成 I2C 功能。
       *
       * 板子 dts 用 pcfg-pull-none-smt（禁内部上下拉），依赖板上外部
       * 上拉。健康的 I2C 总线空闲时 SCL/SDA 都应为高。读到 0 说明
       * 没有上拉或引脚根本没接到总线上 —— 那样的话应答位会恒被读成
       * ACK，表现为"每个地址都有响应"。
       */

      rk3576_pinmux_set(cfg->scl_bank, cfg->scl_pin, RK3576_PINMUX_GPIO);
      rk3576_pinmux_set(cfg->sda_bank, cfg->sda_pin, RK3576_PINMUX_GPIO);
      rk3576_gpio_setdir(cfg->scl_bank, cfg->scl_pin, false);
      rk3576_gpio_setdir(cfg->sda_bank, cfg->sda_pin, false);
      up_udelay(100);
      syslog(LOG_INFO, "I2C%d: 空闲电平 SCL=%d SDA=%d（应均为 1）\n",
             port,
             rk3576_gpio_read(cfg->scl_bank, cfg->scl_pin),
             rk3576_gpio_read(cfg->sda_bank, cfg->sda_pin));

      rk3576_pinmux_set(cfg->scl_bank, cfg->scl_pin, cfg->pin_func);
      rk3576_pinmux_set(cfg->sda_bank, cfg->sda_pin, cfg->pin_func);
    }

  /* 3) 读出实际时钟源再算分频，不做假设。
   *
   *    SCL = src / (8 * (divl + 1 + divh + 1))，取 divl = divh。
   */

  sel = cfg->pmu ?
        rk3576_pmu_clk_getmux(cfg->sel_con, cfg->sel_shift, 2) :
        rk3576_clk_getmux(cfg->sel_con, cfg->sel_shift, 2);
  priv->src_hz = g_i2c_src_hz[sel];

  div_half = priv->src_hz / (8 * I2C_TARGET_SCL_HZ) / 2;
  if (div_half < 1)
    {
      div_half = 1;
    }

  clkdiv = ((div_half - 1) << 16) | (div_half - 1);

  /* 4) 自检：写 CLKDIV 再读回。读回一致说明 PCLK 已使能、基址正确、
   *    MMU 已映射。注意这只验证 PCLK，不验证功能时钟。
   */

  i2c_putreg(priv, RK3576_I2C_CLKDIV, clkdiv);
  readback = i2c_getreg(priv, RK3576_I2C_CLKDIV);

  if (readback != clkdiv)
    {
      syslog(LOG_ERR,
             "ERROR: I2C%d CLKDIV 写 0x%08" PRIx32 " 读回 0x%08" PRIx32
             " —— 寄存器块无响应\n", port, clkdiv, readback);
      return NULL;
    }

  /* CON[24:16] 是只读版本字段，顺带打出来佐证读到的确实是 I2C 控制器。
   * 引脚复用读回值可用来核对第 2 步是否生效。
   */

  syslog(LOG_INFO,
         "I2C%d: 版本 %" PRIu32 " 源 %" PRIu32 "MHz SCL≈%" PRIu32 "kHz"
         " mux=%u%s\n",
         port, (i2c_getreg(priv, RK3576_I2C_CON) >> 16) & 0x1ff,
         priv->src_hz / 1000000,
         priv->src_hz / (8 * 2 * div_half) / 1000, sel,
         cfg->scl_bank >= 0 ? "" : "（复用沿用 U-Boot）");

  if (cfg->scl_bank >= 0)
    {
      syslog(LOG_INFO, "I2C%d: 复用 SCL=gpio%d-%d→%d SDA=gpio%d-%d→%d\n",
             port, cfg->scl_bank, cfg->scl_pin,
             rk3576_pinmux_get(cfg->scl_bank, cfg->scl_pin),
             cfg->sda_bank, cfg->sda_pin,
             rk3576_pinmux_get(cfg->sda_bank, cfg->sda_pin));
    }

  i2c_putreg(priv, RK3576_I2C_IEN, 0);              /* 轮询式，不用中断 */
  i2c_putreg(priv, RK3576_I2C_IPD, I2C_INT_ALL);    /* 清残留状态       */
  i2c_putreg(priv, RK3576_I2C_CON, 0);

  priv->initialized = true;
  return (struct i2c_master_s *)priv;
}

/****************************************************************************
 * Name: rk3576_i2cbus_uninitialize
 ****************************************************************************/

int rk3576_i2cbus_uninitialize(struct i2c_master_s *dev)
{
  struct rk3576_i2c_priv_s *priv = (struct rk3576_i2c_priv_s *)dev;

  if (priv == NULL)
    {
      return -EINVAL;
    }

  i2c_putreg(priv, RK3576_I2C_CON, 0);
  priv->initialized = false;
  return OK;
}

#endif /* CONFIG_RK3576_I2C */
