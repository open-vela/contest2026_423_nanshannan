/****************************************************************************
 * chip/rk3576/rk3576_power.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_POWER_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_POWER_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdbool.h>

/* 电源域编号。
 *
 * ★ 必须逐个照抄 include/dt-bindings/power/rockchip,rk3576-power.h，
 *   不能按已知的几个外推 —— 编号不是连续排列的语义分组。
 *   本文件初版就是外推出来的，把 AUDIO 写成 7（实为 USB）、
 *   VOP 写成 10（实为 AUDIO）、VO0 写成 12（实为 VEPU1）。
 *   当时硬件行为侥幸正确（表项与常量用了同一套错编号），
 *   直到要新增 PD_AUDIO 才因编号撞车暴露。
 */

#define RK3576_PD_NPU        0
#define RK3576_PD_NPUTOP     1
#define RK3576_PD_NPU0       2
#define RK3576_PD_NPU1       3
#define RK3576_PD_GPU        4
#define RK3576_PD_NVM        5    /* eMMC（U-Boot 已开）      */
#define RK3576_PD_SDGMAC     6
#define RK3576_PD_USB        7
#define RK3576_PD_PHP        8    /* USB3OTG1（DRD1）         */
#define RK3576_PD_SUBPHP     9
#define RK3576_PD_AUDIO     10    /* SAI 音频接口             */
#define RK3576_PD_VEPU0     11
#define RK3576_PD_VEPU1     12
#define RK3576_PD_VPU       13
#define RK3576_PD_VDEC      14
#define RK3576_PD_VI        15
#define RK3576_PD_VO0       16    /* MIPI DSI 主机            */
#define RK3576_PD_VO1       17
#define RK3576_PD_VOP       18    /* 显示控制器               */

/****************************************************************************
 * Name: rk3576_power_on
 *
 * Description:
 *   打开一个电源域。已经打开时直接返回 OK。
 *
 *   ★ 上电有严格的握手顺序，顺序错会挂死总线：
 *       解除时钟门控 → 写电源位 → 等 repair_status → 撤销 idle 请求
 *       → 等 idle_ack 清零 → 重新门控时钟
 *     其中"撤销 idle 请求"必须在上电**之后**，掉电时则相反 ——
 *     要先请求 idle、等 NIU 应答，再断电，否则总线上会留下未完成的事务。
 *
 * Returned Value:
 *   OK；域号非法返回 -EINVAL；握手超时返回 -ETIMEDOUT。
 *
 ****************************************************************************/

int rk3576_power_on(int domain);

/****************************************************************************
 * Name: rk3576_power_is_on
 ****************************************************************************/

bool rk3576_power_is_on(int domain);

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_POWER_H */
