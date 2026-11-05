/****************************************************************************
 * chip/rk3576/rk3576_i2c.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_I2C_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_I2C_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/i2c/i2c_master.h>

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
 * Name: rk3576_i2cbus_initialize
 *
 * Description:
 *   初始化并返回一条 I2C 总线。当前只支持 port 2 —— 板上 RTC
 *   (HYM8563 @0x51) 与 Type-C PD (HUSB311 @0x4e) 都挂在这条上。
 *
 * Returned Value:
 *   成功返回总线句柄；端口不支持或寄存器块无响应返回 NULL。
 *
 ****************************************************************************/

struct i2c_master_s *rk3576_i2cbus_initialize(int port);

/****************************************************************************
 * Name: rk3576_i2cbus_uninitialize
 ****************************************************************************/

int rk3576_i2cbus_uninitialize(struct i2c_master_s *dev);

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_I2C_H */
