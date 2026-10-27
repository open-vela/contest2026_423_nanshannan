/****************************************************************************
 * vendor/rockchip/boards/rk3576/kickpi-k7/include/board.h
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

#ifndef __VENDOR_ROCKCHIP_BOARDS_RK3576_KICKPI_K7_INCLUDE_BOARD_H
#include <stdbool.h>

#define __VENDOR_ROCKCHIP_BOARDS_RK3576_KICKPI_K7_INCLUDE_BOARD_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* KICKPI-K7 (Rockchip RK3576)
 *
 * SoC     : RK3576, quad Cortex-A72 @2.2GHz + quad Cortex-A53 @2.0GHz
 * DRAM    : 4/8/16 GB, 物理基址 0x40000000
 * eMMC    : 16/32/64 GB
 * 网络    : 双千兆以太网
 * 按键    : RESET / POWER / RECOVERY / MASKROM
 * 扩展    : GPIO x22, UART x5, PWM x8, ADC x3, I2C x1, PDM x1, CAN x1, I3C x1
 *
 * 调试串口：★ 已确认为 UART0 @ 0x2ad40000，波特率 1500000，时钟 24 MHz
 * （依据 KICKPI 官方 Armbian 源码的 U-Boot defconfig
 *  kickpi-k7-rk3576_defconfig 中的 CONFIG_DEBUG_UART_* 与 CONFIG_BAUDRATE）。
 */

/* 板级 GPIO 分配
 *
 * 来源：KICKPI 官方 Armbian 源码的厂商内核设备树
 * patch/kernel/rk35xx-vendor-6.1/dt/rk3576-kickpi-k7.dts
 *
 * Rockchip 引脚编码：bank 内按组 A/B/C/D 各 8 根，组内 0-7。
 * 线号 = 组序号 * 8 + 组内序号，例如 RK_PB4 = 1 * 8 + 4 = 12。
 *
 * RK3576 的 GPIO 控制器基址见 hardware/rk3576_memorymap.h：
 *   GPIO0 0x27320000（与 GPIO1-4 不在同一地址段）
 *   GPIO1 0x2ae10000  GPIO2 0x2ae20000  GPIO3 0x2ae30000  GPIO4 0x2ae40000
 */

/* LED（gpio-leds） */

#define BOARD_LED_WORK_BANK      0            /* work，dts 默认 heartbeat 触发 */
#define BOARD_LED_WORK_PIN       12           /* RK_PB4 = 1*8+4 */

#define BOARD_FAN_PWR_BANK       2            /* 风扇电源，默认 on */
#define BOARD_FAN_PWR_PIN        11           /* RK_PB3 = 1*8+3 */

#define BOARD_4G_PWR_BANK        0            /* 4G 模组电源，默认 on */
#define BOARD_4G_PWR_PIN         8            /* RK_PB0 = 1*8+0 */

#define BOARD_SD_PWR_BANK        0            /* SD 卡电源，默认 on */
#define BOARD_SD_PWR_PIN         14           /* RK_PB6 = 1*8+6 */

/* 以太网 PHY 复位（RTL8211F，rgmii-rxid） */

#define BOARD_GMAC0_RST_BANK     2
#define BOARD_GMAC0_RST_PIN      13           /* RK_PB5 = 1*8+5，低有效 */

#define BOARD_GMAC1_RST_BANK     3
#define BOARD_GMAC1_RST_PIN      3            /* RK_PA3 = 0*8+3，低有效 */

