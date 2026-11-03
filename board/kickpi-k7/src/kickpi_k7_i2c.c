/****************************************************************************
 * vendor/rockchip/boards/rk3576/kickpi-k7/src/kickpi_k7_i2c.c
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

/* 注册板上 I2C 总线为 /dev/i2cN，可用 nsh 的 i2c 命令扫描与读写。
 *
 * 板上器件（出处：原厂 boot.img 里的 dtb，用 scripts/dtb-query.py 查得）：
 *
 *   i2c@2ac50000 (I2C2)  hym8563@51   RTC，同时给 SDIO WiFi 供 32.768kHz
 *                        husb311@4e   Type-C PD 控制器
 *
 * 验证方式：i2c dev 2 0x03 0x77 扫描总线，应恰好出现 0x4e 和 0x51 ——
 * 与 dtb 记载一致才算通过。扫到别的地址或扫不到，都说明有问题。
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <debug.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/i2c/i2c_master.h>

#include <arch/board/board.h>
#include <nuttx/timers/hym8563.h>

#include "rk3576_i2c.h"
#include "kickpi_k7.h"

#ifdef CONFIG_RK3576_I2C

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 板上要注册的 I2C 总线。
 *
 * I2C1 上的 PMIC 是 U-Boot 启动时必然访问过的器件，因此这条总线的
 * 功能时钟一定处于开启状态。把它一并注册出来，可作为驱动逻辑的
 * 对照组：若 I2C1 能扫到 PMIC 而 I2C2 扫不到任何东西，则驱动本身
 * 没问题，缺的是 I2C2 的时钟使能。
 */

static const struct
{
  int         port;
  const char *devices;
}
g_kickpi_i2c[] =
{
  { 0, "触摸 GT9xx@0x5d（5 寸屏 FPC Pin23/24，时钟在 PMU 域）" },
  { 1, "PMIC RK806@0x23" },
  { 2, "RTC HYM8563@0x51、PD HUSB311@0x4e" },
  { 3, "音频 codec ES8388@0x10" },

  /* 以下四条加入是为了定位触摸控制器：屏接上后 I2C1/2/3 均未扫到
   * GT9xx（0x5d 或 0x14）。原厂 dtb 中这四条状态为 okay。
   */

  { 4, "（探测用）dtb 记载 imx415_0@0x37" },
  { 5, "（探测用）dtb 记载 imx415_1@0x37" },
  { 7, "（探测用）" },
  { 6, "（探测用）dtb 标 disabled" },
#ifndef CONFIG_KICKPI_K7_CAMERA_LINUX
  /* 摄像头归 Linux 时 I2C8 是 Linux 的：在这边初始化就是把它的控制器复位 */

  { 8, "（探测用）dtb 记载 imx415_3@0x37" },
#endif
  { 9, "（探测用）dtb 标 disabled" },
};

#define KICKPI_NI2C (sizeof(g_kickpi_i2c) / sizeof(g_kickpi_i2c[0]))

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_i2c_initialize
 *
 * Description:
 *   初始化板上 I2C 总线并注册为字符设备 /dev/i2cN。
 *
 ****************************************************************************/

int kickpi_k7_i2c_initialize(void)
{
  struct i2c_master_s *i2c;
  int ret;
  int i;

  for (i = 0; i < KICKPI_NI2C; i++)
    {
      int port = g_kickpi_i2c[i].port;

      i2c = rk3576_i2cbus_initialize(port);
      if (i2c == NULL)
        {
          syslog(LOG_ERR, "ERROR: I2C%d 初始化失败\n", port);
          continue;
        }

#ifdef CONFIG_I2C_DRIVER
      ret = i2c_register(i2c, port);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: 注册 /dev/i2c%d 失败: %d\n", port, ret);
          rk3576_i2cbus_uninitialize(i2c);
          continue;
        }

      syslog(LOG_INFO, "I2C: /dev/i2c%d 就绪（dtb 记载：%s）\n",
             port, g_kickpi_i2c[i].devices);
#else
      UNUSED(ret);
#endif
    }

  return OK;
}

