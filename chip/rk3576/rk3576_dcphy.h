/****************************************************************************
 * chip/rk3576/rk3576_dcphy.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_DCPHY_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_DCPHY_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>

/****************************************************************************
 * Name: rk3576_dcphy_probe
 *
 * Description:
 *   打开 D-PHY 的时钟并做寄存器可访问性自检。
 *
 ****************************************************************************/

int rk3576_dcphy_probe(void);

/****************************************************************************
 * Name: rk3576_dcphy_enable
 *
 * Description:
 *   按目标每通道码率配置 PLL 并使能 D-PHY。
 *
 * Input Parameters:
 *   bitrate_kbps - 每条数据通道的码率（kbps）。
 *                  由像素时钟算得：pixclk * bpp / lanes。
 *                  720x1280@60 RGB888 4 lane 时约 390000。
 *   lanes        - 数据通道数
 *
 * Returned Value:
 *   OK；PLL 未锁定返回 -ETIMEDOUT。
 *
 *   ★ PLL_LOCK 是整条显示链路上第一个真正的硬件反馈 —— 在它之前
 *     所有配置都只是"写进去了"，没有任何东西能证明配置是对的。
 *     锁定失败说明分频参数算错或参考时钟不对，与 VOP/DSI 无关。
 *
 ****************************************************************************/

int rk3576_dcphy_enable(uint32_t bitrate_kbps, int lanes);

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_DCPHY_H */
