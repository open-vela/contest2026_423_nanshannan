/****************************************************************************
 * chip/rk3576/rk3576_spi.c
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

/* RK3576 SPI 主机驱动（轮询式）。
 *
 * ★ 只做 SPI4
 *
 *   板上四个通用 SPI 控制器在基础 dtb 里全是 disabled，只有 SPI4 被
 *   厂商的 40 针扩展 dtsi 使能并引到排针上（走 m2 复用组）。其余三个
 *   没有对外引脚，实现了也无从验证，所以配置表里只放 SPI4。
 *
 * ★ 片选用 GPIO 而不是控制器的原生 CS
 *
 *   厂商 dtsi 写的是 cs-gpios = <&gpio4 RK_PA5>，即把 SPI4_CSN1_M2 那
 *   根线当普通 GPIO 用。这么做不是多此一举：NuttX 的 SPI 接口把片选
 *   拆成独立的 select()/deselect() 调用，语义是"整个事务期间保持有效"，
 *   而原生 CS 的有效区间由控制器按帧决定，跨多次 send()/exchange() 时
 *   会在中间抬起来。用 GPIO 才能对上 NuttX 的语义。
 *
 *   ★ 但 SER 仍然要置位。SER=0 时控制器认为没有选中任何从机，不产生
 *     时钟。内核驱动在 cs_gpiod 分支里照样写 SER 的 bit0，就是这个原因。
 *     只用 GPIO 拉低 CS 而忘了 SER，现象是波形上 CS 有效但 SCLK 不动。
 *
 * ★ 为什么是轮询
 *
 *   与 rk3576_i2c.c 同样的取舍：先把总线跑通，少一个 GIC 挂接的出错
 *   环节。FIFO 有 64 项，8 位帧下一次能吞 64 字节，短事务几乎不需要
 *   等待；长传输才会在循环里空转。有性能需求时再加中断/DMA。
 *
 * ★ 分频必须是偶数
 *
 *   BAUDR 的硬件只认偶数分频。向上取偶保证实际频率不高于请求值 ——
 *   方向是安全的：从机都规定 SCLK 上限，低于上限一律工作。
 *
 * 寄存器布局与配置流程的出处是内核 drivers/spi/spi-rockchip.c；
 * 引脚与时钟的出处是 RK3576 TRM 与 clk-rk3576.c，逐条记在下面。
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <debug.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/mutex.h>
#include <nuttx/spi/spi.h>

#include "arm64_internal.h"
#include "rk3576_cru.h"
#include "rk3576_gpio.h"
#include "rk3576_pinmux.h"
#include "rk3576_spi.h"
#include "hardware/rk3576_spi.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 等待 BUSY 清除的上限。按最慢的 SCLK（源 24MHz / 65534）算，一帧
 * 也只要几百微秒；给到 100ms 是纯粹的兜底，正常路径永远走不到。
 */

#define SPI_BUSY_TIMEOUT_US   100000

/* 收发循环里"毫无进展"的连续轮数上限，每轮等 1us。
 * 按最慢的 SCLK 算一帧也用不了 1ms，10 万轮是纯兜底。
 */

#define SPI_XFER_STALL_LIMIT  100000

/* 默认参数，与厂商 dtsi 里 spidev 的 spi-max-frequency 一致 */

#define SPI_DEFAULT_FREQ_HZ   24000000
#define SPI_DEFAULT_NBITS     8

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_spi_config_s
{
  uint8_t   port;
  uintptr_t base;
  uint8_t   gate_con;      /* CLKGATE_CON 序号（PCLK 与 CLK 同属一个） */
  uint8_t   gate_pclk;     /* PCLK_SPIn 的位号 */
  uint8_t   gate_clk;      /* CLK_SPIn 的位号  */
  uint8_t   sel_con;       /* CLKSEL_CON 序号  */
  uint8_t   sel_shift;     /* 时钟源位域起始位 */
  int8_t    clk_bank;      /* SCLK 引脚 */
  int8_t    clk_pin;
  int8_t    mosi_bank;
  int8_t    mosi_pin;
  int8_t    miso_bank;
  int8_t    miso_pin;
  uint8_t   pin_func;      /* 三根信号线的复用功能号相同 */
  int8_t    cs_bank;       /* 片选，当普通 GPIO 用 */
  int8_t    cs_pin;
};

