/****************************************************************************
 * chip/rk3576/hardware/rk3576_vop2.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_VOP2_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_VOP2_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* RK3576 VOP2 显示控制器。
 *
 * ★ 出处：Linux drivers/gpu/drm/rockchip/rockchip_drm_vop2.h 与
 *   rockchip_vop2_reg.c 的 rk3576_vop_video_ports[] /
 *   rk3576_vop_smart_regs[]。
 *   基址取自主线 rk3576.dtsi 的 vop@27d00000。
 *
 * ★★ RK3576 与 RK3568 的显示接口寄存器完全不同，不能照抄。
 *
 *   RK3568：DSP_IF_EN (0x028) 里用位域选接口
 *   RK3576：每个接口一个独立寄存器，MIPI0 在 0x180
 *
 *   照抄 RK3568 的写法不会报错 —— 只是写到了别的寄存器上，屏不亮而
 *   无从查起。本文件只收录 RK3576 实际使用的那一套。
 */

/* 系统寄存器 */

#define RK3576_VOP2_REG_CFG_DONE     0x0000  /* 配置生效，写 1 提交 */
#define RK3576_VOP2_VERSION_INFO     0x0004  /* 只读，版本号        */
#define RK3576_VOP2_SYS_AUTO_GATING  0x0008
#define RK3576_VOP2_SYS_MMU_CTRL_IMD 0x0020
#define RK3576_VOP2_SYS_PORT_CTRL_IMD 0x0028
#define RK3576_VOP2_MIPI0_IF_CTRL    0x0180  /* ★ RK3576 专属       */

/* SYS_CTRL_MIPI0_INFACE_CTRL 的字段（TRM Part2 第 11 章，复位值 0x80000030）
 *
 * ★ 其中 mipi_port_sel 决定 MIPI 接口从哪个视频端口取像素。引导器留下的
 *   值是 0x80100037 —— port_sel = 01 = VP1。若驱动在 VP0 上出图，两边
 *   对不上，VP0 扫描一切正常但像素进不了 DSI，现象就是背光亮、无显示。
 *   这个寄存器必须显式写，不能沿用引导器的。
 */

#define RK3576_MIPI_IF_REGDONE_IMD   (1u << 31)
#define RK3576_MIPI_IF_DCLK_OUT      (1u << 21)  /* 0=Dclk core 1=Dclk out */
#define RK3576_MIPI_IF_PIXCLK_DIV4   (1u << 20)  /* 0=Div2 1=Div4          */
#define RK3576_MIPI_IF_CMD_MODE      (1u << 11)  /* 0=视频模式 1=命令模式  */
#define RK3576_MIPI_IF_DATA1_SEL     (1u << 9)
#define RK3576_MIPI_IF_SPLIT_EN      (1u << 8)
#define RK3576_MIPI_IF_VSYNC_POS     (1u << 5)
#define RK3576_MIPI_IF_HSYNC_POS     (1u << 4)
#define RK3576_MIPI_IF_PORT_SEL_SHIFT 2          /* 00=VP0 01=VP1 1x=VP2   */
#define RK3576_MIPI_IF_PORT_SEL_MASK (3u << 2)
#define RK3576_MIPI_IF_CLK_OUT_EN    (1u << 1)
#define RK3576_MIPI_IF_OUT_EN        (1u << 0)
#define RK3576_VOP2_HDMI0_IF_CTRL    0x0184
#define RK3576_VOP2_EDP0_IF_CTRL     0x0188
#define RK3576_VOP2_DP0_IF_CTRL      0x018c

#define RK3576_VOP2_CFG_DONE_GLB_EN  (1 << 15)

/* 视频端口。VP0 支持到 4096x2304，接 MIPI DSI 足够。 */

/* 视频端口的中断寄存器在全局空间，不在 VPn_BASE 内。
 * 偏移沿用 RK3568 的排布（drivers/gpu/drm/rockchip/rockchip_drm_vop2.h）。
 * 用途不是接中断，而是做"VP 到底有没有在扫描"的判据：清掉原始状态，
 * 等一帧以上再读，帧起始位置起来才说明像素真的在往外送。
 */

