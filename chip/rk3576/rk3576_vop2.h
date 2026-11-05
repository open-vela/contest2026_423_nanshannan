/****************************************************************************
 * chip/rk3576/rk3576_vop2.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_VOP2_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_VOP2_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* 显示时序。字段与设备树的 display-timings 一一对应，便于从厂商
 * dtsi 直接搬运。
 */

struct rk3576_vop2_timing_s
{
  uint32_t pixclk_hz;
  uint16_t hactive;
  uint16_t hfront_porch;
  uint16_t hsync_len;
  uint16_t hback_porch;
  uint16_t vactive;
  uint16_t vfront_porch;
  uint16_t vsync_len;
  uint16_t vback_porch;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_vop2_probe
 *
 * Description:
 *   打开 PD_VOP 电源域与 VOP 时钟，读版本寄存器自检。
 *
 ****************************************************************************/

int rk3576_vop2_probe(void);

/****************************************************************************
 * Name: rk3576_vop2_colorbar
 *
 * Description:
 *   按给定时序配置 VP0 并打开内置彩条。
 *
 *   ★ 彩条是验证 VOP2 的最省事判据：它由 VOP 内部产生，不需要帧缓冲、
 *     不需要图层、也不需要 DSI 之外的任何东西。屏上出现彩条即说明
 *     时序发生器、像素时钟、以及到 MIPI 接口的通路都对了；
 *     此后再接图层输出真实画面，问题范围就小得多。
 *
 ****************************************************************************/

int rk3576_vop2_colorbar(const struct rk3576_vop2_timing_s *timing,
                         bool enable);

/****************************************************************************
 * Name: rk3576_vop2_check_scanning
 *
 * Description:
 *   用帧起始中断的原始状态判断 VP 是否真的在扫描输出，而不只是
 *   寄存器写对了。返回 OK 表示观测到帧起始。
 *
 ****************************************************************************/

int rk3576_vop2_check_scanning(int vp);

/****************************************************************************
 * Name: rk3576_vop2_dump_uboot_state
 *
 * Description:
 *   打印 U-Boot 留下的 VOP2 寄存器状态。出厂固件能点亮这块屏，它留下的
 *   配置是一份已知可用的参考，必须在本驱动写入之前读。
 *
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_vop2_takeover
 *
 * Description:
 *   接管 U-Boot 已经跑起来的显示：只把 ESMART1 图层的帧缓冲地址换成
 *   buffer，其余配置（时序、格式、DSI、面板初始化的效果）一概保留。
 *   通过出参返回 U-Boot 所用的尺寸、跨距与位深。
 *
 ****************************************************************************/

int rk3576_vop2_takeover(uintptr_t buffer, uint32_t *width, uint32_t *height,
                         uint32_t *stride_bytes, uint32_t *bpp);

/****************************************************************************
 * Name: rk3576_vop2_get_mode
 *
 * Description:
 *   从 VP1 的时序寄存器读出面板实际分辨率（U-Boot 按厂商 dtsi 配好的）。
 *
 ****************************************************************************/

int rk3576_vop2_get_mode(uint32_t *width, uint32_t *height);

/****************************************************************************
 * Name: rk3576_vop2_fb_pan
 *
 * Description:
 *   翻页：把 ESMART1 的取数地址切到 buffer。只写 YRGB_MST + CFG_DONE，
 *   在下一个 VSYNC 整帧原子生效 —— 双缓冲靠它避免撕裂。
 *
 ****************************************************************************/

int rk3576_vop2_fb_pan(uintptr_t buffer);

/****************************************************************************
 * Name: rk3576_vop2_fb_setup
 *
 * Description:
 *   把 ESMART1 图层扩成全屏并指向 buffer。沿用 U-Boot 建立的面板、DSI、
 *   D-PHY 与 VP1 时序，只改图层几何、跨距与地址。
 *
 ****************************************************************************/

int rk3576_vop2_fb_setup(uintptr_t buffer, uint32_t width, uint32_t height,
                         uint32_t stride_bytes);

void rk3576_vop2_dump_uboot_state(void);

/****************************************************************************
 * Name: rk3576_vop2_restore_uboot
 *
 * Description:
 *   把 ESMART1 恢复成 U-Boot 留下的窗口配置（那是唯一一组实测可用的
 *   参数），并返回其帧缓冲地址与几何，供直接写像素使用。
 *
 ****************************************************************************/

int rk3576_vop2_restore_uboot(uintptr_t *buffer, uint32_t *width,
                              uint32_t *height, uint32_t *stride_bytes);

/****************************************************************************
 * Name: rk3576_vop2_dump_win
 *
 * Description:
 *   打印 ESMART1 图层的整个寄存器块与 VP1 时序，并把编码字段解码成
 *   尺寸/跨距/起点。可在 nsh 里随时调用 —— 影子寄存器要等 VSYNC 才
 *   latch，"写完立刻回读"读到的是旧值，隔一会儿再看才是硬件在用的。
 *
 ****************************************************************************/

void rk3576_vop2_dump_win(void);

/****************************************************************************
 * Name: rk3576_vop2_try_window
 *
 * Description:
 *   以 U-Boot 那组已知可用的窗口参数为原点，只改指定的几项。传 0 的
 *   参数沿用原值。origin：0 沿用 U-Boot 的显示起点，1 用有效区原点
 *   （0，正确值），2 用把消隐段算进去的错误值以复现故障。
 *
 *   用来一次只挪动一个变量：fb_setup 同时改了地址、宽、高、跨距、起点
 *   五样，任何一样错了现象都一样，一次实验排除不掉任何一个。
 *
 ****************************************************************************/

int rk3576_vop2_try_window(uintptr_t buffer, uint32_t width, uint32_t height,
                           int origin, uintptr_t *out_buffer,
                           uint32_t *out_width, uint32_t *out_height,
                           uint32_t *out_stride);

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_VOP2_H */
