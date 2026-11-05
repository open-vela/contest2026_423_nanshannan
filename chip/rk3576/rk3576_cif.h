/****************************************************************************
 * chip/rk3576/rk3576_cif.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_CIF_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_CIF_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>

/****************************************************************************
 * Name: rk3576_cif_start
 *
 * Description:
 *   让 CIF 把某一路 CSI 输入的像素写进 DDR。输出为非压缩 RAW12，
 *   每像素 16 位，行跨距 width*2。
 *
 * Input Parameters:
 *   host       - CSI 输入号 0..4（对应 dtb 的 mipiN_csi2）
 *   buf0,buf1  - 乒乓缓冲的两个物理地址，**都必须给**
 *   width,height - 帧尺寸
 *
 ****************************************************************************/

int rk3576_cif_start(int host, uintptr_t buf0, uintptr_t buf1,
                     int width, int height);

/****************************************************************************
 * Name: rk3576_cif_stop
 ****************************************************************************/

int rk3576_cif_stop(int host);

/****************************************************************************
 * Name: rk3576_cif_status
 *
 * Description:
 *   读并清中断状态，返回清除前的原值。
 *
 ****************************************************************************/

uint32_t rk3576_cif_status(int host);

/****************************************************************************
 * Name: rk3576_cif_clear_status
 *
 *   清中断状态位（写 1 清）。走中断路径时 ISR 必须调它 ——
 *   CIF 的中断是电平触发的，不清就重入。
 ****************************************************************************/

void rk3576_cif_clear_status(int host, uint32_t status);

/****************************************************************************
 * Name: rk3576_cif_wait_frame
 *
 * Description:
 *   轮询等一帧写完，返回刚写完的缓冲序号（0 或 1），超时返回
 *   -ETIMEDOUT。want 指定只等哪一个缓冲（-1 表示两个都行），status
 *   非空时带回消费掉的 INTSTAT 原值。
 *
 *   取代"延时一段时间再读"：盲等不知道等到的是整帧还是半帧，而半帧的
 *   撕裂看起来很像图像处理写错了，会把排查引到无关的方向。
 *
 ****************************************************************************/

int rk3576_cif_wait_frame(int host, int want, int timeout_ms,
                          uint32_t *status);

/****************************************************************************
 * Name: rk3576_cif_set_buffer
 *
 * Description:
 *   运行中改写某个乒乓槽（slot 0/1 对应 FRM0/FRM1）的目标地址。配合第三
 *   块备用缓冲，可以让正在被消费的那一块永远不是 DMA 的目标 —— 否则渲染
 *   慢于出帧时，硬件会在我们读到一半时开始覆盖，画面撕裂。
 *
 ****************************************************************************/

int rk3576_cif_set_buffer(int host, int slot, uintptr_t addr);

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_CIF_H */
