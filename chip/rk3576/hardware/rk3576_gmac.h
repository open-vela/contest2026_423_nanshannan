/****************************************************************************
 * chip/rk3576/hardware/rk3576_gmac.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_GMAC_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_GMAC_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* RK3576 的以太网控制器是 Synopsys DesignWare MAC 4.20a
 * （dtsi: compatible = "rockchip,rk3576-gmac", "snps,dwmac-4.20a"），
 * 即 Linux 里 stmmac 驱动那一系。
 *
 * ★ NuttX 里已有同 IP 家族的成熟实现可参照：
 *     arch/arm/src/stm32h7/stm32_ethernet.c（4267 行）
 *   STM32H7 用的是 DWC_ether_qos，寄存器布局与 4.20a 一致 ——
 *   已核对三处标志性偏移：MACCR=0x0000、MTL=0x0C00、DMA=0x1000。
 *   描述符结构、MDIO 时序、中断处理都可以照它的结构来。
 *
 * ★ 板上连接（原厂 dtb）：
 *     GMAC0 0x2a220000  GIC_SPI 293 → IRQ 325
 *     GMAC1 0x2a230000  GIC_SPI 301 → IRQ 333
 *     PHY 挂在各自的 MDIO 上，地址 0，c22 兼容
 *     两者同属电源域 PD_SDGMAC
 */

/* MAC 寄存器块 */

#define RK3576_GMAC_MAC_CONFIG        0x0000
#define RK3576_GMAC_MAC_PKT_FILTER    0x0008
#define RK3576_GMAC_MAC_HW_FEATURE0   0x011c   /* 只读，硬件能力     */
#define RK3576_GMAC_MAC_HW_FEATURE1   0x0120
#define RK3576_GMAC_MAC_VERSION       0x0110   /* 只读，IP 版本      */
#define RK3576_GMAC_MAC_MDIO_ADDR     0x0200
#define RK3576_GMAC_MAC_MDIO_DATA     0x0204
#define RK3576_GMAC_MAC_ADDR0_HIGH    0x0300
#define RK3576_GMAC_MAC_ADDR0_LOW     0x0304

/* MTL（队列层） */

#define RK3576_GMAC_MTL_OPMODE        0x0c00
#define RK3576_GMAC_MTL_TXQ0_OPMODE   0x0d00
#define RK3576_GMAC_MTL_RXQ0_OPMODE   0x0d30

/* DMA */

#define RK3576_GMAC_DMA_MODE          0x1000
#define RK3576_GMAC_DMA_SYSBUS_MODE   0x1004
#define RK3576_GMAC_DMA_CH0_CTRL      0x1100
#define RK3576_GMAC_DMA_CH0_TX_CTRL   0x1104
#define RK3576_GMAC_DMA_CH0_RX_CTRL   0x1108
#define RK3576_GMAC_DMA_CH0_TXDESC_LA 0x1114   /* 发送描述符表基址   */
#define RK3576_GMAC_DMA_CH0_RXDESC_LA 0x111c
#define RK3576_GMAC_DMA_CH0_TXDESC_TP 0x1120   /* 发送尾指针         */
#define RK3576_GMAC_DMA_CH0_RXDESC_TP 0x1128
#define RK3576_GMAC_DMA_CH0_TXDESC_RL 0x112c   /* 描述符环长度       */
#define RK3576_GMAC_DMA_CH0_RXDESC_RL 0x1130
#define RK3576_GMAC_DMA_CH0_IE        0x1134
#define RK3576_GMAC_DMA_CH0_STATUS    0x1160

/* MAC_CONFIG */

#define GMAC_MAC_CONFIG_RE            (1 << 0)   /* 接收使能  */
#define GMAC_MAC_CONFIG_TE            (1 << 1)   /* 发送使能  */
#define GMAC_MAC_CONFIG_DM            (1 << 13)  /* 全双工    */
#define GMAC_MAC_CONFIG_FES           (1 << 14)  /* 100Mbps   */
#define GMAC_MAC_CONFIG_PS            (1 << 15)  /* 端口选择：MII/RMII */

/* DMA_MODE */

#define GMAC_DMA_MODE_SWR             (1 << 0)   /* 软复位，自清 */

/* MDIO_ADDR */

#define GMAC_MDIO_ADDR_GB             (1 << 0)   /* 忙标志       */
#define GMAC_MDIO_ADDR_C45E           (1 << 1)
#define GMAC_MDIO_ADDR_GOC_READ       (3 << 2)
#define GMAC_MDIO_ADDR_GOC_WRITE      (1 << 2)
#define GMAC_MDIO_ADDR_CR_SHIFT       8           /* 时钟分频     */
#define GMAC_MDIO_ADDR_RDA_SHIFT      16          /* 寄存器地址   */
#define GMAC_MDIO_ADDR_PA_SHIFT       21          /* PHY 地址     */

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_GMAC_H */
