/****************************************************************************
 * chip/rk3576/rk3576_vop2.c
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

/* RK3576 VOP2 显示控制器 —— 第一步：电源域/时钟自检 + 内置彩条。
 *
 * 显示链路分三块：VOP2（本文件）、MIPI DSI2 主机、MIPI D-PHY。
 * 逐块验证，每块都要有独立判据，否则屏不亮时无从判断是哪一环。
 *
 * ★ 本文件的判据是"彩条"。
 *
 *   VOP 内置了彩条发生器，不需要帧缓冲、图层、也不经过内存 —— 只要
 *   时序发生器和像素时钟对了、且到 MIPI 接口的通路通了，屏上就有彩条。
 *   这把"VOP 配置对不对"与"帧缓冲/图层/DMA 对不对"彻底分开了。
 *
 * ★ 配置要写 CFG_DONE 才生效。
 *
 *   VOP2 的大部分寄存器是"影子寄存器"：写入后不立即作用于硬件，
 *   要往 REG_CFG_DONE 写使能位才整批提交。漏掉这一步时所有配置看似
 *   写成功（回读也对），但显示毫无变化 —— 这是 VOP 调试最常见的坑。
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

#include <nuttx/arch.h>

#include "arm64_internal.h"
#include "rk3576_vop2.h"
#include "rk3576_cru.h"
#include "rk3576_power.h"
#include "hardware/rk3576_vop2.h"
#include "hardware/rk3576_memorymap.h"

#ifdef CONFIG_RK3576_VOP2

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VOP2_BASE       RK3576_VOP2_ADDR
#define VP0(off)        (RK3576_VOP2_VP0_BASE + (off))
#define VP1(off)        (RK3576_VOP2_VP1_BASE + (off))

/* 时钟。出处：clk-rk3576.c
 *   COMPOSITE(ACLK_VOP_ROOT, ... CLKGATE_CON(61), 0)
 *   COMPOSITE_NODIV(HCLK_VOP_ROOT, ... CLKGATE_CON(61), 2)
 *   GATE(HCLK_VOP, ... CLKGATE_CON(61), 8)
 *   GATE(ACLK_VOP, ... CLKGATE_CON(61), 9)
 *   COMPOSITE(DCLK_VP0_SRC, ... CLKGATE_CON(61), 10)
 *   COMPOSITE_NODIV(DCLK_VP0, ... CLKGATE_CON(61), 13)
 */

#define VOP_GATE_CON        61
#define VOP_GATE_ACLK_ROOT  0
#define VOP_GATE_HCLK_ROOT  2
#define VOP_GATE_HCLK       8
#define VOP_GATE_ACLK       9
#define VOP_GATE_DCLK_SRC   10
#define VOP_GATE_DCLK_VP0   13

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* 寄存器 dump 用的名字表 */

struct vop2_regname_s
{
  const char *name;
  uint32_t    off;
};

/* U-Boot 留下的窗口配置。
 *
 * ★ 在覆盖之前存下来，因为它是**唯一一组被实测验证过能正常显示**的
 *   参数 —— U-Boot 用它把 logo 显示出来了。我们自己配的参数取出来的
 *   画面是错乱的，原因未明；有了这份备份，就能随时退回一个已知可用的
 *   状态，把"显示通路本身有没有问题"和"我的配置对不对"分开验证。
 */

