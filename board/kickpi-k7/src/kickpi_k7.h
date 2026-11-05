/****************************************************************************
 * board/kickpi-k7/src/kickpi_k7.h
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

#ifndef __VENDOR_ROCKCHIP_BOARDS_RK3576_KICKPI_K7_SRC_KICKPI_K7_H
#define __VENDOR_ROCKCHIP_BOARDS_RK3576_KICKPI_K7_SRC_KICKPI_K7_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>

#ifndef __ASSEMBLY__

/****************************************************************************
 * Public Functions Definitions
 ****************************************************************************/

#ifdef CONFIG_DEV_GPIO
/****************************************************************************
 * Name: kickpi_k7_gpio_initialize
 *
 * Description:
 *   注册板上 GPIO 输出引脚为 /dev/gpioN。由 board_app_initialize() 调用。
 *
 ****************************************************************************/

int kickpi_k7_gpio_initialize(void);
#endif

#ifdef CONFIG_RK3576_I2C
/****************************************************************************
 * Name: kickpi_k7_i2c_initialize
 *
 * Description:
 *   初始化板上 I2C 总线并注册为 /dev/i2cN。由 board_app_initialize() 调用。
 *
 ****************************************************************************/

int kickpi_k7_i2c_initialize(void);

/****************************************************************************
 * Name: kickpi_k7_mpu6050_initialize
 *
 * Description:
 *   40 针口上的 MPU-6050 注册成 /dev/accel0 与 /dev/gyro0（uORB）。
 *
 ****************************************************************************/

#ifdef CONFIG_KICKPI_K7_MPU6050
int kickpi_k7_mpu6050_initialize(void);
#endif

/****************************************************************************
 * Name: kickpi_k7_spi_initialize
 *
 * Description:
 *   注册 SPI4 为 /dev/spi4。板上只有它引到了 40 针扩展口。
 *
 ****************************************************************************/

int kickpi_k7_spi_initialize(void);

/****************************************************************************
 * Name: kickpi_spi_loopback
 *
 * Description:
 *   SPI4 的 MOSI/MISO 短接回环自检，需要人工把排针 19/21 脚接起来。
 *
 ****************************************************************************/

int kickpi_spi_loopback(void);

/****************************************************************************
 * Name: kickpi_camera_stream
 *
 * Description:
 *   让已识别的 IMX415 开始/停止在 MIPI 上输出图像。
 *   接收端未实现前，这是取图链路上唯一可独立验证的一级。
 *
 ****************************************************************************/

int kickpi_camera_stream(bool on);
int kickpi_camera_receiver(bool on);
int kickpi_camera_status(void);
bool kickpi_camera_detected(void);

/****************************************************************************
 * 去马赛克 + JPEG 编码（kickpi_k7_imgproc.c）
 *
 *   stride_pix 是以 uint16 计的行跨距，不是宽度 —— CIF 要求 256 字节
 *   对齐，1932x2=3864 会被对齐到 4096。写成「行号 x 宽度」就错了。
 ****************************************************************************/

int kickpi_imgproc_jpeg(FAR const uint16_t *raw, int stride_pix,
                        int width, int height, int phase,
                        uint16_t black, uint16_t white,
                        int quality, FAR const char *path);

int kickpi_imgproc_selftest(int phase, FAR const char *path);
int kickpi_camera_bayerstat(void);
int kickpi_imgproc_bayerstat(FAR const uint16_t *raw, int stride_pix,
                             int width, int height);
int kickpi_imgproc_jpeg_mem(FAR const uint16_t *raw, int stride_pix,
                            int width, int height, int phase,
                            uint16_t black, uint16_t white, int quality,
                            FAR uint8_t *out, size_t outlen);

/* 同上，但同时缩放到 dstw x dsth（只支持缩小）。
 *
 * ★ 传感器模式是固定的 1932x1096，而 ai_agent 的视觉工具要 1280x720
 *   或 320x180（见 packages/ai_agent/src/tools/tool_camera.c）。能变的
 *   是**输出**尺寸，不是采集尺寸 —— 缩放放在这一层。
 */

