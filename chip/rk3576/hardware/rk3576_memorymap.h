/****************************************************************************
 * arch/arm64/src/rk3576/hardware/rk3576_memorymap.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_MEMORYMAP_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_MEMORYMAP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <arch/chip/chip.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* ==========================================================================
 * 外设基址。来源：Linux 主线 arch/arm64/boot/dts/rockchip/rk3576.dtsi
 * 各节点的 reg 属性（许可证 GPL-2.0+ OR MIT，按 MIT 分支使用）。
 *
 * 全部落在 CONFIG_DEVICEIO_BASEADDR(0x20000000) + 192MB 的映射范围内。
 * 新增外设地址时先确认这一点，否则 MMU 不建映射，访问时直接异常。
 * ==========================================================================
 */

/* UART —— RK3576 共 12 路，M1 只需要调试串口一路。
 *
 * 全部为 snps,dw-apb-uart（DesignWare 8250，16550 兼容），
 * dtsi 属性 reg-shift = <2>、reg-io-width = <4>，即寄存器按 4 字节步进、
 * 32 位访问。对应 defconfig 的 CONFIG_16550_REGINCR=4 / REGWIDTH=32。
 *
 * 中断号：dtsi 给的是 GIC_SPI 号，NuttX 的 IRQ 号需 +32。
 *
 * ★ 调试口已确认为 UART0，依据 KICKPI 官方 Armbian 源码中的 U-Boot
 * defconfig（kickpi-k7-rk3576_defconfig）：CONFIG_DEBUG_UART_BASE=0x2ad40000、
 * CONFIG_DEBUG_UART_CLOCK=24000000、CONFIG_DEBUG_UART_SHIFT=2、
 * CONFIG_BAUDRATE=1500000。
 */

#define RK3576_UART0_ADDR      0x2ad40000   /* SPI 76 -> IRQ 108  <- M1 调试口 */
#define RK3576_UART1_ADDR      0x27310000
#define RK3576_UART2_ADDR      0x2ad50000   /* SPI 78 -> IRQ 110 */
#define RK3576_UART3_ADDR      0x2ad60000   /* SPI 79 -> IRQ 111 */
#define RK3576_UART4_ADDR      0x2ad70000   /* SPI 80 -> IRQ 112 */
#define RK3576_UART5_ADDR      0x2ad80000   /* SPI 81 -> IRQ 113 */
#define RK3576_UART6_ADDR      0x2ad90000   /* SPI 82 -> IRQ 114 */
#define RK3576_UART7_ADDR      0x2ada0000   /* SPI 83 -> IRQ 115 */
#define RK3576_UART8_ADDR      0x2adb0000   /* SPI 84 -> IRQ 116 */
#define RK3576_UART9_ADDR      0x2adc0000   /* SPI 85 -> IRQ 117 */
#define RK3576_UART10_ADDR     0x2afc0000   /* SPI 86 -> IRQ 118 */
#define RK3576_UART11_ADDR     0x2afd0000   /* SPI 87 -> IRQ 119 */

#define RK3576_UART_DBG_ADDR   RK3576_UART0_ADDR
#define RK3576_UART_DBG_IRQ    108

/* CRU —— M1 用不到（调试串口的时钟由 U-Boot 配好）。
 * RK3576 的 CRU 与 RK3399/RK3568 差异很大，是本平台最不能照抄的部分。
 */

/* ★ CRU 不是单块，而是分散在多个基址上（出处：RK3576 TRM 第 2 章
 *   "The CRU is located at several addresses"）。
 *   主线源码里 RK3576_CLKGATE_CON() 与 RK3576_PMU_CLKGATE_CON() 两套宏
 *   正是对应不同的块，用错基址会写到别的时钟上且不报错。
 */

#define RK3576_CRU_ADDR        0x27200000   /* always-on，多数外设在此   */
#define RK3576_PPLL_CRU_ADDR   0x27208000
#define RK3576_SECURE_CRU_ADDR 0x27210000
#define RK3576_PMU1_CRU_ADDR   0x27220000   /* PMU 域，如 PCLK_GPIO0     */
#define RK3576_DDR0_CRU_ADDR   0x27228000
#define RK3576_DDR1_CRU_ADDR   0x27230000
#define RK3576_BIGCORE_CRU_ADDR 0x27238000
#define RK3576_LITCORE_CRU_ADDR 0x27240000
#define RK3576_CCI_CRU_ADDR    0x27248000

/* GRF —— 引脚复用 IOMUX 在 ioc_grf 里。
 * 出处：主线 rk3576.dtsi 的 pinctrl 节点 rockchip,grf = <&ioc_grf>，
 *       ioc_grf: syscon@26040000（已与 rk3576_pinmux.c 的偏移表核对）。
 */

#define RK3576_SYS_GRF_ADDR    0x2600a000
#define RK3576_IOC_GRF_ADDR    0x26040000
#define RK3576_PMU0_GRF_ADDR   0x26024000
#define RK3576_PMU1_GRF_ADDR   0x26026000

/* GPIO —— 注意 GPIO0 与 GPIO1-4 不在同一段地址 */