#define RK3576_VOP2_VP_INT_EN(vp)    (0x00a0 + (vp) * 0x10)
#define RK3576_VOP2_VP_INT_CLR(vp)   (0x00a4 + (vp) * 0x10)
#define RK3576_VOP2_VP_INT_STATUS(vp) (0x00a8 + (vp) * 0x10)
#define RK3576_VOP2_VP_INT_RAW(vp)   (0x00ac + (vp) * 0x10)

#define RK3576_VP_INT_FS             (1u << 0)   /* 帧起始           */
#define RK3576_VP_INT_FS_FIELD       (1u << 5)   /* 场起始           */
#define RK3576_VP_INT_POST_BUF_EMPTY (1u << 4)   /* 后级缓冲欠载     */

/* ★ 本板的 MIPI 屏挂在 VP1 上，不是 VP0。
 *
 * 出处是出厂 U-Boot 留下的寄存器状态与它的启动日志：
 *   VP0_DSP_CTRL=0x8000000f  (bit31 STANDBY=1，未使用)
 *   VP1_DSP_CTRL=0x00060000  (未 standby，在用)
 *   SYS_CTRL_MIPI0_INFACE 的 port_sel=01 -> VP1
 *   日志："update mode to: 720x1280p79, type: MIPI0 for VP1"
 */

#define RK3576_VOP2_MIPI_VP          1

#define RK3576_VOP2_VP0_BASE         0x0c00
#define RK3576_VOP2_VP1_BASE         0x0d00

/* ★ VP 的中断块**不在 VP 寄存器块里**，是另一段绝对偏移。
 *
 *   出处：原厂 drivers/gpu/drm/rockchip/rockchip_vop_reg.h
 *     RK3568_VP0_INT_EN 0xA0 / VP1 0xB0，每个 VP 占 0x10
 *     RK3568_VP1_INT_EN 0xB0  INT_CLR 0xB4  INT_STATUS 0xB8  INT_RAW 0xBC
 *
 *   rockchip_vop2_reg.c 里 RK3576 直接复用 rk3568_vp1_intr，而且
 *   RK3568_VP1_DSP_CTRL = 0xD00 与我们的 VP1_BASE 一致 —— 说明这套偏移
 *   在 RK3576 上通用。
 *
 *   状态位的位号就是 rk3568_vop_intrs[] 的下标：
 *     0 FS_INTR（帧开始）  1 FS_NEW  2 LINE_FLAG  3 LINE_FLAG1
 *     4 POST_BUF_EMPTY     5 FS_FIELD  6 DSP_HOLD_VALID
 */

#define RK3576_VOP2_VP1_INT_EN       0x00b0
#define RK3576_VOP2_VP1_INT_CLR      0x00b4
#define RK3576_VOP2_VP1_INT_STATUS   0x00b8
#define RK3576_VOP2_VP1_INT_RAW      0x00bc
#define RK3576_VOP2_INT_FS           (1u << 0)
#define RK3576_VOP2_VP2_BASE         0x0e00

/* 视频端口内的偏移（相对 VPn_BASE） */

#define RK3576_VP_DSP_CTRL           0x00
#define RK3576_VP_MIPI_CTRL          0x04
#define RK3576_VP_COLOR_BAR_CTRL     0x08   /* 内置彩条，点屏第一步用 */
#define RK3576_VP_DSP_BG             0x2c
#define RK3576_VP_POST_DSP_HACT_INFO 0x34
#define RK3576_VP_POST_DSP_VACT_INFO 0x38
#define RK3576_VP_DSP_HTOTAL_HS_END  0x48
#define RK3576_VP_DSP_HACT_ST_END    0x4c
#define RK3576_VP_DSP_VTOTAL_VS_END  0x50
#define RK3576_VP_DSP_VACT_ST_END    0x54

