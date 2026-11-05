/****************************************************************************
 * chip/rk3576/rk3576_spi.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_SPI_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_SPI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/spi/spi.h>

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
 * Name: rk3576_spibus_initialize
 *
 * Description:
 *   初始化并返回一条 SPI 总线。
 *
 *   当前只支持 port 4 —— KICKPI-K7 上只有 SPI4 引到了 40 针扩展口
 *   （厂商 dtsi rk3576-kickpi-k7c-extend-40pin.dtsi 里也只使能了它，
 *   走 m2 复用组）。其余三个通用 SPI 在基础 dtb 里都是 disabled。
 *
 * Returned Value:
 *   成功返回总线句柄；端口不支持或寄存器块无响应返回 NULL。
 *
 ****************************************************************************/

struct spi_dev_s *rk3576_spibus_initialize(int port);

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_SPI_H */
