/****************************************************************************
 * chip/rk3576/rk3576_timer.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_TIMER_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_TIMER_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/timers/oneshot.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_oneshot_initialize
 *
 * Description:
 *   用 RK3576 独立 TIMER 的两个通道做一个 oneshot 下半部，供
 *   oneshot_register("/dev/oneshot", ...) 使用。
 *
 *   ★ 不要拿 arm64_oneshot_initialize() 来干这件事。那个下半部是调度器
 *     的，struct oneshot_lowerhalf_s 只有一对 callback/arg，注册
 *     /dev/oneshot 会把调度器的定时回调顶掉，整个系统的时基当场失效
 *     （sleep 不返回、延时 work 不触发），而串口与 nsh 一切正常，
 *     极难察觉。详见 notes/DEBUG-CASES.md 案例 16。
 *
 * Returned Value:
 *   成功返回下半部实例；计数器自检不通过返回 NULL。
 *
 ****************************************************************************/

struct oneshot_lowerhalf_s *rk3576_oneshot_initialize(void);

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_TIMER_H */