#define RK3576_VP_DSP_CTRL_STANDBY   (1u << 31)
#define RK3576_VP_DSP_CTRL_OUT_MODE_MASK  0xf
#define RK3576_VP_DSP_CTRL_OUT_MODE_RGB888 0

/* 图层。Esmart 是不带 AFBC 压缩的普通覆盖层，最适合做帧缓冲输出。 */

/* ★ 取数紧急度（urgency / hurry）—— 防 DDR 争用时的行缓冲欠载
 *
 *   出处：u-boot/drivers/video/drm/rockchip_vop2.c
 *     RK3576_SYS_AXI_HURRY_CTRL0_IMD 0x014 / CTRL1_IMD 0x018，
 *     bit(24 + crtc_id) 打开该 VP 的 AXI 端口紧急标志；
 *     VPn_COLOR_BAR_CTRL 的 bit8 = POST_URGENCY_EN，
 *     bit[19:16] = 低水位、bit[23:20] = 高水位。
 *   该文件里的注释写着：
 *     "RK3576 VP0 has 8 lines post linebuffer, when full post line buffer
 *      is less 4, the urgency signal will be set to 1, when full post line
 *      buffer is over 6, the urgency signal will be set to 0."
 *
 *   ★ 为什么我们要自己开
 *
 *     U-Boot 的 rk3576_vp_data[] 里**只有 VP0 配了 urgency**，VP1/VP2 是空的
 *     —— 它单独跑时没人抢 DDR，不开也够。而 AMP 下四核 A72 上的 Linux 一起
 *     压 DDR，VP1 的读请求排不进去，行缓冲见底，屏上就是
 *     **几条黑色细横条纹**。
 *
 *     这个故障的所有特征都指向"取数跟不上"而不是"内容不对"：
 *       - VOP2 内置彩条干净（彩条不走 DDR）
 *       - 绝大部分正常，只有几条细横带（只有带宽不足的那几行受害）
 *       - 整屏刷 cache 无效、把帧缓冲填成纯色仍然花（与内容无关）
 *       - **只在双核下出现，单跑 openvela 时正常**（这一条最关键）
 *
 *   这几个寄存器都是普通读改写，不是 hiword-mask（U-Boot 那边
 *   vop2_mask_write 传的 write_mask = false）。
 */

#define RK3576_VOP2_SYS_AXI_HURRY_CTRL0_IMD 0x0014
#define RK3576_VOP2_SYS_AXI_HURRY_CTRL1_IMD 0x0018
#define RK3576_AXI_PORT_URGENCY_EN_SHIFT    24

#define RK3576_VP_URGENCY_EN                (1u << 8)
#define RK3576_VP_URGENCY_THL_SHIFT         16
#define RK3576_VP_URGENCY_THH_SHIFT         20
#define RK3576_VP_URGENCY_TH_MASK           0xf

/* 水位沿用厂商给 VP0 的值：8 行缓冲，低于 4 行拉紧急，高于 6 行撤销。 */

#define RK3576_VP_URGENCY_THL               4
#define RK3576_VP_URGENCY_THH               6

#define RK3576_VOP2_ESMART0_BASE     0x1800
#define RK3576_VOP2_ESMART1_BASE     0x1a00

/* 图层内的偏移（相对 ESMARTn_BASE），字段位置取自 rk3576_vop_smart_regs[] */

#define RK3576_SMART_CTRL0           0x00
#define RK3576_SMART_CTRL1           0x04
#define RK3576_SMART_REGION0_CTRL    0x10   /* bit0 使能，bit[5:1] 格式 */
#define RK3576_SMART_REGION0_YRGB_MST 0x14  /* 帧缓冲物理地址          */
#define RK3576_SMART_REGION0_VIR     0x1c   /* 行跨距，单位为 4 字节   */
#define RK3576_SMART_REGION0_ACT_INFO 0x20  /* 源尺寸 (h-1)<<16|(w-1)  */
#define RK3576_SMART_REGION0_DSP_INFO 0x24  /* 显示尺寸                */
#define RK3576_SMART_REGION0_DSP_ST  0x28   /* 显示起点                */

