/****************************************************************************
 * chip/rk3576/rk3576_csihost.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_CSIHOST_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_CSIHOST_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Name: rk3576_csihost_start
 *
 * Description:
 *   打开一路 MIPI CSI-2 主机控制器。host 0..4 对应 dtb 的 mipiN_csi2。
 *
 ****************************************************************************/

int rk3576_csihost_start(int host, int lanes);

/****************************************************************************
 * Name: rk3576_csihost_stop
 ****************************************************************************/

int rk3576_csihost_stop(int host);

/****************************************************************************
 * Name: rk3576_csihost_status
 *
 * Description:
 *   打印通道状态与错误计数，并区分「线上没数据」与「有数据但收错」。
 *   这是取图链路上第一个可观测点。
 *
 ****************************************************************************/

int rk3576_csihost_status(int host);

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_CSIHOST_H */
