/****************************************************************************
 * chip/rk3576/hardware/rk3576_i2c.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_I2C_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_I2C_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Rockchip 自家的 I2C 控制器，不是 DesignWare。
 *
 * ★ 出处：原厂 dtb 里 i2c@2ac50000 的 compatible 是
 *      "rockchip,rk3576-i2c", "rockchip,rk3399-i2c"
 *   回退串指向 RK3399 同代 IP，寄存器布局与 Linux 的 i2c-rk3x.c 一致。
 *   （用 scripts/dtb-query.py 从原厂 boot.img 里查得，见该脚本说明。）
 *
 * 与常见 I2C 控制器的两点不同，写驱动时最容易踩：
 *
 *   1. 收发数据不走单字节的数据寄存器，而是两块 32 位寄存器窗口
 *      （TXDATA/RXDATA 各 8 个字），字节按小端打包进字。
 *
 *   2. 从机地址不总是放在数据流里：
 *      - MOD_TX 模式下，地址是 TXDATA 的第一个字节（自己拼进去）；
 *      - MOD_RX 模式下，地址放在 MRXADDR 寄存器，且要置 VALID 位。
 */

#define RK3576_I2C_CON            0x0000  /* 控制寄存器           */
#define RK3576_I2C_CLKDIV         0x0004  /* SCL 分频             */
#define RK3576_I2C_MRXADDR        0x0008  /* 接收模式的从机地址   */
#define RK3576_I2C_MRXRADDR       0x000c  /* 接收模式的寄存器地址 */
#define RK3576_I2C_MTXCNT         0x0010  /* 待发送字节数         */
#define RK3576_I2C_MRXCNT         0x0014  /* 待接收字节数         */
#define RK3576_I2C_IEN            0x0018  /* 中断使能             */
#define RK3576_I2C_IPD            0x001c  /* 中断挂起，写 1 清     */
#define RK3576_I2C_FCNT           0x0020  /* 已传输字节数         */
#define RK3576_I2C_TXDATA_BASE    0x0100  /* 发送窗口，8 个 32 位字 */
#define RK3576_I2C_RXDATA_BASE    0x0200  /* 接收窗口，8 个 32 位字 */

/* 单次传输的字节上限：8 个字 × 4 字节。超过要拆成多次。 */

#define RK3576_I2C_FIFO_BYTES     32

/* CON */

#define I2C_CON_EN                (1 << 0)  /* 使能控制器          */
#define I2C_CON_MOD_SHIFT         1
#define I2C_CON_MOD_MASK          (3 << 1)
#define I2C_CON_START             (1 << 3)  /* 发起 (重复)起始条件 */
#define I2C_CON_STOP              (1 << 4)  /* 发起停止条件        */
#define I2C_CON_LASTACK           (1 << 5)  /* 收最后一字节时回 NAK */
#define I2C_CON_ACT2NAK           (1 << 6)  /* 收到 NAK 即中止      */

/* CON 的模式字段。REGISTER_RX 在部分版本上有缺陷（Linux 注释为
 * "broken"），本驱动只用 TX 和 RX 两种，靠重复起始条件组合出
 * "写寄存器地址再读" 的时序，不依赖硬件的组合模式。
 */

#define I2C_CON_MOD_TX            (0 << 1)  /* 发送，地址在 TXDATA */
#define I2C_CON_MOD_REGISTER_TX   (1 << 1)
#define I2C_CON_MOD_RX            (2 << 1)  /* 接收，地址在 MRXADDR */
#define I2C_CON_MOD_REGISTER_RX   (3 << 1)

/* IEN / IPD 共用同一组位定义 */

#define I2C_INT_BTF               (1 << 0)  /* 字节发送完成        */
#define I2C_INT_BRF               (1 << 1)  /* 字节接收完成        */
#define I2C_INT_MBTF              (1 << 2)  /* 批量发送完成        */
#define I2C_INT_MBRF              (1 << 3)  /* 批量接收完成        */
#define I2C_INT_START             (1 << 4)  /* 起始条件已发出      */
#define I2C_INT_STOP              (1 << 5)  /* 停止条件已发出      */
#define I2C_INT_NAKRCV            (1 << 6)  /* 收到 NAK            */
#define I2C_INT_ALL               0x7f

/* MRXADDR：地址放低 24 位，并按字节置 VALID 位说明用了几个字节。
 * 7 位地址只用第 0 个字节，因此是 VALID(0)。
 */

#define I2C_MRXADDR_VALID(x)      (1 << (24 + (x)))

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_I2C_H */