int kickpi_imgproc_jpeg_scaled(FAR const uint16_t *raw, int stride_pix,
                               int srcw, int srch, int dstw, int dsth,
                               int phase, uint16_t black, uint16_t white,
                               int quality, FAR uint8_t *out, size_t outlen);
int kickpi_camera_jpeg(FAR const char *path, int phase, int quality);

/* 蓝牙（AP6256 / BCM4345C5，UART4）。固件由 kickpi_k7_bt_firmware.c 提供。 */

int kickpi_k7_bt_initialize(void);
extern const long int g_bt_firmware_len;

/* LSC（镜头阴影）与 CCM（色彩校正矩阵）。
 *
 * ★ 两者的系数都只能实测，默认恒等 —— 这一级存在但不改变画面。
 *   LSC 用 kickpi_imgproc_lsccal() 对着均匀白面自标；CCM 需要色卡，
 *   目前只提供手工设置的通路。
 */

void kickpi_imgproc_set_lsc(int k1, int k2);
void kickpi_imgproc_get_lsc(FAR int *k1, FAR int *k2);
void kickpi_imgproc_set_ccm(FAR const int *m);
int  kickpi_imgproc_lsccal(FAR const uint16_t *raw, int stride_pix,
                           int width, int height, int phase,
                           FAR int *k1_out, FAR int *k2_out);
int  kickpi_camera_lsccal(int phase);

/****************************************************************************
 * Name: kickpi_k7_video_initialize
 *
 *   注册 V4L2 设备 /dev/video0（IMX415 imgsensor + CIF imgdata）。
 *   必须在 kickpi_k7_camera_initialize() 之后调用。
 ****************************************************************************/

int kickpi_k7_video_initialize(void);
#endif

#ifdef CONFIG_INPUT_FT5X06
/****************************************************************************
 * Name: kickpi_k7_touch_initialize
 *
 * Description:
 *   注册 GT9xx 电容触摸为 /dev/input0。由 board_app_initialize() 调用。
 *
 ****************************************************************************/

int kickpi_k7_touch_initialize(void);
#endif

/****************************************************************************
 * Name: kickpi_k7_lcd_power
 *
 * Description:
 *   开关 VCC3V3_LCD_S0 电源轨（LCD_PWREN_H / GPIO0_C6），并顺带处理屏的
 *   复位脚。屏和触摸共用这条轨，必须由一处统一管理，任一方各自开关都会
 *   把另一方带掉电。
 *
 *   on=true 时返回后电源已稳定、屏复位已释放，可以开始 I2C / DSI 通信。
 *
 ****************************************************************************/

int kickpi_k7_lcd_power(bool on);

/****************************************************************************
 * Name: kickpi_k7_camera_initialize
 *
 * Description:
 *   给 IMX415 供电、唤醒并读型号寄存器确认在位。只做传感器探测，
 *   不建立取图通路（CSI2/CIF/ISP 尚未实现）。
 *
 ****************************************************************************/

int kickpi_k7_camera_initialize(void);

/****************************************************************************
 * Name: kickpi_k7_rtc_initialize
 *
 * Description:
 *   注册板上 HYM8563 为 /dev/rtc0。
 *
 ****************************************************************************/

int kickpi_k7_rtc_initialize(void);

#if defined(CONFIG_AUDIO_ES8388) && defined(CONFIG_RK3576_SAI)
/****************************************************************************
 * Name: kickpi_k7_audio_initialize
 *
 * Description:
 *   ES8388 codec + SAI1，注册为 /dev/audio/pcm0。
 *
 ****************************************************************************/

int kickpi_k7_audio_initialize(void);
#endif

#endif /* __ASSEMBLY__ */
#endif /* __VENDOR_ROCKCHIP_BOARDS_RK3576_KICKPI_K7_SRC_KICKPI_K7_H */
