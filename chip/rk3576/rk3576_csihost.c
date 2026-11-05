/****************************************************************************
 * chip/rk3576/rk3576_csihost.c
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

/* RK3576 MIPI CSI-2 主机控制器（Synopsys CSI-2 host）。
 *
 * 取图链路的第二级：D-PHY 把差分信号还原成字节流之后，由它解析
 * CSI-2 协议 —— 拆包、按数据类型分流、校验 ECC/CRC。
 *
 * ★ 这一级的真正价值是**可观测性**
 *
 *   链路上前两级（传感器出流、D-PHY 收）都没有任何可读的状态：
 *   传感器只能靠 I2C 回读确认"配置进去了"，D-PHY 连状态寄存器都没有。
 *   到了这里才第一次有东西可看：
 *
 *     PHY_STATE  各通道的 stopstate 与时钟通道是否在跑
 *     ERR1/ERR2  SoT 同步错、ECC 错、CRC 错、行/帧边界错……
 *
 *   所以即使 CIF 还没写完、拿不到图像，这一级也能回答那个最关键的
 *   问题：**线上到底有没有数据，数据是不是完整的**。
 *
 *   这两者要分开看：
 *     - PHY_STATE 全是 stopstate、ERR 全 0  -> 线上没数据，问题在发送端
 *     - PHY_STATE 有活动、ERR1 里 CRC/ECC 非 0 -> 有数据但收错了，
 *       多半是 THS-SETTLE 档位不对
 *   没有这一级的读数，这两种情况的表现都是"看不到图"。
 *
 * 寄存器与使能序列出处：厂商
 * drivers/media/platform/rockchip/cif/mipi-csi2.{c,h}
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <debug.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <syslog.h>

#include <nuttx/arch.h>

#include "arm64_internal.h"
#include "rk3576_cru.h"
#include "rk3576_csihost.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 五个 CSI-2 host，基址等距排列。出处 dtb 的 mipiN-csi2-hw 节点：
 * mipi0 @0x27c80000 ... mipi4 @0x27cc0000
 */

#define CSIHOST_BASE(n)          (0x27c80000 + (n) * 0x10000)
#define CSIHOST_NHOSTS           5

/* PCLK_CSI_HOST_n 的门控。出处 clk-rk3576.c 的
 *   GATE(PCLK_CSI_HOST_0, ... RK3576_CLKGATE_CON(54), 4)
 * 五个连续排列，host0 在 bit4。
 *
 * ★ 用 CRU 驱动里的寄存器/位，不用 dt-bindings 的时钟索引 ——
 *   手上这份 binding 头与板子 dtb 的索引对不上（dtb 写 379，头里
 *   379 是别的时钟），索引这条路不可靠；寄存器表是直接的。
 */

#define CSIHOST_GATE_CON         54
#define CSIHOST_GATE_BIT(n)      (4 + (n))

/* 寄存器偏移 */

#define CSIHOST_N_LANES          0x04
#define CSIHOST_DPHY_SHUTDOWNZ   0x08
#define CSIHOST_PHY_RSTZ         0x0c
#define CSIHOST_RESETN           0x10
#define CSIHOST_PHY_STATE        0x14
#define CSIHOST_ERR1             0x20
#define CSIHOST_ERR2             0x24
#define CSIHOST_MSK1             0x28
#define CSIHOST_MSK2             0x2c
#define CSIHOST_CONTROL          0x40

/* CONTROL 位域 */

#define SW_CPHY_EN(x)            ((x) << 0)
#define SW_DSI_EN(x)             ((x) << 4)
#define SW_DATATYPE_FS(x)        ((x) << 8)
#define SW_DATATYPE_FE(x)        ((x) << 14)
#define SW_DATATYPE_LS(x)        ((x) << 20)
#define SW_DATATYPE_LE(x)        ((x) << 26)

/* ERR1 里的分组，报错时按组说明比给一个裸数值有用 */

#define ERR1_SOT_SYNC            0x0000000f   /* SoT 同步失败 */
#define ERR1_BNDRY_MATCH         0x000000f0   /* 帧边界不匹配 */
#define ERR1_SEQ                 0x00000f00   /* 帧序号错 */
#define ERR1_FRM_DATA            0x0000f000   /* 帧数据错 */
#define ERR1_CTRL                0x000f0000   /* 控制错 */
#define ERR1_CRC                 0x0f000000   /* 载荷 CRC 错 */
#define ERR1_ECC2                0x10000000   /* 包头两位 ECC 错（不可纠）*/

#define ERR2_ESC                 0x0000000f   /* 转义序列错 */
#define ERR2_SOT_HS              0x000000f0   /* HS 起始错 */
#define ERR2_ECC_CORRECTED       0x00000f00   /* ECC 已纠正（可容忍）*/
#define ERR2_ERR_ID              0x0000f000   /* 未知数据类型 */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_csihost_start
 ****************************************************************************/

