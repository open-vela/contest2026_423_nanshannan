/****************************************************************************
 * chip/rk3576/hardware/rk3576_pl330.h
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

#ifndef __CHIP_RK3576_HARDWARE_RK3576_PL330_H
#define __CHIP_RK3576_HARDWARE_RK3576_PL330_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* ARM PrimeCell PL330 (DMA-330)。
 *
 * ★ 这颗 DMA 不是"填几个寄存器就开跑"的那种。
 *
 *   PL330 是**微码机**：每条通道跑一段放在内存里的指令流，由 DMAC 自己
 *   取指执行。要搬数据，得先在内存里**编译**出一小段程序（DMAMOV 设地址、
 *   DMAWFP 等外设请求、DMALDP/DMAST 搬一个突发、DMALPEND 循环、DMASEV
 *   发事件、DMAEND 结束），再通过调试接口塞一条 DMAGO 让某条通道从那个
 *   地址开始执行。
 *
 *   因此本驱动里有两套完全不同的东西，别混：
 *     - 寄存器（下面 offset 那批）：给 CPU 读状态、发调试指令用；
 *     - 指令编码（CMD_* / SZ_*）：给我们生成微码用，DMAC 才是执行者。
 *
 * 出处：ARM DMA-330 TRM，以及 Linux drivers/dma/pl330.c 的寄存器与
 *       指令编码（后者是可执行的、被大量硬件验证过的参考）。
 *
 * ★ 板上连接（rk3576.dtsi）：
 *     dmac0 @0x2ab90000  中断 SPI 32/33   时钟 ACLK_DMAC0
 *     dmac1 @0x2abb0000  中断 SPI 34/35
 *     dmac2 @0x2abd0000  中断 SPI 36/37
 *     sai1: dmas = <&dmac0 2>, <&dmac0 3>  → 发送请求 2、接收请求 3
 */

#define RK3576_DMAC0_BASE      0x2ab90000
#define RK3576_DMAC1_BASE      0x2abb0000
#define RK3576_DMAC2_BASE      0x2abd0000

/* 中断：NuttX 的中断号 = GIC SPI 号 + 32
 * （对照 uart4 的 SPI 80 = 112 确认）
 */

#define RK3576_IRQ_DMAC0_0     (32 + 32)
#define RK3576_IRQ_DMAC0_1     (33 + 32)

/* 寄存器偏移 */

#define PL330_DS               0x0000  /* 管理线程状态 */
#define PL330_DPC              0x0004  /* 管理线程 PC  */
#define PL330_INTEN            0x0020  /* 事件→中断使能 */
#define PL330_ES               0x0024  /* 事件状态      */
#define PL330_INTSTATUS        0x0028  /* 中断状态      */
#define PL330_INTCLR           0x002c  /* 中断清除      */
#define PL330_FSM              0x0030  /* 管理线程故障  */
#define PL330_FSC              0x0034  /* 通道故障位图  */
#define PL330_FTM              0x0038  /* 管理线程故障类型 */

#define PL330_FTC(n)           (0x0040 + (n) * 4) /* 通道故障类型 */
#define PL330_CS(n)            (0x0100 + (n) * 8) /* 通道状态 */
#define PL330_CPC(n)           (0x0104 + (n) * 8) /* 通道 PC  */
#define PL330_SA(n)            (0x0400 + (n) * 0x20)
#define PL330_DA(n)            (0x0404 + (n) * 0x20)
#define PL330_CC(n)            (0x0408 + (n) * 0x20)
#define PL330_LC0(n)           (0x040c + (n) * 0x20)
#define PL330_LC1(n)           (0x0410 + (n) * 0x20)
#define PL330_DBGSTATUS        0x0d00
#define PL330_DBGCMD           0x0d04
#define PL330_DBGINST0         0x0d08
#define PL330_DBGINST1         0x0d0c
#define PL330_CR0              0x0e00
#define PL330_CR1              0x0e04
#define PL330_CR2              0x0e08
#define PL330_CR3              0x0e0c
#define PL330_CR4              0x0e10
#define PL330_CRD              0x0e14
#define PL330_PERIPH_ID        0x0fe0

/* 通道状态 CS(n) 低 4 位 */

#define PL330_CS_STATUS(v)     ((v) & 0xf)
#define PL330_ST_STOP          0x0
#define PL330_ST_EXEC          0x1
#define PL330_ST_CMISS         0x2
#define PL330_ST_UPDTPC        0x3
#define PL330_ST_WFE           0x4
#define PL330_ST_ATBRR         0x5
#define PL330_ST_QBUSY         0x6
#define PL330_ST_WFP           0x7
#define PL330_ST_KILL          0x8
#define PL330_ST_CMPLT         0x9
#define PL330_ST_FLTCMP        0xe
#define PL330_ST_FAULT         0xf

