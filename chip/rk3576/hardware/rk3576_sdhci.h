/****************************************************************************
 * chip/rk3576/hardware/rk3576_sdhci.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_SDHCI_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_SDHCI_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* RK3576 的 eMMC 控制器是 DesignWare "dwcmshc"，遵循 SD Host Controller
 * 标准寄存器布局（与 RK3588 同款）。
 *
 * 出处：主线 rk3576.dtsi
 *   sdhci: mmc@2a330000 {
 *       compatible = "rockchip,rk3576-dwcmshc", "rockchip,rk3588-dwcmshc";
 *       reg = <0x0 0x2a330000 0x0 0x10000>;
 *       interrupts = <GIC_SPI 253 IRQ_TYPE_LEVEL_HIGH>;   → NuttX IRQ 285
 *       bus-width = <8>; mmc-hs400-1_8v; supports-cqe;
 *   };
 *
 * ★ 板上实测（U-Boot 交接后）：
 *     HOSTVER = 0x0005          SDHCI 规范版本 4.20
 *     CAP0    = 0x3a6dc881      基准时钟 200MHz、8 位总线、ADMA2、High Speed
 *     CAP1    = 0x08000007      SDR50 / SDR104 / DDR50
 *   基准时钟 200MHz 与 dtsi 的 assigned-clock-rates 一致，说明 U-Boot 已把
 *   PD_NVM 电源域和五路时钟配置好并保留，本端口暂不需要自行配置。
 */

/* --- SD Host Controller 标准寄存器 --- */

#define RK3576_SDHCI_DMAADDR        0x0000  /* SDMA 地址 / 参数2         */
#define RK3576_SDHCI_BLKSIZE        0x0004  /* [11:0] 块大小 [31:16] 块数 */
#define RK3576_SDHCI_ARG            0x0008  /* 命令参数                   */
#define RK3576_SDHCI_XFERMODE       0x000c  /* [15:0] 传输模式 [31:16] 命令 */
#define RK3576_SDHCI_RESP0          0x0010  /* 响应 0-3                   */
#define RK3576_SDHCI_RESP1          0x0014
#define RK3576_SDHCI_RESP2          0x0018
#define RK3576_SDHCI_RESP3          0x001c
#define RK3576_SDHCI_BUFDATA        0x0020  /* PIO 数据端口               */
#define RK3576_SDHCI_PRESENT        0x0024  /* 当前状态                   */
#define RK3576_SDHCI_HOSTCTRL1      0x0028  /* [7:0]主机控制 [15:8]电源   */
#define RK3576_SDHCI_CLKCTRL        0x002c  /* [15:0]时钟 [23:16]超时     */
#define RK3576_SDHCI_INTSTAT        0x0030  /* 中断状态，写 1 清          */
#define RK3576_SDHCI_INTEN          0x0034  /* 中断状态使能               */
#define RK3576_SDHCI_SIGEN          0x0038  /* 中断信号使能               */
#define RK3576_SDHCI_ACMDERR        0x003c
#define RK3576_SDHCI_HOSTCTRL2      0x003e  /* 16 位                      */
#define RK3576_SDHCI_CAP0           0x0040
#define RK3576_SDHCI_CAP1           0x0044
#define RK3576_SDHCI_ADMAERR        0x0054
#define RK3576_SDHCI_ADMAADDR       0x0058
#define RK3576_SDHCI_HOSTVER        0x00fc  /* [31:16] 版本               */

/* PRESENT（当前状态） */

#define SDHCI_PRESENT_CMDINHIBIT    (1 << 0)   /* 命令线忙 */
#define SDHCI_PRESENT_DATINHIBIT    (1 << 1)   /* 数据线忙 */
#define SDHCI_PRESENT_DATACTIVE     (1 << 2)
#define SDHCI_PRESENT_WRITEACTIVE   (1 << 8)
#define SDHCI_PRESENT_READACTIVE    (1 << 9)
#define SDHCI_PRESENT_BUFWRENABLE   (1 << 10)
#define SDHCI_PRESENT_BUFRDENABLE   (1 << 11)
#define SDHCI_PRESENT_CARDINSERTED  (1 << 16)

/* CLKCTRL（时钟与超时） */