/* 测试用输入引脚
 *
 * ★ 必须选一个**确实空闲**的脚，不能图方便借用已有器件的中断线。
 *
 *   最初借用了 TP_INT_L(GPIO0_C5)，理由是它有外部上拉、读起来安全 ——
 *   但触摸驱动已经 attach 了自己的中断处理，测试用例再把它配成双边沿
 *   并使能，两个使用者对同一根中断线做了冲突配置，结果板子挂死。
 *   电气上可用不等于空闲。
 *
 *   ★ 更正：原来选的是 GPIO4_B3，理由是"厂商 extend-40pin.dtsi 里列了它"。
 *     但 dtsi 列出的是**芯片上存在的引脚**，不等于**连接器上引出的引脚**。
 *     查 KICKPI-K7 规格书的「40Pin 引脚定义」表，GPIO4 实际只引出五根：
 *
 *       GPIO4_A4 → 5 脚      GPIO4_B0 → 8 脚（SPI4_CLK）
 *       GPIO4_A6 → 7 脚      GPIO4_B1 → 10 脚（SPI4_MOSI）
 *                            GPIO4_B2 → 12 脚（SPI4_MISO）
 *
 *     GPIO4_A7 和 GPIO4_B3 都不在表里 —— 插不上跳线，这个测试根本做不了。
 *     从 dtsi 挑引脚而没查连接器表，是一个只有到了插线那一刻才会暴露的
 *     错误。**"软件上能配"与"手上能接"是两件事。**
 *
 *   改用 GPIO4_A6（7 脚）作输入。B0/B1/B2 留给 SPI4，不占。
 */

#define BOARD_TESTPIN_BANK       4            /* GPIO4_A6，排针第 7 脚 */
#define BOARD_TESTPIN_PIN        6            /* RK_PA6 = 0*8+6 */

/* 与上面那一脚配对的输出脚，用于 cmocka_driver_gpio 的中断子项。
 *
 * 那个用例需要一个输出脚和一个输入脚：它拉动输出脚产生边沿，再等
 * 输入脚上的中断。两脚必须用杜邦线短接，否则输入脚上永远不会有边沿。
 *
 * ★ 必须和 GPIO4_B3 同在 1.8V 域
 *
 *   40 针口上的 GPIO 分属两个电压域（厂商 dtsi 逐条标了 1.8V / 3.3V）。
 *   拿 3.3V 的输出脚去驱动 1.8V 的输入脚会超出该脚耐压。GPIO4_A4 与
 *   GPIO4_A6 同属 GPIO4 的 A 组，电压域一致，可以直接对接。
 *
 *   接线：**排针第 5 脚（GPIO4_A4，输出） ↔ 第 7 脚（GPIO4_A6，输入）**，
 *   同在奇数排、相隔一个位置，一根短杜邦线即可。
 */

#define BOARD_TESTPIN_OUT_BANK   4            /* GPIO4_A4，排针第 5 脚 */
#define BOARD_TESTPIN_OUT_PIN    4            /* RK_PA4 = 0*8+4 */

/* 实时时钟 HYM8563
 *
 * 出处：原厂 dtb 的 /i2c@2ac50000/hym8563@51（compatible "haoyu,hym8563"）。
 * 该芯片同时输出 32.768kHz 给 SDIO WiFi 模块用。
 */

#define BOARD_RTC_I2C_BUS        2
#define BOARD_RTC_I2C_ADDR       0x51

/* 5 寸 MIPI 屏 + 电容触摸（F050008M01，720x1280）
 *
 * ★ 出处：原理图 K7_V1.1_20241211_SCH.pdf 第 27 页 "Single-MIPI LCM"，
 *   配合同一份 PDF 里的芯片引脚复用表。这是此前一直缺失的那份信息 ——
 *   厂商的 LCD overlay dtsi 拿不到，但原理图给出了同样的答案。
 *
 * 30pin FPC (J5100) 关键脚：
 *   Pin17 LCD_PWM_BL   <- LCD_BL_PWM1_CH1_M0  GPIO0_B5 (PWM1_CH1_M0)
 *   Pin18 LCD_TE       ── 原理图上打叉，未连线（见下方说明）
 *   Pin19 VCC3V3_LCD   <- VCC3V3_LCD_S0（受 LCD_PWREN 控制的电源轨）
 *   Pin20 LCD_RST      <- LCD_RESET_L3 = LCD_RESET_L 经电平转换
 *   Pin21 LCD_ID       -> SARADC_IN7（电阻分压识别屏型号，可选）
 *   Pin22 LCD_PWREN    <- LCD_PWREN_H         GPIO0_C6
 *   Pin23 TP_I2C_SCL   <- I2C0_SCL_M1_TP      GPIO0_C1 (func 9)
 *   Pin24 TP_I2C_SDA   <- I2C0_SDA_M1_TP      GPIO0_C2 (func 9)
 *   Pin25 TP_INT       <- TP_INT_L            GPIO0_C5
 *   Pin26 TP_RST       <- TP_RST_L            GPIO0_D0
 *   Pin28-30 5V0       <- VCC5V0_DEVICE_S0（常供，不受软件控制）
 *
 * ★ 电源轨：屏和触摸共用 VCC3V3_LCD_S0，由 LCD_PWREN_H 经
 *   Q5002(S8050 NPN) -> Q5100(WPM2341 P-MOS) 开关。LCD_PWREN_H 拉高
 *   才有 3.3V。此前扫遍 I2C 找不到触摸，根因就是这一条没拉高 ——
 *   芯片没电，任何总线上都不会应答。
 *
 * ★ LCD_RESET_L 是 1.8V 域，经 Q5101 电平转换成 3.3V 的 LCD_RESET_L3
 *   再送到 FPC。转换是非反相的，软件按低有效复位即可。
 *
 * ★ LCD_TE 未连线（原理图 Pin18 打叉）。没有 TE 信号，DSI 命令模式无法
 *   与屏刷新同步，因此这块屏必须走**视频模式**。
 */