/* ★ RK3576 专有：图层归属哪个视频口，由窗口自己的这个寄存器决定，
 * 不是 RK3568/3588 的 OVL_LAYER_SEL。低 2 位是视频口号。
 * 名字里的 IMD = immediate，它不走影子寄存器，写下去立即生效。
 *
 * 这一条搞错的后果很隐蔽：VOP2 的影子寄存器是**按视频口**提交的，
 * 图层若不归你提交的那个口，CFG_DONE 不会刷新它的影子 —— 所有配置
 * 写进去都停在影子里，回读全是旧值，屏幕纹丝不动，且不报任何错。
 */

/* 缩放器。出处厂商 rockchip_drm_vop2.h：
 *   SCL_CTRL bit[1:0] 水平模式、bit[5:4] 垂直模式，0 = 不缩放
 *   SCL_FACTOR_YRGB 低 16 位 X、高 16 位 Y，1:1 时为 0x1000
 *
 * 改了 ACT_INFO/DSP_INFO 却不管缩放器，硬件会拿旧系数去采样新的源图。
 * 即使当前是旁路，也显式写一遍 —— 从 U-Boot 继承下来的状态里，
 * 每一个没被显式设定的寄存器都是一个未知量。
 */

#define RK3576_SMART_REGION0_SCL_CTRL        0x30
#define RK3576_SMART_REGION0_SCL_FACTOR_YRGB 0x34
#define RK3576_SMART_SCL_FACTOR_1TO1         0x1000

/* ESMART_AXI_CTRL_IMD —— 出处 RK3576 TRM
 *   「ESMART0_ESMART_AXI_CTRL_IMD, Base(0x27D01800) + offset(0x0008)」
 *
 * ★ bit2 esmart_mmu_bypass 复位为 0，也就是**默认走 VOP2 自己的 IOMMU**。
 *
 *   不旁路的话，写进 YRGB_MST 的地址会被 IOMMU 按页表翻译一遍。页表是
 *   U-Boot 为它自己那块帧缓冲建的，我们给的地址翻译过去落到零散的页
 *   上 —— 现象非常具有迷惑性：
 *     纯色填充   看起来完全正常（每一页都是同一个颜色）
 *     粗粒度图案 大致正确（页粒度 4KB ≈ 1.4 行，行方向的变化还看得出）
 *     细节图案   彻底打乱（竖条纹变斜纹、方块变成断续的横带）
 *   于是"整屏填白正常"会让人误以为地址和跨距都对，实际上根本没读对。
 *
 *   名字里的 IMD = immediate，写下去立即生效，不需要 CFG_DONE。
 */

#define RK3576_SMART_AXI_CTRL_IMD    0x08
#define RK3576_SMART_MMU_BYPASS      (1u << 2)

#define RK3576_SMART_PORT_SEL_IMD    0xf4
#define RK3576_SMART_PORT_SEL_MASK   0x3

#define RK3576_SMART_REGION0_CTRL_EN (1 << 0)
#define RK3576_SMART_REGION0_CTRL_FORMAT_SHIFT 1

/* 像素格式（REGION0_CTRL 的 bit[5:1]） */

/* 出处 RK3576 TRM「ESMART0_REGION0_CTL」的 region0_data_fmt（bit[5:1]）。
 * ★ 原来 RGB888 写成 2、RGB565 写成 3，是照搬别的芯片的编码，错的。
 *   ARGB8888=0 碰巧一致，所以当前用法没暴露问题 —— 这种"错了但没现象"
 *   的常量最危险，等哪天真去设 RGB565 才会发现。
 */

#define RK3576_VOP2_FMT_ARGB8888     0
#define RK3576_VOP2_FMT_RGB888       1
#define RK3576_VOP2_FMT_RGB565       2

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_VOP2_H */
