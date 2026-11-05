/****************************************************************************
 * chip/rk3576/hardware/rk3576_dsi2.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_DSI2_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_DSI2_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Synopsys MIPI DSI 2.0 主机（"DSI2"）。
 *
 * ★ 出处：Linux drivers/gpu/drm/bridge/synopsys/dw-mipi-dsi2.c
 *   基址取自主线 rk3576.dtsi 的 dsi@27d80000
 *   （compatible = "rockchip,rk3576-mipi-dsi2"，电源域 PD_VO0）。
 *
 * ★★ 这是 DSI **2.0** 主机，与老的 dw-mipi-dsi（1.x）寄存器布局完全
 *    不同。网上大量 Rockchip MIPI 资料讲的是 1.x（RK3399/RK3568 那代），
 *    寄存器名相似但偏移不同 —— 照着 1.x 写不会报错，只是配置全部落空。
 *    RK3576 用的是 2.0。
 */

#define RK3576_DSI2_PWR_UP            0x000c
#define RK3576_DSI2_SOFT_RESET        0x0010
#define RK3576_DSI2_INT_ST_MAIN       0x0014
#define RK3576_DSI2_MODE_CTRL         0x0018
#define RK3576_DSI2_MODE_STATUS       0x001c
#define RK3576_DSI2_CORE_STATUS       0x0020
#define RK3576_DSI2_PHY_MODE_CFG      0x0100
#define RK3576_DSI2_PHY_CLK_CFG       0x0104
#define RK3576_DSI2_PHY_LP2HS_MAN_CFG 0x010c
#define RK3576_DSI2_PHY_HS2LP_MAN_CFG 0x0114
#define RK3576_DSI2_PHY_IPI_RATIO_MAN_CFG 0x0134
#define RK3576_DSI2_PHY_SYS_RATIO_MAN_CFG 0x013c
#define RK3576_DSI2_DSI_GENERAL_CFG   0x0200
#define RK3576_DSI2_DSI_VCID_CFG      0x0204
#define RK3576_DSI2_DSI_VID_TX_CFG    0x020c
#define RK3576_DSI2_CRI_TX_HDR        0x02c0   /* 命令发送：包头 */
#define RK3576_DSI2_CRI_TX_PLD        0x02c4   /* 命令发送：负载 */
#define RK3576_DSI2_CRI_RX_HDR        0x02c8
#define RK3576_DSI2_CRI_RX_PLD        0x02cc
#define RK3576_DSI2_IPI_COLOR_MAN_CFG 0x0300
#define RK3576_DSI2_IPI_VID_HSA_MAN_CFG   0x0304
#define RK3576_DSI2_IPI_VID_HBP_MAN_CFG   0x030c
#define RK3576_DSI2_IPI_VID_HACT_MAN_CFG  0x0314
#define RK3576_DSI2_IPI_VID_HLINE_MAN_CFG 0x031c
#define RK3576_DSI2_IPI_VID_VSA_MAN_CFG   0x0324
#define RK3576_DSI2_IPI_VID_VBP_MAN_CFG   0x032c
#define RK3576_DSI2_IPI_VID_VACT_MAN_CFG  0x0334
#define RK3576_DSI2_IPI_VID_VFP_MAN_CFG   0x033c
#define RK3576_DSI2_IPI_PIX_PKT_CFG       0x0344
#define RK3576_DSI2_MANUAL_MODE_CFG       0x0024
#define RK3576_DSI2_PHY_LP2HS_MAN_CFG     0x010c
#define RK3576_DSI2_PHY_HS2LP_MAN_CFG     0x0114
#define RK3576_DSI2_PHY_IPI_RATIO_MAN_CFG 0x0134
#define RK3576_DSI2_PHY_SYS_RATIO_MAN_CFG 0x013c

#define DSI2_MANUAL_MODE_EN               (1u << 0)

/* DSI_VID_TX_CFG (0x020c) —— 视频模式类型与消隐段是否走高速 */

#define DSI2_VID_MODE_NON_BURST_SYNC_PULSES 0
#define DSI2_VID_MODE_NON_BURST_SYNC_EVENTS 1
#define DSI2_VID_MODE_BURST                 2
#define DSI2_BLK_HSA_HS_EN                (1u << 4)
#define DSI2_BLK_HBP_HS_EN                (1u << 5)
#define DSI2_BLK_HFP_HS_EN                (1u << 6)

/* IPI_COLOR_MAN_CFG 的位域 */

#define DSI2_IPI_DEPTH_8_BITS             5
#define DSI2_IPI_DEPTH_SHIFT              4
#define DSI2_IPI_FORMAT_RGB               0
#define RK3576_DSI2_IPI_PIX_PKT_CFG   0x0344
#define RK3576_DSI2_INT_ST_PHY        0x0400
#define RK3576_DSI2_INT_MASK_PHY      0x0404
#define RK3576_DSI2_INT_ST_CRI        0x0460
#define RK3576_DSI2_INT_MASK_CRI      0x0464

/* PWR_UP */

#define DSI2_POWER_UP                 (1 << 0)
#define DSI2_RESET                    0

/* SOFT_RESET —— 三个复位位，全部置 1 才是"释放复位" */

#define DSI2_SYS_RSTN                 (1 << 2)
#define DSI2_PHY_RSTN                 (1 << 1)
#define DSI2_IPI_RSTN                 (1 << 0)

/* PHY_MODE_CFG */

#define DSI2_PHY_LANES(x)             (((x) - 1) << 4)
#define DSI2_PPI_WIDTH(x)             ((x) << 8)
#define DSI2_PHY_TYPE_DPHY            0
#define DSI2_PHY_TYPE_CPHY            1

/* PHY_CLK_CFG */

#define DSI2_PHY_LPTX_CLK_DIV(x)      ((x) << 8)
#define DSI2_NON_CONTINUOUS_CLK       (1 << 0)
#define DSI2_CONTINUOUS_CLK           0

/* MODE_CTRL：0 = 命令模式，1 = 视频模式 */

/* MODE_CTRL / MODE_STATUS 的取值。
 *
 * ★ 出处 drivers/gpu/drm/bridge/synopsys/dw-mipi-dsi2.c 的 enum mode_ctrl。
 *   此前这里错写成 CMD=0、VIDEO=1 —— 那其实是 IDLE 和 AUTOCALC，
 *   DSI 从来没进过视频模式。而 MODE_STATUS 读回 1 看着像"成功"，
 *   实际是 AUTOCALC 的编号。枚举值一定要抄来源，不能按顺序猜。
 */

#define DSI2_MODE_IDLE                0
#define DSI2_MODE_AUTOCALC            1
#define DSI2_MODE_CMD                 2
#define DSI2_MODE_VIDEO               3
#define DSI2_MODE_DATA_STREAM         4

/* CORE_STATUS */

#define DSI2_CRI_BUSY                 (1 << 16)
#define DSI2_CRI_RD_DATA_AVAIL        (1 << 18)

/* DSI_GENERAL_CFG */

#define DSI2_EOTP_TX_EN               (1 << 0)
#define DSI2_BTA_EN                   (1 << 1)

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_DSI2_H */