#define BOARD_LCD_PWREN_BANK     0            /* GPIO0_C6，高有效，屏 3V3 使能 */
#define BOARD_LCD_PWREN_PIN      22           /* RK_PC6 = 2*8+6 */

#define BOARD_LCD_RST_BANK       0            /* GPIO0_A2，低有效 */
#define BOARD_LCD_RST_PIN        2            /* RK_PA2 = 0*8+2 */

#define BOARD_LCD_BL_BANK        0            /* GPIO0_B5，PWM1_CH1_M0 */
#define BOARD_LCD_BL_PIN         13           /* RK_PB5 = 1*8+5 */

#define BOARD_TP_I2C_BUS         0            /* I2C0，M1 复用（时钟在 PMU 域） */

#define BOARD_TP_INT_BANK        0            /* GPIO0_C5 */
#define BOARD_TP_INT_PIN         21           /* RK_PC5 = 2*8+5 */

#define BOARD_TP_RST_BANK        0            /* GPIO0_D0，低有效 */
#define BOARD_TP_RST_PIN         24           /* RK_PD0 = 3*8+0 */

/* MIPI CSI 摄像头（Sony IMX415，cam0）
 *
 * 出处：厂商 Armbian 源码
 *   patch/kernel/rk35xx-vendor-6.1/dt/rk3576-kickpi-k7-cam0.dtsi
 *
 *   imx415_0@37   I2C4（i2c4m3_xfer = GPIO3_B0/GPIO3_A7 功能 11）
 *   xvclk         板上 37.125MHz 固定晶振，非 SoC 提供
 *   avdd          vcc_mipidcphy0，GPIO0_D2 高有效
 *   data-lanes    4
 *   通路          imx415 -> csi2_dcphy0 -> mipi0_csi2 -> rkcif -> rkisp
 */

#define BOARD_CAM_PWR_BANK       0            /* GPIO0_D2，高有效 */
#define BOARD_CAM_PWR_PIN        26           /* RK_PD2 = 3*8+2 */

/* 存储配置（供 M4 存储适配参考）
 *
 *   sdhci  : eMMC，8 位总线，HS400 1.8V + enhanced strobe，non-removable
 *   sdmmc  : SD 卡，4 位总线，pinctrl 用 sdmmc0_clk/cmd/det/bus4
 *
 * 上电顺序注意：SD 卡供电由 BOARD_SD_PWR 控制，需先拉高再枚举。
 */

/* 其它板级信息
 *
 *   PMIC     : RK806（厂商 dts 引入 rk3576-rk806.dtsi）
 *   RTC      : HYM8563（同时为 SDIO WiFi 提供 32.768kHz 外部时钟）
 *   DDR blob : rk3576_ddr_lp4_1560MHz_lp5_2736MHz_v1.08.bin
 *
 * DRAM：★ 已实测为 4 GB 版本。板上出厂 Android 的 /proc/meminfo 报
 *   MemTotal: 3989804 kB（≈3.8 GiB，差额是固件保留区与内核自身占用）。
 *   物理范围 0x40000000 .. 0x140000000。
 *
 *   端口当前取 CONFIG_RAMBANK1_ADDR=0x42000000 + 512MB，即映射
 *   0x42000000..0x62000000，稳落在物理范围内。openvela 侧任务极少，
 *   512MB 绰绰有余；保守取值同时也为将来 AMP 划分共享内存留出空间。
 */

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifndef __ASSEMBLY__

