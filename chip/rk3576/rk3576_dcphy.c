/****************************************************************************
 * chip/rk3576/rk3576_dcphy.c
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

/* RK3576 MIPI D/C-PHY（Samsung IP）。
 *
 * ★ 出处：Linux drivers/phy/rockchip/phy-rockchip-samsung-dcphy.c
 *   基址取自主线 rk3576.dtsi 的 mipidcphy@2b020000
 *   （compatible = "rockchip,rk3576-mipi-dcphy"）。
 *
 * ★ 本文件只做 D-PHY，不做 C-PHY。
 *   这是一颗 combo PHY，两种模式共用寄存器块但配置完全不同。
 *   MIPI 显示屏用 D-PHY；C-PHY 用不到，实现了也无从验证。
 *
 * ★ PLL_LOCK 是整条显示链路上第一个真正的硬件反馈
 *
 *   VOP 与 DSI 的配置写下去只能"回读一致"，证明不了配置是对的 ——
 *   写错寄存器一样能读回。而 PLL 锁定与否由硬件决定：锁上了说明
 *   分频参数和参考时钟都对。因此这是显示调试中第一个可信的里程碑，
 *   锁不上就不必往下查 VOP/DSI。
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <syslog.h>

#include <nuttx/arch.h>

#include "arm64_internal.h"
#include "rk3576_dcphy.h"
#include "rk3576_cru.h"
#include "hardware/rk3576_memorymap.h"

#ifdef CONFIG_RK3576_DCPHY

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define DCPHY_BASE          RK3576_DCPHY_ADDR

/* PLL 寄存器。出处：phy-rockchip-samsung-dcphy.c */

#define DCPHY_PLL_CON0      0x0100
#define DCPHY_PLL_CON1      0x0104   /* DSM 小数部分 K */
#define DCPHY_PLL_CON2      0x0108
#define DCPHY_PLL_CON5      0x0114
#define DCPHY_PLL_CON7      0x011c
#define DCPHY_PLL_CON8      0x0120
#define DCPHY_PLL_STAT0     0x0140

#define PLL_EN              (1u << 12)
#define PLL_S_SHIFT         8            /* CON0 [10:8]  */
#define PLL_S_MASK          (0x7u << 8)
#define PLL_P_SHIFT         0            /* CON0 [5:0]   */
#define PLL_P_MASK          0x3fu
#define PLL_M_MASK          0x3ffu       /* CON2 [9:0]   */
#define PLL_RESET_N_SEL     (1u << 10)
#define PLL_ENABLE_SEL      (1u << 8)
#define PLL_LOCK            (1u << 0)

/* 参考时钟。RK3576 的 D-PHY 参考时钟为 24MHz 晶振。 */

#define DCPHY_REF_CLK_KHZ   24000

/* PLL 约束，出处同上：
 *   2600MHz <= Fvco <= 6600MHz
 *   6MHz    <= Fin/P <= 30MHz
 */

#define PLL_FVCO_MIN_KHZ    2600000
#define PLL_FVCO_MAX_KHZ    6600000
#define PLL_FREF_MIN_KHZ    6000
#define PLL_FREF_MAX_KHZ    30000

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t phy_getreg(uint32_t off)
{
  return getreg32(DCPHY_BASE + off);
}

static inline void phy_putreg(uint32_t off, uint32_t val)
{
  putreg32(val, DCPHY_BASE + off);
}

static inline void phy_modreg(uint32_t off, uint32_t mask, uint32_t val)
{
  phy_putreg(off, (phy_getreg(off) & ~mask) | (val & mask));
}

/****************************************************************************
 * Name: dcphy_pll_calc
 *
 * Description:
 *   为目标输出频率求分频参数。
 *
 *     Fvco = (M + K/65536) * 2 * Fin / P
 *     Fout = Fvco / 2^S
 *
 *   遍历 S 与 P，取使 Fout 最接近目标的一组。K 取 0（不用小数分频）：
 *   显示的码率允许有千分之几的偏差，整数分频足够，省掉一整块
 *   补码换算逻辑（负 DSM 要按 16 位补码写入，容易写错且难验证）。
 *
 * Returned Value:
 *   OK 并填回 p/m/s；找不到可行解返回 -ERANGE。
 *
 ****************************************************************************/