struct vop2_uboot_win_s
{
  uint32_t mst;
  uint32_t vir;
  uint32_t act;
  uint32_t dsp;
  uint32_t dsp_st;
  uint32_t ctrl;
  bool     valid;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* rk3576_vop2_dump_uboot_state()：U-Boot 留下的关键寄存器
 * （相对 VOP2 基址）
 */

static const struct vop2_regname_s g_vop2_uboot_regs[] =
{
  {
    "SYS_CTRL_MIPI0_INFACE", 0x0180
  },
  {
    "SYS_AUTO_GATING",       0x0008
  },
  {
    "SYS_PORT_CTRL_IMD",     0x0028
  },
  {
    "VP0_DSP_CTRL",          0x0c00
  },
  {
    "VP0_MIPI_CTRL",         0x0c04
  },
  {
    "VP0_COLOR_BAR_CTRL",    0x0c08
  },
  {
    "VP0_HTOTAL_HS_END",     0x0c48
  },
  {
    "VP0_HACT_ST_END",       0x0c4c
  },
  {
    "VP0_VTOTAL_VS_END",     0x0c50
  },
  {
    "VP0_VACT_ST_END",       0x0c54
  },
  {
    "VP1_DSP_CTRL",          0x0d00
  },
  {
    "VP1_MIPI_CTRL",         0x0d04
  },
  {
    "VP1_COLOR_BAR_CTRL",    0x0d08
  },
  {
    "VP1_HTOTAL_HS_END",     0x0d48
  },
  {
    "VP1_HACT_ST_END",       0x0d4c
  },
  {
    "VP1_VTOTAL_VS_END",     0x0d50
  },
  {
    "VP1_VACT_ST_END",       0x0d54
  },
  {
    "VP2_DSP_CTRL",          0x0e00
  },
  {
    "ESMART0_REGION0_CTRL",  0x1810
  },
  {
    "ESMART0_REGION0_MST",   0x1814
  },
  {
    "ESMART0_REGION0_VIR",   0x181c
  },
  {
    "ESMART0_REGION0_ACT",   0x1820
  },
  {
    "ESMART0_REGION0_DSP",   0x1824
  },
  {
    "OVL_CTRL_VP0",          0x0600
  },
  {
    "OVL_LAYER_SEL_VP0",     0x0604
  },
  {
    "OVL_CTRL_VP1",          0x0700
  },
  {
    "OVL_LAYER_SEL_VP1",     0x0704
  },

  /* ESMART0 全为 0，说明 U-Boot 的 logo 走的是别的图层。
   * Cluster 是带 AFBC 的图层，Rockchip 的 U-Boot logo 常用它。
   */

  {
    "CLUSTER0_WIN0_CTRL0",   0x1000
  },
  {
    "CLUSTER0_WIN0_YRGB_MST", 0x1010
  },
  {
    "CLUSTER0_WIN0_VIR",     0x101c
  },
  {
    "CLUSTER0_WIN0_ACT_INFO", 0x1020
  },
  {
    "CLUSTER0_WIN0_DSP_INFO", 0x1024
  },
  {
    "CLUSTER0_WIN0_DSP_ST",  0x1028
  },
  {
    "CLUSTER1_WIN0_CTRL0",   0x1200
  },
  {
    "CLUSTER1_WIN0_YRGB_MST", 0x1210
  },
  {
    "ESMART1_REGION0_CTRL",  0x1a10
  },
  {
    "ESMART1_REGION0_MST",   0x1a14
  },
};

/* rk3576_vop2_dump_win()：ESMART1 图层寄存器块（相对图层基址） */

static const struct vop2_regname_s g_vop2_win_regs[] =
{
  {
    "ESMART_CTRL0",      RK3576_SMART_CTRL0
  },
  {
    "ESMART_CTRL1",      RK3576_SMART_CTRL1
  },
  {
    "AXI_CTRL_IMD",      RK3576_SMART_AXI_CTRL_IMD
  },
  {
    "REGION0_MST_CTL",   RK3576_SMART_REGION0_CTRL
  },
  {
    "REGION0_MST_YRGB",  RK3576_SMART_REGION0_YRGB_MST
  },
  {
    "REGION0_MST_CBCR",  0x18
  },
  {
    "REGION0_VIR",       RK3576_SMART_REGION0_VIR
  },
  {
    "REGION0_ACT_INFO",  RK3576_SMART_REGION0_ACT_INFO
  },
  {
    "REGION0_DSP_INFO",  RK3576_SMART_REGION0_DSP_INFO
  },
  {
    "REGION0_DSP_OFFSET", RK3576_SMART_REGION0_DSP_ST
  },
  {
    "REGION0_SCL_CTRL",  RK3576_SMART_REGION0_SCL_CTRL
  },
  {
    "REGION0_SCL_FACTOR", RK3576_SMART_REGION0_SCL_FACTOR_YRGB
  },
  {
    "REGION0_SCL_OFFSET", 0x3c
  },
  {
    "REGION1_MST_CTL",   0x40
  },
  {
    "PORT_SEL_IMD",      RK3576_SMART_PORT_SEL_IMD
  },
};

static struct vop2_uboot_win_s g_uboot_win;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t vop_getreg(uint32_t off)
{
  return getreg32(VOP2_BASE + off);
}

static inline void vop_putreg(uint32_t off, uint32_t val)
{
  putreg32(val, VOP2_BASE + off);
}

/****************************************************************************
 * Name: vop_cfg_done
 *
 * Description:
 *   提交影子寄存器。
 *
 *   ★ 提交是**按视频端口分别生效**的，必须指定端口号；而且低位的数据
 *     还要配上高 16 位的写使能掩码：
 *
 *       drivers/gpu/drm/rockchip/rockchip_drm_vop2.h vop2_cfg_done()
 *         val = GLB_CFG_DONE_EN | BIT(vp->id) | (BIT(vp->id) << 16);
 *
 *     此前这里写死了 GLB_EN | 1 —— 提交的是 VP0，掩码位也没给。而本板
 *     的画面在 VP1 上（U-Boot 的配置），于是图层寄存器写进了影子寄存器
 *     却从未被提交，帧缓冲地址改了也毫无效果。
 *
 *     GLB_CFG_DONE_EN 本身没有掩码位，直接写。
 *
 ****************************************************************************/

static void vop_cfg_done(int vp)
{
  vop_putreg(RK3576_VOP2_REG_CFG_DONE,
             RK3576_VOP2_CFG_DONE_GLB_EN |
             (1u << vp) | (1u << (vp + 16)));
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int rk3576_vop2_probe(void)
{
  uint32_t version;
  int ret;

  ret = rk3576_power_on(RK3576_PD_VOP);
  if (ret < 0)
    {
      syslog(LOG_ERR, "VOP2: PD_VOP 上电失败: %d\n", ret);
      return ret;
    }

  /* 六路时钟。root 是总线时钟树的根，不开的话下面的 ACLK/HCLK 也没用。
   * DCLK_VP0 是像素时钟，彩条也要靠它才有输出。
   */

  rk3576_clk_gate(VOP_GATE_CON, VOP_GATE_ACLK_ROOT, true);
  rk3576_clk_gate(VOP_GATE_CON, VOP_GATE_HCLK_ROOT, true);
  rk3576_clk_gate(VOP_GATE_CON, VOP_GATE_HCLK, true);
  rk3576_clk_gate(VOP_GATE_CON, VOP_GATE_ACLK, true);
  rk3576_clk_gate(VOP_GATE_CON, VOP_GATE_DCLK_SRC, true);
  rk3576_clk_gate(VOP_GATE_CON, VOP_GATE_DCLK_VP0, true);

  version = vop_getreg(RK3576_VOP2_VERSION_INFO);
  if (version == 0 || version == 0xffffffff)
    {
      syslog(LOG_ERR,
             "VOP2: VERSION=0x%08" PRIx32 " —— 电源域/时钟/基址有问题\n",
             version);
      return -ENODEV;
    }

  syslog(LOG_INFO,
         "VOP2: VERSION=0x%08" PRIx32 " MIPI0_IF=0x%08" PRIx32 "\n",
         version, vop_getreg(RK3576_VOP2_MIPI0_IF_CTRL));
  return OK;
}

int rk3576_vop2_colorbar(const struct rk3576_vop2_timing_s *t, bool enable)
{
  uint32_t htotal;
  uint32_t vtotal;
  uint32_t hact_st;
  uint32_t vact_st;
  uint32_t iface;

  if (t == NULL)
    {
      return -EINVAL;
    }

  if (!enable)
    {
      vop_putreg(VP0(RK3576_VP_COLOR_BAR_CTRL), 0);
      vop_putreg(VP0(RK3576_VP_DSP_CTRL),
                 vop_getreg(VP0(RK3576_VP_DSP_CTRL)) |
                 RK3576_VP_DSP_CTRL_STANDBY);
      vop_cfg_done(0);
      return OK;
    }

  htotal  = t->hactive + t->hfront_porch + t->hsync_len + t->hback_porch;
  vtotal  = t->vactive + t->vfront_porch + t->vsync_len + t->vback_porch;

  /* 有效区起点 = 同步脉冲 + 后肩。VOP 的 ST_END 寄存器存的是
   * (起点 << 16) | 终点，两者都相对于同步脉冲的起始。
   */

  hact_st = t->hsync_len + t->hback_porch;
  vact_st = t->vsync_len + t->vback_porch;

  vop_putreg(VP0(RK3576_VP_DSP_HTOTAL_HS_END),
             (htotal << 16) | t->hsync_len);
  vop_putreg(VP0(RK3576_VP_DSP_HACT_ST_END),
             (hact_st << 16) | (hact_st + t->hactive));
  vop_putreg(VP0(RK3576_VP_DSP_VTOTAL_VS_END),
             (vtotal << 16) | t->vsync_len);
  vop_putreg(VP0(RK3576_VP_DSP_VACT_ST_END),
             (vact_st << 16) | (vact_st + t->vactive));

  vop_putreg(VP0(RK3576_VP_POST_DSP_HACT_INFO),
             (hact_st << 16) | (hact_st + t->hactive));
  vop_putreg(VP0(RK3576_VP_POST_DSP_VACT_INFO),
             (vact_st << 16) | (vact_st + t->vactive));

  /* ★ 把 MIPI 接口接到 VP0。
   *
   * 引导器留下的是 0x80100037，其中 port_sel=01 指向 VP1 —— 而上面这
   * 一大段配的全是 VP0。不改这一位的话，VP0 扫描、时序、DSI 全部正常，
   * 像素却送不进 DSI，现象是背光亮但无显示，且链路上每一处自检都通过。
   *
   * 像素时钟保持 Div4：DSI 的 PHY_IPI_RATIO 就是按 pixclk/4 算的，
   * 两处必须一致。
   */

  vop_putreg(RK3576_VOP2_MIPI0_IF_CTRL,
             RK3576_MIPI_IF_REGDONE_IMD |
             RK3576_MIPI_IF_PIXCLK_DIV4 |
             RK3576_MIPI_IF_VSYNC_POS |
             RK3576_MIPI_IF_HSYNC_POS |
             (0u << RK3576_MIPI_IF_PORT_SEL_SHIFT) |   /* VP0 */
             RK3576_MIPI_IF_CLK_OUT_EN |
             RK3576_MIPI_IF_OUT_EN);

  /* 输出格式 RGB888，撤掉 standby 让端口开始扫描 */

  vop_putreg(VP0(RK3576_VP_DSP_CTRL), RK3576_VP_DSP_CTRL_OUT_MODE_RGB888);

  /* 把 VP0 接到 MIPI0 接口。
   * ★ RK3576 每个接口一个独立寄存器，与 RK3568 用位域选接口完全不同。
   */

  vop_putreg(RK3576_VOP2_MIPI0_IF_CTRL, 1);

  /* 打开内置彩条 */

  vop_putreg(VP0(RK3576_VP_COLOR_BAR_CTRL), 1);

  vop_cfg_done(0);

  syslog(LOG_INFO,
         "VOP2: 彩条已开 %ux%u htotal=%" PRIu32 " vtotal=%" PRIu32
         " pixclk=%" PRIu32 "Hz\n",
         t->hactive, t->vactive, htotal, vtotal, t->pixclk_hz);

  /* 回读接口路由，确认 port_sel 真的落到了 VP0（bit[3:2] 应为 0）。 */

  iface = vop_getreg(RK3576_VOP2_MIPI0_IF_CTRL);

  syslog(LOG_INFO,
         "VOP2: MIPI0 接口 0x%08" PRIx32 " 取自 VP%" PRIu32
         " out_en=%" PRIu32 " clk_out_en=%" PRIu32 "\n",
         iface, (iface >> RK3576_MIPI_IF_PORT_SEL_SHIFT) & 3,
         iface & RK3576_MIPI_IF_OUT_EN ? 1 : 0,
         (iface & RK3576_MIPI_IF_CLK_OUT_EN) ? 1 : 0);

  return OK;
}

#endif /* CONFIG_RK3576_VOP2 */

/****************************************************************************
 * Name: rk3576_vop2_check_scanning
 *
 * Description:
 *   判断视频端口是否真的在扫描输出。
 *
 *   寄存器读回一致只能证明"写进去了"，证明不了像素在动。这里清掉帧起始
 *   的原始中断状态，等一帧以上再读 —— 位起来了才说明 VP 确实在按时序
 *   往外送像素。这是整条显示链路上第一个能区分"配置对了"和"真的在跑"
 *   的观测。
 *
 *   POST_BUF_EMPTY 一并打出来：它置位说明 VP 在扫描但取不到数据
 *   （图层没使能、帧缓冲地址错、AXI 带宽不够），与"完全没扫描"是
 *   两种不同的故障。
 *
 ****************************************************************************/

int rk3576_vop2_check_scanning(int vp)
{
  uint32_t raw;
  uint32_t frame_us;

  /* 清原始状态。这些中断寄存器同样是高 16 位写使能掩码。 */

  putreg32(0xffffu << 16 | 0xffffu,
           RK3576_VOP2_ADDR + RK3576_VOP2_VP_INT_CLR(vp));

  /* 等两帧。720x1280@65MHz、htotal 828 vtotal 1317 -> 约 16.8ms/帧。 */

  frame_us = 40000;
  up_udelay(frame_us);

  raw = getreg32(RK3576_VOP2_ADDR + RK3576_VOP2_VP_INT_RAW(vp));

  syslog(LOG_INFO,
         "VOP2: VP%d 扫描判据 RAW=0x%08" PRIx32 " —— %s%s\n",
         vp, raw,
         (raw & RK3576_VP_INT_FS) ? "有帧起始，VP 正在扫描" :
                                    "无帧起始，VP 没有输出",
         (raw & RK3576_VP_INT_POST_BUF_EMPTY) ?
           "；且后级缓冲欠载（在扫描但取不到像素）" : "");

  return (raw & RK3576_VP_INT_FS) ? OK : -EIO;
}

/****************************************************************************
 * Name: rk3576_vop2_takeover
 *
 * Description:
 *   接管 U-Boot 已经跑起来的显示，只改帧缓冲地址，其余一概不动。
 *
 *   ★ 为什么是接管而不是重建：
 *     出厂固件在这块屏上显示过 Rockchip logo，寄存器现状表明这条链路
 *     此刻仍是活的 —— VP1 未 standby、DSI 处于视频模式、ESMART1 图层
 *     使能并指向 0xfdf00000。整条链路连同**我们拿不到的那份面板初始化
 *     序列**都已经在跑。重建的话，面板初始化那一环是无论如何补不上的。
 *
 *     所以只做一件事：把图层的帧缓冲地址换成我们自己的缓冲区。
 *     这同时是一次决定性实验 —— 屏幕内容若随之改变，就一次性证实了
 *     VOP2、DSI、D-PHY、面板全都正常。
 *
 *   buffer 必须是物理地址。本端口的 RAM 是恒等映射，虚拟地址即物理地址。
 *
 ****************************************************************************/

int rk3576_vop2_takeover(uintptr_t buffer, uint32_t *width, uint32_t *height,
                         uint32_t *stride_bytes, uint32_t *bpp)
{
  uint32_t ctrl;
  uint32_t act;
  uint32_t dsp;
  uint32_t vir;
  uint32_t fmt;
  uint32_t w;
  uint32_t h;

  ctrl = vop_getreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_CTRL);
  act  = vop_getreg(RK3576_VOP2_ESMART1_BASE +
                    RK3576_SMART_REGION0_ACT_INFO);
  dsp  = vop_getreg(RK3576_VOP2_ESMART1_BASE +
                    RK3576_SMART_REGION0_DSP_INFO);
  vir  = vop_getreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_VIR);

