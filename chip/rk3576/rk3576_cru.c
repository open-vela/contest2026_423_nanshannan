/****************************************************************************
 * arch/arm64/src/rk3576/rk3576_cru.c
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

/* RK3576 CRU —— 只做本端口用得到的最小时钟控制。
 *
 * 这不是一个完整的时钟框架：没有 PLL 配置、没有父子关系、没有频率
 * 计算。只提供"打开某一路门控"和"选择某一路时钟源"两件事，够外设
 * 驱动用，也把出错面压到最小。
 *
 * ★ 寄存器写法：高 16 位是写使能掩码，低 16 位是数据，硬件只更新掩码
 *   中置 1 的位。与 GPIO v2 同一套约定，因此不需要读改写，天然原子。
 *
 * ★ 门控位的极性是反的：1 = 关断，0 = 放行。这一点在 Rockchip 的文档
 *   里叫 "clock gate"，写 1 是"门关上"。函数接口按直觉命名，内部取反，
 *   避免调用处每次都要想一遍。
 *
 * 数据出处：Linux drivers/clk/rockchip/clk-rk3576.c 与 clk.h。
 * 每个使用点都在调用处注明了对应的 GATE()/COMPOSITE_NODIV() 行，
 * 便于日后核对。
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>

#include "arm64_internal.h"
#include "rk3576_cru.h"
#include "hardware/rk3576_memorymap.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void rk3576_clk_gate(unsigned int con, unsigned int bit, bool enable)
{
  uint32_t regval;

  /* 门控位 1 = 关断，所以 enable 时数据位写 0。 */

  regval = (1u << (bit + 16)) | ((enable ? 0u : 1u) << bit);
  putreg32(regval, RK3576_CRU_ADDR + RK3576_CRU_CLKGATE_CON(con));
}

void rk3576_clk_setmux(unsigned int con, unsigned int shift,
                       unsigned int width, unsigned int val)
{
  uint32_t mask = (1u << width) - 1;
  uint32_t regval;

  regval = (mask << (shift + 16)) | ((val & mask) << shift);
  putreg32(regval, RK3576_CRU_ADDR + RK3576_CRU_CLKSEL_CON(con));
}

void rk3576_reset(unsigned int id, bool assert)
{
  /* 每个 SOFTRST_CON 寄存器装 16 个复位位，编号线性铺开。
   * 与门控寄存器一样带高 16 位写使能掩码。
   */

  unsigned int con = id / 16;
  unsigned int bit = id % 16;
  uint32_t regval;

  regval = (1u << (bit + 16)) | ((assert ? 1u : 0u) << bit);

  /* PMU1 CRU 的复位号从 524288 起编（dt-bindings 注释
   * "PMU1SOFTRST_CON00,Offset=0x20A00"），不能按主 CRU 线性换算。
   */

  if (id >= RK3576_PMU1CRU_SRST_BASE)
    {
      con = (id - RK3576_PMU1CRU_SRST_BASE) / 16;
      putreg32(regval, RK3576_CRU_ADDR + RK3576_PMUCRU_SOFTRST_CON(con));
      return;
    }

  putreg32(regval, RK3576_CRU_ADDR + RK3576_CRU_SOFTRST_CON(con));
}

unsigned int rk3576_clk_getmux(unsigned int con, unsigned int shift,
                               unsigned int width)
{
  uint32_t mask = (1u << width) - 1;
  uint32_t regval;

  regval = getreg32(RK3576_CRU_ADDR + RK3576_CRU_CLKSEL_CON(con));
  return (regval >> shift) & mask;
}

void rk3576_pmu_clk_gate(unsigned int con, unsigned int bit, bool enable)
{
  uint32_t regval;

  regval = (1u << (bit + 16)) | ((enable ? 0u : 1u) << bit);
  putreg32(regval, RK3576_CRU_ADDR + RK3576_PMUCRU_CLKGATE_CON(con));
}

unsigned int rk3576_pmu_clk_getmux(unsigned int con, unsigned int shift,
                                   unsigned int width)
{
  uint32_t mask = (1u << width) - 1;
  uint32_t regval;

  regval = getreg32(RK3576_CRU_ADDR + RK3576_PMUCRU_CLKSEL_CON(con));
  return (regval >> shift) & mask;
}
