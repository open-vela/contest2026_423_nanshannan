/****************************************************************************
 * chip/rk3576/rk3576_sdhci.c
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

/* RK3576 eMMC (dwcmshc / SD Host Controller 标准)。
 *
 * 分两层：
 *   1. 底层寄存器操作 + rk3576_sdhci_probe()，先前已在板上验证过
 *      命令通路（CMD0/1/2/3）与数据通路（CMD9/7/17，块0 读出 MBR 签名）。
 *   2. sdio_dev_s 实现，交给 drivers/mmcsd 上层注册 /dev/mmcsd0。
 *
 * ★ 轮询式，不使用中断。
 *
 *   上层的调用时序是：
 *       BLOCKSETUP → WAITENABLE → RECVSETUP → SENDCMD → RECVR1 → EVENTWAIT
 *   中断式实现里 EVENTWAIT 等信号量、由 ISR 搬数据；这里把数据搬运放在
 *   EVENTWAIT 内部同步完成，返回时事务已结束。少一个中断挂接环节，
 *   出错面更小；代价是传输期间占着 CPU。
 *
 *   PIO 而非 DMA 也是同样的取舍：DMA 要处理地址对齐、cache 一致性和
 *   描述符表。先把通路做对，提速留到后面换 ADMA2。
 *
 * ★ 不配置电源域与时钟：板上实测 U-Boot 交接后 CAP0 的基准时钟字段为
 *   200MHz，与 dtsi 的 assigned-clock-rates 一致，说明 PD_NVM 与五路时钟
 *   都是活的。与 GPIO/I2C 同样的取舍 —— 先用引导器留下的状态，
 *   需要时再补。
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <syslog.h>

#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/sdio.h>
#include <nuttx/mmcsd.h>
#include <nuttx/wqueue.h>

#include "arm64_internal.h"
#include "rk3576_sdhci.h"
#include "hardware/rk3576_sdhci.h"
#include "hardware/rk3576_memorymap.h"

#ifdef CONFIG_RK3576_SDHCI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SDHCI_BASE          RK3576_SDHCI_ADDR
#define SDHCI_CMD_TIMEOUT_US   100000
#define SDHCI_RESET_TIMEOUT_US 100000

/* 识别阶段的卡时钟。eMMC 规范要求上电识别期间不超过 400kHz。
 * 分频值 = 基准时钟 / (2 * 目标频率)，写在 CLKCTRL 的 [15:8]+[7:6]。
 */

#define SDHCI_ID_CLOCK_HZ   400000

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t sdhci_getreg(uint32_t off)
{
  return getreg32(SDHCI_BASE + off);
}

static inline void sdhci_putreg(uint32_t off, uint32_t val)
{
  putreg32(val, SDHCI_BASE + off);
}

/****************************************************************************
 * Name: sdhci_reset
 *
 * Description:
 *   软复位。复位位是自清的：写 1 后硬件在完成时清零，因此要轮询等它归零。
 *
 ****************************************************************************/

