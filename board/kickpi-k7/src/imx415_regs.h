/****************************************************************************
 * board/kickpi-k7/src/imx415_regs.h
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

/* IMX415 寄存器表，取自厂商 drivers/media/i2c/imx415.c。
 *
 * ★ 为什么选 1932x1096 / 12bit / 线性 / 594Mbps 这一档
 *
 *   传感器支持的模式里，这一档在四个维度上同时是最省事的：
 *
 *   - **链路频率最低**（297MHz DDR = 594Mbps/lane，mipi_freq_idx=0）。
 *     D-PHY 的接收端要按链路速率整定 HS settle 时间，速率越低这个
 *     窗口越宽容 —— 首次点亮一条没跑过的 CSI 通路时，这一条最值钱。
 *   - **外部时钟 37.125MHz**，与板上晶振一致。2 通道那几档要 27MHz，
 *     对不上（而且 720p 那档的链路频率反而高到 2376M）。
 *   - **4 通道**，与板上布线一致，不用改 PHY 的通道数。
 *   - **单帧 3.17MB**（1932×1096×12/8）。全分辨率 3864×2192 是 12.7MB，
 *     在 63MB 的系统里再加上帧缓冲就很紧张了。
 *
 *   代价是分辨率只有一半、且不是 HDR —— 对"把图像送到屏上"这个目标
 *   没有影响。
 *
 * ★ 两张表的关系
 *
 *   global 是与模式无关的基础配置，先写；mode 是这一档特有的，后写。
 *   顺序反了会被 global 覆盖掉。
 */

#ifndef __BOARDS_RK3576_KICKPI_K7_SRC_IMX415_REGS_H
#define __BOARDS_RK3576_KICKPI_K7_SRC_IMX415_REGS_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define IMX415_MODE_WIDTH     1932
#define IMX415_MODE_HEIGHT    1096
#define IMX415_MODE_BPP       12
#define IMX415_MODE_LANES     4
#define IMX415_MODE_MBPS      594     /* 每通道，D-PHY 整定要用 */
#define IMX415_XVCLK_HZ       37125000

/* 单帧字节数。RAW12 在 CSI-2 上按 MIPI_CSI2_DT_RAW12 打包，
 * 每 2 像素 3 字节，所以不是 width*height*2。
 */

#define IMX415_FRAME_BYTES    (IMX415_MODE_WIDTH * IMX415_MODE_HEIGHT * \
                               IMX415_MODE_BPP / 8)

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct imx415_reg_s
{
  uint16_t addr;
  uint8_t  val;
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* 与模式无关的基础配置（84 条） */

static const struct imx415_reg_s g_imx415_global[] =
{
  { 0x3002, 0x00 },
  { 0x3008, 0x7f },
  { 0x300a, 0x5b },
  { 0x30c1, 0x00 },
  { 0x3031, 0x01 },
  { 0x3032, 0x01 },
  { 0x30d9, 0x06 },
  { 0x3116, 0x24 },
  { 0x3118, 0xc0 },
  { 0x311e, 0x24 },
  { 0x32d4, 0x21 },
  { 0x32ec, 0xa1 },
  { 0x3452, 0x7f },
  { 0x3453, 0x03 },
  { 0x358a, 0x04 },
  { 0x35a1, 0x02 },
  { 0x36bc, 0x0c },
  { 0x36cc, 0x53 },
  { 0x36cd, 0x00 },
  { 0x36ce, 0x3c },
  { 0x36d0, 0x8c },
  { 0x36d1, 0x00 },
  { 0x36d2, 0x71 },
  { 0x36d4, 0x3c },
  { 0x36d6, 0x53 },
  { 0x36d7, 0x00 },
  { 0x36d8, 0x71 },
  { 0x36da, 0x8c },
  { 0x36db, 0x00 },
  { 0x3701, 0x03 },
  { 0x3724, 0x02 },
  { 0x3726, 0x02 },
  { 0x3732, 0x02 },
  { 0x3734, 0x03 },
  { 0x3736, 0x03 },
  { 0x3742, 0x03 },
  { 0x3862, 0xe0 },
  { 0x38cc, 0x30 },
  { 0x38cd, 0x2f },
  { 0x395c, 0x0c },
  { 0x3a42, 0xd1 },
  { 0x3a4c, 0x77 },
  { 0x3ae0, 0x02 },
  { 0x3aec, 0x0c },
  { 0x3b00, 0x2e },
  { 0x3b06, 0x29 },
  { 0x3b98, 0x25 },
  { 0x3b99, 0x21 },
  { 0x3b9b, 0x13 },
  { 0x3b9c, 0x13 },
  { 0x3b9d, 0x13 },
  { 0x3b9e, 0x13 },
  { 0x3ba1, 0x00 },
  { 0x3ba2, 0x06 },
  { 0x3ba3, 0x0b },
  { 0x3ba4, 0x10 },
  { 0x3ba5, 0x14 },
  { 0x3ba6, 0x18 },
  { 0x3ba7, 0x1a },
  { 0x3ba8, 0x1a },
  { 0x3ba9, 0x1a },
  { 0x3bac, 0xed },
  { 0x3bad, 0x01 },
  { 0x3bae, 0xf6 },
  { 0x3baf, 0x02 },
  { 0x3bb0, 0xa2 },
  { 0x3bb1, 0x03 },
  { 0x3bb2, 0xe0 },
  { 0x3bb3, 0x03 },
  { 0x3bb4, 0xe0 },
  { 0x3bb5, 0x03 },
  { 0x3bb6, 0xe0 },
  { 0x3bb7, 0x03 },
  { 0x3bb8, 0xe0 },
  { 0x3bba, 0xe0 },
  { 0x3bbc, 0xda },
  { 0x3bbe, 0x88 },
  { 0x3bc0, 0x44 },
  { 0x3bc2, 0x7b },
  { 0x3bc4, 0xa2 },
  { 0x3bc8, 0xbd },
  { 0x3bca, 0xbd },
  { 0x4004, 0x48 },
  { 0x4005, 0x09 },
};

/* 1932x1096 12bit 线性 594Mbps（35 条） */

static const struct imx415_reg_s g_imx415_mode_1932x1096[] =
{
  { 0x3020, 0x01 },
  { 0x3021, 0x01 },
  { 0x3022, 0x01 },
  { 0x3024, 0x5d },
  { 0x3025, 0x0c },
  { 0x3028, 0x0e },
  { 0x3029, 0x03 },
  { 0x302c, 0x00 },
  { 0x302d, 0x00 },
  { 0x3031, 0x00 },
  { 0x3033, 0x07 },
  { 0x3050, 0x08 },
  { 0x3051, 0x00 },
  { 0x3054, 0x19 },
  { 0x3058, 0x3e },
  { 0x3060, 0x25 },
  { 0x3064, 0x4a },
  { 0x30cf, 0x00 },
  { 0x30d9, 0x02 },
  { 0x30da, 0x01 },
  { 0x3118, 0x80 },
  { 0x3260, 0x01 },
  { 0x3701, 0x00 },
  { 0x400c, 0x00 },
  { 0x4018, 0x67 },
  { 0x401a, 0x27 },
  { 0x401c, 0x27 },
  { 0x401e, 0xb7 },
  { 0x401f, 0x00 },
  { 0x4020, 0x2f },
  { 0x4022, 0x4f },
  { 0x4024, 0x2f },
  { 0x4026, 0x47 },
  { 0x4028, 0x27 },
  { 0x4074, 0x01 },
};

#endif /* __BOARDS_RK3576_KICKPI_K7_SRC_IMX415_REGS_H */