/****************************************************************************
 * Name: kickpi_camera_stream
 *
 * Description:
 *   让已识别的 IMX415 开始/停止在 MIPI 上输出图像。
 *
 *   放在 board.h 而不是板级私有的 kickpi_k7.h 里，是因为调试用的
 *   cam 命令是一个独立的应用，够不到私有头。
 *
 * Input Parameters:
 *   on - true 出流，false 回待机
 *
 * Returned Value:
 *   OK 成功；-ENODEV 表示启动时没识别到传感器。
 *
 ****************************************************************************/

int kickpi_camera_stream(bool on);

/****************************************************************************
 * 去马赛克 + JPEG（kickpi_k7_imgproc.c）
 *
 *   phase 是 Bayer 相位（0=RGGB 1=GRBG 2=GBRG 3=BGGR）。
 *   ★ 必须拿彩色标定图实测确认 —— 相位错了图像不会崩，只会红蓝互换
 *     或整体偏色，看起来像白平衡问题，很容易归错因。
 ****************************************************************************/

int kickpi_camera_jpeg(FAR const char *path, int phase, int quality);

/* HDMI（chip/rk3576/rk3576_hdmi.c）。声明放在 board.h 是为了让 app 能用 ——
 * app 只能 include <arch/board/board.h>，够不到芯片层的私有头。
 */

int rk3576_hdmi_prepare(void);
int rk3576_hdmi_probe(void);

/* 蓝牙（AP6256 / BCM4345C5，UART4）。由 `bt init` 命令手动触发 ——
 * 不在启动路径上，理由见 kickpi_k7_appinit.c 里的说明。
 */

int kickpi_k7_bt_probe(void);
int kickpi_k7_bt_initialize(void);
int kickpi_k7_wifi_initialize(void);
int kickpi_k7_wifi_scan(void);

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

/****************************************************************************
 * Name: kickpi_camera_receiver
 *
 * Description:
 *   打开/关闭取图链路的接收端（D-PHY RX + CSI-2 host）。
 *   与出流分开下令，好让启动瞬态不被当成真错误。
 *
 ****************************************************************************/

int kickpi_camera_receiver(bool on);

/****************************************************************************
 * Name: kickpi_camera_status
 *
 * Description:
 *   读 CSI-2 host 的通道状态与错误计数，区分「线上没数据」与
 *   「有数据但收错」。
 *
 ****************************************************************************/

int kickpi_camera_status(void);

/****************************************************************************
 * Name: kickpi_camera_capture
 *
 * Description:
 *   启动 CIF 取一帧到 DDR，并报告像素统计（最小/最大/均值）。
 *   先用统计量确认"取到的是真实图像"，再谈送屏显示。
 *
 ****************************************************************************/

int kickpi_camera_capture(void);

/****************************************************************************
 * Name: kickpi_camera_show
 *
 * Description:
 *   把最近取到的一帧以灰度送到 /dev/fb0，画面竖直居中。按 p1~p99 拉伸，
 *   gamma 非 0 时再做一次 gamma 0.5 编码 —— 线性数据直接送显示器，
 *   中间调会被压到接近黑，那不是画面问题而是缺了这一步。
 *   收尾会打印**写出去的**灰度分布，用来把"写的就是黑"与"写对了但
 *   显示不出来"分开。
 *
 ****************************************************************************/

int kickpi_camera_show(int gamma);

/* 同上，但 seq >= 0 时在黑边画一个随帧号移动的白块 —— 用来把
 * "屏幕没更新"与"屏幕更新了但画面没变"分开。
 */

int kickpi_camera_show_seq(int gamma, int seq);

