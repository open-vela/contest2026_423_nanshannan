/****************************************************************************
 * board/kickpi-k7/src/kickpi_k7_audio.c
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

/* 音频：ES8388 codec + SAI1。
 *
 * ★ 麦克风输入必须选 LINE2，出处是原理图 K7_V2.1 第 25 页（Audio-CODEC）：
 *
 *     ES8388(U7000) 24 LIN1 <- MIC_INP_PHONE  4 极耳机座上的麦克风
 *                   23 RIN1 <- HP_GND         耳机地检测，不是麦克风
 *                   22 LIN2 <- MIC2P  \
 *                   21 RIN2 <- MIC2N  /      板上麦克风座 J7001
 *
 *   驱动默认的 LINE1 选中的是"耳机座麦克风 + 耳机地" —— 不插耳机时
 *   LIN1 悬空、RIN1 就是地，**采到的必然是零**，而且每一层都不会报错。
 *
 *   注意 dtb 的 rockchip,audio-routing 里 LINPUT1/LINPUT2 都标着
 *   "Main Mic" —— 那是 DAPM 的控件名，表示"这两条路都可以通到主麦"，
 *   **不说明物理上接的是什么**。物理连接只有原理图算数（案例 27 同理）。
 *
 * 板上连接（出处：原厂 dtb 的 es8388-sound 节点）：
 *     rockchip,cpu   = sai@2a610000   → SAI1
 *     rockchip,codec = es8388@10      → I2C3 上的 0x10
 *
 * codec 驱动本体是上游现成的 drivers/audio/es8388.c，本文件只负责
 * 把 I2C 与 I2S 两个句柄交给它，并注册成 /dev/audio/pcm0。
 *
 * ★ ES8388 的 0x10 已在板上实测应答（i2c dev -b 3 扫到），
 *   因此这条链路的 I2C 一侧是确定的，不像触摸那样带推测成分。
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <debug.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/audio/audio.h>
#include <nuttx/audio/es8388.h>
#include <nuttx/audio/i2s.h>
#include <nuttx/audio/pcm.h>

#include "rk3576_gpio.h"
#include "rk3576_i2c.h"
#include "rk3576_pinmux.h"
#include "rk3576_sai.h"
#include "kickpi_k7.h"

#if defined(CONFIG_AUDIO_ES8388) && defined(CONFIG_RK3576_SAI)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define AUDIO_I2C_BUS      3        /* ES8388 所在总线，已实测 */
#define AUDIO_I2C_ADDR     0x10     /* 已实测应答              */
#define AUDIO_I2C_FREQ     400000
#define AUDIO_SAI_PORT     1

/* 喇叭功放使能：原厂 dtsi es8388-sound 节点的
 *   spk-con-gpio = <&gpio2 RK_PB1 GPIO_ACTIVE_HIGH>
 * 路由是 "Speaker" <- LOUT2/ROUT2 + "Speaker Power"。
 */

#define AUDIO_SPK_EN_BANK  2
#define AUDIO_SPK_EN_PIN   9        /* B1 = 8 + 1 */

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* SAI 发送开始 / 结束时开关喇叭功放 */

static void kickpi_k7_speaker(bool on)
{
  rk3576_gpio_write(AUDIO_SPK_EN_BANK, AUDIO_SPK_EN_PIN, on);
}