  syslog(LOG_INFO,
         "VOP2: 接管 ESMART1 CTRL=0x%08" PRIx32 " ACT=0x%08" PRIx32
         " DSP=0x%08" PRIx32 " VIR=%" PRIu32 "\n", ctrl, act, dsp, vir);

  if ((ctrl & RK3576_SMART_REGION0_CTRL_EN) == 0)
    {
      syslog(LOG_ERR,
             "ERROR: ESMART1 图层未使能，U-Boot 的显示不在活动状态\n");
      return -ENODEV;
    }

  /* ACT_INFO 存的是 (h-1)<<16 | (w-1) */

  w = (act & 0xffff) + 1;
  h = ((act >> 16) & 0xffff) + 1;

  fmt = (ctrl >> RK3576_SMART_REGION0_CTRL_FORMAT_SHIFT) & 0x1f;

  syslog(LOG_INFO,
         "VOP2: U-Boot 帧缓冲 %" PRIu32 "x%" PRIu32 " 格式=%" PRIu32
         " 行跨距=%" PRIu32 " 字节\n", w, h, fmt, vir * 4);

  /* buffer 为 0 表示只查询参数，不写寄存器 —— 调用方要先知道尺寸和
   * 跨距才能分配缓冲区。这里若照写不误，会把 MST 置 0 先把画面清掉。
   */

  if (buffer != 0)
    {
      /* 只换地址。格式、尺寸、跨距全部沿用 U-Boot 的设置 —— 它们与
       * 面板和这条链路是配套的，改一个就要改一串。
       */

      vop_putreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_YRGB_MST,
                 (uint32_t)buffer);

