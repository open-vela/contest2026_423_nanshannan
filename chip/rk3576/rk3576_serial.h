/****************************************************************************
 * chip/rk3576/rk3576_serial.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_SERIAL_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_SERIAL_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include "hardware/rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* RK3576 的串口控制器是 Synopsys DW 8250，寄存器模型与 16550 兼容，
 * 因此本端口不自带串口驱动，直接复用 NuttX 通用的
 * drivers/serial/uart_16550.c，参数全部经 defconfig 配置：
 *
 *   CONFIG_16550_UART0_BASE    控制器基址
 *   CONFIG_16550_UART0_CLOCK   串口时钟源频率
 *   CONFIG_16550_UART0_IRQ     GIC 中断号（dtsi 的 SPI 号 + 32）
 *   CONFIG_16550_UART0_BAUD    波特率（RK 平台调试口惯例 1500000）
 *   CONFIG_16550_REGINCR=4     DW UART 寄存器按 4 字节步进
 *   CONFIG_16550_ADDRWIDTH=64  arm64
 *
 * 先例：boards/risc-v/sg2000/milkv_duos（同为 DW UART）
 *       boards/arm64/vdk/vdk-armv8r（同为 arm64 + 16550）
 *
 * 若后续发现 RK3576 有通用驱动覆盖不了的特性（如 DLF 小数分频），
 * 再参照 arch/arm64/src/rk3399/rk3399_serial.c 写专用驱动。
 */

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_SERIAL_H */
