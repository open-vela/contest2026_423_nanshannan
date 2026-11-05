/****************************************************************************
 * chip/rk3576/rk3576_dsi2.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_DSI2_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_DSI2_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>

#include "rk3576_vop2.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* MIPI DSI 数据类型（DCS / Generic 写命令）。
 * 与设备树 panel-init-sequence 每条命令的第一个字节一致 ——
 * 那份数据可以逐条喂给 rk3576_dsi2_send_cmd()。
 */

#define MIPI_DSI_DCS_SHORT_WRITE        0x05
#define MIPI_DSI_DCS_SHORT_WRITE_PARAM  0x15
#define MIPI_DSI_DCS_LONG_WRITE         0x39
#define MIPI_DSI_GENERIC_SHORT_WRITE_0  0x03
#define MIPI_DSI_GENERIC_SHORT_WRITE_1  0x13
#define MIPI_DSI_GENERIC_SHORT_WRITE_2  0x23
#define MIPI_DSI_GENERIC_LONG_WRITE     0x29

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_dsi2_probe
 *
 * Description:
 *   打开 PD_VO0 电源域与 DSI 时钟，读寄存器自检。
 *
 ****************************************************************************/

int rk3576_dsi2_probe(void);

/****************************************************************************
 * Name: rk3576_dsi2_configure
 *
 * Description:
 *   按时序与通道数配置 DSI2 主机，进入命令模式（可发面板初始化序列）。
 *
 * Input Parameters:
 *   timing - 显示时序，与 VOP2 用同一份
 *   lanes  - 数据通道数（本板面板为 4）
 *
 ****************************************************************************/

int rk3576_dsi2_configure(const struct rk3576_vop2_timing_s *timing,
                          int lanes, uint32_t lane_mbps);

/****************************************************************************
 * Name: rk3576_dsi2_send_cmd
 *
 * Description:
 *   发一条 DSI 命令。用于面板初始化序列。
 *
 * Input Parameters:
 *   dtype - MIPI_DSI_* 数据类型
 *   data  - 负载
 *   len   - 负载长度。短命令 0-2 字节走包头，长命令走负载 FIFO。
 *
 ****************************************************************************/

int rk3576_dsi2_send_cmd(uint8_t dtype, const uint8_t *data, size_t len);

/****************************************************************************
 * Name: rk3576_dsi2_set_video_mode
 *
 * Description:
 *   切到视频模式，开始接收 VOP 送来的像素流。
 *   面板初始化序列发完之后才能调用。
 *
 ****************************************************************************/

int rk3576_dsi2_set_video_mode(void);

/****************************************************************************
 * Name: rk3576_dsi2_dump_uboot_state
 *
 * Description:
 *   打印 U-Boot 留下的 DSI 寄存器状态，须在本驱动写入之前调用。
 *
 ****************************************************************************/

void rk3576_dsi2_dump_uboot_state(void);

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_DSI2_H */