      /* ★ ESMART1 归 VP1 所有（OVL_LAYER_SEL_VP1=0xfff2，U-Boot 的配置），
       * 必须提交到 VP1。提交到 VP0 的话寄存器停在影子里，画面不变。
       */

      vop_cfg_done(RK3576_VOP2_MIPI_VP);

      syslog(LOG_INFO,
             "VOP2: 帧缓冲已切到 0x%08lx\n", (unsigned long)buffer);
    }

  if (width  != NULL) *width  = w;
  if (height != NULL) *height = h;
  if (stride_bytes != NULL) *stride_bytes = vir * 4;
  if (bpp != NULL) *bpp = (fmt == RK3576_VOP2_FMT_RGB565) ? 16 :
                          (fmt == RK3576_VOP2_FMT_RGB888) ? 24 : 32;

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop2_fb_pan
 *
 * Description:
 *   把 ESMART1 的取数地址切到另一个缓冲（翻页）。
 *
 * ★ 为什么需要翻页 —— 这才是"花屏"的真正原因
 *
 *   之前只有**一个**帧缓冲：LVGL 往里画的同时，VOP2 正在把同一块内存
 *   逐行扫出去。LVGL 重画一个控件时是"先用背景色填满脏矩形，再把文字
 *   画上去"；扫描线如果正好在这两步之间经过那几行，扫出去的就是**只有
 *   背景、还没有内容**的一条横带。
 *
 *   这完全对得上板上的现象：
 *     - 条纹是**横的**，因为扫描是按行的；
 *     - 条纹**很细**，因为它只有一个控件脏矩形那么高；
 *     - 条纹出现在"shared 下面""第三行"这些**正在刷新的标签**处；
 *     - **一触摸就出现**，因为触摸触发重画；
 *     - k7diag 的 ramp / bars 图案**完全正常**，因为那是一次性写完
 *       就不再改的静态图，没有"边扫边改"。
 *
 *   所以问题从来不在 VOP2 取数（地址/跨距/格式/刷 cache 全是对的），
 *   而在于没有双缓冲。这也是为什么在显示链路上查了几轮都查不到。
 *
 * ★ 参考实现
 *
 *   nuttx/arch/arm/src/stm32h7/stm32_ltdc.c   yres_virtual = HEIGHT * 2
 *   nuttx/arch/sim/src/sim/sim_framebuffer.c  同上
 *   lv_nuttx_fbdev.c 自己就带双缓冲分支：
 *     double_buffer = (pinfo.yres_virtual == vinfo.yres * 2)
 *   —— 驱动不报 yres_virtual，LVGL 就只能走单缓冲那条会撕裂的路。
 *
 *   翻页只改一个寄存器：REGION0_YRGB_MST。它是影子寄存器，CFG_DONE
 *   之后在**下一个 VSYNC** 整帧原子生效，所以切换本身不会撕裂。
 *
 ****************************************************************************/