static int sdhci_reset(uint32_t mask)
{
  uint32_t regval;
  int us;

  regval = sdhci_getreg(RK3576_SDHCI_CLKCTRL);
  sdhci_putreg(RK3576_SDHCI_CLKCTRL, regval | mask);

  for (us = 0; us < SDHCI_RESET_TIMEOUT_US; us++)
    {
      if ((sdhci_getreg(RK3576_SDHCI_CLKCTRL) & mask) == 0)
        {
          return OK;
        }

      up_udelay(1);
    }

  syslog(LOG_ERR, "SDHCI: 复位 0x%08" PRIx32 " 超时\n", mask);
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: sdhci_setclock
 *
 * Description:
 *   设置卡时钟。顺序不能颠倒：先关 SDCLK，再改分频，等内部时钟稳定
 *   （INTCLKSTABLE 置位）之后才能开 SDCLK —— 时钟未稳就放行会让卡收到
 *   毛刺，表现为命令随机超时。
 *
 ****************************************************************************/

static int sdhci_setclock(uint32_t freq_hz)
{
  uint32_t regval;
  uint32_t div;
  int us;

  /* ★ 超过 25MHz 必须置 HOSTCTRL1 的 High Speed 位。
   *
   *   SDHCI 规范：High Speed 清零时控制器按"数据在 SDCLK 上升沿之后
   *   保持"的低速时序采样，25MHz 以上会采错。置位后改用高速时序。
   *
   *   漏掉它的表现是命令全部正常、数据相位静默超时 —— 因为命令线
   *   速率低、时序余量大，先坏的总是数据线。本端口实测：400kHz 下
   *   自写的探测能正确读出块0 的 MBR 签名，一旦上层把时钟提到 50MHz
   *   就 SDIOWAIT_TIMEOUT。
   */

  regval = sdhci_getreg(RK3576_SDHCI_HOSTCTRL1);
  if (freq_hz > 25000000)
    {
      regval |= SDHCI_HOSTCTRL1_HISPEED;
    }
  else
    {
      regval &= ~SDHCI_HOSTCTRL1_HISPEED;
    }

  sdhci_putreg(RK3576_SDHCI_HOSTCTRL1, regval);

  /* 关掉卡时钟再动分频 */

  regval = sdhci_getreg(RK3576_SDHCI_CLKCTRL);
  regval &= ~(SDHCI_CLK_SDCLKEN | SDHCI_CLK_INTCLKEN);
  sdhci_putreg(RK3576_SDHCI_CLKCTRL, regval);

  /* 10 位分频，实际分频比是 2*div（div=0 表示不分频）。
   * 向上取整，保证实际频率不超过请求值。
   */

  div = 0;
  if (freq_hz < RK3576_SDHCI_BASECLK_HZ)
    {
      div = (RK3576_SDHCI_BASECLK_HZ + (2 * freq_hz) - 1) / (2 * freq_hz);
      if (div > 0x3ff)
        {
          div = 0x3ff;
        }
    }

  regval = ((div & 0xff) << SDHCI_CLK_DIV_SHIFT) |
           (((div >> 8) & 0x3) << SDHCI_CLK_DIVHI_SHIFT) |
           (0xe << SDHCI_TIMEOUT_SHIFT) |      /* 数据超时取较大值 */
           SDHCI_CLK_INTCLKEN;
  sdhci_putreg(RK3576_SDHCI_CLKCTRL, regval);

  for (us = 0; us < SDHCI_RESET_TIMEOUT_US; us++)
    {
      if ((sdhci_getreg(RK3576_SDHCI_CLKCTRL) & SDHCI_CLK_INTCLKSTABLE) != 0)
        {
          break;
        }

      up_udelay(1);
    }

  if (us >= SDHCI_RESET_TIMEOUT_US)
    {
      syslog(LOG_ERR, "SDHCI: 内部时钟未稳定\n");
      return -ETIMEDOUT;
    }

  sdhci_putreg(RK3576_SDHCI_CLKCTRL, regval | SDHCI_CLK_SDCLKEN);

  syslog(LOG_INFO, "SDHCI: 卡时钟 %" PRIu32 "Hz (div=%" PRIu32
                   " 实际约 %" PRIu32 "Hz) HS=%d\n",
         freq_hz, div,
         div ? RK3576_SDHCI_BASECLK_HZ / (2 * div) : RK3576_SDHCI_BASECLK_HZ,
         (sdhci_getreg(RK3576_SDHCI_HOSTCTRL1) &
          SDHCI_HOSTCTRL1_HISPEED) ? 1 : 0);
  return OK;
}

/****************************************************************************
 * Name: sdhci_sendcmd
 *
 * Description:
 *   发一条无数据的命令并等待完成。
 *
 * Input Parameters:
 *   cmdidx  - 命令号
 *   arg     - 参数
 *   resptype- SDHCI_CMD_RESP_* 之一，可再或上 CRCCHECK/INDEXCHECK
 *   resp    - 输出，至少 4 个 uint32_t；长响应(R2)写满 4 个
 *
 ****************************************************************************/

static int sdhci_sendcmd(uint32_t cmdidx, uint32_t arg,
                         uint32_t resptype, uint32_t *resp)
{
  uint32_t regval;
  uint32_t status;
  int us;

  /* 命令线忙时不能下发新命令 */

  for (us = 0; us < SDHCI_CMD_TIMEOUT_US; us++)
    {
      if ((sdhci_getreg(RK3576_SDHCI_PRESENT) &
           SDHCI_PRESENT_CMDINHIBIT) == 0)
        {
          break;
        }

      up_udelay(1);
    }

  if (us >= SDHCI_CMD_TIMEOUT_US)
    {
      syslog(LOG_ERR, "SDHCI: CMD%" PRIu32 " 命令线一直忙\n", cmdidx);
      return -EBUSY;
    }

  sdhci_putreg(RK3576_SDHCI_INTSTAT, 0xffffffff);   /* 写 1 清 */
  sdhci_putreg(RK3576_SDHCI_ARG, arg);

  regval = (cmdidx << SDHCI_CMD_INDEX_SHIFT) | resptype;
  sdhci_putreg(RK3576_SDHCI_XFERMODE, regval << 16);

  for (us = 0; us < SDHCI_CMD_TIMEOUT_US; us++)
    {
      status = sdhci_getreg(RK3576_SDHCI_INTSTAT);
      if ((status & (SDHCI_INT_CMDCOMPLETE | SDHCI_INT_ALLERRORS)) != 0)
        {
          break;
        }

      up_udelay(1);
    }

  if (us >= SDHCI_CMD_TIMEOUT_US)
    {
      syslog(LOG_ERR,
             "SDHCI: CMD%" PRIu32 " 超时 INTSTAT=0x%08" PRIx32
             " PRESENT=0x%08" PRIx32 " CLK=0x%08" PRIx32
             " HC1=0x%08" PRIx32 " INTEN=0x%08" PRIx32 "\n",
             cmdidx, sdhci_getreg(RK3576_SDHCI_INTSTAT),
             sdhci_getreg(RK3576_SDHCI_PRESENT),
             sdhci_getreg(RK3576_SDHCI_CLKCTRL),
             sdhci_getreg(RK3576_SDHCI_HOSTCTRL1),
             sdhci_getreg(RK3576_SDHCI_INTEN));
      return -ETIMEDOUT;
    }

  if ((status & SDHCI_INT_ALLERRORS) != 0)
    {
      /* CMD1 在卡未就绪时返回超时是正常的，由调用方决定是否报错。 */

      sdhci_reset(SDHCI_RESET_CMD);
      return -EIO;
    }

  if (resp != NULL)
    {
      resp[0] = sdhci_getreg(RK3576_SDHCI_RESP0);
      if ((resptype & 0x3) == SDHCI_CMD_RESP_LEN136)
        {
          resp[1] = sdhci_getreg(RK3576_SDHCI_RESP1);
          resp[2] = sdhci_getreg(RK3576_SDHCI_RESP2);
          resp[3] = sdhci_getreg(RK3576_SDHCI_RESP3);
        }
    }

  return OK;
}

/****************************************************************************
 * Name: sdhci_readblock
 *
 * Description:
 *   用 PIO 读一个 512 字节块（CMD17 READ_SINGLE_BLOCK）。
 *
 *   先不上 DMA：DMA 要处理地址对齐、cache 一致性、描述符表，出错面大。
 *   PIO 慢但路径短，用来证明数据通路是通的。
 *
 *   ★ 数据端口每次读出 4 字节，必须严格按 BUFRDRDY 的节奏读满一个块，
 *     少读或多读都会让控制器状态机卡在 DATINHIBIT。
 *
 ****************************************************************************/

static int sdhci_readblock(uint32_t blkaddr, uint8_t *buf)
{
  uint32_t status;
  uint32_t word;
  int us;
  int i;

  sdhci_putreg(RK3576_SDHCI_INTSTAT, 0xffffffff);
  sdhci_putreg(RK3576_SDHCI_BLKSIZE, (1 << 16) | 512);
  sdhci_putreg(RK3576_SDHCI_ARG, blkaddr);

  /* 传输模式在低 16 位、命令在高 16 位，同一个 32 位寄存器一次写完。
   * DMAEN 不置（PIO），BLKCNTEN + DATAREAD 表示"读 1 个块"。
   */

  sdhci_putreg(RK3576_SDHCI_XFERMODE,
               ((17u << SDHCI_CMD_INDEX_SHIFT) | SDHCI_CMD_RESP_LEN48 |
                SDHCI_CMD_CRCCHECK | SDHCI_CMD_INDEXCHECK |
                SDHCI_CMD_DATAPRESENT) << 16 |
               (SDHCI_XFER_BLKCNTEN | SDHCI_XFER_DATAREAD));

  /* 等命令完成 */

  for (us = 0; us < SDHCI_CMD_TIMEOUT_US; us++)
    {
      status = sdhci_getreg(RK3576_SDHCI_INTSTAT);
      if ((status & (SDHCI_INT_CMDCOMPLETE | SDHCI_INT_ALLERRORS)) != 0)
        {
          break;
        }

      up_udelay(1);
    }

  if (us >= SDHCI_CMD_TIMEOUT_US || (status & SDHCI_INT_ALLERRORS) != 0)
    {
      syslog(LOG_ERR, "SDHCI: CMD17 失败 INTSTAT=0x%08" PRIx32 "\n", status);
      return -EIO;
    }

  /* 等数据就绪，然后一次读满 512 字节 */

  for (us = 0; us < SDHCI_CMD_TIMEOUT_US; us++)
    {
      status = sdhci_getreg(RK3576_SDHCI_INTSTAT);
      if ((status & (SDHCI_INT_BUFRDRDY | SDHCI_INT_DATAERRORS)) != 0)
        {
          break;
        }

      up_udelay(1);
    }

  if ((status & SDHCI_INT_DATAERRORS) != 0 || us >= SDHCI_CMD_TIMEOUT_US)
    {
      syslog(LOG_ERR, "SDHCI: 数据相位失败 INTSTAT=0x%08" PRIx32 "\n",
             status);
      return -EIO;
    }

  for (i = 0; i < 512; i += 4)
    {
      word = sdhci_getreg(RK3576_SDHCI_BUFDATA);
      buf[i]     = (uint8_t)word;
      buf[i + 1] = (uint8_t)(word >> 8);
      buf[i + 2] = (uint8_t)(word >> 16);
      buf[i + 3] = (uint8_t)(word >> 24);
    }

  /* 等传输结束，否则下一条命令会撞上 DATINHIBIT */

  for (us = 0; us < SDHCI_CMD_TIMEOUT_US; us++)
    {
      if ((sdhci_getreg(RK3576_SDHCI_INTSTAT) &
           SDHCI_INT_XFERCOMPLETE) != 0)
        {
          return OK;
        }

      up_udelay(1);
    }

  syslog(LOG_ERR, "SDHCI: 传输未结束\n");
  return -ETIMEDOUT;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * sdio_dev_s 实现
 ****************************************************************************/

/* 上层通过这些回调驱动控制器。数据缓冲区在 RECVSETUP/SENDSETUP 时登记，
 * 真正的搬运发生在 EVENTWAIT。
 */

struct rk3576_sdio_dev_s
{
  struct sdio_dev_s dev;              /* 必须是第一个成员 */
  uint8_t          *buffer;           /* 当前事务的数据缓冲区 */
  size_t            remaining;        /* 待传字节数 */
  bool             is_write;          /* true 发送，false 接收 */
  bool             xfer_armed;        /* 是否已登记数据传输 */
  uint16_t         blocksize;
  uint16_t         nblocks;
  sdio_eventset_t  waitevents;
  uint32_t         waittimeout_ms;
  bool             widebus;
};

static struct rk3576_sdio_dev_s g_sdio_dev;

/****************************************************************************
 * Name: rk3576_sdio_lock / reset / capabilities / status
 ****************************************************************************/

#ifdef CONFIG_SDIO_MUXBUS
static int rk3576_sdio_lock(struct sdio_dev_s *dev, bool lock)
{
  UNUSED(dev);
  UNUSED(lock);
  return OK;              /* 单一从设备，无需总线仲裁 */
}
#endif

static void rk3576_sdio_reset(struct sdio_dev_s *dev)
{
  struct rk3576_sdio_dev_s *priv = (struct rk3576_sdio_dev_s *)dev;

  sdhci_reset(SDHCI_RESET_CMD | SDHCI_RESET_DATA);
  sdhci_putreg(RK3576_SDHCI_INTSTAT, 0xffffffff);
  sdhci_putreg(RK3576_SDHCI_INTEN, 0xffffffff);
  sdhci_putreg(RK3576_SDHCI_SIGEN, 0);
  priv->xfer_armed = false;
  priv->remaining  = 0;
}

static sdio_capset_t rk3576_sdio_capabilities(struct sdio_dev_s *dev)
{
  UNUSED(dev);

  /* ★ 必须声明 SDIO_CAPS_4BIT_ONLY，否则卡和主机的位宽会不一致。
   *
   *   drivers/mmcsd/mmcsd_sdio.c 里两处判断条件不同：
   *
   *     用 CMD6 把【卡】切到 4 位 —— 要求 priv->buswidth 带 4BIT 标志
   *     用 SDIO_WIDEBUS 把【主机】设成 4 位 —— 只要求 IS_MMC()
   *
   *   而对 MMC 卡，priv->buswidth 的 4BIT 标志只在
   *   (caps & SDIO_CAPS_4BIT_ONLY) 成立时才被置上
   *   （mmcsd_sdio.c 约 3938 行）。
   *
   *   所以 capabilities() 返回 0 时：CMD6 被跳过、卡仍是 1 位，
   *   而主机被设成 4 位 —— 命令线照常工作（命令走 CMD 线，与位宽无关），
   *   唯独数据相位永远等不到 BUFRDRDY。本端口实测就是这个现象，
   *   且降到 1MHz 也不改善，因为它根本不是时序问题。
   *
   *   不声明 DMASUPPORTED：CAP0 里有 ADMA2，但本驱动是 PIO，
   *   声明了会让上层走没实现的 dmarecvsetup 路径。
   *
   * ★ SDIO_CAPS_DMABEFOREWRITE 管的是**调用顺序**，与 DMA 无关
   *   （rk3576_dwmmc.c 里同一个坑）。不报的话 mmcsd 写单块是先发
   *   CMD24、再 SENDSETUP —— 而 SDHCI 的 DATAPRESENT/方向位必须和命令
   *   在同一次寄存器写里给出，于是 CMD24 被当成无数据命令发出去。
   *   2026-10-25 第一次 mkfatfs 实测：CMD24 命令超时（INTSTAT
   *   0x00018000），一个字节都没写进去。
   */

  return SDIO_CAPS_4BIT_ONLY | SDIO_CAPS_DMABEFOREWRITE;
}

static sdio_statset_t rk3576_sdio_status(struct sdio_dev_s *dev)
{
  UNUSED(dev);

  /* eMMC 是板载焊死的，dtsi 里标了 non-removable，因此恒为"已插入"。
   * 不去读 SDHCI PRESENT 寄存器的 CARDINSERTED —— 那一位是给可插拔
   * 卡座用的，对焊死器件没有意义。
   *
   * ★ 这里必须返回 SDIO_STATUS_PRESENT。返回 0 表示"没有任何状态标志"，
   *   上层会判定插槽为空，日志显示 "MMC/SD slot is empty"，
   *   随后不注册块设备 —— 而 mmcsd_slotinitialize() 仍返回 OK
   *   （它只把 -ENODEV 之外的错误当失败），因此调用方看不出异常。
   */

  return SDIO_STATUS_PRESENT;
}

static void rk3576_sdio_widebus(struct sdio_dev_s *dev, bool enable)
{
  struct rk3576_sdio_dev_s *priv = (struct rk3576_sdio_dev_s *)dev;
  uint32_t regval;

  regval = sdhci_getreg(RK3576_SDHCI_HOSTCTRL1);
  regval &= ~(SDHCI_HOSTCTRL1_DTW4BIT | SDHCI_HOSTCTRL1_DTW8BIT);
  if (enable)
    {
      regval |= SDHCI_HOSTCTRL1_DTW4BIT;
    }

  sdhci_putreg(RK3576_SDHCI_HOSTCTRL1, regval);
  priv->widebus = enable;
}

static void rk3576_sdio_clock(struct sdio_dev_s *dev, enum sdio_clock_e rate)
{
  UNUSED(dev);

  switch (rate)
    {
      case CLOCK_SDIO_DISABLED:
        sdhci_putreg(RK3576_SDHCI_CLKCTRL,
                     sdhci_getreg(RK3576_SDHCI_CLKCTRL) &
                     ~SDHCI_CLK_SDCLKEN);
        break;

      case CLOCK_IDMODE:
        sdhci_setclock(SDHCI_ID_CLOCK_HZ);      /* 400kHz 识别用 */
        break;

      default:
        /* MMC 传输模式，52MHz High Speed。
         * HS200/HS400 需要 DLL 采样窗口整定，后续再做。
         */

        sdhci_setclock(52000000);
        break;
    }
}

static int rk3576_sdio_attach(struct sdio_dev_s *dev)
{
  UNUSED(dev);
  return OK;              /* 轮询式，不挂中断 */
}

/****************************************************************************
 * Name: rk3576_sdio_sendcmd
 *
 * Description:
 *   把上层的命令编码翻译成 SDHCI 的 XFERMODE 写入。
 *   若此前 RECVSETUP/SENDSETUP 登记过数据传输，这里补上数据相关位 ——
 *   SDHCI 要求命令和传输模式在同一次 32 位写里完成。
 *
 ****************************************************************************/

static int rk3576_sdio_sendcmd(struct sdio_dev_s *dev, uint32_t cmd,
                               uint32_t arg)
{
  struct rk3576_sdio_dev_s *priv = (struct rk3576_sdio_dev_s *)dev;
  uint32_t cmdidx = cmd & MMCSD_CMDIDX_MASK;
  uint32_t cmdreg;
  uint32_t xfer = 0;
  int us;

  for (us = 0; us < SDHCI_CMD_TIMEOUT_US; us++)
    {
      uint32_t busy = SDHCI_PRESENT_CMDINHIBIT;

      /* ★ 带数据的命令要等 DAT 线，R1b 同样要等 —— 它虽然没有数据相位，
       * 却会用 DAT0 表示"卡忙"。只按 xfer_armed 判断会漏掉 R1b，
       * 于是在卡还忙时就下发下一条命令。
       *
       * 这一点很隐蔽：mmcsd 用 CMD6 SWITCH(R1b) 切换卡的总线宽度，
       * 如果没等它真正完成就继续，切换可能未生效 —— 主机按 4 位收、
       * 卡仍按 1 位发，后续读数据永远等不到 BUFRDRDY。
       */

      if (priv->xfer_armed ||
          (cmd & MMCSD_RESPONSE_MASK) == MMCSD_R1B_RESPONSE)
        {
          busy |= SDHCI_PRESENT_DATINHIBIT;
        }

      if ((sdhci_getreg(RK3576_SDHCI_PRESENT) & busy) == 0)
        {
          break;
        }

      up_udelay(1);
    }

  if (us >= SDHCI_CMD_TIMEOUT_US)
    {
      return -EBUSY;
    }

  switch (cmd & MMCSD_RESPONSE_MASK)
    {
      case MMCSD_NO_RESPONSE:
        cmdreg = SDHCI_CMD_RESP_NONE;
        break;

      case MMCSD_R2_RESPONSE:
        cmdreg = SDHCI_CMD_RESP_LEN136 | SDHCI_CMD_CRCCHECK;
        break;

      case MMCSD_R1B_RESPONSE:
        cmdreg = SDHCI_CMD_RESP_LEN48BUSY | SDHCI_CMD_CRCCHECK |
                 SDHCI_CMD_INDEXCHECK;
        break;

      case MMCSD_R3_RESPONSE:
      case MMCSD_R4_RESPONSE:

        /* R3/R4 无 CRC 且命令索引位是保留值，开校验会误报。 */

        cmdreg = SDHCI_CMD_RESP_LEN48;
        break;

      default:
        cmdreg = SDHCI_CMD_RESP_LEN48 | SDHCI_CMD_CRCCHECK |
                 SDHCI_CMD_INDEXCHECK;
        break;
    }

  /* ★ 数据位只给真正带数据的那条命令，不能只看 xfer_armed。
   *
   *   对 MMC 卡，mmcsd 的多块读写是 RECVSETUP → CMD23(SET_BLOCK_COUNT)
   *   → CMD18/25。CMD23 发出时 xfer_armed 已经为真，原先会把它也当成
   *   读命令：控制器为一条没有数据相位的命令开了数据通路，随后的
   *   CMD18 永远等不到 BUFRDRDY。单块的 CMD17 前面不发 CMD23，所以
   *   "单块读正常、多块读必超时"（2026-10-25 双系统下读 GPT 时发现）。
   *
   *   也不开 AUTOCMD12：MMC 用 CMD23 预告块数，卡传完自己停；SD 不支持
   *   CMD23 时 mmcsd 会自己发 CMD12。再叠一个 Auto CMD12 是多余的停止命令。
   */

  if (priv->xfer_armed &&
      (cmd & MMCSD_DATAXFR_MASK) != MMCSD_NODATAXFR)
    {
      cmdreg |= SDHCI_CMD_DATAPRESENT;
      xfer    = SDHCI_XFER_BLKCNTEN;

      if (!priv->is_write)
        {
          xfer |= SDHCI_XFER_DATAREAD;
        }

      if (priv->nblocks > 1)
        {
          xfer |= SDHCI_XFER_MULTIBLK;
        }
    }

  sdhci_putreg(RK3576_SDHCI_INTSTAT, 0xffffffff);
  sdhci_putreg(RK3576_SDHCI_ARG, arg);

  sdhci_putreg(RK3576_SDHCI_XFERMODE,
               ((cmdidx << SDHCI_CMD_INDEX_SHIFT) | cmdreg) << 16 | xfer);
  return OK;
}

#ifdef CONFIG_SDIO_BLOCKSETUP
static void rk3576_sdio_blocksetup(struct sdio_dev_s *dev,
                                   unsigned int blocklen,
                                   unsigned int nblocks)
{
  struct rk3576_sdio_dev_s *priv = (struct rk3576_sdio_dev_s *)dev;

  priv->blocksize = (uint16_t)blocklen;
  priv->nblocks   = (uint16_t)nblocks;
  sdhci_putreg(RK3576_SDHCI_BLKSIZE,
               ((uint32_t)nblocks << 16) | (blocklen & 0xfff));
}
#endif

static int rk3576_sdio_recvsetup(struct sdio_dev_s *dev, uint8_t *buffer,
                                 size_t nbytes)
{
  struct rk3576_sdio_dev_s *priv = (struct rk3576_sdio_dev_s *)dev;

  priv->buffer     = buffer;
  priv->remaining  = nbytes;
  priv->is_write   = false;
  priv->xfer_armed = true;
  return OK;
}

static int rk3576_sdio_sendsetup(struct sdio_dev_s *dev,
                                 const uint8_t *buffer, size_t nbytes)
{
  struct rk3576_sdio_dev_s *priv = (struct rk3576_sdio_dev_s *)dev;

  priv->buffer     = (uint8_t *)buffer;
  priv->remaining  = nbytes;
  priv->is_write   = true;
  priv->xfer_armed = true;
  return OK;
}

static int rk3576_sdio_cancel(struct sdio_dev_s *dev)
{
  struct rk3576_sdio_dev_s *priv = (struct rk3576_sdio_dev_s *)dev;

  priv->xfer_armed = false;
  priv->remaining  = 0;
  sdhci_reset(SDHCI_RESET_CMD | SDHCI_RESET_DATA);
  return OK;
}

static int rk3576_sdio_waitresponse(struct sdio_dev_s *dev, uint32_t cmd)
{
  uint32_t status;
  int us;

  UNUSED(dev);

  for (us = 0; us < SDHCI_CMD_TIMEOUT_US; us++)
    {
      status = sdhci_getreg(RK3576_SDHCI_INTSTAT);
      if ((status & (SDHCI_INT_CMDCOMPLETE | SDHCI_INT_ALLERRORS)) != 0)
        {
          break;
        }

      up_udelay(1);
    }

  if (us >= SDHCI_CMD_TIMEOUT_US)
    {
      return -ETIMEDOUT;
    }

  if ((status & SDHCI_INT_CMDERRORS) != 0)
    {
      sdhci_reset(SDHCI_RESET_CMD);
      return ((status & SDHCI_INT_CMDTIMEOUT) != 0) ? -ETIMEDOUT : -EIO;
    }

  /* ★ R1b：命令完成不等于事务完成。卡会拉低 DAT0 表示"正在处理"，
   * 控制器在忙状态解除时给出 XFERCOMPLETE。不等它就继续下发命令，
   * 会打断尚未生效的操作（典型如 CMD6 SWITCH 改总线宽度）。
   */

  if ((cmd & MMCSD_RESPONSE_MASK) == MMCSD_R1B_RESPONSE)
    {
      for (us = 0; us < SDHCI_CMD_TIMEOUT_US; us++)
        {
          status = sdhci_getreg(RK3576_SDHCI_INTSTAT);
          if ((status & SDHCI_INT_XFERCOMPLETE) != 0)
            {
              sdhci_putreg(RK3576_SDHCI_INTSTAT, SDHCI_INT_XFERCOMPLETE);
              break;
            }

          if ((status & SDHCI_INT_DATAERRORS) != 0)
            {
              sdhci_reset(SDHCI_RESET_DATA);
              return -EIO;
            }

          up_udelay(1);
        }
    }

  return OK;
}

static int rk3576_sdio_recvshort(struct sdio_dev_s *dev, uint32_t cmd,
                                 uint32_t *rshort)
{
  UNUSED(dev);
  UNUSED(cmd);

  if (rshort != NULL)
    {
      *rshort = sdhci_getreg(RK3576_SDHCI_RESP0);
    }

  return OK;
}

static int rk3576_sdio_recvlong(struct sdio_dev_s *dev, uint32_t cmd,
                                uint32_t rlong[4])
{
  UNUSED(dev);
  UNUSED(cmd);

  if (rlong != NULL)
    {
      /* ★ 长响应要做两件事：倒字序，再整体左移 8 位。
       *
       *   字序：RESP0 存响应最低 32 位，而上层期望 rlong[0] 是最高那组。
       *
       *   移位：SDHCI 的响应寄存器里放的是 R[127:8] —— 硬件把 CRC7 和
       *   停止位去掉了，于是整个 128 位内容右移了 8 位。上层
       *   mmcsd_decode_cid/csd 期望的是原始的 128 位布局
       *   （bit127 是 CID 的 MID 最高位）。
       *
       *   漏掉移位不会报错，只会解出错误的字段：本端口实测表现为
       *   mid 解成 00、产品名为空（实际 MID 是 0xec、产品名是
       *   可读 ASCII）。容量因为来自 EXT_CSD（走数据通路读的）
       *   而不受影响，所以更容易漏掉。
       */

      uint32_t r0 = sdhci_getreg(RK3576_SDHCI_RESP0);
      uint32_t r1 = sdhci_getreg(RK3576_SDHCI_RESP1);
      uint32_t r2 = sdhci_getreg(RK3576_SDHCI_RESP2);
      uint32_t r3 = sdhci_getreg(RK3576_SDHCI_RESP3);

      rlong[0] = (r3 << 8) | (r2 >> 24);
      rlong[1] = (r2 << 8) | (r1 >> 24);
      rlong[2] = (r1 << 8) | (r0 >> 24);
      rlong[3] = (r0 << 8);
    }

  return OK;
}

static void rk3576_sdio_waitenable(struct sdio_dev_s *dev,
                                   sdio_eventset_t eventset,
                                   uint32_t timeout)
{
  struct rk3576_sdio_dev_s *priv = (struct rk3576_sdio_dev_s *)dev;

  priv->waitevents     = eventset;
  priv->waittimeout_ms = timeout;
}

/****************************************************************************
 * Name: rk3576_sdio_eventwait
 *
 * Description:
 *   同步完成数据搬运并等待事务结束。
 *
 *   中断式实现里这里等信号量、由 ISR 搬数据；轮询式把搬运放在这里。
 *   上层的时序保证了调用本函数时命令已经发出、响应已经读走。
 *
 ****************************************************************************/

static sdio_eventset_t rk3576_sdio_eventwait(struct sdio_dev_s *dev)
{
  struct rk3576_sdio_dev_s *priv = (struct rk3576_sdio_dev_s *)dev;
  uint32_t status;
  uint32_t word;
  uint32_t limit;
  int us;
  int i;

  if (!priv->xfer_armed)
    {
      /* 没有数据相位（例如 CMD7 的 R1b 忙等），只需等事务结束。 */

      return SDIOWAIT_TRANSFERDONE;
    }

  limit = priv->waittimeout_ms ? priv->waittimeout_ms * 1000
                               : SDHCI_CMD_TIMEOUT_US;

  while (priv->remaining > 0)
    {
      uint32_t rdy = priv->is_write ? SDHCI_INT_BUFWRRDY :
                                      SDHCI_INT_BUFRDRDY;

      for (us = 0; us < limit; us++)
        {
          status = sdhci_getreg(RK3576_SDHCI_INTSTAT);
          if ((status & (rdy | SDHCI_INT_DATAERRORS)) != 0)
            {
              break;
            }

          up_udelay(1);
        }

      if (us >= limit)
        {
          syslog(LOG_ERR,
                 "SDHCI 数据超时: INTSTAT=0x%08" PRIx32
                 " PRESENT=0x%08" PRIx32 " BLKSIZE=0x%08" PRIx32
                 " HC1=0x%08" PRIx32 " CLK=0x%08" PRIx32
                 " 待传=%zu 写=%d\n",
                 sdhci_getreg(RK3576_SDHCI_INTSTAT),
                 sdhci_getreg(RK3576_SDHCI_PRESENT),
                 sdhci_getreg(RK3576_SDHCI_BLKSIZE),
                 sdhci_getreg(RK3576_SDHCI_HOSTCTRL1),
                 sdhci_getreg(RK3576_SDHCI_CLKCTRL),
                 priv->remaining, (int)priv->is_write);

          /* 超时也要复位 DAT 线：不复位的话 DATINHIBIT 一直挂着，
           * 下一次操作跟着失败，看起来像故障在"随机"出现。
           */

          priv->xfer_armed = false;
          sdhci_reset(SDHCI_RESET_CMD | SDHCI_RESET_DATA);
          return SDIOWAIT_TIMEOUT;
        }

      if ((status & SDHCI_INT_DATAERRORS) != 0)
        {
          priv->xfer_armed = false;
          sdhci_reset(SDHCI_RESET_DATA);
          return SDIOWAIT_ERROR;
        }

      sdhci_putreg(RK3576_SDHCI_INTSTAT, rdy);

      /* 缓冲区就绪信号一次覆盖一个块，按块搬运。 */

      for (i = 0; i < priv->blocksize && priv->remaining > 0; i += 4)
        {
          if (priv->is_write)
            {
              word = (uint32_t)priv->buffer[0]        |
                     (uint32_t)priv->buffer[1] << 8   |
                     (uint32_t)priv->buffer[2] << 16  |
                     (uint32_t)priv->buffer[3] << 24;
              sdhci_putreg(RK3576_SDHCI_BUFDATA, word);
            }
          else
            {
              word = sdhci_getreg(RK3576_SDHCI_BUFDATA);
              priv->buffer[0] = (uint8_t)word;
              priv->buffer[1] = (uint8_t)(word >> 8);
              priv->buffer[2] = (uint8_t)(word >> 16);
              priv->buffer[3] = (uint8_t)(word >> 24);
            }

          priv->buffer    += 4;
          priv->remaining -= 4;
        }
    }

  /* 等传输结束，否则下一条命令会撞上 DATINHIBIT。 */

  for (us = 0; us < limit; us++)
    {
      status = sdhci_getreg(RK3576_SDHCI_INTSTAT);
      if ((status & SDHCI_INT_XFERCOMPLETE) != 0)
        {
          priv->xfer_armed = false;
          return SDIOWAIT_TRANSFERDONE;
        }

      if ((status & SDHCI_INT_DATAERRORS) != 0)
        {
          priv->xfer_armed = false;
          sdhci_reset(SDHCI_RESET_DATA);
          return SDIOWAIT_ERROR;
        }

      up_udelay(1);
    }

  priv->xfer_armed = false;
  sdhci_reset(SDHCI_RESET_CMD | SDHCI_RESET_DATA);
  return SDIOWAIT_TIMEOUT;
}

static void rk3576_sdio_callbackenable(struct sdio_dev_s *dev,
                                       sdio_eventset_t eventset)
{
  UNUSED(dev);
  UNUSED(eventset);       /* eMMC 焊死，无插拔事件 */
}

#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
static int rk3576_sdio_registercallback(struct sdio_dev_s *dev,
                                        worker_t callback, void *arg)
{
  UNUSED(dev);
  UNUSED(callback);
  UNUSED(arg);
  return OK;
}
#endif

/* 部分成员是条件编译的，这里的 #ifdef 必须与 include/nuttx/sdio.h 一致：
 *   lock             ← CONFIG_SDIO_MUXBUS
 *   blocksetup       ← CONFIG_SDIO_BLOCKSETUP
 *   registercallback ← CONFIG_SCHED_WORKQUEUE && CONFIG_SCHED_HPWORK
 */

static const struct sdio_dev_s g_sdio_ops =
{
#ifdef CONFIG_SDIO_MUXBUS
  .lock             = rk3576_sdio_lock,
#endif
  .reset            = rk3576_sdio_reset,
  .capabilities     = rk3576_sdio_capabilities,
  .status           = rk3576_sdio_status,
  .widebus          = rk3576_sdio_widebus,
  .clock            = rk3576_sdio_clock,
  .attach           = rk3576_sdio_attach,
  .sendcmd          = rk3576_sdio_sendcmd,
#ifdef CONFIG_SDIO_BLOCKSETUP
  .blocksetup       = rk3576_sdio_blocksetup,
#endif
  .recvsetup        = rk3576_sdio_recvsetup,
  .sendsetup        = rk3576_sdio_sendsetup,
  .cancel           = rk3576_sdio_cancel,
  .waitresponse     = rk3576_sdio_waitresponse,
  .recv_r1          = rk3576_sdio_recvshort,
  .recv_r2          = rk3576_sdio_recvlong,
  .recv_r3          = rk3576_sdio_recvshort,
  .recv_r4          = rk3576_sdio_recvshort,
  .recv_r5          = rk3576_sdio_recvshort,
  .recv_r6          = rk3576_sdio_recvshort,
  .recv_r7          = rk3576_sdio_recvshort,
  .waitenable       = rk3576_sdio_waitenable,
  .eventwait        = rk3576_sdio_eventwait,
  .callbackenable   = rk3576_sdio_callbackenable,
#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
  .registercallback = rk3576_sdio_registercallback,
#endif
};

/****************************************************************************
 * Name: rk3576_sdhci_initialize
 *
 * Description:
 *   返回 sdio_dev_s 句柄，供 mmcsd_slotinitialize() 注册 /dev/mmcsdN。
 *
 ****************************************************************************/

struct sdio_dev_s *rk3576_sdhci_initialize(int slotno)
{
  struct rk3576_sdio_dev_s *priv = &g_sdio_dev;

  if (slotno != 0)
    {
      return NULL;
    }

  memcpy(&priv->dev, &g_sdio_ops, sizeof(struct sdio_dev_s));
  priv->xfer_armed = false;

  sdhci_reset(SDHCI_RESET_ALL);
  sdhci_putreg(RK3576_SDHCI_HOSTCTRL1, SDHCI_PWR_33V | SDHCI_PWR_ON);
  sdhci_putreg(RK3576_SDHCI_INTEN, 0xffffffff);
  sdhci_putreg(RK3576_SDHCI_SIGEN, 0);
  sdhci_setclock(SDHCI_ID_CLOCK_HZ);

  return &priv->dev;
}

/****************************************************************************
 * Name: rk3576_sdhci_probe
 *
 * Description:
 *   走一遍 eMMC 上电识别序列，验证命令通路。
 *   CMD0(无响应) → CMD1(R3) → CMD2(R2 长响应) → CMD3(R1)，
 *   四条命令覆盖了全部响应类型。
 *
 ****************************************************************************/

int rk3576_sdhci_probe(void)
{
  uint32_t resp[4];
  uint32_t ocr = 0;
  int ret;
  int i;
  static uint8_t blk[512];

  syslog(LOG_INFO, "SDHCI: HOSTVER=0x%04" PRIx32 " CAP0=0x%08" PRIx32 "\n",
         (sdhci_getreg(RK3576_SDHCI_HOSTVER) >> 16) & 0xffff,
         sdhci_getreg(RK3576_SDHCI_CAP0));

  ret = sdhci_reset(SDHCI_RESET_ALL);
  if (ret < 0)
    {
      return ret;
    }

  /* 总线电源：eMMC 是 1.8V/3.3V 供电，这里按 3.3V 打开电源域使能位。
   * 板子上 eMMC 的实际供电由 PMIC 提供，这一位只控制控制器内部的
   * 电源使能逻辑。
   */

  sdhci_putreg(RK3576_SDHCI_HOSTCTRL1, SDHCI_PWR_33V | SDHCI_PWR_ON);

  /* ★ 使能中断"状态记录"。
   *
   *   SDHCI 规范里 INTSTAT 的位只有在 INTEN 对应位为 1 时才会被硬件置位。
   *   RESET_ALL 会把 INTEN 清零，此后轮询 INTSTAT 将永远读到 0 ——
   *   表现为每条命令都超时，即使命令其实已经发出去了。
   *
   *   SIGEN 保持 0：那是"是否向 CPU 拉中断线"，本实现是轮询式，不需要。
   *   两者容易混淆，但作用完全不同：INTEN 管"记不记"，SIGEN 管"报不报"。
   */

  sdhci_putreg(RK3576_SDHCI_INTEN, 0xffffffff);
  sdhci_putreg(RK3576_SDHCI_SIGEN, 0);

  ret = sdhci_setclock(SDHCI_ID_CLOCK_HZ);
  if (ret < 0)
    {
      return ret;
    }

  up_udelay(2000);        /* 规范要求上电后等待至少 1ms 再发命令 */

  /* CMD0 GO_IDLE_STATE —— 无响应 */

  ret = sdhci_sendcmd(0, 0, SDHCI_CMD_RESP_NONE, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "SDHCI: CMD0 失败 %d\n", ret);
      return ret;
    }

  up_udelay(2000);

  /* CMD1 SEND_OP_COND —— R3。卡忙时 bit31=0，要反复问直到就绪。
   * 参数 0x40ff8080：bit30 = 支持扇区寻址（>2GB 的 eMMC 必需），
   * 电压窗口 2.7-3.6V。
   */

  for (i = 0; i < 1000; i++)
    {
      ret = sdhci_sendcmd(1, 0x40ff8080, SDHCI_CMD_RESP_LEN48, resp);
      if (ret == OK)
        {
          ocr = resp[0];
          if ((ocr & (1u << 31)) != 0)
            {
              break;                 /* 上电完成 */
            }
        }

      up_udelay(1000);
    }

  if ((ocr & (1u << 31)) == 0)
    {
      syslog(LOG_ERR, "SDHCI: CMD1 卡未就绪，OCR=0x%08" PRIx32 "\n", ocr);
      return -ETIMEDOUT;
    }

  syslog(LOG_INFO, "SDHCI: CMD1 OCR=0x%08" PRIx32 " (%s寻址)\n",
         ocr, (ocr & (1u << 30)) ? "扇区" : "字节");

  /* CMD2 ALL_SEND_CID —— R2 长响应，136 位 */

  ret = sdhci_sendcmd(2, 0, SDHCI_CMD_RESP_LEN136 | SDHCI_CMD_CRCCHECK,
                      resp);
  if (ret < 0)
    {
      syslog(LOG_ERR, "SDHCI: CMD2 失败 %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO, "SDHCI: CID = %08" PRIx32 " %08" PRIx32
                   " %08" PRIx32 " %08" PRIx32 "\n",
         resp[3], resp[2], resp[1], resp[0]);

  /* CMD3 SET_RELATIVE_ADDR —— eMMC 由主机指定 RCA（SD 卡是卡自己给）。
   * 取 1（0 保留给"取消选中"）。
   */

  ret = sdhci_sendcmd(3, 1 << 16,
                      SDHCI_CMD_RESP_LEN48 | SDHCI_CMD_CRCCHECK |
                      SDHCI_CMD_INDEXCHECK, resp);
  if (ret < 0)
    {
      syslog(LOG_ERR, "SDHCI: CMD3 失败 %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO, "SDHCI: CMD3 RCA=1 状态=0x%08" PRIx32 "\n", resp[0]);
  syslog(LOG_INFO, "SDHCI: ★ 命令通路验证通过\n");

  /* CMD9 SEND_CSD —— R2，取容量等参数。要在 stby 状态下发。 */

  ret = sdhci_sendcmd(9, 1 << 16, SDHCI_CMD_RESP_LEN136 | SDHCI_CMD_CRCCHECK,
                      resp);
  if (ret < 0)
    {
      syslog(LOG_ERR, "SDHCI: CMD9 失败 %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO, "SDHCI: CSD = %08" PRIx32 " %08" PRIx32
                   " %08" PRIx32 " %08" PRIx32 "\n",
         resp[3], resp[2], resp[1], resp[0]);

  /* CMD7 SELECT_CARD —— R1b，把卡从 stby 带进 tran 状态才能传数据 */

  ret = sdhci_sendcmd(7, 1 << 16,
                      SDHCI_CMD_RESP_LEN48BUSY | SDHCI_CMD_CRCCHECK |
                      SDHCI_CMD_INDEXCHECK, resp);
  if (ret < 0)
    {
      syslog(LOG_ERR, "SDHCI: CMD7 失败 %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO, "SDHCI: CMD7 已选中，状态=0x%08" PRIx32 "\n", resp[0]);

  /* 读第 0 块。判据：eMMC 上原本装着 Android，块 0 应是分区表 ——
   * 末尾两字节为 0x55 0xAA（MBR 签名），或起始为 "EFI PART"（GPT）。
   * 读到这些说明数据通路真的通了，而不是读回一片零。
   */

  ret = sdhci_readblock(0, blk);
  if (ret < 0)
    {
      return ret;
    }

  syslog(LOG_INFO,
         "SDHCI: 块0 前16字节 %02x %02x %02x %02x %02x %02x %02x %02x "
         "%02x %02x %02x %02x %02x %02x %02x %02x\n",
         blk[0], blk[1], blk[2], blk[3], blk[4], blk[5], blk[6], blk[7],
         blk[8], blk[9], blk[10], blk[11], blk[12], blk[13], blk[14],
         blk[15]);

  syslog(LOG_INFO, "SDHCI: 块0 末2字节 %02x %02x  %s\n",
         blk[510], blk[511],
         (blk[510] == 0x55 && blk[511] == 0xaa)
           ? "← MBR 签名，数据通路验证通过" : "← 非 MBR，请核对内容");

  return OK;
}

#endif /* CONFIG_RK3576_SDHCI */
