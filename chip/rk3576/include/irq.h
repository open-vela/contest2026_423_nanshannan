/****************************************************************************
 * chip/rk3576/include/irq.h
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

/* This file should never be included directly but, rather,
 * only indirectly through nuttx/irq.h
 */

#ifndef __ARCH_ARM64_INCLUDE_RK3576_IRQ_H
#define __ARCH_ARM64_INCLUDE_RK3576_IRQ_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Rockchip RK3576 Interrupts
 *
 * GICv3 中断号布局（与 SoC 无关的固定部分）：
 *   0  - 15   SGI  软件生成中断
 *   16 - 31   PPI  私有外设中断（含 Generic Timer）
 *   32 - ...  SPI  共享外设中断  ← 外设中断号 = dtsi 里的 SPI 号 + 32
 *
 * NR_IRQS：rk3576.dtsi 中出现的最大 SPI 号是 388，对应中断号
 * 388 + 32 = 420。取 448（32 的整数倍）留余量。
 * 该值偏大只多占一点内存，偏小会让对应外设中断挂不上。
 *
 * ★ 这里原先写的是 288，依据是"最大 SPI 号为 231" —— 那个数来自
 *   **RK3568** 的 rk356x-base.dtsi，不是 RK3576。实际影响到的是所有
 *   SPI 号大于 255 的外设，其中就有 GMAC：
 *       gmac0 macirq = GIC_SPI 293 -> IRQ 325
 *       gmac1 macirq = GIC_SPI 301 -> IRQ 333
 *   两个都落在 288 之外，irq_attach() 直接失败。
 */

#define NR_IRQS                 448

/* Software Generated Interrupts (SGI) */

#define RK3576_IRQ_SGI0         (0)
#define RK3576_IRQ_SGI1         (1)
#define RK3576_IRQ_SGI2         (2)
#define RK3576_IRQ_SGI3         (3)
#define RK3576_IRQ_SGI4         (4)
#define RK3576_IRQ_SGI5         (5)
#define RK3576_IRQ_SGI6         (6)
#define RK3576_IRQ_SGI7         (7)
#define RK3576_IRQ_SGI8         (8)
#define RK3576_IRQ_SGI9         (9)
#define RK3576_IRQ_SGI10        (10)
#define RK3576_IRQ_SGI11        (11)
#define RK3576_IRQ_SGI12        (12)
#define RK3576_IRQ_SGI13        (13)
#define RK3576_IRQ_SGI14        (14)
#define RK3576_IRQ_SGI15        (15)

/* Private Peripheral Interrupts (PPI) */

#define RK3576_IRQ_PPI0         (16)
#define RK3576_IRQ_PPI1         (17)
#define RK3576_IRQ_PPI2         (18)
#define RK3576_IRQ_PPI3         (19)
#define RK3576_IRQ_PPI4         (20)
#define RK3576_IRQ_PPI5         (21)
#define RK3576_IRQ_PPI6         (22)
#define RK3576_IRQ_PPI7         (23)
#define RK3576_IRQ_PPI8         (24)
#define RK3576_IRQ_PPI9         (25)
#define RK3576_IRQ_PPI10        (26)
#define RK3576_IRQ_PPI11        (27)
#define RK3576_IRQ_PPI12        (28)
#define RK3576_IRQ_PPI13        (29)
#define RK3576_IRQ_PPI14        (30)
#define RK3576_IRQ_PPI15        (31)

/* Shared Peripheral Interrupts (SPI)
 *
 * dtsi 里写的是 SPI 号，实际中断号要 +32。用下面的宏做转换，
 * 避免每次手算出错。
 */

#define RK3576_IRQ_SPI(n)       ((n) + 32)

/* UART。出处：厂商 SDK 的 kernel-6.1 rk3576.dtsi。
 *
 * ★ 这里原先写的是 SPI 116~120，标注「来源：rk356x-base.dtsi」——
 *   那是 **RK3568** 的编号，整整差了 40。RK3576 的 UART0 是 SPI 76。
 *
 *   之所以一直没暴露：串口实际用的中断号来自 defconfig 的
 *   CONFIG_16550_UART0_IRQ=108（= 76 + 32，是对的），这些常量没人引用。
 *   同一份 RK3568 dtsi 还带来了 NR_IRQS=288 那个真缺陷 —— GMAC 的
 *   中断号 325/333 落在范围外，irq_attach() 直接失败。
 *
 *   **凡是标注来源为 rk356x 的常量都要按 RK3576 的 dtsi 复核一遍。**
 */

#define RK3576_IRQ_UART0        RK3576_IRQ_SPI(76)    /* 108 ← 调试口 */
#define RK3576_IRQ_UART1        RK3576_IRQ_SPI(77)    /* 109 */
#define RK3576_IRQ_UART2        RK3576_IRQ_SPI(78)    /* 110 */
#define RK3576_IRQ_UART3        RK3576_IRQ_SPI(79)    /* 111 */
#define RK3576_IRQ_UART4        RK3576_IRQ_SPI(80)    /* 112 */
#define RK3576_IRQ_UART5        RK3576_IRQ_SPI(81)    /* 113 */
#define RK3576_IRQ_UART6        RK3576_IRQ_SPI(82)    /* 114 */
#define RK3576_IRQ_UART7        RK3576_IRQ_SPI(83)    /* 115 */
#define RK3576_IRQ_UART10       RK3576_IRQ_SPI(86)    /* 118 */
#define RK3576_IRQ_UART11       RK3576_IRQ_SPI(87)    /* 119 */

/* Mailbox。出处：TRM Part1 V1.2 中断表（表里列的就是 GIC INTID，不用再 +32；
 * 与 dtsi 的 mailbox0 = GIC_SPI 125 → 157 一致，两处互为旁证）。
 *
 * AP = B2A 方向的收端，BB = A2B 方向的收端。AMP 里 Linux 站 AP、
 * openvela 站 BB，所以我们挂的是 BB 那一组。
 */

#define RK3576_IRQ_MAILBOX_AP(n) (157 + (n))   /* n = 0..13 */
#define RK3576_IRQ_MAILBOX_BB(n) (171 + (n))   /* n = 0..13 */

/* GMAC。出处同上：gmac0 macirq = SPI 293、gmac1 macirq = SPI 301。 */

#define RK3576_IRQ_GMAC0        RK3576_IRQ_SPI(293)   /* 325 */
#define RK3576_IRQ_GMAC1        RK3576_IRQ_SPI(301)   /* 333 */

#endif /* __ARCH_ARM64_INCLUDE_RK3576_IRQ_H */