int rk3576_vop2_fb_pan(uintptr_t buffer)
{
  int us;

  if (buffer == 0)
    {
      return -EINVAL;
    }

  vop_putreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_YRGB_MST,
             (uint32_t)buffer);
  vop_cfg_done(RK3576_VOP2_MIPI_VP);

  /* ★ 等这次翻页真的在帧边界生效再返回 —— 没有这一步就是"没有 vsync"。
   *
   *   CFG_DONE 只是"请求提交"，真正 latch 发生在下一个 VSYNC。而我们的
   *   pandisplay 为了不让 poll 卡死会把 fb 的 paninfo 队列立刻排空，于是
   *   上层（LVGL）完全不受约束地往下画。实测 k7diag anim 能跑到 99fps，
   *   而屏是 60Hz —— LVGL 比硬件快，就会往**正在被扫描的那一块**里画。
   *
   *   板上现象正是"刷新时上一帧没清干净、有花屏和黑色短线"。这不是帧率
   *   太低，是快得没有节制。
   *
   *   做法照原厂：VP1 的中断块里 bit0 是 FS（帧开始）。先把它清掉，再等
   *   它重新置位 —— 等到就说明中间经过了一个帧边界，CFG_DONE 已经 latch。
   *   这样一次 pandisplay 最多阻塞一帧（60Hz 约 17ms），上层自然被压到
   *   屏幕的刷新率上。
   *
   *   只读 INT_RAW（原始状态，不受使能位影响），不去开 VP 的中断使能 ——
   *   NuttX 侧没有注册 VOP2 的中断处理，开了却没人应答会变成中断风暴。
   */

  vop_putreg(RK3576_VOP2_VP1_INT_CLR,
             (RK3576_VOP2_INT_FS << 16) | RK3576_VOP2_INT_FS);

  for (us = 0; us < 25000; us++)     /* 25ms > 60Hz 的一帧 16.7ms */
    {
      if ((vop_getreg(RK3576_VOP2_VP1_INT_RAW) & RK3576_VOP2_INT_FS) != 0)
        {
          break;
        }

      up_udelay(1);
    }

  if (us >= 25000)
    {
      /* 等不到帧开始就别一直等下去 —— 宁可这一帧不同步，也不要把
       * 上层卡死。只报一次，避免刷屏。
       */

      static bool warned = false;

      if (!warned)
        {
          warned = true;
          syslog(LOG_WARNING,
                 "VOP2: 等不到 VP%d 的帧开始（INT_RAW=0x%08" PRIx32
                 "），翻页未与 vsync 同步\n",
                 RK3576_VOP2_MIPI_VP,
                 vop_getreg(RK3576_VOP2_VP1_INT_RAW));
        }
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop2_fb_setup
 *
 * Description:
 *   把 ESMART1 图层扩成全屏并指向我们自己的帧缓冲。
 *
 *   仍然沿用 U-Boot 建立的一切：面板初始化、DSI、D-PHY、VP1 的时序与
 *   MIPI 接口路由。只改图层的几何、跨距和地址。
 *
 *   ★ 图层的显示起点 DSP_ST 是相对于**有效区**的，不含消隐段。
 *
 *     此前这里把 VP1 的 HACT_ST/VACT_ST（即 hsync+hback_porch）当成全屏
 *     摆放的起点写了进去 —— 那是 RK3568 的老写法。厂商 SDK 里两处独立
 *     实现都只写目标矩形的坐标：
 *
 *       u-boot/drivers/video/drm/rockchip_vop2.c  vop2_set_smart_win()
 *         dsp_stx = crtc_x;  dsp_sty = crtc_y;
 *       kernel-6.1 .../rockchip_drm_vop2.c  vop2_win_atomic_update()
 *         dsp_stx = dst->x1; dsp_sty = dst->y1;
 *
 *     RK3576 TRM 的字段说明也是"Display image horizon/vertical offset
 *     **in panel**"。所以全屏摆放时 DSP_ST 就是 0。
 *
 *     多加一个消隐量的后果不是"整幅图平移一点"：窗口的
 *     DSP_ST + DSP_INFO 越过了有效区，右边和下边被绕回，每一行都比上一行
 *     多错开固定的像素数 —— 屏上看到的就是斜纹，以及方块被切成断续的横带。
 *     纯色和按行变化的图案反而看不出异常，因为它们对水平错位不敏感。
 *
 ****************************************************************************/

int rk3576_vop2_fb_setup(uintptr_t buffer, uint32_t width, uint32_t height,
                         uint32_t stride_bytes)
{
  uint32_t ctrl;
  uint32_t s0;
  uint32_t s1;
  uint32_t axi;
  uint32_t v;
  int vp;
  uint32_t rb_act;
  uint32_t rb_dsp;
  uint32_t rb_vir;
  uint32_t rb_mst;
  uint32_t rb_ctl;

  if (buffer == 0 || width == 0 || height == 0)
    {
      return -EINVAL;
    }

  /* ★ 先把两个 ESMART 的归属和地址都读出来。
   *
   *   之前默认 ESMART1 归 VP1（照搬 RK3568/3588 的 OVL_LAYER_SEL 思路），
   *   结果所有配置都停在影子里 —— RK3576 是每个窗口自己一个
   *   PORT_SEL_IMD。先看清楚现状，再决定动哪个窗口。
   */

  s0 = vop_getreg(RK3576_VOP2_ESMART0_BASE +
                  RK3576_SMART_PORT_SEL_IMD);
  s1 = vop_getreg(RK3576_VOP2_ESMART1_BASE +
                  RK3576_SMART_PORT_SEL_IMD);

  syslog(LOG_INFO,
         "VOP2: ESMART0 归VP%" PRIu32 " CTRL=0x%08" PRIx32
         " MST=0x%08" PRIx32 " VIR=%" PRIu32 "\n",
         s0 & RK3576_SMART_PORT_SEL_MASK,
         vop_getreg(RK3576_VOP2_ESMART0_BASE +
                    RK3576_SMART_REGION0_CTRL),
         vop_getreg(RK3576_VOP2_ESMART0_BASE +
                    RK3576_SMART_REGION0_YRGB_MST),
         vop_getreg(RK3576_VOP2_ESMART0_BASE +
                    RK3576_SMART_REGION0_VIR));

  syslog(LOG_INFO,
         "VOP2: ESMART1 归VP%" PRIu32 " CTRL=0x%08" PRIx32
         " MST=0x%08" PRIx32 " VIR=%" PRIu32 "\n",
         s1 & RK3576_SMART_PORT_SEL_MASK,
         vop_getreg(RK3576_VOP2_ESMART1_BASE +
                    RK3576_SMART_REGION0_CTRL),
         vop_getreg(RK3576_VOP2_ESMART1_BASE +
                    RK3576_SMART_REGION0_YRGB_MST),
         vop_getreg(RK3576_VOP2_ESMART1_BASE +
                    RK3576_SMART_REGION0_VIR));

  if (!g_uboot_win.valid)
    {
      g_uboot_win.mst    = vop_getreg(RK3576_VOP2_ESMART1_BASE +
                                      RK3576_SMART_REGION0_YRGB_MST);
      g_uboot_win.vir    = vop_getreg(RK3576_VOP2_ESMART1_BASE +
                                      RK3576_SMART_REGION0_VIR);
      g_uboot_win.act    = vop_getreg(RK3576_VOP2_ESMART1_BASE +
                                      RK3576_SMART_REGION0_ACT_INFO);
      g_uboot_win.dsp    = vop_getreg(RK3576_VOP2_ESMART1_BASE +
                                      RK3576_SMART_REGION0_DSP_INFO);
      g_uboot_win.dsp_st = vop_getreg(RK3576_VOP2_ESMART1_BASE +
                                      RK3576_SMART_REGION0_DSP_ST);
      g_uboot_win.ctrl   = vop_getreg(RK3576_VOP2_ESMART1_BASE +
                                      RK3576_SMART_REGION0_CTRL);
      g_uboot_win.valid  = true;

      syslog(LOG_INFO,
             "VOP2: 已备份 U-Boot 窗口 MST=0x%08" PRIx32 " VIR=%" PRIu32
             " ACT=0x%08" PRIx32 "\n",
             g_uboot_win.mst, g_uboot_win.vir, g_uboot_win.act);
    }

  syslog(LOG_INFO,
         "VOP2: VP%d 图层将铺满 %" PRIu32 "x%" PRIu32 "，起点 (0,0)\n",
         RK3576_VOP2_MIPI_VP, width, height);

  /* ACT/DSP_INFO 存的是 (h-1)<<16 | (w-1) */

  vop_putreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_ACT_INFO,
             ((height - 1) << 16) | (width - 1));
  vop_putreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_DSP_INFO,
             ((height - 1) << 16) | (width - 1));
  vop_putreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_DSP_ST, 0);

  /* ★ 先旁路 ESMART 的 IOMMU。
   *
   *   VOP2 取帧缓冲默认经过它自己的 MMU（bit2 复位为 0）。页表是
   *   U-Boot 建的，只覆盖它自己那块缓冲；我们换了地址却没换页表，
   *   读到的就是按旧映射拼出来的零散页面。
   *
   *   我们用的是 kmm 分配的物理连续内存，本来就不需要 MMU，直接旁路。
   */

  axi = vop_getreg(RK3576_VOP2_ESMART1_BASE +
                   RK3576_SMART_AXI_CTRL_IMD);

  syslog(LOG_INFO,
         "VOP2: ESMART1 AXI_CTRL=0x%08" PRIx32 " MMU %s\n",
         axi, (axi & RK3576_SMART_MMU_BYPASS) ? "已旁路" : "生效中");

  /* ★ 不改这一位 —— 置 1 去旁路会让屏幕彻底变黑，连整屏填白
   * 都读不到。U-Boot 正是用 bit2=0 显示了自己的 logo，说明
   * 这条路径本就是直通的，动它反而破坏取数。这条负面结论
   * 留在这里，下次不必重走一遍。
   */

  /* ★ 缩放器必须一起设成 1:1。
   *
   *   ACT_INFO(源尺寸) 和 DSP_INFO(显示尺寸) 改了，缩放系数却还是
   *   U-Boot 为它那张 654x270 logo 留下的值 —— 硬件会按旧系数去采样
   *   新的源图，输出是被错误重采样的内容，看起来像"图像被剪切/重复"。
   *
   *   本例里 U-Boot 的 ACT 与 DSP 相同（没缩放），所以大概率本来就是
   *   旁路；但"大概率"不是依据。从 U-Boot 继承来的状态里，每一个没被
   *   显式设定的寄存器都是一个未知量，显式写一遍才能把它从嫌疑名单里
   *   划掉。
   */

  vop_putreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_SCL_CTRL, 0);
  vop_putreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_SCL_FACTOR_YRGB,
             (RK3576_SMART_SCL_FACTOR_1TO1 << 16) |
             RK3576_SMART_SCL_FACTOR_1TO1);

  /* VIR 的单位是 4 字节 */

  vop_putreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_VIR,
             stride_bytes / 4);

  vop_putreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_YRGB_MST,
             (uint32_t)buffer);

  /* ★ 必须显式设成 ARGB8888，不能沿用 U-Boot 的格式。
   *
   *   原来这里写的是"保持 U-Boot 设好的格式不动"，只补一个使能位。
   *   但上层 rk3576_fb.c 是**硬编码**按 32 位 ARGB8888 对外声明的
   *   （fmt=FB_FMT_RGB32、bpp=32）。两边一旦不一致，就出现这种故障：
   *
   *     U-Boot 用 RGB888(3 字节/像素) -> 硬件按 3 字节读，
   *     应用按 4 字节写 -> 每行错开 720/3=240 像素，
   *     竖条纹在屏上变成近乎水平的斜纹。
   *
   *   而且不报任何错 —— 地址、跨距、缓存刷回全都对，只有格式对不上。
   *
   *   与其让上层去迁就硬件里一个不确定的值，不如在这里把硬件设成上层
   *   声明的那一种。格式是本函数的职责，它决定了 stride_bytes 的含义。
   */

  ctrl = vop_getreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_CTRL);

  syslog(LOG_INFO,
         "VOP2: 原格式 %" PRIu32 "（0=ARGB8888 2=RGB888 3=RGB565）\n",
         (ctrl >> RK3576_SMART_REGION0_CTRL_FORMAT_SHIFT) & 0x1f);

  ctrl &= ~(0x1fu << RK3576_SMART_REGION0_CTRL_FORMAT_SHIFT);
  ctrl |= (uint32_t)RK3576_VOP2_FMT_ARGB8888 <<
          RK3576_SMART_REGION0_CTRL_FORMAT_SHIFT;

  vop_putreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_CTRL,
             ctrl | RK3576_SMART_REGION0_CTRL_EN);

  /* ★ 打开这个 VP 的取数紧急度。
   *
   *   厂商驱动只给 VP0 配了 urgency（rk3576_vp_data[] 里 VP1/VP2 的
   *   .urgency 是空的），因为 U-Boot 单独跑时没人跟它抢 DDR。AMP 下
   *   四核 A72 的 Linux 一起压 DDR，VP1 的行缓冲会见底 —— 屏上就是
   *   几条黑色细横条纹。详见 hardware/rk3576_vop2.h 里的长注释。
   *
   *   三个寄存器都是普通读改写：前两个是 IMD（立即生效），
   *   COLOR_BAR_CTRL 是影子寄存器，跟着下面那次 CFG_DONE 一起提交。
   */

  vp = RK3576_VOP2_MIPI_VP;

  v = vop_getreg(RK3576_VOP2_SYS_AXI_HURRY_CTRL0_IMD);
  vop_putreg(RK3576_VOP2_SYS_AXI_HURRY_CTRL0_IMD,
             v | (1u << (RK3576_AXI_PORT_URGENCY_EN_SHIFT + vp)));

  v = vop_getreg(RK3576_VOP2_SYS_AXI_HURRY_CTRL1_IMD);
  vop_putreg(RK3576_VOP2_SYS_AXI_HURRY_CTRL1_IMD,
             v | (1u << (RK3576_AXI_PORT_URGENCY_EN_SHIFT + vp)));

  /* bit0 是内置彩条使能，必须保持原样（0）。 */

  v = vop_getreg(VP1(RK3576_VP_COLOR_BAR_CTRL));
  v &= ~((RK3576_VP_URGENCY_TH_MASK << RK3576_VP_URGENCY_THL_SHIFT) |
         (RK3576_VP_URGENCY_TH_MASK << RK3576_VP_URGENCY_THH_SHIFT));
  v |= RK3576_VP_URGENCY_EN |
       ((uint32_t)RK3576_VP_URGENCY_THL << RK3576_VP_URGENCY_THL_SHIFT) |
       ((uint32_t)RK3576_VP_URGENCY_THH << RK3576_VP_URGENCY_THH_SHIFT);
  vop_putreg(VP1(RK3576_VP_COLOR_BAR_CTRL), v);

  syslog(LOG_INFO,
         "VOP2: VP%d 取数紧急度已开 HURRY0=%08" PRIx32 " HURRY1=%08" PRIx32
         " COLOR_BAR_CTRL=%08" PRIx32 "\n",
         vp,
         vop_getreg(RK3576_VOP2_SYS_AXI_HURRY_CTRL0_IMD),
         vop_getreg(RK3576_VOP2_SYS_AXI_HURRY_CTRL1_IMD),
         vop_getreg(VP1(RK3576_VP_COLOR_BAR_CTRL)));

  vop_cfg_done(RK3576_VOP2_MIPI_VP);

  /* ★ 回读硬件实际在用的值。
   *
   *   写进去不等于生效：VOP2 是影子寄存器，CFG_DONE 没提交到正确的
   *   视频口就停在影子里；VIR 的单位若理解错，写 720 硬件可能按 180
   *   行宽去取。这些错误的共同现象是"图像倾斜"，而地址、缓存、格式
   *   全都正确 —— 光看写入值一个也发现不了。
   *
   *   把回读值和意图值一起打出来，对不上时一眼可见。
   */

  /* ★ 必须等一帧再回读。
   *
   *   VOP2 是影子寄存器：写入先进影子，CFG_DONE 只是"请求提交"，真正
   *   latch 发生在下一个 VSYNC。紧接着回读，读到的还是**旧的活动值** ——
   *   看起来像"写入没生效"，实际只是还没到时候。
   *
   *   我第一次读到 MST 仍是 U-Boot 的 0xfdf00000，据此判断配置没落地，
   *   方向就偏了。60Hz 下一帧约 17ms，等 50ms 足够覆盖。
   */

  up_mdelay(50);

  rb_act = vop_getreg(RK3576_VOP2_ESMART1_BASE +
                      RK3576_SMART_REGION0_ACT_INFO);
  rb_dsp = vop_getreg(RK3576_VOP2_ESMART1_BASE +
                      RK3576_SMART_REGION0_DSP_INFO);
  rb_vir = vop_getreg(RK3576_VOP2_ESMART1_BASE +
                      RK3576_SMART_REGION0_VIR);
  rb_mst = vop_getreg(RK3576_VOP2_ESMART1_BASE +
                      RK3576_SMART_REGION0_YRGB_MST);
  rb_ctl = vop_getreg(RK3576_VOP2_ESMART1_BASE +
                      RK3576_SMART_REGION0_CTRL);

  syslog(LOG_INFO,
         "VOP2: 帧缓冲 %" PRIu32 "x%" PRIu32 " 跨距 %" PRIu32
         " 字节 @0x%08lx\n",
         width, height, stride_bytes, (unsigned long)buffer);

  syslog(LOG_INFO,
         "VOP2: 回读 ACT=0x%08" PRIx32 "(%" PRIu32 "x%" PRIu32 ")"
         " DSP=0x%08" PRIx32 " VIR=%" PRIu32 " MST=0x%08" PRIx32
         " CTRL=0x%08" PRIx32 "\n",
         rb_act, (rb_act & 0xffff) + 1, ((rb_act >> 16) & 0xffff) + 1,
         rb_dsp, rb_vir, rb_mst, rb_ctl);

  syslog(LOG_INFO,
         "VOP2: 回读 SCL_CTRL=0x%08" PRIx32 " SCL_FACTOR=0x%08" PRIx32
         " DSP_ST=0x%08" PRIx32 "\n",
         vop_getreg(RK3576_VOP2_ESMART1_BASE +
                    RK3576_SMART_REGION0_SCL_CTRL),
         vop_getreg(RK3576_VOP2_ESMART1_BASE +
                    RK3576_SMART_REGION0_SCL_FACTOR_YRGB),
         vop_getreg(RK3576_VOP2_ESMART1_BASE +
                    RK3576_SMART_REGION0_DSP_ST));

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop2_get_mode
 *
 * Description:
 *   从 VP1 的时序寄存器读出面板的实际分辨率。不写死数值 —— 这些值是
 *   U-Boot 按厂商 dtsi 配好的，比任何文档都准。
 *
 ****************************************************************************/

