/****************************************************************************
 * arch/arm64/src/rk3576/rk3576_pinmux.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_PINMUX_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_PINMUX_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 功能号 0 恒为 GPIO。其余功能号是每个引脚各自定义的，必须查 dts —— 
 * 同一个功能号在不同引脚上含义完全不同，不能类推。
 */

#define RK3576_PINMUX_GPIO   0

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#undef EXTERN
#if defined(__cplusplus)
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Name: rk3576_pinmux_set
 *
 * Description:
 *   设置一个引脚的复用功能。
 *
 * Input Parameters:
 *   bank - GPIO 组号 0-4
 *   pin  - 组内引脚号 0-31
 *   func - 功能号 0-15（0 = GPIO）。取值须查板子 dts 的
 *          rockchip,pins 属性，可用 scripts/dtb-query.py 从原厂
 *          boot.img 里查出。
 *
 * Returned Value:
 *   OK；参数越界返回 -EINVAL。
 *
 ****************************************************************************/

int rk3576_pinmux_set(int bank, int pin, unsigned int func);

/* RK3576 uses PULL_TYPE_IO_1 for every bank: 0/2 disable, 1 down,
 * 3 up. Bus hold is not supported. Use 0 for disabled pulls.
 */

#define RK3576_PULL_NONE     0
#define RK3576_PULL_UP       3
#define RK3576_PULL_DOWN     1

/****************************************************************************
 * Name: rk3576_pinmux_setpull
 *
 * Description:
 *   配置引脚内部上下拉。做外部上拉在位判断前必须先关掉内部拉，
 *   否则读数恒为 1，判据无效。
 *
 ****************************************************************************/

int rk3576_pinmux_setpull(int bank, int pin, unsigned int pull);

/****************************************************************************
 * Name: rk3576_pinmux_get
 *
 * Description:
 *   读回一个引脚当前的复用功能。用于确认引导器留下的状态，
 *   而不是假设。
 *
 * Returned Value:
 *   功能号；参数越界返回 -EINVAL。
 *
 ****************************************************************************/

int rk3576_pinmux_get(int bank, int pin);

/****************************************************************************
 * Name: rk3576_pinmux_getpull
 *
 * Description:
 *   Read the raw pull encoding: 0/2 disabled, 1 pull-down, 3 pull-up.
 *   Invalid bank or pin arguments return -EINVAL.
 *
 ****************************************************************************/

int rk3576_pinmux_getpull(int bank, int pin);

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_PINMUX_H */
