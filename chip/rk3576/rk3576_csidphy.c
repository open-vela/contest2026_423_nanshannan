/****************************************************************************
 * chip/rk3576/rk3576_csidphy.c
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

/* RK3576 MIPI CSI-2 D-PHY 接收端。
 *
 * ★ 这是接收方向，与 rk3576_dcphy.c 完全不同的硬件
 *
 *   屏用的 rk3576_dcphy.c 是 Samsung DCPHY 的**发送**端（DSI TX）。
 *   摄像头这条是 Innosilicon 的**接收**端，寄存器组毫无交集 —— 参考
 *   实现分别是 phy-rockchip-samsung-dcphy.c 和
 *   phy-rockchip-csi2-dphy-hw.c 两个文件。想拿 DSI 那份改是走不通的。
 *
 * ★ 六个逻辑 PHY 只有两套物理硬件
 *
 *   dtb 里有 csi2-dphy0..5，物理块却只有 csi2-dphy0-hw@2b030000 与
 *   csi2-dphy1-hw@2b070000。厂商驱动按 phy_index 分：< 3 走 PHY0，
 *   >= 3 走 PHY1。本板摄像头挂在 csi2_dphy3，所以用 PHY1。
 *
 *   分法还与"整用/拆用"有关：FULL 模式一颗 PHY 的 4 条通道全给一路
 *   传感器；SPLIT 模式拆成 0-1 和 2-3 两组给两路。本板只有一路在用，
 *   走 FULL，这也是最简单的一档。
 *
 * ★ 唯一需要按速率整定的量：THS-SETTLE
 *
 *   接收端要在数据通道从 LP 切到 HS 之后、等信号稳定了再开始采样。
 *   这个等待时间就是 THS-SETTLE，必须按链路速率给。给错的现象是
 *   "有数据但全是错包"，而不是"没数据" —— 两者要分清。
 *
 *   速率到档位的换算表照抄厂商的 rk3568 表（RK3576 复用同一张）。
 *   594Mbps 落在 500~599 档，档位值 0x0e。
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
#include <syslog.h>

#include <nuttx/arch.h>

#include "arm64_internal.h"
#include "rk3576_csidphy.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 两套物理 PHY 的基址，出处 dtb 的 csi2-dphy{0,1}-hw 节点 */

#define CSIDPHY0_BASE            0x2b030000
#define CSIDPHY1_BASE            0x2b070000

/* MIPI D-PHY GRF。dtb 里两个 rockchip,rk3576-mipi-dphy-grf 节点，
 * 分别配给两套 PHY。
 */

#define CSIDPHY0_GRF_BASE        0x2603a000
#define CSIDPHY1_GRF_BASE        0x2604c000

/* SYS GRF —— 通道整用/拆用的选择位在这里，不在 dphy GRF 里。
 * 厂商代码用 write_sys_grf_reg() 与 write_grf_reg() 区分这两组。
 */

#define SYS_GRF_BASE             0x2600a000
#define SYS_GRF_SOC_CON5         0x0014

/* PHY 寄存器偏移，出处 phy-rockchip-csi2-dphy-hw.c 的
 * rk3588_csi2dphy_regs（RK3576 复用同一组）
 */

#define DPHY_CTRL_LANE_ENABLE    0x0000
#define DPHY_DUAL_CAL_EN         0x0080
#define DPHY_CLK_CONTINUE_MODE   0x0128
#define DPHY_CLK_THS_SETTLE      0x0160
#define DPHY_LANE0_THS_SETTLE    0x01e0
#define DPHY_LANE1_THS_SETTLE    0x0260
#define DPHY_LANE2_THS_SETTLE    0x02e0
#define DPHY_LANE3_THS_SETTLE    0x0360

#define DPHY_DATALANE_EN_SHIFT   2
#define DPHY_CLKLANE_EN_SHIFT    6
#define DPHY_CLK_CONTINUE_MASK   0x30

/* GRF_DPHY_CON0 里的位域 */