int rk3576_vop2_get_mode(uint32_t *width, uint32_t *height)
{
  uint32_t hact = vop_getreg(VP1(RK3576_VP_DSP_HACT_ST_END));
  uint32_t vact = vop_getreg(VP1(RK3576_VP_DSP_VACT_ST_END));

  uint32_t w = (hact & 0xffff) - ((hact >> 16) & 0xffff);
  uint32_t h = (vact & 0xffff) - ((vact >> 16) & 0xffff);

  if (w == 0 || h == 0 || w > 4096 || h > 4096)
    {
      syslog(LOG_ERR,
             "ERROR: VP%d 时序读出的分辨率不合理 %" PRIu32 "x%" PRIu32
             "（HACT=0x%08" PRIx32 " VACT=0x%08" PRIx32 "）\n",
             RK3576_VOP2_MIPI_VP, w, h, hact, vact);
      return -EIO;
    }

  if (width  != NULL) *width  = w;
  if (height != NULL) *height = h;
  return OK;
}

/****************************************************************************
 * Name: rk3576_vop2_dump_uboot_state
 *
 * Description:
 *   在本驱动写任何寄存器之前，把 VOP2 与 DSI 的关键寄存器原样打印出来。
 *
 *   ★ 为什么值得单独做这件事：这块板的出厂固件能在这块屏上显示
 *     Rockchip logo，也就是说 U-Boot 已经完整跑通过一次「面板上电 ->
 *     初始化序列 -> VOP2 -> DSI -> D-PHY」。它留在寄存器里的，是这块
 *     屏的一份**已知可用配置**，比任何文档和参考驱动都贴近这块硬件。
 *
 *     此前一直在用自己算出来的值去覆盖它，等于把唯一的正确答案擦掉了。
 *     先读再写。
 *
 ****************************************************************************/

