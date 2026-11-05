/****************************************************************************
 * chip/rk3576/hardware/rk3576_sai.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_SAI_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_SAI_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* RK3576 的音频接口是 SAI（Serial Audio Interface），不是传统 I2S。
 *
 * ★ 出处：主线 rk3576.dtsi
 *     sai1: sai@2a610000 {
 *         compatible = "rockchip,rk3576-sai";
 *         clocks = <&cru MCLK_SAI1_8CH>, <&cru HCLK_SAI1_8CH>;
 *         interrupts = <GIC_SPI 188 IRQ_TYPE_LEVEL_HIGH>;   → IRQ 220
 *         power-domains = <&power RK3576_PD_AUDIO>;
 *         dmas = <&dmac0 2>, <&dmac0 3>;   （PL330）
 *     };
 *   寄存器定义取自 Linux sound/soc/rockchip/rockchip_sai.h。
 *
 * ★ 板上连接（原厂 dtb 的 es8388-sound：rockchip,cpu = sai1）：
 *     SAI1 ←→ ES8388 codec @I2C3:0x10
 *     引脚 sai1m0：sclk=gpio4-3  lrck=gpio4-5  sdo0=gpio4-7  sdi0=gpio4-11
 *          功能号均为 1
 */

#define RK3576_SAI_TXCR        0x0000  /* 发送控制           */
#define RK3576_SAI_FSCR        0x0004  /* 帧同步             */
#define RK3576_SAI_RXCR        0x0008  /* 接收控制           */
#define RK3576_SAI_MONO_CR     0x000c
#define RK3576_SAI_XFER        0x0010  /* 传输使能/状态      */
#define RK3576_SAI_CLR         0x0014  /* 复位               */
#define RK3576_SAI_CKR         0x0018  /* 时钟分频           */
#define RK3576_SAI_TXFIFOLR    0x001c  /* 发送 FIFO 水位     */
#define RK3576_SAI_RXFIFOLR    0x0020
#define SAI_RXFIFOLR_FULL       (1 << 23)   /* RX FIFO 满标志(实测: FIFO=8 时 0x208208 满位=0) */
#define SAI_RXFIFOLR_FULL2      (1 << 13)   /* FIFO=32 时 0x820820 满位=1 */

#define RK3576_SAI_DMACR       0x0024
#define RK3576_SAI_INTCR       0x0028
#define RK3576_SAI_INTSR       0x002c
#define RK3576_SAI_TXDR        0x0030  /* 发送数据端口(PIO)  */
#define RK3576_SAI_RXDR        0x0034
#define RK3576_SAI_PATH_SEL    0x0038
#define RK3576_SAI_TX_DATA_CNT 0x005c
#define RK3576_SAI_RX_DATA_CNT 0x0060

/* ★ 帧同步移位寄存器 —— 本文件此前**完全没有**这两个寄存器。
 *
 *   标准 I2S 的定义是"数据比帧同步边沿延后一个 BCLK"。这个延迟在
 *   RK3576 的 SAI 上就是靠 XSHIFT 实现的（原厂 rockchip_sai.c 的
 *   SND_SOC_DAIFMT_I2S 分支写 SAI_XSHIFT_RIGHT(2)）。不写它，控制器
 *   在错误的位位置上采样 —— 时钟、帧率、位宽全都对，只有数据不对。
 */

#define RK3576_SAI_TX_SHIFT    0x0064
#define RK3576_SAI_RX_SHIFT    0x0068

#define SAI_XSHIFT_RIGHT(x)    (x)          /* [23:0] 右移拍数 */
#define SAI_XSHIFT_LEFT(x)     ((x) << 24)  /* [25:24]        */
#define RK3576_SAI_STATUS      0x006c
#define RK3576_SAI_VERSION     0x0070  /* 只读，版本号       */

/* XFER —— 传输使能与空闲状态。
 *
 * ★ 使能位不是按 "TX/RX/FS" 的名字顺序排的，逐个照抄 Linux 的
 *   sound/soc/rockchip/rockchip_sai.h，不要按直觉推：
 *     bit0 = CLK 使能，bit1 = FS 使能，bit2 = TX 使能，bit3 = RX 使能
 *   本文件初版把 TXS 写成了 bit0（实为 CLK 使能）。
 */

