/****************************************************************************
 * chip/rk3576/rk3576_dsi2.c
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

/* RK3576 MIPI DSI2 主机（Synopsys DSI 2.0）。
 *
 * ★ 是 DSI 2.0，不是 1.x
 *
 *   网上多数 Rockchip MIPI 资料讲的是 dw-mipi-dsi 1.x（RK3399/RK3568
 *   那代），寄存器名相似但偏移不同。照 1.x 写不会报错，只是配置全部
 *   落空、屏不亮。本文件寄存器取自 Linux
 *   drivers/gpu/drm/bridge/synopsys/dw-mipi-dsi2.c。
 *
 * ★ 命令模式与视频模式
 *
 *   面板初始化序列必须在**命令模式**下发送（走 CRI 接口）；发完再切到
 *   视频模式，DSI 才开始接收 VOP 送来的像素流。顺序反了的话初始化
 *   命令进不去，屏保持黑屏。
 *
 * ★ 尚未实现 D-PHY
 *
 *   本文件只管 DSI 主机侧。PHY 的 PLL 配置在 rk3576_dcphy.c，没有它
 *   链路不会真正工作 —— 本文件的自检只能验证寄存器可访问。
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/arch.h>

#include "arm64_internal.h"
#include "rk3576_dsi2.h"
#include "rk3576_cru.h"
#include "rk3576_power.h"
#include "hardware/rk3576_dsi2.h"
#include "hardware/rk3576_memorymap.h"

#ifdef CONFIG_RK3576_DSI2

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define DSI_BASE   RK3576_DSI0_ADDR

/* 时钟。出处：clk-rk3576.c
 *   GATE(PCLK_DSIHOST0, ... CLKGATE_CON(64), 5)
 *   COMPOSITE(CLK_DSIHOST0, ... CLKGATE_CON(64), 6)
 */

/* CLK_DSIHOST0 的选源与分频（clk-rk3576.c）：
 *   COMPOSITE(CLK_DSIHOST0, "clk_dsihost0",
 *             gpll_cpll_spll_vpll_bpll_lpll_p,
 *             RK3576_CLKSEL_CON(151), 7, 3, MFLAGS,   选源 bit[9:7]
 *                                     0, 7, DFLAGS,   分频 bit[6:0]
 *             RK3576_CLKGATE_CON(64), 6, GFLAGS)
 * 父时钟表索引 2 = spll，固定 702MHz（rk3576.dtsi 的 clock-spll）。
 */

#define DSI_SEL_CON        151
#define DSI_SEL_MUX_SHIFT  7
#define DSI_SEL_MUX_SPLL   2
#define DSI_SPLL_HZ        702000000u
#define DSI_SYS_CLK_DIV    3
#define DSI_SYS_CLK_HZ     (DSI_SPLL_HZ / DSI_SYS_CLK_DIV)

#define DSI_GATE_CON    64
#define DSI_GATE_PCLK   5
#define DSI_GATE_CLK    6

#define DSI_CMD_TIMEOUT_US  50000

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* 寄存器 dump 用的名字表 */

