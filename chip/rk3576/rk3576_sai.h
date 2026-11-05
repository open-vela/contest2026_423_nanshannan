/****************************************************************************
 * chip/rk3576/rk3576_sai.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_SAI_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_SAI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>

struct i2s_dev_s;

/****************************************************************************
 * Name: rk3576_sai_probe
 *
 * Description:
 *   打开 SAI1 所需的电源域与时钟、配置引脚复用，然后读版本寄存器自检。
 *
 *   与完整的 i2s_dev_s 实现分开，是为了先把"电源域 + 时钟 + 复用"这条
 *   前置链路验证掉 —— 它们出错时的表现都是寄存器读全 0，与驱动逻辑
 *   的错误无法区分。先确认能读到像样的版本号，后面写传输逻辑才有
 *   可靠的地基。GPIO 的 VER_ID、eMMC 的 CAP0 用的是同一套思路。
 *
 * Returned Value:
 *   OK；失败返回负的 errno。
 *
 ****************************************************************************/

int rk3576_sai_probe(void);

/****************************************************************************
 * Name: rk3576_sai_initialize
 *
 * Description:
 *   返回 i2s_dev_s 句柄，供 drivers/audio/es8388.c 使用。
 *   调用前须先 rk3576_sai_probe() 成功。
 *
 * Input Parameters:
 *   port - SAI 实例号。板上 ES8388 接的是 SAI1，只支持 1。
 *
 ****************************************************************************/

struct i2s_dev_s *rk3576_sai_initialize(int port);

/****************************************************************************
 * Name: rk3576_sai_set_txhook
 *
 * Description:
 *   注册"发送开始 / 结束"回调：一条放音流开始发送前调 hook(true)，
 *   最后一个缓冲排空、TX 停下后调 hook(false)。板级用它开关喇叭功放 ——
 *   功放常开的话不放音时也有明显底噪。hook 在音频工作线程里调，
 *   里面只做 GPIO 这类不阻塞的事。
 *
 ****************************************************************************/

typedef void (*rk3576_sai_txhook_t)(bool on);

void rk3576_sai_set_txhook(rk3576_sai_txhook_t hook);

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_SAI_H */