struct rk3576_spi_priv_s
{
  const struct spi_ops_s            *ops;   /* 必须是第一个成员 */
  const struct rk3576_spi_config_s  *cfg;
  uintptr_t                          base;
  uint32_t                           src_hz;    /* 实测的输入时钟频率 */
  uint32_t                           fifo_len;  /* 由 VERSION 推出 */
  uint32_t                           frequency; /* 当前请求的 SCLK */
  uint32_t                           actual;    /* 实际得到的 SCLK */
  uint8_t                            nbits;
  uint8_t                            mode;
  mutex_t                            lock;
  bool                               initialized;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int      spi_lock(struct spi_dev_s *dev, bool lock);
static void     spi_select(struct spi_dev_s *dev, uint32_t devid,
                           bool selected);
static uint32_t spi_setfrequency(struct spi_dev_s *dev, uint32_t frequency);
static void     spi_setmode(struct spi_dev_s *dev, enum spi_mode_e mode);
static void     spi_setbits(struct spi_dev_s *dev, int nbits);
static uint8_t  spi_status(struct spi_dev_s *dev, uint32_t devid);
static uint32_t spi_send(struct spi_dev_s *dev, uint32_t wd);
#ifdef CONFIG_SPI_EXCHANGE
static void     spi_exchange(struct spi_dev_s *dev, const void *txbuffer,
                             void *rxbuffer, size_t nwords);
#else
static void     spi_sndblock(struct spi_dev_s *dev, const void *buffer,
                             size_t nwords);
static void     spi_recvblock(struct spi_dev_s *dev, void *buffer,
                              size_t nwords);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 时钟源父表。clk-rk3576.c 里 CLK_SPI4 用的是
 *   mux_200m_150m_100m_24m_p = { 200M, 150M, 100M, 24M }
 * 与 I2C 的父表(200/100/50/24)不同，别抄错。
 */

static const uint32_t g_spi_src_hz[4] =
{
  200000000, 150000000, 100000000, 24000000
};

static const struct rk3576_spi_config_s g_spi_config[] =
{
  /* SPI4 —— 40 针扩展口。
   *
   * 出处 1（引脚）：RK3576 TRM 的 IOMUX 表，func 9 时
   *   GPIO4_B0 = SPI4_CLK_M2    (bank4 pin 8)
   *   GPIO4_B1 = SPI4_MOSI_M2   (bank4 pin 9)
   *   GPIO4_B2 = SPI4_MISO_M2   (bank4 pin 10)
   *   GPIO4_A5 = SPI4_CSN1_M2   (bank4 pin 5) —— 这里当 GPIO 用
   * 与 dtb 里 spi4m2-pins 的 <4 8 9>,<4 10 9>,<4 9 9> 逐条对得上
   * （dtsi 组内顺序是 clk/miso/mosi，不是 clk/mosi/miso，容易看反）。
   *
   * 出处 2（时钟）：clk-rk3576.c
   *   GATE(PCLK_SPI4, "pclk_spi4", ... CLKGATE_CON(16), 1)
   *   COMPOSITE_NODIV(CLK_SPI4, ... CLKSEL_CON(71), 6, 2,
   *                                 CLKGATE_CON(16), 6)
   */

  {
    .port       = 4,
    .base       = 0x2ad20000,
    .gate_con   = 16,
    .gate_pclk  = 1,
    .gate_clk   = 6,
    .sel_con    = 71,
    .sel_shift  = 6,
    .clk_bank   = 4, .clk_pin  = 8,
    .mosi_bank  = 4, .mosi_pin = 9,
    .miso_bank  = 4, .miso_pin = 10,
    .pin_func   = 9,
    .cs_bank    = 4, .cs_pin   = 5,
  },
};

#define RK3576_NSPI (sizeof(g_spi_config) / sizeof(g_spi_config[0]))

static const struct spi_ops_s g_spi_ops =
{
  .lock         = spi_lock,
  .select       = spi_select,
  .setfrequency = spi_setfrequency,
  .setmode      = spi_setmode,
  .setbits      = spi_setbits,
  .status       = spi_status,
  .send         = spi_send,
#ifdef CONFIG_SPI_EXCHANGE
  .exchange     = spi_exchange,
#else
  .sndblock     = spi_sndblock,
  .recvblock    = spi_recvblock,
#endif
};

static struct rk3576_spi_priv_s g_spi_priv[RK3576_NSPI] =
{
  {
    .ops         = &g_spi_ops,
    .lock        = NXMUTEX_INITIALIZER,
    .initialized = false,
  },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t spi_getreg(struct rk3576_spi_priv_s *priv,
                                  unsigned int offset)
{
  return getreg32(priv->base + offset);
}

static inline void spi_putreg(struct rk3576_spi_priv_s *priv,
                              unsigned int offset, uint32_t value)
{
  putreg32(value, priv->base + offset);
}

/****************************************************************************
 * Name: spi_enable
 *
 * Description:
 *   开关控制器。CTRLR0 与 BAUDR 只有在关闭状态下写才生效，关闭同时会
 *   清空收发 FIFO —— 每次传输前后都关一次，可以保证不会有上一次事务
 *   的残留数据混进来。
 *
 ****************************************************************************/

static void spi_enable(struct rk3576_spi_priv_s *priv, bool enable)
{
  spi_putreg(priv, RK3576_SPI_SSIENR, enable ? 1 : 0);
}

/****************************************************************************
 * Name: spi_wait_idle
 *
 * Description:
 *   等 BUSY 清除。返回 false 表示超时 —— 只可能是时钟没跑起来，
 *   继续下去只会读到一堆 0，所以调用方要把它当错误看待。
 *
 ****************************************************************************/

static bool spi_wait_idle(struct rk3576_spi_priv_s *priv)
{
  unsigned int elapsed;

  for (elapsed = 0; elapsed < SPI_BUSY_TIMEOUT_US; elapsed++)
    {
      if ((spi_getreg(priv, RK3576_SPI_SR) & SPI_SR_BUSY) == 0)
        {
          return true;
        }

      up_udelay(1);
    }

  spierr("ERROR: SPI%d BUSY 一直不清，SR=0x%08" PRIx32 "\n",
         priv->cfg->port, spi_getreg(priv, RK3576_SPI_SR));
  return false;
}

/****************************************************************************
 * Name: spi_setup_ctrlr0
 *
 * Description:
 *   按当前的 mode/nbits 拼出 CTRLR0 并写入。必须在控制器关闭时调用。
 *
 *   固定位的取值照搬内核驱动：帧格式 SPI、APB 侧按字节访问、CS 到
 *   SCLK 间隔一个周期、大端。RSD（采样延迟）保持 0：厂商 dtsi 没有给
 *   rx-sample-delay-ns，说明这块板子不需要补偿。
 *
 ****************************************************************************/

static void spi_setup_ctrlr0(struct rk3576_spi_priv_s *priv)
{
  uint32_t cr0;

  cr0 = (SPI_CR0_FRF_SPI  << SPI_CR0_FRF_SHIFT)  |
        (SPI_CR0_BHT_8BIT << SPI_CR0_BHT_SHIFT)  |
        (SPI_CR0_SSD_ONE  << SPI_CR0_SSD_SHIFT)  |
        (SPI_CR0_EM_BIG   << SPI_CR0_EM_SHIFT)   |
        (SPI_CR0_OPM_MASTER << SPI_CR0_OPM_SHIFT) |
        (SPI_CR0_CSM_KEEP << SPI_CR0_CSM_SHIFT)  |
        (SPI_CR0_XFM_TR   << SPI_CR0_XFM_SHIFT);

  /* NuttX 的 mode 枚举与硬件的 SCPH/SCPOL 位序一致：
   *   MODE0=0 -> SCPH=0 SCPOL=0 ... MODE3=3 -> SCPH=1 SCPOL=1
   * 所以低两位直接搬过去即可。
   */

  cr0 |= ((uint32_t)priv->mode & 3) << SPI_CR0_SCPH_SHIFT;

  if (priv->nbits == 16)
    {
      cr0 |= SPI_CR0_DFS_16BIT << SPI_CR0_DFS_SHIFT;
    }
  else if (priv->nbits == 4)
    {
      cr0 |= SPI_CR0_DFS_4BIT << SPI_CR0_DFS_SHIFT;
    }
  else
    {
      cr0 |= SPI_CR0_DFS_8BIT << SPI_CR0_DFS_SHIFT;
    }

  spi_putreg(priv, RK3576_SPI_CTRLR0, cr0);
}

/****************************************************************************
 * Name: spi_transfer
 *
 * Description:
 *   全双工搬运 nwords 帧。txbuffer 为 NULL 时发 0xff，rxbuffer 为
 *   NULL 时把收到的丢掉。
 *
 *   ★ 为什么不用只发(TO)/只收(RO)模式
 *
 *     两种单向模式各有额外约束：RO 的帧数由 CTRLR1 决定而不是由你压
 *     进去多少，TO 在轮询下要另外处理 RX FIFO 溢出。统一走全双工、
 *     压多少帧就取多少帧，只有一条路径要验证。代价是收发都占 FIFO，
 *     在这个速率下无所谓。
 *
 *   ★ 在途帧数必须卡住
 *
 *     压进去还没取走的帧会堆在 RX FIFO 里。只看 TX 有没有空位就继续
 *     压，RX 满了之后新到的帧会被丢掉，表现为读回的数据错位 —— 而且
 *     不报错。所以每轮压入量同时受三个上限约束：TX 空位、RX 剩余
 *     容量、剩余帧数。
 *
 ****************************************************************************/

static void spi_transfer(struct rk3576_spi_priv_s *priv,
                         const void *txbuffer, void *rxbuffer,
                         size_t nwords)
{
  const uint8_t  *tx8  = txbuffer;
  const uint16_t *tx16 = txbuffer;
  uint8_t        *rx8  = rxbuffer;
  uint16_t       *rx16 = rxbuffer;
  bool     wide    = (priv->nbits > 8);
  size_t   to_push = nwords;
  size_t   to_pop  = nwords;
  size_t   inflight;
  uint32_t room;
  uint32_t stall;

  if (nwords == 0)
    {
      return;
    }

  spi_enable(priv, false);
  spi_setup_ctrlr0(priv);
  spi_putreg(priv, RK3576_SPI_CTRLR1, nwords - 1);
  spi_putreg(priv, RK3576_SPI_IMR, 0);
  spi_putreg(priv, RK3576_SPI_DMACR, 0);
  spi_enable(priv, true);

  stall = 0;

  while (to_pop > 0)
    {
      size_t before = to_pop;

      /* 压入：三个上限取最小 */

      inflight = to_pop - to_push;             /* 已压未取 */
      room     = priv->fifo_len -
                 spi_getreg(priv, RK3576_SPI_TXFLR);

      if (room > priv->fifo_len - inflight)
        {
          room = priv->fifo_len - inflight;
        }

      while (room > 0 && to_push > 0)
        {
          uint32_t wd;

          if (wide)
            {
              wd = tx16 ? *tx16++ : 0xffff;
            }
          else
            {
              wd = tx8 ? *tx8++ : 0xff;
            }

          spi_putreg(priv, RK3576_SPI_TXDR, wd);
          to_push--;
          room--;
        }

      /* 取出：FIFO 里有多少取多少 */

      while (spi_getreg(priv, RK3576_SPI_RXFLR) > 0 && to_pop > 0)
        {
          uint32_t rd = spi_getreg(priv, RK3576_SPI_RXDR);

          if (wide)
            {
              if (rx16)
                {
                  *rx16++ = (uint16_t)rd;
                }
            }
          else
            {
              if (rx8)
                {
                  *rx8++ = (uint8_t)rd;
                }
            }

          to_pop--;
        }

      /* ★ 循环必须有出口。
       *
       *   若控制器不产生时钟（时钟没使能、模块仍在复位、SER 没置位），
       *   TX FIFO 压满之后 RX 永远是空的，to_pop 不再减少 —— 这个循环
       *   会一直转下去，占着 CPU 不放，现象是整块板子失去响应而不是
       *   报错。调试期正是最容易撞上这种配置的时候，不能靠"配置对了
       *   就不会发生"来免责。
       *
       *   只在一轮下来毫无进展时才累加，有进展就清零：长传输本来就要
       *   转很多轮，按总轮数设限会误伤。
       */

      if (to_pop == before)
        {
          if (++stall > SPI_XFER_STALL_LIMIT)
            {
              spierr("ERROR: SPI%d 传输停滞，剩 %zu 帧未收 "
                     "(SR=0x%08" PRIx32 " TXFLR=%" PRIu32
                     " RXFLR=%" PRIu32 ")\n",
                     priv->cfg->port, to_pop,
                     spi_getreg(priv, RK3576_SPI_SR),
                     spi_getreg(priv, RK3576_SPI_TXFLR),
                     spi_getreg(priv, RK3576_SPI_RXFLR));
              break;
            }

          up_udelay(1);
        }
      else
        {
          stall = 0;
        }
    }

  spi_wait_idle(priv);
  spi_enable(priv, false);
}

/****************************************************************************
 * Name: spi_lock
 ****************************************************************************/

static int spi_lock(struct spi_dev_s *dev, bool lock)
{
  struct rk3576_spi_priv_s *priv = (struct rk3576_spi_priv_s *)dev;

  return lock ? nxmutex_lock(&priv->lock) : nxmutex_unlock(&priv->lock);
}

/****************************************************************************
 * Name: spi_select
 *
 * Description:
 *   片选。GPIO 拉低表示选中（厂商 dtsi 是 GPIO_ACTIVE_LOW），同时置位
 *   SER 的 bit0 —— 少了它控制器不产生时钟，见文件头说明。
 *
 ****************************************************************************/

static void spi_select(struct spi_dev_s *dev, uint32_t devid, bool selected)
{
  struct rk3576_spi_priv_s *priv = (struct rk3576_spi_priv_s *)dev;
  const struct rk3576_spi_config_s *cfg = priv->cfg;

  if (selected)
    {
      spi_putreg(priv, RK3576_SPI_SER, 1);
      rk3576_gpio_write(cfg->cs_bank, cfg->cs_pin, false);
    }
  else
    {
      rk3576_gpio_write(cfg->cs_bank, cfg->cs_pin, true);
      spi_putreg(priv, RK3576_SPI_SER, 0);
    }
}

/****************************************************************************
 * Name: spi_setfrequency
 *
 * Description:
 *   设置 SCLK。分频只能是偶数，向上取偶保证实际值不超过请求值。
 *
 * Returned Value:
 *   实际得到的频率。调用方拿它去核对，不要假设等于请求值。
 *
 ****************************************************************************/

static uint32_t spi_setfrequency(struct spi_dev_s *dev, uint32_t frequency)
{
  struct rk3576_spi_priv_s *priv = (struct rk3576_spi_priv_s *)dev;
  uint32_t div;

  if (frequency == priv->frequency)
    {
      return priv->actual;
    }

  if (frequency > SPI_MAX_SCLK_HZ)
    {
      frequency = SPI_MAX_SCLK_HZ;
    }

  /* 2 * ceil(src / (2 * f))：先按半频向上取整再乘 2，结果一定是偶数
   * 且分出来的频率 <= f。
   */

  div = 2 * ((priv->src_hz + 2 * frequency - 1) / (2 * frequency));

  if (div < SPI_BAUDR_MIN)
    {
      div = SPI_BAUDR_MIN;
    }
  else if (div > SPI_BAUDR_MAX)
    {
      div = SPI_BAUDR_MAX;
    }

  spi_enable(priv, false);
  spi_putreg(priv, RK3576_SPI_BAUDR, div);

  priv->frequency = frequency;
  priv->actual    = priv->src_hz / div;

  spiinfo("SPI%d: 请求 %" PRIu32 "Hz 分频 %" PRIu32 " 实际 %" PRIu32 "Hz\n",
          priv->cfg->port, frequency, div, priv->actual);

  return priv->actual;
}

/****************************************************************************
 * Name: spi_setmode
 ****************************************************************************/

static void spi_setmode(struct spi_dev_s *dev, enum spi_mode_e mode)
{
  struct rk3576_spi_priv_s *priv = (struct rk3576_spi_priv_s *)dev;

  /* 只记下来，真正写 CTRLR0 是在传输前 —— 那时控制器一定是关闭的，
   * 写才会生效。
   */

  priv->mode = (uint8_t)mode;
}

/****************************************************************************
 * Name: spi_setbits
 ****************************************************************************/

static void spi_setbits(struct spi_dev_s *dev, int nbits)
{
  struct rk3576_spi_priv_s *priv = (struct rk3576_spi_priv_s *)dev;

  if (nbits != 4 && nbits != 8 && nbits != 16)
    {
      spierr("ERROR: SPI%d 不支持 %d 位帧，保持 %d 位\n",
             priv->cfg->port, nbits, priv->nbits);
      return;
    }

  priv->nbits = (uint8_t)nbits;
}

/****************************************************************************
 * Name: spi_status
 ****************************************************************************/

static uint8_t spi_status(struct spi_dev_s *dev, uint32_t devid)
{
  /* 排针上接什么由用户决定，驱动无从判断有没有卡/设备在位 */

  return 0;
}

/****************************************************************************
 * Name: spi_send
 ****************************************************************************/

static uint32_t spi_send(struct spi_dev_s *dev, uint32_t wd)
{
  struct rk3576_spi_priv_s *priv = (struct rk3576_spi_priv_s *)dev;
  uint16_t tx16 = (uint16_t)wd;
  uint16_t rx16 = 0;
  uint8_t  tx8  = (uint8_t)wd;
  uint8_t  rx8  = 0;

  if (priv->nbits > 8)
    {
      spi_transfer(priv, &tx16, &rx16, 1);
      return rx16;
    }

  spi_transfer(priv, &tx8, &rx8, 1);
  return rx8;
}

/****************************************************************************
 * Name: spi_exchange / spi_sndblock / spi_recvblock
 ****************************************************************************/

#ifdef CONFIG_SPI_EXCHANGE
static void spi_exchange(struct spi_dev_s *dev, const void *txbuffer,
                         void *rxbuffer, size_t nwords)
{
  spi_transfer((struct rk3576_spi_priv_s *)dev, txbuffer, rxbuffer, nwords);
}
#else
static void spi_sndblock(struct spi_dev_s *dev, const void *buffer,
                         size_t nwords)
{
  spi_transfer((struct rk3576_spi_priv_s *)dev, buffer, NULL, nwords);
}

static void spi_recvblock(struct spi_dev_s *dev, void *buffer, size_t nwords)
{
  spi_transfer((struct rk3576_spi_priv_s *)dev, NULL, buffer, nwords);
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_spibus_initialize
 ****************************************************************************/

struct spi_dev_s *rk3576_spibus_initialize(int port)
{
  const struct rk3576_spi_config_s *cfg = NULL;
  struct rk3576_spi_priv_s *priv = NULL;
  uint32_t version;
  uint32_t readback;
  unsigned int sel;
  int i;

  for (i = 0; i < RK3576_NSPI; i++)
    {
      if (g_spi_config[i].port == port)
        {
          cfg  = &g_spi_config[i];
          priv = &g_spi_priv[i];
          break;
        }
    }

  if (cfg == NULL)
    {
      spierr("ERROR: SPI%d 不在支持列表里（当前只有 SPI4 引到排针）\n",
             port);
      return NULL;
    }

  if (priv->initialized)
    {
      return (struct spi_dev_s *)priv;
    }

  priv->cfg  = cfg;
  priv->base = cfg->base;

  /* 1) 时钟：PCLK 和功能时钟同属一个 CLKGATE_CON */

  rk3576_clk_gate(cfg->gate_con, cfg->gate_pclk, true);
  rk3576_clk_gate(cfg->gate_con, cfg->gate_clk, true);

  /* ★ 不撤销模块复位。
   *
   *   一度怀疑 SPI4 上电后压在复位里（它在基础 dtb 里是 disabled，
   *   U-Boot 从没碰过），加过 rk3576_reset() 撤复位。撤掉了，原因有二：
   *
   *   1. 当初的依据不成立 —— "SPI4 没出现"那次烧的是过期镜像，
   *      里面根本没有这个驱动，那个观测证明不了任何事。
   *   2. dt-bindings 的 SRST_* 是**连续序号**（…95, 96, 97, 98…），
   *      是查表索引；而 rk3576_reset() 按 id/16、id%16 当成
   *      "寄存器号 × 16 + 位号"来算。两者对不上时，复位的就是
   *      别的模块 —— 不报错，后果还未必立刻显现。
   *
   *   要用复位，得先从厂商的 rk3576_rst_init() 拿到真正的映射表。
   */

  /* 2) 引脚复用。CS 那根走 GPIO：先拉高（不选中）再设成输出，
   *    顺序反了会在设成输出的一瞬间输出上一次的残留电平，
   *    对从机就是一次伪片选。
   */

  rk3576_pinmux_set(cfg->clk_bank,  cfg->clk_pin,  cfg->pin_func);
  rk3576_pinmux_set(cfg->mosi_bank, cfg->mosi_pin, cfg->pin_func);
  rk3576_pinmux_set(cfg->miso_bank, cfg->miso_pin, cfg->pin_func);

  /* 上拉。厂商 pinctrl 给 spi4m2 这一组配的是 pcfg_pull_up_drv_level_1，
   * 照做。对 MISO 尤其有意义：排针上没接从机时它是悬空的，没有上拉就
   * 随噪声浮动，读回来的值不稳定 —— 排查时会把"没接线"误当成"数据错"。
   * 有上拉，空载读回的是稳定的 0xff，一眼能认出是空载。
   */

  rk3576_pinmux_setpull(cfg->clk_bank,  cfg->clk_pin,  RK3576_PULL_UP);
  rk3576_pinmux_setpull(cfg->mosi_bank, cfg->mosi_pin, RK3576_PULL_UP);
  rk3576_pinmux_setpull(cfg->miso_bank, cfg->miso_pin, RK3576_PULL_UP);

  rk3576_gpio_write(cfg->cs_bank, cfg->cs_pin, true);
  rk3576_pinmux_set(cfg->cs_bank, cfg->cs_pin, 0);
  rk3576_gpio_setdir(cfg->cs_bank, cfg->cs_pin, true);
  rk3576_gpio_write(cfg->cs_bank, cfg->cs_pin, true);

  /* 3) 读时钟源。父表与 I2C 的不同，见 g_spi_src_hz 的注释 */

  sel = rk3576_clk_getmux(cfg->sel_con, cfg->sel_shift, 2);
  priv->src_hz = g_spi_src_hz[sel];

  /* 4) 自检：控制器必须先关掉，BAUDR 在使能状态下写不进去。
   *    写一个值再读回，一致说明 PCLK 已使能、基址正确、MMU 已映射。
   */

  spi_enable(priv, false);
  spi_putreg(priv, RK3576_SPI_BAUDR, 8);
  readback = spi_getreg(priv, RK3576_SPI_BAUDR);

  if (readback != 8)
    {
      /* 只读的 VERSION 一并报出来，让这条日志能区分两种情况：
       *   VERSION 也是 0  -> 整个寄存器块没响应（基址错 / 时钟没开 /
       *                      仍在复位），写和读都到不了硬件。
       *   VERSION 有值    -> 块是活的，只是 BAUDR 没写进去 —— 那就是
       *                      写入时机不对（控制器没关干净）。
       * 少了这一个读数，两种情况的现象一模一样，只能靠猜。
       */

      spierr("ERROR: SPI%d BAUDR 写 8 读回 0x%08" PRIx32
             "，VERSION=0x%08" PRIx32 "\n",
             port, readback, spi_getreg(priv, RK3576_SPI_VERSION));
      return NULL;
    }

  /* 5) FIFO 深度由版本号决定，不能写死 */

  version = spi_getreg(priv, RK3576_SPI_VERSION);
  priv->fifo_len = (version == SPI_VER2_TYPE1 || version == SPI_VER2_TYPE2) ?
                   SPI_FIFO_LEN_V2 : SPI_FIFO_LEN_V1;

  /* 6) 默认参数 */

  priv->mode      = SPIDEV_MODE0;
  priv->nbits     = SPI_DEFAULT_NBITS;
  priv->frequency = 0;
  spi_setfrequency((struct spi_dev_s *)priv, SPI_DEFAULT_FREQ_HZ);

  spi_putreg(priv, RK3576_SPI_SER, 0);
  spi_putreg(priv, RK3576_SPI_IMR, 0);
  spi_putreg(priv, RK3576_SPI_ICR, 0xffffffff);

  priv->initialized = true;

  /* 上下拉读回来核对。写进去不等于生效：偏移算错时写入不报错，
   * 而现象只是"MISO 空载读到 0 而不是 1"，很容易被当成没接线放过去。
   */

  syslog(LOG_INFO,
         "SPI%d: 版本 0x%08" PRIx32 " FIFO %" PRIu32 " 源 %" PRIu32
         "MHz SCLK %" PRIu32 "Hz 上拉(clk/mosi/miso)=%d/%d/%d\n",
         port, version, priv->fifo_len, priv->src_hz / 1000000,
         priv->actual,
         rk3576_pinmux_getpull(cfg->clk_bank,  cfg->clk_pin),
         rk3576_pinmux_getpull(cfg->mosi_bank, cfg->mosi_pin),
         rk3576_pinmux_getpull(cfg->miso_bank, cfg->miso_pin));

  return (struct spi_dev_s *)priv;
}