#define PL330_DBG_BUSY         (1 << 0)

/* CR0：这颗核实际配了几条通道、几个外设请求口、几个事件 */

/* ★ bit2 = 管理线程的启动安全态。
 *
 *   这一位决定两件事必须一致：DMAGO 的 ns 位，和 CCR 里的 SRCNS/DSTNS。
 *   非安全的管理线程发 ns=0 的 DMAGO，或非安全通道配安全的 AXI 访问，
 *   都会被判安全违例 —— 现象是"DMA 起了但永远不完成"，看不出跟安全态
 *   有关。实测本 SoC 的 CR0=0x001ff075，bit2=1，是**非安全**启动。
 */

#define PL330_CR0_BOOT_MAN_NS    (1 << 2)

#define PL330_CR0_NUM_CHANS(v)   ((((v) >> 4)  & 0x7)  + 1)
#define PL330_CR0_NUM_PERIPH(v)  ((((v) >> 12) & 0x1f) + 1)
#define PL330_CR0_NUM_EVENTS(v)  ((((v) >> 17) & 0x1f) + 1)

/* 通道控制字 CC(n) / DMAMOV CCR 的内容
 *
 * ★ 突发"大小"和"长度"是两回事，写错了不报错、只是搬得不对：
 *     size = 每次传输的字节数，编码是 log2(bytes)，[2:0] 源 / [17:15] 目的
 *     len  = 一个突发里做几次传输，编码是 n-1，[7:4] 源 / [21:18] 目的
 */

#define PL330_CC_SRCINC          (1 << 0)
#define PL330_CC_SRCBRSTSIZE(n)  ((n) << 1)    /* log2(字节) */
#define PL330_CC_SRCBRSTLEN(n)   (((n) - 1) << 4)
#define PL330_CC_SRCPROT_PRIV    (1 << 8)
#define PL330_CC_SRCPROT_NS      (1 << 9)
#define PL330_CC_SRCCCTRL(n)     ((n) << 11)
#define PL330_CC_DSTINC          (1 << 14)
#define PL330_CC_DSTBRSTSIZE(n)  ((n) << 15)
#define PL330_CC_DSTBRSTLEN(n)   (((n) - 1) << 18)
#define PL330_CC_DSTPROT_PRIV    (1 << 22)
#define PL330_CC_DSTPROT_NS      (1 << 23)
#define PL330_CC_DSTCCTRL(n)     ((n) << 25)
#define PL330_CC_SWAP(n)         ((n) << 28)

/* cache 控制编码：0 = 不可缓存不可缓冲，1 = 只缓冲。
 * 我们靠软件做 cache 维护（见 rk3576_pl330.c 的说明），所以取 0。
 */

#define PL330_CCTRL_NONCACHEABLE 0

/* 微码指令编码 */

#define PL330_CMD_DMAEND       0x00
#define PL330_CMD_DMAKILL      0x01
#define PL330_CMD_DMALD        0x04
#define PL330_CMD_DMAST        0x08
#define PL330_CMD_DMAADDH      0x54
#define PL330_CMD_DMAFLUSHP    0x35
#define PL330_CMD_DMAGO        0xa0
#define PL330_CMD_DMALDP       0x25
#define PL330_CMD_DMALP        0x20
#define PL330_CMD_DMALPEND     0x28
#define PL330_CMD_DMAMOV       0xbc
#define PL330_CMD_DMANOP       0x18
#define PL330_CMD_DMARMB       0x12
#define PL330_CMD_DMASEV       0x34
#define PL330_CMD_DMASTP       0x29
#define PL330_CMD_DMAWFP       0x30
#define PL330_CMD_DMAWMB       0x13

#define PL330_SZ_DMAEND        1
#define PL330_SZ_DMAKILL       1
#define PL330_SZ_DMALD         1
#define PL330_SZ_DMAST         1
#define PL330_SZ_DMAFLUSHP     2
#define PL330_SZ_DMALDP        2
#define PL330_SZ_DMALP         2
#define PL330_SZ_DMALPEND      2
#define PL330_SZ_DMASEV        2
#define PL330_SZ_DMASTP        2
#define PL330_SZ_DMAWFP        2
#define PL330_SZ_DMARMB        1
#define PL330_SZ_DMAWMB        1
#define PL330_SZ_DMAMOV        6
#define PL330_SZ_DMAGO         6

/* DMAMOV 的目标寄存器编码 */

#define PL330_MOV_SAR          0
#define PL330_MOV_CCR          1
#define PL330_MOV_DAR          2

#endif /* __CHIP_RK3576_HARDWARE_RK3576_PL330_H */
