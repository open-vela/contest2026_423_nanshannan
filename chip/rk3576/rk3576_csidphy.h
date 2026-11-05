/****************************************************************************
 * chip/rk3576/rk3576_csidphy.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_CSIDPHY_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_CSIDPHY_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Name: rk3576_csidphy_start
 *
 * Description:
 *   打开一路 MIPI CSI-2 D-PHY 接收端。
 *
 * Input Parameters:
 *   index - 逻辑 PHY 号 0..5（dtb 的 csi2_dphyN）。0..2 走物理 PHY0，
 *           3..5 走物理 PHY1。
 *   lanes - 数据通道数 1..4
 *   mbps  - 每通道链路速率，用来查 THS-SETTLE 档位
 *
 ****************************************************************************/

int rk3576_csidphy_start(int index, int lanes, unsigned int mbps);

/****************************************************************************
 * Name: rk3576_csidphy_stop
 ****************************************************************************/

int rk3576_csidphy_stop(int index);

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_CSIDPHY_H */
