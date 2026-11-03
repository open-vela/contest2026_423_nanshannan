/****************************************************************************
 * arch/arm64/src/rk3576/rk3576_cru.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_CRU_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_CRU_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* CRU 寄存器窗口。偏移取自 Linux drivers/clk/rockchip/clk.h：
 *
 *   #define RK3576_CLKSEL_CON(x)   ((x) * 0x4 + 0x300)
 *   #define RK3576_CLKGATE_CON(x)  ((x) * 0x4 + 0x800)
 *   #define RK3576_SOFTRST_CON(x)  ((x) * 0x4 + 0xa00)
 */

#define RK3576_CRU_CLKSEL_CON(x)   ((x) * 4 + 0x300)
#define RK3576_CRU_CLKGATE_CON(x)  ((x) * 4 + 0x800)
#define RK3576_CRU_SOFTRST_CON(x)  ((x) * 4 + 0xa00)

/* PMU 域（常开域）另有一套 CRU，寄存器排布与主 CRU 相同，只是整体
 * 偏移 0x20000。I2C0、UART1 这类常开外设的门控在这里，用主 CRU 的
 * 偏移去开会写到别的寄存器上，静默无效。
 *
 *   drivers/clk/rockchip/clk.h:
 *     #define RK3576_PMU_CRU_BASE       0x20000
 *     #define RK3576_PMU_CLKSEL_CON(x)  ((x)*0x4 + RK3576_PMU_CRU_BASE + 0x300)
 *     #define RK3576_PMU_CLKGATE_CON(x) ((x)*0x4 + RK3576_PMU_CRU_BASE + 0x800)
 */

#define RK3576_PMUCRU_OFFSET         0x20000
#define RK3576_PMUCRU_CLKSEL_CON(x)  ((x) * 4 + RK3576_PMUCRU_OFFSET + 0x300)
#define RK3576_PMUCRU_CLKGATE_CON(x) ((x) * 4 + RK3576_PMUCRU_OFFSET + 0x800)
#define RK3576_PMUCRU_SOFTRST_CON(x) ((x) * 4 + RK3576_PMUCRU_OFFSET + 0xa00)
#define RK3576_PMU1CRU_SRST_BASE     524288   /* SRST_P_HDPTX_GRF */

/* CLK_I2Cn 的时钟源选择（mux_200m_100m_50m_24m_p） */

#define RK3576_CLKSEL_I2C_200M     0
#define RK3576_CLKSEL_I2C_100M     1
#define RK3576_CLKSEL_I2C_50M      2
#define RK3576_CLKSEL_I2C_24M      3

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#undef EXTERN
#if defined(__cplusplus)
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Name: rk3576_clk_gate
 *
 * Description:
 *   开关一路时钟门控。
 *
 *   ★ Rockchip 的门控位是"反"的：1 = 关断，0 = 放行。本函数的 enable
 *     参数按直觉理解（true 表示让时钟跑起来），内部做取反。
 *
 * Input Parameters:
 *   con    - CLKGATE_CON 的序号（Linux 里 RK3576_CLKGATE_CON(n) 的 n）
 *   bit    - 该寄存器内的位号 0-15
 *   enable - true 放行，false 关断
 *
 ****************************************************************************/

void rk3576_clk_gate(unsigned int con, unsigned int bit, bool enable);

/****************************************************************************
 * Name: rk3576_clk_setmux
 *
 * Description:
 *   设置时钟源选择位域。
 *
 * Input Parameters:
 *   con   - CLKSEL_CON 的序号
 *   shift - 位域起始位
 *   width - 位域宽度
 *   val   - 要写入的值
 *
 ****************************************************************************/

void rk3576_clk_setmux(unsigned int con, unsigned int shift,
                       unsigned int width, unsigned int val);

/****************************************************************************
 * Name: rk3576_clk_getmux
 *
 * Description:
 *   读回时钟源选择位域。用于确认实际频率而不是靠假设。
 *
 ****************************************************************************/

unsigned int rk3576_clk_getmux(unsigned int con, unsigned int shift,
                               unsigned int width);

/****************************************************************************
 * Name: rk3576_pmu_clk_gate / rk3576_pmu_clk_getmux
 *
 * Description:
 *   同名函数的 PMU 域版本，con 取 clk-rk3576.c 里
 *   RK3576_PMU_CLKGATE_CON(n) / RK3576_PMU_CLKSEL_CON(n) 的 n。
 *
 ****************************************************************************/

void rk3576_pmu_clk_gate(unsigned int con, unsigned int bit, bool enable);

unsigned int rk3576_pmu_clk_getmux(unsigned int con, unsigned int shift,
                                   unsigned int width);

/****************************************************************************
 * Name: rk3576_reset
 *
 * Description:
 *   置位/撤销一个模块的软复位。
 *
 *   ★ 复位号是跨寄存器的线性编号，映射规则为每个 SOFTRST_CON 装 16 个：
 *       寄存器 = SOFTRST_CON 基址 + (id / 16) * 4
 *       位     = id % 16
 *     编号取自 include/dt-bindings/reset/rockchip,rk3576-cru.h。
 *     524288 起是 PMU1 CRU 的复位（基址 +0x20a00），减去基数后同样换算。
 *
 *   ★ 引导器没用过的模块，其复位很可能仍是置位状态。此时寄存器读写
 *     一切正常、版本号也读得到，但状态机不工作 —— 本端口的 GMAC
 *     就是这样：四路时钟都开了，DMA 软复位的自清位仍然不归零。
 *     与"功能时钟没开"表现相同，需要分别排查。
 *
 * Input Parameters:
 *   id     - dt-bindings 里的 SRST_* 值
 *   assert - true 置位复位，false 撤销
 *
 ****************************************************************************/

void rk3576_reset(unsigned int id, bool assert);

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_CRU_H */