#endif /* CONFIG_RK3576_I2C */

#ifdef CONFIG_RTC_HYM8563
/****************************************************************************
 * Name: kickpi_k7_rtc_initialize
 *
 * Description:
 *   注册板上的 HYM8563 为 /dev/rtc0。
 *
 *   出处：原厂 dtb 的 /i2c@2ac50000/hym8563@51，compatible 为
 *   "haoyu,hym8563"，同时对外输出 32.768kHz 给 SDIO WiFi 用。
 *
 ****************************************************************************/

int kickpi_k7_rtc_initialize(void)
{
  struct i2c_master_s *i2c;

  i2c = rk3576_i2cbus_initialize(BOARD_RTC_I2C_BUS);
  if (i2c == NULL)
    {
      syslog(LOG_ERR, "ERROR: RTC 所在 I2C%d 初始化失败\n",
             BOARD_RTC_I2C_BUS);
      return -ENODEV;
    }

  /* ★ 使能 HYM8563 的 CLKOUT，32.768kHz —— WiFi/蓝牙模组靠它才能起来。
   *
   *   依据是板子原理图 K7_V2.1.pdf：
   *       HYM8563TS 的 32KOUT_RTC -> 32KOUT_RTC2CON -> WIFIBT_32KIN_2T2R
   *   即无线模组的 32kHz 输入**由这颗 RTC 提供**，没有别的来源。
   *
   *   HYM8563 的 CLKOUT 要显式打开：寄存器 0x0D，bit7=FE（输出使能），
   *   bit1:0=FD（00 = 32.768kHz）。写 0x80 即"使能 + 32.768kHz"。
   *
   *   ★ 这一条本文件顶部的注释里早就写着（"同时给 SDIO WiFi 供
   *     32.768kHz"），但代码从来没写过 —— 知道和做到是两回事。
   *
   *   ★ 也曾据"dtb 里没有节点引用 RTC 的时钟输出"否定过 32k 这条线索。
   *     那个推理是错的：连接在硬件上，dtb 不引用只说明 Linux 侧没走
   *     时钟框架，不代表模组不需要。**电路连接看原理图，不看设备树。**
   */

  {
    struct i2c_msg_s msg;
    uint8_t buf[2];
    int ret;

    buf[0] = 0x0d;          /* CLKOUT 控制寄存器 */
    buf[1] = 0x80;          /* FE=1 使能，FD=00 -> 32.768kHz */

    msg.frequency = 400000;
    msg.addr      = BOARD_RTC_I2C_ADDR;
    msg.flags     = 0;
    msg.buffer    = buf;
    msg.length    = 2;

    ret = I2C_TRANSFER(i2c, &msg, 1);
    if (ret < 0)
      {
        syslog(LOG_ERR, "ERROR: 使能 RTC 32.768kHz 输出失败: %d\n", ret);
      }
    else
      {
        syslog(LOG_INFO,
               "RTC: CLKOUT 32.768kHz 已使能（供 WiFi/蓝牙模组）\n");
      }
  }

  return hym8563_rtc_initialize(0, i2c, BOARD_RTC_I2C_ADDR, 400000);
}
#endif

#ifdef CONFIG_RTC_ARCH
/****************************************************************************
 * Name: up_rtc_initialize
 *
 * Description:
 *   NuttX 在 clock_initialize() 里调用它初始化 RTC。
 *
 *   ★ 本板的 RTC 挂在 I2C 上，而这个函数在系统启动很早期被调用 ——
 *     那时 I2C 控制器尚未初始化，真正的注册必须放到 board_app_initialize()
 *     里做（见 kickpi_k7_rtc_initialize）。这里只返回成功，让时钟子系统
 *     先用软件计时启动，等 RTC 就绪后由 up_rtc_set_lowerhalf() 接管。
 *
 *     如果在这里访问 I2C，会在 PMIC/时钟都还没配好的时候操作总线，
 *     现象是启动早期挂死且没有任何日志。
 *
 ****************************************************************************/

int up_rtc_initialize(void)
{
  return OK;
}
#endif
