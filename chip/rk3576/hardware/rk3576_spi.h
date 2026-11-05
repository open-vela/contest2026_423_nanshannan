/****************************************************************************
 * chip/rk3576/hardware/rk3576_spi.h
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

/* RK3576 SPI 控制器寄存器定义。
 *
 * IP 是 Synopsys DesignWare SSI 的 Rockchip 改版，compatible
 * "rockchip,rk3066-spi"。寄存器布局取自内核 drivers/spi/spi-rockchip.c，
 * 与 RK3576 TRM 的 SPI 章节一致。
 *
 * 与原版 DW SSI 的区别（照抄 DW 手册会踩的坑）：
 *   - 数据口不在 0x60，收发分开：TXDR 0x400 / RXDR 0x800。
 *   - CTRLR0 的位域被重排过，SCPH/SCPOL 在 6/7 而不是 DW 的 6/7 之外，
 *     XFM(传输方向) 在 18，DW 原版叫 TMOD 在 8。
 *   - BAUDR 的分频值必须是偶数。
 */

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_SPI_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_SPI_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 寄存器偏移 ***************************************************************/

#define RK3576_SPI_CTRLR0        0x0000  /* 控制寄存器 0 */
#define RK3576_SPI_CTRLR1        0x0004  /* 帧数 - 1（只对只收模式有意义）*/
#define RK3576_SPI_SSIENR        0x0008  /* 控制器使能 */
#define RK3576_SPI_SER           0x000c  /* 片选使能（每位一个 CS）*/
#define RK3576_SPI_BAUDR         0x0010  /* 分频，必须是偶数 */
#define RK3576_SPI_TXFTLR        0x0014  /* 发送 FIFO 阈值 */
#define RK3576_SPI_RXFTLR        0x0018  /* 接收 FIFO 阈值 */
#define RK3576_SPI_TXFLR         0x001c  /* 发送 FIFO 当前深度 */
#define RK3576_SPI_RXFLR         0x0020  /* 接收 FIFO 当前深度 */
#define RK3576_SPI_SR            0x0024  /* 状态 */
#define RK3576_SPI_IPR           0x0028
#define RK3576_SPI_IMR           0x002c  /* 中断屏蔽 */
#define RK3576_SPI_ISR           0x0030
#define RK3576_SPI_RISR          0x0034
#define RK3576_SPI_ICR           0x0038  /* 中断清除 */
#define RK3576_SPI_DMACR         0x003c
#define RK3576_SPI_DMATDLR       0x0040
#define RK3576_SPI_DMARDLR       0x0044
#define RK3576_SPI_VERSION       0x0048  /* 版本，用来推 FIFO 深度 */
#define RK3576_SPI_TXDR          0x0400  /* 发送数据 */
#define RK3576_SPI_RXDR          0x0800  /* 接收数据 */

/* CTRLR0 位域 **************************************************************/

#define SPI_CR0_DFS_SHIFT        0       /* 数据帧长度 */
#define SPI_CR0_DFS_4BIT         0
#define SPI_CR0_DFS_8BIT         1
#define SPI_CR0_DFS_16BIT        2

#define SPI_CR0_SCPH_SHIFT       6       /* 时钟相位  = SPI mode 的 bit0 */
#define SPI_CR0_SCPOL_SHIFT      7       /* 时钟极性  = SPI mode 的 bit1 */

#define SPI_CR0_CSM_SHIFT        8       /* 帧间 CS 保持方式 */
#define SPI_CR0_CSM_KEEP         0       /* 连续传输期间 CS 一直有效 */

#define SPI_CR0_SSD_SHIFT        10      /* CS 有效到 SCLK 有效的间隔 */
#define SPI_CR0_SSD_HALF         0
#define SPI_CR0_SSD_ONE          1

#define SPI_CR0_EM_SHIFT         11      /* 字节序 */
#define SPI_CR0_EM_BIG           1

#define SPI_CR0_FBM_SHIFT        12      /* 先发高位还是低位 */
#define SPI_CR0_FBM_MSB          0
#define SPI_CR0_FBM_LSB          1

#define SPI_CR0_BHT_SHIFT        13      /* APB 侧半字/字节访问宽度 */
#define SPI_CR0_BHT_8BIT         1

#define SPI_CR0_RSD_SHIFT        14      /* 采样延迟，单位为 SCLK 周期 */
#define SPI_CR0_RSD_MAX          3

#define SPI_CR0_FRF_SHIFT        16      /* 帧格式 */
#define SPI_CR0_FRF_SPI          0

#define SPI_CR0_XFM_SHIFT        18      /* 传输方向 */
#define SPI_CR0_XFM_TR           0       /* 全双工，收发同时 */
#define SPI_CR0_XFM_TO           1       /* 只发 */
#define SPI_CR0_XFM_RO           2       /* 只收（帧数由 CTRLR1 决定）*/

#define SPI_CR0_OPM_SHIFT        20      /* 主/从 */
#define SPI_CR0_OPM_MASTER       0

#define SPI_CR0_SOI_SHIFT        23      /* CS 空闲电平（置位 = 高有效）*/

/* SR 位域 ******************************************************************/

#define SPI_SR_BUSY              (1 << 0)
#define SPI_SR_TF_FULL           (1 << 1)
#define SPI_SR_TF_EMPTY          (1 << 2)
#define SPI_SR_RF_EMPTY          (1 << 3)
#define SPI_SR_RF_FULL           (1 << 4)

/* BAUDR 取值范围 ***********************************************************/

#define SPI_BAUDR_MIN            2
#define SPI_BAUDR_MAX            65534

/* VERSION -> FIFO 深度 *****************************************************/

#define SPI_VER2_TYPE1           0x05ec0002
#define SPI_VER2_TYPE2           0x00110002
#define SPI_FIFO_LEN_V2          64
#define SPI_FIFO_LEN_V1          32

/* 主机内部逻辑能跑到的 SCLK 上限，出处是内核驱动的 MAX_SCLK_OUT */

#define SPI_MAX_SCLK_HZ          50000000u

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_SPI_H */