#define GRF_DPHY_CON0            0x0000
#define GRF_FORCERXMODE_SHIFT    0
#define GRF_FORCERXMODE_MASK     0xf
#define GRF_DATALANE_EN_SHIFT    4
#define GRF_DATALANE_EN_MASK     0xf
#define GRF_CLKLANE_EN_SHIFT     8
#define GRF_CLKLANE_EN_MASK      0x1

/* SYS_GRF_SOC_CON5 里的通道选择位 */

#define SYS_GRF_PHY0_LANE_SEL_SHIFT  1
#define SYS_GRF_PHY1_LANE_SEL_SHIFT  2

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct hsfreq_range_s
{
  uint16_t range_h;   /* 该档覆盖到的最高 Mbps */
  uint8_t  cfg;       /* 写进 THS_SETTLE 低 7 位的档位值 */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 速率 -> THS-SETTLE 档位。必须按 range_h 升序，查找取第一个不小于
 * 目标速率的档。照抄厂商 rk3568_csi2_dphy_hw_hsfreq_ranges，
 * RK3576 的 drv_data 直接复用了这张表。
 */

static const struct hsfreq_range_s g_hsfreq[] =
{
  {
    109, 0x02
  },
  {
    149, 0x03
  },
  {
    199, 0x06
  },
  {
    249, 0x06
  },
  {
    299, 0x06
  },
  {
    399, 0x08
  },
  {
    499, 0x0b
  },
  {
    599, 0x0e
  },
  {
    699, 0x10
  },
  {
    799, 0x12
  },
  {
    999, 0x16
  },
  {
    1199, 0x1e
  },
  {
    1399, 0x23
  },
  {
    1599, 0x2d
  },
  {
    1799, 0x32
  },
  {
    1999, 0x37
  },
  {
    2199, 0x3c
  },
  {
    2399, 0x41
  },
  {
    2499, 0x46
  },
};

#define NHSFREQ (sizeof(g_hsfreq) / sizeof(g_hsfreq[0]))

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uintptr_t phy_base(int index)
{
  return (index < 3) ? CSIDPHY0_BASE : CSIDPHY1_BASE;
}

static inline uintptr_t phy_grf(int index)
{
  return (index < 3) ? CSIDPHY0_GRF_BASE : CSIDPHY1_GRF_BASE;
}

/****************************************************************************
 * Name: grf_update
 *
 * Description:
 *   写 GRF 的一个位域。Rockchip 的 GRF 一律是高 16 位写使能掩码、
 *   低 16 位数据；不带掩码写等于没写。
 *
 ****************************************************************************/

static void grf_update(uintptr_t base, uint32_t off, uint32_t mask,
                       uint32_t shift, uint32_t value)
{
  putreg32(((mask << shift) << 16) | ((value & mask) << shift), base + off);
}

/****************************************************************************
 * Name: dphy_ths_settle
 *
 * Description:
 *   给一条通道写 THS-SETTLE 档位。只改低 7 位，其余位保持 ——
 *   这个寄存器里还有别的控制位，整字写会把它们抹掉。
 *
 ****************************************************************************/

static void dphy_ths_settle(uintptr_t base, uint32_t off, uint8_t cfg)
{
  uint32_t val = getreg32(base + off);

  putreg32((val & ~0x7fu) | cfg, base + off);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_csidphy_start
 ****************************************************************************/

int rk3576_csidphy_start(int index, int lanes, unsigned int mbps)
{
  uintptr_t base = phy_base(index);
  uintptr_t grf  = phy_grf(index);
  uint32_t lane_mask;
  uint32_t val;
  uint8_t cfg;
  int i;

  if (index < 0 || index > 5 || lanes < 1 || lanes > 4)
    {
      return -EINVAL;
    }

  lane_mask = (1u << lanes) - 1;

  /* 1) 复位数字部分。两次写是厂商的做法：先给 0x1e 让复位生效，
   *    再给 0x1f 释放。少写一次的话内部状态机停在上次的残留上。
   */

  putreg32(0x1e, base + DPHY_DUAL_CAL_EN);
  putreg32(0x1f, base + DPHY_DUAL_CAL_EN);

  /* 2) 使能通道。读改写：这个寄存器在 SPLIT 模式下由两路共用，
   *    整字写会把另一路的使能位抹掉。本板只有一路，但保持读改写
   *    是为了将来接第二路时不必回头改。
   */

  val  = getreg32(base + DPHY_CTRL_LANE_ENABLE);
  val |= (lane_mask << DPHY_DATALANE_EN_SHIFT) |
         (1u << DPHY_CLKLANE_EN_SHIFT);
  putreg32(val, base + DPHY_CTRL_LANE_ENABLE);

  /* 3) 连续时钟模式。IMX415 默认是连续时钟（不在行间把时钟通道
   *    落回 LP），接收端要跟着配，否则每行开头都要重新同步。
   */

  val = getreg32(base + DPHY_CLK_CONTINUE_MODE);
  putreg32((val & ~(uint32_t)DPHY_CLK_CONTINUE_MASK) | 0x30,
           base + DPHY_CLK_CONTINUE_MODE);

  /* 4) GRF：通道数与时钟通道使能 */

  grf_update(grf, GRF_DPHY_CON0, GRF_DATALANE_EN_MASK,
             GRF_DATALANE_EN_SHIFT, lane_mask);
  grf_update(grf, GRF_DPHY_CON0, GRF_CLKLANE_EN_MASK,
             GRF_CLKLANE_EN_SHIFT, 1);

  /* 5) SYS GRF：整用而不是拆用（0 = FULL） */

  grf_update(SYS_GRF_BASE, SYS_GRF_SOC_CON5, 0x1,
             (index < 3) ? SYS_GRF_PHY0_LANE_SEL_SHIFT :
                           SYS_GRF_PHY1_LANE_SEL_SHIFT, 0);

  /* 6) 退出强制接收模式，让 PHY 按真实的 LP/HS 跳变工作 */

  grf_update(grf, GRF_DPHY_CON0, GRF_FORCERXMODE_MASK,
             GRF_FORCERXMODE_SHIFT, 0);

  /* 7) THS-SETTLE。查表取第一个覆盖到目标速率的档；速率超出表尾时
   *    退到最后一档并明确告警 —— 静默按最高档跑会得到"有数据但全是
   *    错包"，那种现象很难往"速率超范围"上想。
   */

  for (i = 0; i < (int)NHSFREQ; i++)
    {
      if (mbps <= g_hsfreq[i].range_h)
        {
          break;
        }
    }

  if (i >= (int)NHSFREQ)
    {
      i = NHSFREQ - 1;
      syslog(LOG_WARNING,
             "CSI D-PHY%d: %uMbps 超出档位表上限 %u，按最高档处理\n",
             index, mbps, g_hsfreq[i].range_h);
    }

  cfg = g_hsfreq[i].cfg;

  dphy_ths_settle(base, DPHY_CLK_THS_SETTLE, cfg);
  if (lanes > 0)
    {
      dphy_ths_settle(base, DPHY_LANE0_THS_SETTLE, cfg);
    }

  if (lanes > 1)
    {
      dphy_ths_settle(base, DPHY_LANE1_THS_SETTLE, cfg);
    }

  if (lanes > 2)
    {
      dphy_ths_settle(base, DPHY_LANE2_THS_SETTLE, cfg);
    }

  if (lanes > 3)
    {
      dphy_ths_settle(base, DPHY_LANE3_THS_SETTLE, cfg);
    }

  syslog(LOG_INFO,
         "CSI D-PHY%d: PHY%d@0x%08lx %d lane %uMbps THS_SETTLE=0x%02x "
         "LANE_EN=0x%08" PRIx32 "\n",
         index, (index < 3) ? 0 : 1, (unsigned long)base, lanes, mbps, cfg,
         getreg32(base + DPHY_CTRL_LANE_ENABLE));

  return OK;
}

/****************************************************************************
 * Name: rk3576_csidphy_stop
 ****************************************************************************/

int rk3576_csidphy_stop(int index)
{
  uintptr_t base = phy_base(index);

  if (index < 0 || index > 5)
    {
      return -EINVAL;
    }

  putreg32(0, base + DPHY_CTRL_LANE_ENABLE);
  return OK;
}