#define SAI_XFER_CLK_EN        (1 << 0)   /* 时钟输出使能       */
#define SAI_XFER_FSS_EN        (1 << 1)   /* 帧同步使能         */
#define SAI_XFER_TXS_EN        (1 << 2)   /* 发送使能           */
#define SAI_XFER_RXS_EN        (1 << 3)   /* 接收使能           */
#define SAI_XFER_TX_CNT_EN     (1 << 4)
#define SAI_XFER_RX_CNT_EN     (1 << 5)
#define SAI_XFER_FS_IDLE       (1 << 6)   /* 只读：帧同步空闲   */
#define SAI_XFER_TX_IDLE       (1 << 7)   /* 只读：发送空闲     */
#define SAI_XFER_RX_IDLE       (1 << 8)   /* 只读：接收空闲     */

/* CLR —— 复位，写 1 后自清，需轮询归零 */

#define SAI_CLR_TXC            (1 << 0)
#define SAI_CLR_RXC            (1 << 1)
#define SAI_CLR_FSC            (1 << 2)

/* TXCR / RXCR 共用同一套位定义（原文前缀 SAI_XCR_，X = T 或 R）。
 * 括号里的参数都是"实际值"，宏内部会减一。
 */

#define SAI_XCR_CSR(x)         (((x) - 1) << 20)  /* 通道段数 */
#define SAI_XCR_SJM_L          (1 << 19)          /* 左对齐   */
#define SAI_XCR_SJM_R          0                  /* 右对齐   */
#define SAI_XCR_FBM_LSB        (1 << 18)
#define SAI_XCR_FBM_MSB        0                  /* MSB 在先 */
#define SAI_XCR_SNB(x)         (((x) - 1) << 11)  /* 每帧槽数 */
#define SAI_XCR_VDJ_L          (1 << 10)
#define SAI_XCR_VDJ_R          0
#define SAI_XCR_EDGE_SHIFT_1   (1 << 22)
#define SAI_XCR_EDGE_SHIFT_0   0
#define SAI_XCR_SBW(x)         (((x) - 1) << 5)   /* 槽位宽   */
#define SAI_XCR_VDW(x)         (((x) - 1) << 0)   /* 有效数据位宽 */

/* FSCR —— 帧同步 */

#define SAI_FSCR_EDGE_DUAL     (1 << 24)
#define SAI_FSCR_EDGE_RISING   0
#define SAI_FSCR_FPW(x)        (((x) - 1) << 12)  /* 帧脉冲宽 */
#define SAI_FSCR_FW(x)         (((x) - 1) << 0)   /* 帧宽     */

/* CKR —— 时钟生成 */

#define SAI_CKR_MDIV(x)        (((x) - 1) << 3)   /* MCLK 分频 */
#define SAI_CKR_MSS_SLAVE      (1 << 2)
#define SAI_CKR_MSS_MASTER     0                  /* 主机模式  */
#define SAI_CKR_CKP_INVERTED   (1 << 1)
#define SAI_CKR_CKP_NORMAL     0
#define SAI_CKR_FSP_INVERTED   (1 << 0)
#define SAI_CKR_FSP_NORMAL     0

/* DMACR —— DMA 控制。出处：Linux sound/soc/rockchip/rockchip_sai.h
 *   bit24 RDE  RX DMA 请求使能
 *   bits20:16 RDL  RX FIFO 水位（FIFO 里攒满多少个 32 位 entry 就拉
 *                 DMA 请求线；写入值为 entry 数减一）
 *   bit8  TDE  TX DMA 请求使能
 *   bits4:0 TDL  TX FIFO 水位
 */

#define SAI_DMACR_RDE           (1 << 24)
#define SAI_DMACR_RDL(x)        (((x) - 1) << 16)
#define SAI_DMACR_TDE           (1 << 8)
#define SAI_DMACR_TDL(x)        ((x) << 0)

#define RK3576_SAI_BASECLK_HZ  12288000   /* dtb 的 assigned-clock-rates */

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_SAI_H */