static const struct es8388_lower_s g_es8388_lower =
{
  .frequency = AUDIO_I2C_FREQ,
  .address   = AUDIO_I2C_ADDR,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_audio_initialize
 *
 * Description:
 *   把 ES8388 与 SAI1 组合成音频设备并注册。
 *
 ****************************************************************************/

int kickpi_k7_audio_initialize(void)
{
  struct audio_lowerhalf_s *codec;
  struct audio_lowerhalf_s *pcm;
  struct i2c_master_s *i2c;
  struct i2s_dev_s *i2s;
  int ret;

  i2c = rk3576_i2cbus_initialize(AUDIO_I2C_BUS);
  if (i2c == NULL)
    {
      syslog(LOG_ERR, "ERROR: 音频 I2C%d 初始化失败\n", AUDIO_I2C_BUS);
      return -ENODEV;
    }

  i2s = rk3576_sai_initialize(AUDIO_SAI_PORT);
  if (i2s == NULL)
    {
      syslog(LOG_ERR, "ERROR: SAI%d 初始化失败\n", AUDIO_SAI_PORT);
      return -ENODEV;
    }

  codec = es8388_initialize(i2c, i2s, &g_es8388_lower);
  if (codec == NULL)
    {
      syslog(LOG_ERR, "ERROR: ES8388 初始化失败（I2C%d:0x%02x 无应答？）\n",
             AUDIO_I2C_BUS, AUDIO_I2C_ADDR);
      return -ENODEV;
    }

  /* ★ 喇叭功放。
   *
   *   ES8388 在放音时 OUT1（耳机）和 OUT2（喇叭）都会打开
   *   （CONFIG_ES8388_OUTPUT_CHANNEL_ALL），但 OUT2 后面还有一颗功放，
   *   它的使能脚 GPIO2_B1 之前没人拉高 —— 表现是"只有耳机有声音，接上
   *   喇叭不响"。
   *
   *   常开的话不放音时也有明显底噪（用户反馈），所以平时关着，由 SAI
   *   在一条放音流开始 / 结束时回调开关。AMP 下 Linux 没开声卡子系统，
   *   不会动这个脚。
   */

  rk3576_pinmux_set(AUDIO_SPK_EN_BANK, AUDIO_SPK_EN_PIN, 0);
  rk3576_gpio_setdir(AUDIO_SPK_EN_BANK, AUDIO_SPK_EN_PIN, true);
  rk3576_gpio_write(AUDIO_SPK_EN_BANK, AUDIO_SPK_EN_PIN, false);
  rk3576_sai_set_txhook(kickpi_k7_speaker);
  syslog(LOG_INFO, "音频: 喇叭功放 GPIO2_B1 随放音开关\n");

  /* 套一层 PCM 解码，使 /dev/audio/pcm0 能直接吃 WAV */

  pcm = pcm_decode_initialize(codec);
  if (pcm == NULL)
    {
      syslog(LOG_ERR, "ERROR: PCM 解码层初始化失败\n");
      return -ENODEV;
    }

  ret = audio_register("pcm0", pcm);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 注册 /dev/audio/pcm0 失败: %d\n", ret);
      return ret;
    }

  /* ★ 再把**未经 PCM 解码包装**的编解码器注册成 pcm1，专供录音。
   *
   *   pcm_decode 是给放音用的解码器：它在收到缓冲区时会调 pcm_parsewav()
   *   去解析 WAV 头。录音递进去的是**空缓冲区**，解析必然失败 ——
   *   实测 AUDIOIOC_ENQUEUEBUFFER 直接返回 ENOENT。
   *
   *   也就是说 pcm0 这条路天然只能放音。ES8388 本身是全双工的
   *   （getcaps 上报 INPUT|OUTPUT，原厂 Android 下同一颗芯片
   *   playback 1 : capture 1），所以把裸设备另外注册一个名字，录音走它。
   *
   *   两个设备共用同一个下半部：同一时刻只跑一个方向，这与该编解码器
   *   的实际用法一致。
   */

  ret = audio_register("pcm1", codec);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 注册 /dev/audio/pcm1（录音）失败: %d\n", ret);

      /* 不致命：放音仍可用 */
    }

  syslog(LOG_INFO,
         "音频: /dev/audio/pcm0 放音 + pcm1 录音"
         "（ES8388@I2C%d:0x%02x + SAI%d）\n",
         AUDIO_I2C_BUS, AUDIO_I2C_ADDR, AUDIO_SAI_PORT);
  return OK;
}

#endif /* CONFIG_AUDIO_ES8388 && CONFIG_RK3576_SAI */
