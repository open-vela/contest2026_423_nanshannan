/****************************************************************************
 * arch/arm64/src/rk3576/rk3576_gpio.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_GPIO_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_GPIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/irq.h>

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
 * Name: rk3576_gpio_setdir
 *
 * Description:
 *   设置引脚方向。
 *
 * Input Parameters:
 *   bank   - GPIO 组号 0-4
 *   pin    - 组内引脚号 0-31
 *   output - true 为输出，false 为输入
 *
 * Returned Value:
 *   成功返回 OK，参数越界返回 -EINVAL。
 *
 * Assumptions:
 *   引脚复用（pinmux）需已配置为 GPIO 功能。本函数不碰 IOC/GRF ——
 *   M2 阶段依赖 U-Boot 留下的复用设置，见 rk3576_gpio.c 顶部说明。
 *
 ****************************************************************************/

int rk3576_gpio_setdir(int bank, int pin, bool output);

/****************************************************************************
 * Name: rk3576_gpio_write
 *
 * Description:
 *   设置输出引脚电平。引脚须已配置为输出。
 *
 ****************************************************************************/

int rk3576_gpio_write(int bank, int pin, bool value);

/****************************************************************************
 * Name: rk3576_gpio_read
 *
 * Description:
 *   读引脚实际电平（EXT_PORT），输入输出均可读。
 *
 * Returned Value:
 *   0 或 1；参数越界返回 -EINVAL。
 *
 ****************************************************************************/

int rk3576_gpio_read(int bank, int pin);

/****************************************************************************
 * Name: rk3576_gpio_read_output
 *
 * Description:
 *   读输出数据寄存器（引脚被驱动成什么），而不是引脚实际电平。
 *   输出脚读回用它；输入脚用 rk3576_gpio_read()。
 *
 ****************************************************************************/

int rk3576_gpio_read_output(int bank, int pin);

/****************************************************************************
 * Name: rk3576_gpio_verid
 *
 * Description:
 *   读控制器版本寄存器。用于自检：读到 RK3576_GPIO_VER_V2* 说明基址、
 *   时钟、MMU 映射三者都正确；读到 0 或 0xffffffff 说明其中某环有问题。
 *
 * Returned Value:
 *   VER_ID 的值；bank 越界返回 0。
 *
 ****************************************************************************/

uint32_t rk3576_gpio_verid(int bank);

/****************************************************************************
 * Name: rk3576_gpio_irq_config
 *
 * Description:
 *   把一个引脚配成中断源。引脚会被设为输入。
 *
 * Input Parameters:
 *   bank, pin - 引脚
 *   rising    - true 上升沿/高电平触发，false 下降沿/低电平
 *   level     - true 电平触发，false 边沿触发
 *
 ****************************************************************************/

int rk3576_gpio_irq_config(int bank, int pin, bool rising, bool level);

/****************************************************************************
 * Name: rk3576_gpio_irq_attach
 *
 * Description:
 *   为一个引脚挂中断处理函数。
 *
 *   ★ 五个 bank 各共用一个 GIC 中断号（bank 内 32 个引脚共享），
 *     因此本层维护一张每引脚的回调表，在 bank 级 ISR 里读 INT_STATUS
 *     分发到具体引脚。调用方拿到的是"这个引脚的中断"，不必关心共享。
 *
 ****************************************************************************/

int rk3576_gpio_irq_attach(int bank, int pin, xcpt_t isr, void *arg);

/****************************************************************************
 * Name: rk3576_gpio_irq_enable
 ****************************************************************************/

int rk3576_gpio_irq_enable(int bank, int pin, bool enable);

#undef EXTERN
#if defined(__cplusplus)
}
#endif

/* 诊断用：读 bank 级 ISR 的进入次数与最近一次 INT_STATUS。 */

uint32_t rk3576_gpio_irq_count(int bank, uint32_t *last_status);
uint32_t rk3576_gpio_rawstatus(int bank);
bool rk3576_gpio_irq_masked(int bank, int pin);

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_GPIO_H */