void rk3576_vop2_dump_uboot_state(void)
{
  int i;

  syslog(LOG_INFO, "==== U-Boot 留下的 VOP2 状态（未改动）====\n");

  for (i = 0;
       i < (int)(sizeof(g_vop2_uboot_regs) / sizeof(g_vop2_uboot_regs[0]));
       i++)
    {
      syslog(LOG_INFO, "  %-22s 0x%08" PRIx32 "\n",
             g_vop2_uboot_regs[i].name,
             getreg32(RK3576_VOP2_ADDR + g_vop2_uboot_regs[i].off));

      /* 串口在连续刷屏时会丢字节，把 dump 打断行读不全就白读了。 */

      up_mdelay(3);
    }
}

/****************************************************************************
 * Name: rk3576_vop2_restore_uboot
 *
 * Description:
 *   把 ESMART1 恢复成 U-Boot 留下的配置，并返回它的帧缓冲地址与几何。
 *
 *   ★ 这是一个"退回已知可用状态"的手段
 *
 *     我们自己配的窗口，VOP2 取出来的画面是错乱的（纯色正常、细节乱），
 *     根因尚未定位。U-Boot 那组参数则被实测证明可用。恢复之后往它的
 *     缓冲里写像素，如果画面正确，就说明显示通路本身没问题、问题出在
 *     我们改的某个寄存器上 —— 这一步把两件事分开了，而且顺带得到一条
 *     马上能用的出图通路。
 *
 ****************************************************************************/

int rk3576_vop2_restore_uboot(uintptr_t *buffer, uint32_t *width,
                              uint32_t *height, uint32_t *stride_bytes)
{
  if (!g_uboot_win.valid)
    {
      return -ENODATA;
    }

  vop_putreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_YRGB_MST,
             g_uboot_win.mst);
  vop_putreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_VIR,
             g_uboot_win.vir);
  vop_putreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_ACT_INFO,
             g_uboot_win.act);
  vop_putreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_DSP_INFO,
             g_uboot_win.dsp);
  vop_putreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_DSP_ST,
             g_uboot_win.dsp_st);
  vop_putreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_CTRL,
             g_uboot_win.ctrl);

  vop_cfg_done(RK3576_VOP2_MIPI_VP);
  up_mdelay(50);

  if (buffer != NULL)
    {
      *buffer = g_uboot_win.mst;
    }

  if (width != NULL)
    {
      *width = (g_uboot_win.act & 0xffff) + 1;
    }

  if (height != NULL)
    {
      *height = ((g_uboot_win.act >> 16) & 0xffff) + 1;
    }

  if (stride_bytes != NULL)
    {
      *stride_bytes = g_uboot_win.vir * 4;
    }

  syslog(LOG_INFO,
         "VOP2: 已恢复 U-Boot 窗口 %" PRIu32 "x%" PRIu32
         " 跨距 %" PRIu32 " @0x%08" PRIx32 "\n",
         (g_uboot_win.act & 0xffff) + 1,
         ((g_uboot_win.act >> 16) & 0xffff) + 1,
         g_uboot_win.vir * 4, g_uboot_win.mst);
  return OK;
}

/****************************************************************************
 * Name: rk3576_vop2_dump_win
 *
 * Description:
 *   把 ESMART1 图层的整个寄存器块和 VP1 的时序原样打出来。
 *
 *   ★ 与开机那次 dump 的区别：这个可以在 nsh 里随时敲。
 *
 *     影子寄存器要等一个 VSYNC 才 latch，"写完立刻回读"读到的是旧值；
 *     而配置出错时最需要知道的恰恰是**硬件此刻在用什么**。做成随时可
 *     调用的命令，就能在改完、等一会儿之后再看，把"写进去了"和
 *     "生效了"分开。
 *
 ****************************************************************************/

