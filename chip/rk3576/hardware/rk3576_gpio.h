/****************************************************************************
 * chip/rk3576/hardware/rk3576_gpio.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_GPIO_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_GPIO_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* RK3576 的 GPIO 控制器是 Rockchip "GPIO v2" 形制（RK3568 之后通用），
 * 与更早的 v1（RK3399 等）寄存器布局完全不同，不能照抄。
 *
 * ★ v2 的两个关键特征：
 *
 *   1. 每个 32 位寄存器被拆成 _L / _H 两个，各管 16 个引脚：
 *        _L 管 pin 0-15，_H 管 pin 16-31。
 *
 *   2. 写操作带"写使能掩码"：高 16 位是 mask，低 16 位是数据，
 *      只有 mask 中置 1 的位才会被写入。因此**不需要读改写**，
 *      单次写就是原子的，天然免除了多核/中断竞争。
 *
 *      写 bank 内第 n 位（n 已折算到 0-15）：
 *          value ? (BIT(n + 16) | BIT(n)) : BIT(n + 16)
 *
 * 唯一的例外是 EXT_PORT（读引脚电平）和 VER_ID，它们是普通只读寄存器，
 * 32 位一次读完，不分 _L/_H。
 */

#define RK3576_GPIO_SWPORT_DR_L      0x0000  /* 输出数据 pin 0-15  */
#define RK3576_GPIO_SWPORT_DR_H      0x0004  /* 输出数据 pin 16-31 */
#define RK3576_GPIO_SWPORT_DDR_L     0x0008  /* 方向     pin 0-15  */
#define RK3576_GPIO_SWPORT_DDR_H     0x000c  /* 方向     pin 16-31 */
#define RK3576_GPIO_INT_EN_L         0x0010
#define RK3576_GPIO_INT_EN_H         0x0014
#define RK3576_GPIO_INT_MASK_L       0x0018
#define RK3576_GPIO_INT_MASK_H       0x001c
#define RK3576_GPIO_INT_TYPE_L       0x0020  /* 0=电平 1=边沿      */
#define RK3576_GPIO_INT_TYPE_H       0x0024
#define RK3576_GPIO_INT_POLARITY_L   0x0028  /* 0=低/下降 1=高/上升 */
#define RK3576_GPIO_INT_POLARITY_H   0x002c
#define RK3576_GPIO_INT_BOTHEDGE_L   0x0030
#define RK3576_GPIO_INT_BOTHEDGE_H   0x0034
#define RK3576_GPIO_DEBOUNCE_L       0x0038
#define RK3576_GPIO_DEBOUNCE_H       0x003c
#define RK3576_GPIO_DBCLK_DIV_EN_L   0x0040
#define RK3576_GPIO_DBCLK_DIV_EN_H   0x0044
#define RK3576_GPIO_DBCLK_DIV_CON    0x0048
#define RK3576_GPIO_INT_STATUS       0x0050  /* 只读，32 位        */
#define RK3576_GPIO_INT_RAWSTATUS    0x0058  /* 只读，32 位        */
#define RK3576_GPIO_PORT_EOI_L       0x0060  /* 写 1 清中断        */
#define RK3576_GPIO_PORT_EOI_H       0x0064
#define RK3576_GPIO_EXT_PORT         0x0070  /* 只读，引脚实际电平 */
#define RK3576_GPIO_VER_ID           0x0078  /* 只读，控制器版本   */

/* VER_ID —— 控制器版本寄存器。
 *
 * 上电后读它可以一次性确认三件事：基址填对了、GPIO 的 PCLK 已使能、
 * MMU 把这段映射进来了。这三者任何一个不对，读出的都是 0 或
 * 0xffffffff（总线返回默认值），而不会是一个像模像样的版本号。
 *
 * ★ 判据只用"是不是 0 / 0xffffffff"，不做精确匹配。
 *
 *   下面两个常量来自 Linux 的 gpio-rockchip 驱动（RK3568/RK3588 实测），
 *   但 RK3576 并不在其列 —— 本板实测读出 0x010219c8，是第三个取值。
 *   一开始按精确匹配写断言，结果把一块完全正常的控制器判成了故障。
 *   版本号本就会随修订递增，用白名单卡它是错的判据。
 */

#define RK3576_GPIO_VER_V2           0x01000c2b  /* Linux: GPIO_TYPE_V2   */
#define RK3576_GPIO_VER_V2_1         0x0101157c  /* Linux: GPIO_TYPE_V2_1 */
#define RK3576_GPIO_VER_K7_OBSERVED  0x010219c8  /* KICKPI-K7 实测        */

/* 总线读不到设备时的两种典型返回值 */

#define RK3576_GPIO_VER_INVALID_0    0x00000000
#define RK3576_GPIO_VER_INVALID_1    0xffffffff

#define RK3576_GPIO_NBANKS           5
#define RK3576_GPIO_NPINS            32       /* 每 bank 的引脚数   */

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_GPIO_H */