int rk3576_csihost_start(int host, int lanes)
{
  uintptr_t base;
  uint32_t ctrl;

  if (host < 0 || host >= CSIHOST_NHOSTS || lanes < 1 || lanes > 4)
    {
      return -EINVAL;
    }

  base = CSIHOST_BASE(host);

  rk3576_clk_gate(CSIHOST_GATE_CON, CSIHOST_GATE_BIT(host), true);

  /* 1) 先关干净。RESETN=0 期间寄存器才可改；把中断全屏蔽掉是因为
   *    我们用轮询读 ERR1/ERR2，不挂中断服务程序 —— 不屏蔽的话
   *    错误一来就是没人处理的悬挂中断。
   */

  putreg32(0, base + CSIHOST_RESETN);
  putreg32(0xffffffff, base + CSIHOST_MSK1);
  putreg32(0xffffffff, base + CSIHOST_MSK2);

  /* 2) 通道数。寄存器里放的是"通道数 - 1"。 */

  putreg32((uint32_t)(lanes - 1), base + CSIHOST_N_LANES);

  /* 3) CONTROL。D-PHY（不是 C-PHY）、相机模式（不是 DSI 回环），
   *    四个同步短包的数据类型按 CSI-2 规范给：
   *      FS=0x00 帧开始  FE=0x01 帧结束  LS=0x02 行开始  LE=0x03 行结束
   */

  ctrl = SW_CPHY_EN(0) | SW_DSI_EN(0) |
         SW_DATATYPE_FS(0x00) | SW_DATATYPE_FE(0x01) |
         SW_DATATYPE_LS(0x02) | SW_DATATYPE_LE(0x03);
  putreg32(ctrl, base + CSIHOST_CONTROL);

  /* 4) 错误掩码。厂商在相机模式下用 MSK1=0、MSK2=0xf000 ——
   *    放开大部分错误上报，只屏蔽 ERR2 的"未知数据类型"那一组：
   *    传感器除图像外还会发嵌入数据行，那些数据类型本就不认识，
   *    不屏蔽会被无意义的错误刷屏。
   */

  putreg32(0x0, base + CSIHOST_MSK1);
  putreg32(0xf000, base + CSIHOST_MSK2);

  /* 5) 放开复位，开始收 */

  putreg32(1, base + CSIHOST_RESETN);

  syslog(LOG_INFO,
         "CSI host%d: @0x%08lx %d lane CONTROL=0x%08" PRIx32 "\n",
         host, (unsigned long)base, lanes, ctrl);

  return OK;
}

/****************************************************************************
 * Name: rk3576_csihost_stop
 ****************************************************************************/

int rk3576_csihost_stop(int host)
{
  uintptr_t base;

  if (host < 0 || host >= CSIHOST_NHOSTS)
    {
      return -EINVAL;
    }

  base = CSIHOST_BASE(host);
  putreg32(0, base + CSIHOST_RESETN);
  putreg32(0xffffffff, base + CSIHOST_MSK1);
  putreg32(0xffffffff, base + CSIHOST_MSK2);
  return OK;
}

/****************************************************************************
 * Name: rk3576_csihost_status
 *
 * Description:
 *   打印这一级看得到的全部状态，并给出**结论**而不只是数值。
 *
 *   之所以要给结论：PHY_STATE 和 ERR1/ERR2 单看都不足以判断问题在哪，
 *   要两个一起看 ——「没数据」和「有数据但收错」需要不同的下一步，
 *   而它们在屏幕上的表现是一样的（都看不到图）。
 *
 * Returned Value:
 *   0 表示看起来正常；正数表示检出的错误位；-EINVAL 参数错。
 *
 ****************************************************************************/

int rk3576_csihost_status(int host)
{
  uintptr_t base;
  uint32_t state;
  uint32_t err1;
  uint32_t err2;
  bool active;

  if (host < 0 || host >= CSIHOST_NHOSTS)
    {
      return -EINVAL;
    }

  base = CSIHOST_BASE(host);

  state = getreg32(base + CSIHOST_PHY_STATE);
  err1  = getreg32(base + CSIHOST_ERR1);
  err2  = getreg32(base + CSIHOST_ERR2);

  syslog(LOG_INFO,
         "CSI host%d: PHY_STATE=0x%08" PRIx32 " ERR1=0x%08" PRIx32
         " ERR2=0x%08" PRIx32 "\n", host, state, err1, err2);

  /* PHY_STATE 的低位是各通道的 stopstate：置位 = 该通道停在 LP-11
   * 空闲态。四条数据通道全停、且没有任何错误计数，说明线上根本没有
   * 高速数据 —— 这时候查接收端是白费力气，问题在发送端或走线。
   */

  active = (state & 0xf) != 0xf || err1 != 0 || err2 != 0;

  if (!active)
    {
      syslog(LOG_WARNING,
             "CSI host%d: 所有数据通道都停在空闲态且无任何错误计数 —— "
             "线上没有高速数据，应查传感器是否真在出流\n", host);
      return 0;
    }

  if (err1 & ERR1_SOT_SYNC)
    {
      syslog(LOG_ERR, "  SoT 同步失败 —— THS-SETTLE 档位多半不对\n");
    }

  if (err1 & ERR1_ECC2)
    {
      syslog(LOG_ERR, "  包头 ECC 两位错（不可纠）—— 信号完整性问题\n");
    }

  if (err1 & ERR1_CRC)
    {
      syslog(LOG_ERR, "  载荷 CRC 错 —— 数据在传输中被破坏\n");
    }

  if (err1 & (ERR1_BNDRY_MATCH | ERR1_SEQ | ERR1_FRM_DATA))
    {
      syslog(LOG_ERR, "  帧结构错（边界/序号/数据）\n");
    }

  if (err2 & ERR2_SOT_HS)
    {
      syslog(LOG_ERR, "  HS 起始错\n");
    }

  if (err2 & ERR2_ECC_CORRECTED)
    {
      syslog(LOG_INFO, "  （ECC 已纠正若干，少量可容忍）\n");
    }

  if (err1 == 0 && (err2 & ~ERR2_ECC_CORRECTED) == 0)
    {
      syslog(LOG_INFO, "  通道有活动且无实质错误 —— 收流正常\n");
    }

  return (int)(err1 | (err2 & ~ERR2_ECC_CORRECTED));
}