/* 各 bank 的中断号。出处：主线 rk3576.dtsi 的 gpio@ 节点
 * interrupts = <GIC_SPI n ...>，NuttX 的 IRQ 号 = n + 32。
 */

#define RK3576_GPIO0_IRQ       185   /* GIC_SPI 153 */
#define RK3576_GPIO1_IRQ       189   /* GIC_SPI 157 */
#define RK3576_GPIO2_IRQ       193   /* GIC_SPI 161 */
#define RK3576_GPIO3_IRQ       197   /* GIC_SPI 165 */
#define RK3576_GPIO4_IRQ       201   /* GIC_SPI 169 */

#define RK3576_GPIO0_ADDR      0x27320000
#define RK3576_GPIO1_ADDR      0x2ae10000
#define RK3576_GPIO2_ADDR      0x2ae20000
#define RK3576_GPIO3_ADDR      0x2ae30000
#define RK3576_GPIO4_ADDR      0x2ae40000

/* I2C —— 同样分两段 */

#define RK3576_RNG_ADDR        0x2a410000   /* 硬件随机数 rockchip,rkrng */
#define RK3576_WDT_ADDR        0x2ace0000   /* 看门狗 snps,dw-wdt */
#define RK3576_PWM0_ADDR       0x27330000   /* PWM0 通道 0，板上红外接收 */
#define RK3576_I2C0_ADDR       0x27300000
#define RK3576_I2C1_ADDR       0x2ac40000
#define RK3576_I2C2_ADDR       0x2ac50000
#define RK3576_I2C3_ADDR       0x2ac60000
#define RK3576_I2C4_ADDR       0x2ac70000
#define RK3576_I2C5_ADDR       0x2ac80000
#define RK3576_I2C6_ADDR       0x2ac90000
#define RK3576_I2C7_ADDR       0x2aca0000
#define RK3576_I2C8_ADDR       0x2acb0000
#define RK3576_I2C9_ADDR       0x2ae80000

/* 存储控制器（供 M4 存储适配）
 *
 * 已由板上运行的出厂 Android 确认，其 bootargs 中：
 *   androidboot.boot_devices=2a2d0000.ufs,2a330000.mmc,2a310000.mmc
 * 与主线 dtsi 的节点一一对应。
 *
 *   sdhci : eMMC，8 位总线，HS400 1.8V + enhanced strobe，non-removable
 *   sdmmc : SD 卡，4 位总线
 */

#define RK3576_UFSHC_ADDR      0x2a2d0000   /* UFS 主控 */
#define RK3576_SDHCI_ADDR      0x2a330000   /* eMMC */
#define RK3576_SDMMC_ADDR      0x2a310000   /* SD 卡 */

/* 以太网（供 M5 网络适配）
 *
 * 已由板上运行的出厂 Android 内核日志确认：
 *   rk_gmac-dwmac 2a220000.ethernet eth0: PHY [stmmac-0:00] driver [MAE0621A-Q2C Gigabit Ethernet]
 *   rk_gmac-dwmac 2a230000.ethernet eth1: PHY [stmmac-1:00] driver [MAE0621A-Q2C Gigabit Ethernet]
 *   dwmac4: Master AXI performs any burst length
 *
 * 控制器为 Synopsys DesignWare MAC 4.x（stmmac），PHY 是 MAXIO MAE0621A，
 * rgmii-rxid 模式。PHY 复位脚见 board.h 的 BOARD_GMAC0/1_RST_*。
 */

#define RK3576_GMAC0_ADDR      0x2a220000   /* eth0 */
#define RK3576_GMAC1_ADDR      0x2a230000   /* eth1 */

/* 其它已确认的外设（暂未适配，记录备查） */

#define RK3576_PCIE_ADDR       0x2a200000   /* rk-pcie */
#define RK3576_USB_DWC3_ADDR   0x23000000   /* dwc3 */
#define RK3576_DP_ADDR         0x27e40000   /* DisplayPort */

/* PMU / SRAM */

#define RK3576_PMU_ADDR        0x27380000

/* SAI 音频接口。板上 ES8388 codec 接的是 SAI1（原厂 dtb 的
 * es8388-sound 节点 rockchip,cpu 指向 sai@2a610000）。
 */

/* 显示。出处：主线 rk3576.dtsi
 *   vop@27d00000（VOP2 显示控制器，PD_VOP）
 *   dsi@27d80000（MIPI DSI2 主机，PD_VO0）
 */

#define RK3576_VOP2_ADDR       0x27d00000
#define RK3576_VOP2_IRQ        374   /* GIC_SPI 342 */
#define RK3576_DSI0_ADDR       0x27d80000

/* MIPI D/C-PHY（combo，DSI 与 CSI 共用）。
 * 出处：主线 rk3576.dtsi 的 mipidcphy@2b020000。
 */

#define RK3576_DCPHY_ADDR      0x2b020000

#define RK3576_SAI1_ADDR       0x2a610000
#define RK3576_SAI1_IRQ        220   /* GIC_SPI 188 */
#define RK3576_SRAM_ADDR       0x3ff88000

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_MEMORYMAP_H */