void rk3576_vop2_dump_win(void)
{
  uint32_t vir;
  uint32_t act;
  uint32_t dsp;
  uint32_t off;
  int i;

  syslog(LOG_INFO, "==== ESMART1 当前寄存器 ====\n");

  for (i = 0;
       i < (int)(sizeof(g_vop2_win_regs) / sizeof(g_vop2_win_regs[0]));
       i++)
    {
      syslog(LOG_INFO, "  %-20s 0x%08" PRIx32 "\n", g_vop2_win_regs[i].name,
             vop_getreg(RK3576_VOP2_ESMART1_BASE +
                        g_vop2_win_regs[i].off));

      /* 串口连续刷屏会丢字节，dump 读不全就白读了。 */

      up_mdelay(3);
    }

  /* 把编码过的字段翻成人看的数字。地址、跨距、尺寸三者是否自洽，
   * 是判断"图像倾斜"类故障的第一现场 —— 光看十六进制要心算。
   */

  vir = vop_getreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_VIR);
  act = vop_getreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_ACT_INFO);
  dsp = vop_getreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_DSP_INFO);
  off = vop_getreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_DSP_ST);

  syslog(LOG_INFO,
         "  解码: 源 %" PRIu32 "x%" PRIu32 " 显示 %" PRIu32 "x%" PRIu32
         " 起点 (%" PRIu32 ",%" PRIu32 ") 跨距 %" PRIu32 " 字节\n",
         (act & 0x1fff) + 1, ((act >> 16) & 0x1fff) + 1,
         (dsp & 0x1fff) + 1, ((dsp >> 16) & 0x1fff) + 1,
         off & 0x1fff, (off >> 16) & 0x1fff,
         (vir & 0xffff) * 4);

  up_mdelay(3);

  syslog(LOG_INFO,
         "  VP1: HACT=0x%08" PRIx32 " VACT=0x%08" PRIx32
         " HTOTAL=0x%08" PRIx32 " VTOTAL=0x%08" PRIx32
         " DSP_CTRL=0x%08" PRIx32 "\n",
         vop_getreg(VP1(RK3576_VP_DSP_HACT_ST_END)),
         vop_getreg(VP1(RK3576_VP_DSP_VACT_ST_END)),
         vop_getreg(VP1(RK3576_VP_DSP_HTOTAL_HS_END)),
         vop_getreg(VP1(RK3576_VP_DSP_VTOTAL_VS_END)),
         vop_getreg(VP1(RK3576_VP_DSP_CTRL)));
}

/****************************************************************************
 * Name: rk3576_vop2_try_window
 *
 * Description:
 *   从 U-Boot 那组已知可用的窗口参数出发，只改指定的几项，其余原样保留。
 *
 *   ★ 为什么要这么一个函数
 *
 *     fb_setup 一次改了五样东西：帧缓冲地址、宽、高、跨距、显示起点。
 *     结果画面错乱，而这五样里任何一样错了现象都差不多 —— 一次实验
 *     排除不掉任何一个。反过来，U-Boot 的那组参数已被实测证明可用
 *     （`cam ub 2` 出的是正的竖条纹），它就是一个可靠的原点。
 *
 *     每次只从这个原点挪动一个变量，第一个出错的那次就直接指名了元凶。
 *     代价是每次都要重配一遍窗口，但这是纯寄存器写，比重新烧一版固件
 *     便宜几个数量级 —— 一次烧写换来七次实验。
 *
 *   ★ 帧缓冲一律按 ARGB8888 处理。
 *
 *     U-Boot 的 VIR=654 而窗口宽正是 654，按 TRM 的编码（ARGB8888 的
 *     vir_stride 就等于像素宽）只能是 ARGB8888。所以"强制 ARGB8888"
 *     和"缩放器设 1:1"这两项与 U-Boot 原状一致，不必再单独做成一步。
 *
 * Input Parameters:
 *   buffer - 帧缓冲物理地址，0 表示沿用 U-Boot 的
 *   width  - 窗口宽，0 表示沿用 U-Boot 的
 *   height - 窗口高，0 表示沿用 U-Boot 的
 *   origin - 0 沿用 U-Boot 的显示起点；1 用有效区原点（即 0，正确值）；
 *            2 用 hact_st/vact_st（把消隐段算进去的错误值，用于复现故障）
 *
 * Returned Value:
 *   OK，并通过出参返回最终生效的地址与几何，供调用方照着写像素。
 *
 ****************************************************************************/

int rk3576_vop2_try_window(uintptr_t buffer, uint32_t width, uint32_t height,
                           int origin, uintptr_t *out_buffer,
                           uint32_t *out_width, uint32_t *out_height,
                           uint32_t *out_stride)
{
  uint32_t stride;
  uint32_t dsp_st;
  uint32_t ctrl;

  if (!g_uboot_win.valid)
    {
      return -ENODATA;
    }

  /* 先整体退回 U-Boot 的原状，再往上加改动 —— 否则上一次实验留下的
   * 残留会混进这一次，"只改了一个变量"就不成立了。
   */

  if (buffer == 0)
    {
      buffer = g_uboot_win.mst;
    }

  if (width == 0)
    {
      width = (g_uboot_win.act & 0x1fff) + 1;
    }

  if (height == 0)
    {
      height = ((g_uboot_win.act >> 16) & 0x1fff) + 1;
    }

  stride = width * 4;

  if (origin == 1)
    {
      /* 有效区原点。DSP_ST 不含消隐段，全屏摆放就是 0 —— 依据见
       * fb_setup 的注释（厂商 U-Boot 与内核两处实现 + TRM 字段说明）。
       */

      dsp_st = 0;
    }
  else if (origin == 2)
    {
      /* ★ 故意写回那个错误值，把故障按需重现出来。
       *
       *   诊断是从厂商 SDK 读出来的，但"读代码得出的结论"和"在这块板子
       *   上成立"是两件事。留一条能主动复现的路径，才谈得上证实：
       *   origin=1 出正的竖条纹、origin=2 出斜纹，两次对照才说明错的
       *   确实是 DSP_ST，而不是别的什么恰好被一起改掉了。
       */

      uint32_t hact = vop_getreg(VP1(RK3576_VP_DSP_HACT_ST_END));
      uint32_t vact = vop_getreg(VP1(RK3576_VP_DSP_VACT_ST_END));

      dsp_st = (((vact >> 16) & 0xffff) << 16) | ((hact >> 16) & 0xffff);
    }
  else
    {
      dsp_st = g_uboot_win.dsp_st;
    }

  vop_putreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_ACT_INFO,
             ((height - 1) << 16) | (width - 1));
  vop_putreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_DSP_INFO,
             ((height - 1) << 16) | (width - 1));
  vop_putreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_DSP_ST,
             dsp_st);
  vop_putreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_SCL_CTRL, 0);
  vop_putreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_SCL_FACTOR_YRGB,
             (RK3576_SMART_SCL_FACTOR_1TO1 << 16) |
             RK3576_SMART_SCL_FACTOR_1TO1);
  vop_putreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_VIR,
             stride / 4);
  vop_putreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_YRGB_MST,
             (uint32_t)buffer);

  ctrl = g_uboot_win.ctrl;
  ctrl &= ~(0x1fu << RK3576_SMART_REGION0_CTRL_FORMAT_SHIFT);
  ctrl |= (uint32_t)RK3576_VOP2_FMT_ARGB8888 <<
          RK3576_SMART_REGION0_CTRL_FORMAT_SHIFT;

  vop_putreg(RK3576_VOP2_ESMART1_BASE + RK3576_SMART_REGION0_CTRL,
             ctrl | RK3576_SMART_REGION0_CTRL_EN);

  vop_cfg_done(RK3576_VOP2_MIPI_VP);

  /* 等一帧，让影子寄存器 latch 完再让调用方去看回读值。 */

  up_mdelay(50);

  if (out_buffer != NULL) *out_buffer = buffer;
  if (out_width  != NULL) *out_width  = width;
  if (out_height != NULL) *out_height = height;
  if (out_stride != NULL) *out_stride = stride;

  syslog(LOG_INFO,
         "VOP2: 试配窗口 %" PRIu32 "x%" PRIu32 " 跨距 %" PRIu32
         " 起点 0x%08" PRIx32 " @0x%08lx\n",
         width, height, stride, dsp_st, (unsigned long)buffer);

  return OK;
}