struct dsi2_regname_s
{
  const char *name;
  uint32_t    off;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* rk3576_dsi2_dump_uboot_state()：U-Boot 留下的 DSI 寄存器 */

static const struct dsi2_regname_s g_dsi2_uboot_regs[] =
{
  {
    "PWR_UP",           0x0000
  },
  {
    "SOFT_RESET",       0x0010
  },
  {
    "MODE_CTRL",        0x0018
  },
  {
    "MODE_STATUS",      0x001c
  },
  {
    "CORE_STATUS",      0x0020
  },
  {
    "MANUAL_MODE_CFG",  0x0024
  },
  {
    "PHY_MODE_CFG",     0x0100
  },
  {
    "PHY_CLK_CFG",      0x0104
  },
  {
    "PHY_LP2HS",        0x010c
  },
  {
    "PHY_HS2LP",        0x0114
  },
  {
    "PHY_IPI_RATIO",    0x0134
  },
  {
    "PHY_SYS_RATIO",    0x013c
  },
  {
    "DSI_GENERAL_CFG",  0x0200
  },
  {
    "DSI_VCID_CFG",     0x0204
  },
  {
    "DSI_VID_TX_CFG",   0x020c
  },
  {
    "IPI_COLOR_MAN",    0x0300
  },
  {
    "IPI_VID_HSA",      0x0304
  },
  {
    "IPI_VID_HBP",      0x030c
  },
  {
    "IPI_VID_HACT",     0x0314
  },
  {
    "IPI_VID_HLINE",    0x031c
  },
  {
    "IPI_VID_VSA",      0x0324
  },
  {
    "IPI_VID_VBP",      0x032c
  },
  {
    "IPI_VID_VACT",     0x0334
  },
  {
    "IPI_VID_VFP",      0x033c
  },
  {
    "IPI_PIX_PKT_CFG",  0x0344
  },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t dsi_getreg(uint32_t off)
{
  return getreg32(DSI_BASE + off);
}

static inline void dsi_putreg(uint32_t off, uint32_t val)
{
  putreg32(val, DSI_BASE + off);
}

/****************************************************************************
 * Name: dsi_wait_cri_idle
 *
 * Description:
 *   等命令接口空闲。上一条命令还在发时下发新命令会丢包。
 *
 ****************************************************************************/

static int dsi_wait_cri_idle(void)
{
  int us;

  for (us = 0; us < DSI_CMD_TIMEOUT_US; us++)
    {
      if ((dsi_getreg(RK3576_DSI2_CORE_STATUS) & DSI2_CRI_BUSY) == 0)
        {
          return OK;
        }

      up_udelay(1);
    }

  syslog(LOG_ERR, "DSI2: 命令接口忙超时 STATUS=0x%08" PRIx32 "\n",
         dsi_getreg(RK3576_DSI2_CORE_STATUS));
  return -ETIMEDOUT;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int rk3576_dsi2_probe(void)
{
  uint32_t status;
  int ret;
  uint32_t sel;

  ret = rk3576_power_on(RK3576_PD_VO0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "DSI2: PD_VO0 上电失败: %d\n", ret);
      return ret;
    }

  rk3576_clk_gate(DSI_GATE_CON, DSI_GATE_PCLK, true);
  rk3576_clk_gate(DSI_GATE_CON, DSI_GATE_CLK, true);

  /* ★ CLK_DSIHOST0 是带分频器的 COMPOSITE，不是单纯的门控：
   *     COMPOSITE(CLK_DSIHOST0, ... gpll_cpll_spll_vpll_bpll_lpll_p,
   *               RK3576_CLKSEL_CON(151), 7, 3, MFLAGS,  <- 选源
   *                                       0, 7, DFLAGS,  <- 分频
   *               RK3576_CLKGATE_CON(64), 6, GFLAGS)
   *
   * 只开门控、不管选源与分频，是本端口在别的外设上已经踩过的坑。
   * 这条时钟同时是 DSI 的 sys_clk —— PHY_SYS_RATIO 要用它的真实频率。
   * 这里先把当前值读出来，有了实测再谈配置，不猜。
   */

  /* 明确设成 spll(702MHz) / 3 = 234MHz。实测引导器留下的正是这个值，
   * 但写死一遍才能保证 PHY_SYS_RATIO 用的频率与实际一致 —— 依赖
   * 引导器留下的配置，等于把一个不受控的量代进了公式。
   */

  rk3576_clk_setmux(DSI_SEL_CON, DSI_SEL_MUX_SHIFT, 3, DSI_SEL_MUX_SPLL);
  rk3576_clk_setmux(DSI_SEL_CON, 0, 7, DSI_SYS_CLK_DIV - 1);

  sel = getreg32(RK3576_CRU_ADDR + RK3576_CRU_CLKSEL_CON(151));

  syslog(LOG_INFO,
         "DSI2: CLKSEL_CON(151)=0x%08" PRIx32 " 选源=%" PRIu32
         " 分频=%" PRIu32 " -> sys_clk=%" PRIu32 "Hz\n",
         sel, (sel >> 7) & 0x7, (sel & 0x7f) + 1,
         (uint32_t)DSI_SYS_CLK_HZ);

  /* 自检：DSI2 没有版本寄存器，改用"写一个可写位再读回"。
   * SOFT_RESET 的三个复位位可读可写，适合做这个判断。
   */

  dsi_putreg(RK3576_DSI2_SOFT_RESET, 0);
  if (dsi_getreg(RK3576_DSI2_SOFT_RESET) != 0)
    {
      syslog(LOG_ERR, "DSI2: SOFT_RESET 写 0 读回 0x%08" PRIx32
                      " —— 电源域/时钟/基址有问题\n",
             dsi_getreg(RK3576_DSI2_SOFT_RESET));
      return -ENODEV;
    }

  dsi_putreg(RK3576_DSI2_SOFT_RESET,
             DSI2_SYS_RSTN | DSI2_PHY_RSTN | DSI2_IPI_RSTN);
  status = dsi_getreg(RK3576_DSI2_SOFT_RESET);

  if (status != (DSI2_SYS_RSTN | DSI2_PHY_RSTN | DSI2_IPI_RSTN))
    {
      syslog(LOG_ERR, "DSI2: SOFT_RESET 回读 0x%08" PRIx32 " 与写入不符\n",
             status);
      return -ENODEV;
    }

  syslog(LOG_INFO, "DSI2: 寄存器可访问，SOFT_RESET=0x%08" PRIx32
                   " CORE_STATUS=0x%08" PRIx32 "\n",
         status, dsi_getreg(RK3576_DSI2_CORE_STATUS));
  return OK;
}

int rk3576_dsi2_configure(const struct rk3576_vop2_timing_s *t, int lanes,
                          uint32_t lane_mbps)
{
  uint64_t phy_hs_clk;
  uint64_t pixel_clk;
  uint32_t hline;
  uint64_t ipi_clk;
  uint32_t ipi_ratio;
  uint32_t sys_ratio;
  uint64_t ui_ps;
  uint64_t tick_ps;
  uint64_t lp2hs;
  uint64_t trail;
  uint64_t hs2lp;

  if (t == NULL || lanes < 1 || lanes > 4 || lane_mbps == 0)
    {
      return -EINVAL;
    }

  /* 先断电再配置，最后上电 —— PWR_UP 置位后多数配置寄存器写不进去 */

  dsi_putreg(RK3576_DSI2_PWR_UP, DSI2_RESET);

  dsi_putreg(RK3576_DSI2_SOFT_RESET,
             DSI2_SYS_RSTN | DSI2_PHY_RSTN | DSI2_IPI_RSTN);

  /* PHY：D-PHY、指定通道数、连续时钟。
   * PPI_WIDTH 取 1（16 位），与 RK3576 的 D-PHY 匹配。
   */

  dsi_putreg(RK3576_DSI2_PHY_MODE_CFG,
             DSI2_PHY_TYPE_DPHY | DSI2_PHY_LANES(lanes) | DSI2_PPI_WIDTH(1));
  dsi_putreg(RK3576_DSI2_PHY_CLK_CFG,
             DSI2_CONTINUOUS_CLK | DSI2_PHY_LPTX_CLK_DIV(8));

  /* 手动时序模式。名字带 _MAN_ 的那批寄存器只有在这个模式下才生效，
   * 不开的话下面写的时序全部被忽略。
   */

  dsi_putreg(RK3576_DSI2_MANUAL_MODE_CFG, DSI2_MANUAL_MODE_EN);

  /* 每包最大像素数 = 一行的有效像素 */

  dsi_putreg(RK3576_DSI2_IPI_PIX_PKT_CFG, t->hactive & 0xffff);

  /* 像素格式 RGB888 = 每分量 8 位 */

  dsi_putreg(RK3576_DSI2_IPI_COLOR_MAN_CFG,
             (DSI2_IPI_DEPTH_8_BITS << DSI2_IPI_DEPTH_SHIFT) |
             DSI2_IPI_FORMAT_RGB);

  /* ★ 水平时序的单位不是像素，是 PHY 高速时钟域的时间（16 位定点）。
   *
   *   phy_hs_clk = lane_mbps * 1e6 / 16
   *   H*_TIME    = H* 像素数 * phy_hs_clk * 65536 / pixel_clk
   *
   * 出处 drivers/gpu/drm/bridge/synopsys/dw-mipi-dsi2.c 的
   * dw_mipi_dsi2_ipi_set()。此前这里直接写了像素数 —— 数值差了几个
   * 数量级，DSI 拼不出合法的视频包，现象就是背光亮、无图像。
   *
   * 用 64 位算：hline(828) * phy_hs_clk(24.375e6) << 16 会溢出 32 位。
   */

  phy_hs_clk = ((uint64_t)lane_mbps * 1000000ull + 8) / 16;
  pixel_clk = t->pixclk_hz;
  hline = t->hactive + t->hfront_porch +
               t->hsync_len + t->hback_porch;

  dsi_putreg(RK3576_DSI2_IPI_VID_HSA_MAN_CFG,
             (uint32_t)(((uint64_t)t->hsync_len * phy_hs_clk << 16) /
                        pixel_clk) & 0x3fffffff);
  dsi_putreg(RK3576_DSI2_IPI_VID_HBP_MAN_CFG,
             (uint32_t)(((uint64_t)t->hback_porch * phy_hs_clk << 16) /
                        pixel_clk) & 0x3fffffff);
  dsi_putreg(RK3576_DSI2_IPI_VID_HACT_MAN_CFG,
             (uint32_t)(((uint64_t)t->hactive * phy_hs_clk << 16) /
                        pixel_clk) & 0x3fffffff);
  dsi_putreg(RK3576_DSI2_IPI_VID_HLINE_MAN_CFG,
             (uint32_t)(((uint64_t)hline * phy_hs_clk << 16) /
                        pixel_clk) & 0x3fffffff);

  /* 垂直时序的单位是行，直接写。
   * ★ 此前只写了 VSA，缺 VBP/VACT/VFP —— DSI 不知道一帧有多少行。
   */

  dsi_putreg(RK3576_DSI2_IPI_VID_VSA_MAN_CFG,  t->vsync_len   & 0x3ff);
  dsi_putreg(RK3576_DSI2_IPI_VID_VBP_MAN_CFG,  t->vback_porch & 0x3ff);
  dsi_putreg(RK3576_DSI2_IPI_VID_VACT_MAN_CFG, t->vactive     & 0x3fff);
  dsi_putreg(RK3576_DSI2_IPI_VID_VFP_MAN_CFG,  t->vfront_porch & 0x3ff);

  /* ★ 三个时钟域的比值。此前完全没写过。
   *
   * DSI 内部有三个时钟域：PHY 高速发送时钟、IPI 像素时钟、系统时钟。
   * 这两个寄存器告诉控制器它们的比值（16 位定点），控制器据此把进来的
   * 像素流与出去的高速通道对齐速率。缺了它们，其余配置全对也出不来
   * 有效画面 —— 与"背光亮、VP 在扫描、DSI 已进视频模式却仍无显示"
   * 的现象完全吻合。
   *
   *   PHY_IPI_RATIO = phy_hs_clk / (pixel_clk / 4) << 16
   *   PHY_SYS_RATIO = phy_hs_clk / sys_clk        << 16
   *
   * 出处 dw_mipi_dsi2_phy_ratio_cfg()。
   */

  phy_hs_clk = ((uint64_t)lane_mbps * 1000000ull + 8) / 16;
  ipi_clk = (uint64_t)t->pixclk_hz / 4;

  ipi_ratio = (uint32_t)((phy_hs_clk << 16) / ipi_clk) & 0x3fffff;
  sys_ratio = (uint32_t)((phy_hs_clk << 16) / DSI_SYS_CLK_HZ) & 0x1ffff;

  dsi_putreg(RK3576_DSI2_PHY_IPI_RATIO_MAN_CFG, ipi_ratio);
  dsi_putreg(RK3576_DSI2_PHY_SYS_RATIO_MAN_CFG, sys_ratio);

  /* LP<->HS 转换时间，单位同样是 PHY 高速时钟周期的 16 位定点。
   * 括号里是 MIPI D-PHY 规范的默认参数（单位 ps，UI = 1e12/lane_bps）：
   *   LP2HS = TLPX + THS_PREPARE + THS_ZERO
   *         = 50000 + (40000 + 4*UI) + (105000 + 6*UI)
   *   HS2LP = THS_TRAIL + THS_EXIT
   *         = max(8*UI, 60000 + 4*UI) + 100000
   */

  ui_ps = 1000000000000ull / ((uint64_t)lane_mbps * 1000000ull);
  tick_ps = 1000000000000ull / phy_hs_clk;
  lp2hs = 50000 + (40000 + 4 * ui_ps) + (105000 + 6 * ui_ps);
  trail = 8 * ui_ps;

  if (trail < 60000 + 4 * ui_ps)
    {
      trail = 60000 + 4 * ui_ps;
    }

  hs2lp = trail + 100000;

  dsi_putreg(RK3576_DSI2_PHY_LP2HS_MAN_CFG,
             (uint32_t)((lp2hs << 16) / tick_ps) & 0x1fffffff);
  dsi_putreg(RK3576_DSI2_PHY_HS2LP_MAN_CFG,
             (uint32_t)((hs2lp << 16) / tick_ps) & 0x1fffffff);

  /* 视频模式类型。非突发 + 同步事件是最通用的一档；消隐段走高速
   * （BLK_*_HS_EN）以避免每行都进出低功耗态。
   */

  dsi_putreg(RK3576_DSI2_DSI_VID_TX_CFG,
             DSI2_VID_MODE_NON_BURST_SYNC_EVENTS |
             DSI2_BLK_HSA_HS_EN | DSI2_BLK_HBP_HS_EN | DSI2_BLK_HFP_HS_EN);

  /* 关掉 EOTP 与 BTA：面板 dtsi 的 flags 里带 NO_EOT_PACKET，
   * 且初始化阶段不需要读回。
   */

  dsi_putreg(RK3576_DSI2_DSI_GENERAL_CFG, 0);
  dsi_putreg(RK3576_DSI2_DSI_VCID_CFG, 0);

  /* 先进命令模式，供发送面板初始化序列 */

  dsi_putreg(RK3576_DSI2_MODE_CTRL, DSI2_MODE_CMD);

  dsi_putreg(RK3576_DSI2_PWR_UP, DSI2_POWER_UP);

  syslog(LOG_INFO,
         "DSI2: 已配置 %ux%u %d lane @%" PRIu32 "Mbps，当前为命令模式\n",
         t->hactive, t->vactive, lanes, lane_mbps);

  /* 回读这批时序寄存器。它们是本轮修正的重点，而且只在 MANUAL 模式下
   * 生效 —— 回读为 0 就说明手动模式没开或写入被忽略，光看"写过了"
   * 分辨不出来。
   */

  syslog(LOG_INFO,
         "DSI2: 回读 MANUAL=%" PRIu32 " HSA=%" PRIu32 " HBP=%" PRIu32
         " HACT=%" PRIu32 " HLINE=%" PRIu32 "\n",
         dsi_getreg(RK3576_DSI2_MANUAL_MODE_CFG),
         dsi_getreg(RK3576_DSI2_IPI_VID_HSA_MAN_CFG),
         dsi_getreg(RK3576_DSI2_IPI_VID_HBP_MAN_CFG),
         dsi_getreg(RK3576_DSI2_IPI_VID_HACT_MAN_CFG),
         dsi_getreg(RK3576_DSI2_IPI_VID_HLINE_MAN_CFG));

  syslog(LOG_INFO,
         "DSI2: 回读 VSA=%" PRIu32 " VBP=%" PRIu32 " VACT=%" PRIu32
         " VFP=%" PRIu32 " PIXPKT=%" PRIu32 " VIDTX=0x%08" PRIx32 "\n",
         dsi_getreg(RK3576_DSI2_IPI_VID_VSA_MAN_CFG),
         dsi_getreg(RK3576_DSI2_IPI_VID_VBP_MAN_CFG),
         dsi_getreg(RK3576_DSI2_IPI_VID_VACT_MAN_CFG),
         dsi_getreg(RK3576_DSI2_IPI_VID_VFP_MAN_CFG),
         dsi_getreg(RK3576_DSI2_IPI_PIX_PKT_CFG),
         dsi_getreg(RK3576_DSI2_DSI_VID_TX_CFG));

  syslog(LOG_INFO,
         "DSI2: 回读 IPI_RATIO=%" PRIu32 " SYS_RATIO=%" PRIu32
         " LP2HS=%" PRIu32 " HS2LP=%" PRIu32 "\n",
         dsi_getreg(RK3576_DSI2_PHY_IPI_RATIO_MAN_CFG),
         dsi_getreg(RK3576_DSI2_PHY_SYS_RATIO_MAN_CFG),
         dsi_getreg(RK3576_DSI2_PHY_LP2HS_MAN_CFG),
         dsi_getreg(RK3576_DSI2_PHY_HS2LP_MAN_CFG));
  return OK;
}

int rk3576_dsi2_send_cmd(uint8_t dtype, const uint8_t *data, size_t len)
{
  uint32_t hdr;
  uint32_t word;
  size_t i;
  int ret;

  ret = dsi_wait_cri_idle();
  if (ret < 0)
    {
      return ret;
    }

  if (len <= 2)
    {
      /* 短命令：数据直接放在包头的两个字节里，没有负载 */

      hdr = dtype;
      if (len >= 1)
        {
          hdr |= (uint32_t)data[0] << 8;
        }

      if (len >= 2)
        {
          hdr |= (uint32_t)data[1] << 16;
        }
    }
  else
    {
      /* 长命令：先把负载按 4 字节一组写进 PLD，再写包头触发发送。
       * ★ 顺序不能反 —— 先写包头会让控制器在负载就位前就开始发。
       */

      for (i = 0; i < len; i += 4)
        {
          size_t n = (len - i) > 4 ? 4 : (len - i);

          word = 0;
          memcpy(&word, data + i, n);
          dsi_putreg(RK3576_DSI2_CRI_TX_PLD, word);
        }

      hdr = dtype | ((uint32_t)(len & 0xff) << 8) |
            ((uint32_t)((len >> 8) & 0xff) << 16);
    }

  /* bit24 = 低功耗模式发送。面板初始化序列按规范应走 LP 模式。 */

  dsi_putreg(RK3576_DSI2_CRI_TX_HDR, hdr | (1u << 24));
  return dsi_wait_cri_idle();
}

int rk3576_dsi2_set_video_mode(void)
{
  uint32_t status = 0;
  int us;

  dsi_putreg(RK3576_DSI2_MODE_CTRL, DSI2_MODE_VIDEO);

  /* 轮询到状态真的变成 VIDEO 为止，不靠固定延时后读一次。
   * 参考驱动的超时是 10ms。
   */

  for (us = 0; us < 10000; us++)
    {
      status = dsi_getreg(RK3576_DSI2_MODE_STATUS);
      if (status == DSI2_MODE_VIDEO)
        {
          syslog(LOG_INFO,
                 "DSI2: 已进入视频模式（等待 %d us）\n", us);
          return OK;
        }

      up_udelay(1);
    }

  syslog(LOG_ERR,
         "DSI2: 切视频模式超时 MODE_STATUS=0x%08" PRIx32
         "（期望 %d）\n", status, DSI2_MODE_VIDEO);
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3576_dsi2_dump_uboot_state
 *
 * Description:
 *   打印 U-Boot 留下的 DSI 寄存器状态，在本驱动写入之前调用。
 *   出厂固件已经用这套配置在这块屏上显示过 logo。
 *
 ****************************************************************************/

void rk3576_dsi2_dump_uboot_state(void)
{
  int i;

  syslog(LOG_INFO, "==== U-Boot 留下的 DSI 状态（未改动）====\n");

  for (i = 0;
       i < (int)(sizeof(g_dsi2_uboot_regs) / sizeof(g_dsi2_uboot_regs[0]));
       i++)
    {
      syslog(LOG_INFO, "  %-18s 0x%08" PRIx32 "\n",
             g_dsi2_uboot_regs[i].name,
             dsi_getreg(g_dsi2_uboot_regs[i].off));
      up_mdelay(3);
    }
}

#endif /* CONFIG_RK3576_DSI2 */