/****************************************************************************
 * Name: kickpi_camera_fbinfo
 *
 * Description:
 *   只读并打印帧缓冲参数，不写入。与 show 分开，是为了在 show 崩溃时
 *   仍能拿到参数 —— 串口是中断发送的，崩溃会带走排队中的日志。
 *
 ****************************************************************************/

int kickpi_camera_fbinfo(void);

/****************************************************************************
 * Name: kickpi_camera_fbtest
 *
 * Description:
 *   往帧缓冲填固定图案（0 白 / 1 黑 / 2 竖条纹），不涉及摄像头数据，
 *   用来把"显示通路"与"图像处理"两类问题分开。
 *
 ****************************************************************************/

int kickpi_camera_fbtest(int pattern);

/****************************************************************************
 * Name: kickpi_camera_ubtest
 *
 * Description:
 *   恢复 U-Boot 的窗口配置并直接往它的帧缓冲写图案（0 白 / 1 黑 /
 *   2 竖条纹）。用来把"显示通路本身"与"我们改的 VOP2 配置"分开。
 *
 ****************************************************************************/

int kickpi_camera_ubtest(int pattern);

/****************************************************************************
 * Name: kickpi_camera_morph
 *
 * Description:
 *   从 U-Boot 那组已知可用的窗口参数出发，按步骤号一次只改一个变量
 *   （地址 / 宽 / 高 / 显示起点），改完画竖条纹。第一个出斜纹的步骤
 *   就指名了 fb_setup 里错的是哪一项。步骤 0~6，含义见实现处的表。
 *
 ****************************************************************************/

int kickpi_camera_morph(int step);

/****************************************************************************
 * Name: kickpi_camera_regs
 *
 * Description:
 *   只读打印 ESMART1 图层与 VP1 的寄存器现状，不写入。
 *
 ****************************************************************************/

int kickpi_camera_regs(void);

/****************************************************************************
 * Name: kickpi_camera_gain
 *
 * Description:
 *   设置 IMX415 的模拟增益（GAIN_PCG_0，0~240，每级 0.3dB），可选地同时
 *   改快门 SHR0（传 -1 不动；曝光行数 = VMAX - SHR0，值越小曝光越长）。
 *
 *   板级模式表里没有 0x3090/0x3091，增益一直是复位的 0dB。做成命令是为了
 *   能连续扫一遍看直方图怎么走，而不是每试一个值重编一次固件。
 *
 ****************************************************************************/

int kickpi_camera_gain(int gain, int shr);

/****************************************************************************
 * Name: kickpi_camera_vmax
 *
 * Description:
 *   改 IMX415 的 VMAX（一帧总行数）即改帧率。本模式默认 3165 行而有效行
 *   只有 1097，三分之二的帧时间在空转；下限 1143（= 有效行 + 46）对应
 *   约 83fps。减的是空行，行速率不变，MIPI 带宽不受影响。
 *   代价是曝光上限同比缩短，要用 cam gain 补回来。
 *
 ****************************************************************************/

int kickpi_camera_vmax(int vmax);

/****************************************************************************
 * Name: kickpi_camera_live_start / _frame / _stop
 *
 * Description:
 *   界面用的实时预览：连续取流，每次把最新一帧缩小成 w x h 的 ARGB8888。
 *   与 /dev/video0 互斥，另一边在用时 start 返回 -EBUSY。
 *
 ****************************************************************************/

int  kickpi_camera_live_start(void);
int  kickpi_camera_live_frame(FAR uint32_t *argb, int w, int h,
                              int timeout_ms);
void kickpi_camera_live_stop(void);
bool kickpi_camera_live_active(void);
void kickpi_camera_v4l2_busy(bool on);

/****************************************************************************
 * Name: kickpi_camera_preview
 *
 * Description:
 *   连续取图送屏，跑满 frames 帧或串口收到任意输入即停，结束时报帧率。
 *   逐帧统计日志会被抑制 —— 否则测到的是串口速度而不是取图送屏的速度。
 *
 ****************************************************************************/

int kickpi_camera_preview(int frames);

#endif /* __ASSEMBLY__ */

#endif /* __VENDOR_ROCKCHIP_BOARDS_RK3576_KICKPI_K7_INCLUDE_BOARD_H */