static int dcphy_pll_calc(uint32_t fout_khz, uint32_t *p, uint32_t *m,
                          uint32_t *s)
{
  uint32_t best_delta = UINT32_MAX;
  uint32_t fin = DCPHY_REF_CLK_KHZ;
  uint32_t _s;

  for (_s = 0; _s < 7; _s++)
    {
      uint64_t fvco = (uint64_t)fout_khz << _s;
      uint32_t _p;

      if (fvco < PLL_FVCO_MIN_KHZ || fvco > PLL_FVCO_MAX_KHZ)
        {
          continue;
        }

      for (_p = 1; _p <= PLL_P_MASK; _p++)
        {
          uint32_t fref = fin / _p;
          uint64_t _m;
          uint64_t got;
          uint32_t delta;

          if (fref < PLL_FREF_MIN_KHZ || fref > PLL_FREF_MAX_KHZ)
            {
              continue;
            }

          /* M = Fvco * P / (2 * Fin)，四舍五入 */

          _m = (fvco * _p + fin) / (2 * (uint64_t)fin);
          if (_m < 64 || _m > PLL_M_MASK)
            {
              continue;
            }

          got   = (_m * 2 * fin / _p) >> _s;
          delta = (got > fout_khz) ? (uint32_t)(got - fout_khz)
                                   : (uint32_t)(fout_khz - got);

          if (delta < best_delta)
            {
              best_delta = delta;
              *p = _p;
              *m = (uint32_t)_m;
              *s = _s;
            }
        }
    }

  return (best_delta == UINT32_MAX) ? -ERANGE : OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int rk3576_dcphy_probe(void)
{
  uint32_t before;
  uint32_t after;

  /* 自检：PLL_CON7 是可读可写的锁定计数器，写两个不同值验证。
   * D-PHY 没有版本寄存器，与 DSI2 同样用"写回读"判断寄存器块是否活着。
   */

  phy_putreg(DCPHY_PLL_CON7, 0x1234);
  before = phy_getreg(DCPHY_PLL_CON7) & 0xffff;
  phy_putreg(DCPHY_PLL_CON7, 0x5678);
  after = phy_getreg(DCPHY_PLL_CON7) & 0xffff;

  if (before != 0x1234 || after != 0x5678)
    {
      syslog(LOG_ERR,
             "DCPHY: 写回读失败（0x1234→0x%04" PRIx32
             " 0x5678→0x%04" PRIx32 "）—— 时钟或基址有问题\n",
             before, after);
      return -ENODEV;
    }

  syslog(LOG_INFO, "DCPHY: 寄存器可访问 PLL_STAT0=0x%08" PRIx32 "\n",
         phy_getreg(DCPHY_PLL_STAT0));
  return OK;
}

int rk3576_dcphy_enable(uint32_t bitrate_kbps, int lanes)
{
  uint32_t p = 0;
  uint32_t m = 0;
  uint32_t s = 0;
  uint32_t got;
  int ret;
  int us;

  UNUSED(lanes);

  ret = dcphy_pll_calc(bitrate_kbps, &p, &m, &s);
  if (ret < 0)
    {
      syslog(LOG_ERR, "DCPHY: %" PRIu32 "kbps 无可行分频\n", bitrate_kbps);
      return ret;
    }

  got = (m * 2 * DCPHY_REF_CLK_KHZ / p) >> s;

  /* ★ 先关掉 PLL 并确认 LOCK 位归零，再配置、使能、等锁定。
   *
   *   只检查"最终 LOCK 为 1"是不够的：本端口实测 probe 时
   *   PLL_STAT0 就已经是 0x1，若直接使能再读，读到的 1 可能是
   *   原有状态而非本次配置的结果 —— 那样即使分频算错也会报"已锁定"。
   *   先让它掉锁、再看它重新锁上，才能证明配置真的生效。
   */

  phy_modreg(DCPHY_PLL_CON0, PLL_EN, 0);

  for (us = 0; us < 1000; us++)
    {
      if ((phy_getreg(DCPHY_PLL_STAT0) & PLL_LOCK) == 0)
        {
          break;
        }

      up_udelay(1);
    }

  if ((phy_getreg(DCPHY_PLL_STAT0) & PLL_LOCK) != 0)
    {
      syslog(LOG_WARNING,
             "DCPHY: 关闭 PLL 后 LOCK 仍为 1，后续的锁定判断不可信\n");
    }

  /* 先写分频参数，最后才置 PLL_EN —— 使能后改分频会让 PLL 失锁 */

  phy_modreg(DCPHY_PLL_CON0, PLL_S_MASK | PLL_P_MASK,
             (s << PLL_S_SHIFT) | (p << PLL_P_SHIFT));
  phy_putreg(DCPHY_PLL_CON1, 0);            /* K = 0，不用小数分频 */
  phy_modreg(DCPHY_PLL_CON2, PLL_M_MASK, m);
  phy_putreg(DCPHY_PLL_CON5, PLL_RESET_N_SEL | PLL_ENABLE_SEL);
  phy_putreg(DCPHY_PLL_CON7, 0xf000);       /* 锁定计数 */
  phy_putreg(DCPHY_PLL_CON8, 0xf000);       /* 稳定计数 */

  phy_modreg(DCPHY_PLL_CON0, PLL_EN, PLL_EN);

  for (us = 0; us < 20000; us++)
    {
      if ((phy_getreg(DCPHY_PLL_STAT0) & PLL_LOCK) != 0)
        {
          syslog(LOG_INFO,
                 "DCPHY: PLL 已锁定 目标 %" PRIu32 "kbps 实际 %" PRIu32
                 "kbps (P=%" PRIu32 " M=%" PRIu32 " S=%" PRIu32 ")\n",
                 bitrate_kbps, got, p, m, s);
          return OK;
        }

      up_udelay(1);
    }

  syslog(LOG_ERR,
         "DCPHY: PLL 未锁定 STAT0=0x%08" PRIx32
         " (P=%" PRIu32 " M=%" PRIu32 " S=%" PRIu32 ")\n",
         phy_getreg(DCPHY_PLL_STAT0), p, m, s);
  return -ETIMEDOUT;
}

#endif /* CONFIG_RK3576_DCPHY */