#define SDHCI_CLK_INTCLKEN          (1 << 0)   /* 内部时钟使能   */
#define SDHCI_CLK_INTCLKSTABLE      (1 << 1)   /* 内部时钟已稳定 */
#define SDHCI_CLK_SDCLKEN           (1 << 2)   /* 卡时钟输出使能 */
#define SDHCI_CLK_GENSEL_PROG       (1 << 5)   /* 可编程分频模式 */
#define SDHCI_CLK_DIV_SHIFT         8          /* [15:8] 分频低 8 位  */
#define SDHCI_CLK_DIVHI_SHIFT       6          /* [7:6]  分频高 2 位  */
#define SDHCI_TIMEOUT_SHIFT         16         /* [19:16] 数据超时    */

/* 软复位（CLKCTRL 的 [26:24]） */

#define SDHCI_RESET_ALL             (1 << 24)
#define SDHCI_RESET_CMD             (1 << 25)
#define SDHCI_RESET_DATA            (1 << 26)

/* HOSTCTRL1 */

#define SDHCI_HOSTCTRL1_DTW4BIT     (1 << 1)
#define SDHCI_HOSTCTRL1_HISPEED     (1 << 2)
#define SDHCI_HOSTCTRL1_DTW8BIT     (1 << 5)
#define SDHCI_PWR_ON                (1 << 8)   /* 总线电源开     */
#define SDHCI_PWR_18V               (5 << 9)   /* 1.8V           */
#define SDHCI_PWR_30V               (6 << 9)
#define SDHCI_PWR_33V               (7 << 9)

/* 中断状态 / 使能位 */

#define SDHCI_INT_CMDCOMPLETE       (1 << 0)
#define SDHCI_INT_XFERCOMPLETE      (1 << 1)
#define SDHCI_INT_BLOCKGAP          (1 << 2)
#define SDHCI_INT_DMA               (1 << 3)
#define SDHCI_INT_BUFWRRDY          (1 << 4)
#define SDHCI_INT_BUFRDRDY          (1 << 5)
#define SDHCI_INT_CARDINSERT        (1 << 6)
#define SDHCI_INT_CARDREMOVE        (1 << 7)
#define SDHCI_INT_ERROR             (1 << 15)
#define SDHCI_INT_CMDTIMEOUT        (1 << 16)
#define SDHCI_INT_CMDCRC            (1 << 17)
#define SDHCI_INT_CMDENDBIT         (1 << 18)
#define SDHCI_INT_CMDINDEX          (1 << 19)
#define SDHCI_INT_DATATIMEOUT       (1 << 20)
#define SDHCI_INT_DATACRC           (1 << 21)
#define SDHCI_INT_DATAENDBIT        (1 << 22)
#define SDHCI_INT_CURRENTLIMIT      (1 << 23)
#define SDHCI_INT_AUTOCMD           (1 << 24)
#define SDHCI_INT_ADMA              (1 << 25)

#define SDHCI_INT_CMDERRORS         (SDHCI_INT_CMDTIMEOUT | SDHCI_INT_CMDCRC | \
                                     SDHCI_INT_CMDENDBIT | SDHCI_INT_CMDINDEX)
#define SDHCI_INT_DATAERRORS        (SDHCI_INT_DATATIMEOUT | SDHCI_INT_DATACRC | \
                                     SDHCI_INT_DATAENDBIT)
#define SDHCI_INT_ALLERRORS         (SDHCI_INT_CMDERRORS | SDHCI_INT_DATAERRORS | \
                                     SDHCI_INT_CURRENTLIMIT | SDHCI_INT_AUTOCMD | \
                                     SDHCI_INT_ADMA)

/* XFERMODE 的命令字段（写在 [31:16]） */

#define SDHCI_CMD_RESP_NONE         (0 << 0)
#define SDHCI_CMD_RESP_LEN136       (1 << 0)
#define SDHCI_CMD_RESP_LEN48        (2 << 0)
#define SDHCI_CMD_RESP_LEN48BUSY    (3 << 0)
#define SDHCI_CMD_CRCCHECK          (1 << 3)
#define SDHCI_CMD_INDEXCHECK        (1 << 4)
#define SDHCI_CMD_DATAPRESENT       (1 << 5)
#define SDHCI_CMD_INDEX_SHIFT       8

/* 传输模式（XFERMODE 的 [15:0]） */

#define SDHCI_XFER_DMAEN            (1 << 0)
#define SDHCI_XFER_BLKCNTEN         (1 << 1)
#define SDHCI_XFER_AUTOCMD12        (1 << 2)
#define SDHCI_XFER_DATAREAD         (1 << 4)
#define SDHCI_XFER_MULTIBLK         (1 << 5)

#define RK3576_SDHCI_BASECLK_HZ     200000000  /* 由 CAP0[15:8] 实测确认 */

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_SDHCI_H */
