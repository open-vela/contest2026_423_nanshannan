/****************************************************************************
 * chip/rk3576/rk3576_gmac.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_GMAC_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_GMAC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Name: rk3576_gmac_probe
 *
 * Description:
 *   打开 PD_SDGMAC 电源域与 GMAC 时钟，复位 DMA，读 IP 版本，
 *   再通过 MDIO 读 PHY 的厂商 ID。
 *
 *   ★ 判据分两层，强弱不同：
 *
 *     MAC_VERSION 只能证明"寄存器块活着"（电源域、时钟、基址对）；
 *     PHY ID 才证明"MDIO 时序正确、PHY 有供电、连线通"。
 *
 *   前者读到合理值而后者读到 0xffff，说明 SoC 侧没问题、问题在板级
 *   （PHY 复位脚、供电、或 MDIO 引脚复用）—— 这个区分能省掉大量
 *   在 MAC 配置里瞎找的时间。
 *
 * Input Parameters:
 *   port - 0 或 1
 *
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_gmac_phy_reset
 *
 * Description:
 *   释放 PHY 的硬件复位。必须在 rk3576_gmac_probe() 之前调用。
 *
 *   ★ DWMAC 的 DMA 软复位需要**所有时钟域**都已就绪，其中包括来自
 *     PHY 的接收时钟（Synopsys 手册明确要求）。PHY 被摁在复位里时
 *     不出时钟，SWR 位就永远不自清 —— 表现为"DMA 软复位超时"，
 *     而寄存器读写一切正常，极易误判成 SoC 侧的时钟或复位问题。
 *
 *   复位脚在板级，因此由板级代码调用本接口传入。
 *
 * Input Parameters:
 *   bank, pin  - 复位脚
 *   active_low - true 表示低电平有效
 *
 ****************************************************************************/

void rk3576_gmac_refclk25m(int port);
void rk3576_gmac_set_speed(int port, int speed);

/****************************************************************************
 * Name: maxio_mae0621a_init
 *
 * Description:
 *   Maxio MAE0621A 的厂商初始化。
 *
 *   ★ 每次 PHY 软复位之后都要重新调用 —— 复位会把这套配置（含
 *     "时钟模式=晶振"）清掉，PHY 的模拟前端随即失效，而 MDIO 仍然
 *     可读可写，看不出异常。Linux 里 phylib 也是在每次复位后重新
 *     调用 config_init 的，同一个道理。
 *
 ****************************************************************************/

void maxio_mae0621a_init(int port);
void rk3576_gmac_phy_reset(int bank, int pin, bool active_low);

int rk3576_gmac_probe(int port);

/* 链路状态：bit0 链路、bit1 自协商完成、bit2.. 对端能力；负数为错误 */

int rk3576_gmac_linkinfo(int port);

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_GMAC_H */
