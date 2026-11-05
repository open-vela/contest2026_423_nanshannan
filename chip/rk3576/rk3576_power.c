/****************************************************************************
 * chip/rk3576/rk3576_power.c
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

/* RK3576 电源域控制（PMU）。
 *
 * 只做"打开"和"查询"，不做掉电 —— 本端口没有休眠需求，而掉电的握手
 * 更复杂（要先让 NIU 进 idle、等应答，还要保存/恢复 QoS），实现了也
 * 无处验证，徒增出错面。
 *
 * ★ 数据出处：Linux drivers/pmdomain/rockchip/pm-domains.c
 *
 *     static const struct rockchip_pmu_info rk3576_pmu = {
 *         .pwr_offset = 0x210,  .status_offset = 0x230,
 *         .req_offset = 0x110,  .idle_offset   = 0x128,
 *         .ack_offset = 0x120,  .repair_status_offset = 0x570,
 *         .clk_ungate_offset = 0x140,
 *     };
 *
 *     DOMAIN_RK3576(name, p_offset, pwr, status, r_status,
 *                   r_offset, req, idle, g_mask, wakeup)
 *
 *   PMU 基址取自主线 rk3576.dtsi 的 pmu: power-management@27380000。
 *
 * ★ 状态判读的两个反直觉之处，看错就会一直等下去：
 *
 *   1. repair_status：1 = 已上电，0 = 已断电（正逻辑）
 *   2. status：      0 = 已上电，1 = 已断电（反逻辑）
 *
 *   RK3576 的各域都给了 repair_status，因此本实现只用第 1 种。
 *
 * ★ 写寄存器一律带高 16 位写使能掩码，与 CRU/GPIO 同一套约定。
 *
 * ★ NPU 的四个域（2026-11-02 加入，尚未上板验证）和前面几个不一样：
 *
 *   - NPUTOP/NPU0/NPU1 的电源位在第二个字（p_offset = 0x4，即 0x214）；
 *   - PD_NPU 没有 repair_status，只能看 status（反逻辑），也没有 idle 握手；
 *   - 厂商驱动对带 r_status 的域，上电时若 mem 状态显示"开"，要做一遍
 *     chain → mem 复位（rockchip_pmu_domain_mem_reset）。已验证过的
 *     NVM/VOP/AUDIO 等一直没做这一步也正常工作，为不改动它们的行为，
 *     这一步只对 NPU 域开启（mem_reset = true）；
 *   - NPUTOP 撤销 idle 后要等 15us（delay_us）。
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <syslog.h>

#include <nuttx/arch.h>

#include "arm64_internal.h"
#include "rk3576_power.h"
#include "hardware/rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define PMU_BASE                  RK3576_PMU_ADDR

#define PMU_REQ_OFFSET            0x0110   /* idle 请求        */
#define PMU_ACK_OFFSET            0x0120   /* idle 应答        */
#define PMU_IDLE_OFFSET           0x0128   /* idle 状态        */
#define PMU_CLK_UNGATE_OFFSET     0x0140   /* 时钟门控解除     */
#define PMU_PWR_OFFSET            0x0210   /* 电源开关         */
#define PMU_STATUS_OFFSET         0x0230   /* 0=已上电         */
#define PMU_CHAIN_STATUS_OFFSET   0x0248   /* 1=电源链已通     */
#define PMU_MEM_STATUS_OFFSET     0x0250   /* 0=mem 已上电     */
#define PMU_MEM_PWR_OFFSET        0x0300
#define PMU_REPAIR_STATUS_OFFSET  0x0570   /* 1=已上电         */

#define PMU_POLL_TIMEOUT_US       10000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_pd_info_s
{
  const char *name;
  uint32_t    pwr_mask;        /* PWR 寄存器里的位            */
  uint32_t    repair_mask;     /* REPAIR_STATUS 里的位        */
  uint32_t    req_offset;      /* ★ 每域各自的 req 寄存器偏移 */
  uint32_t    req_mask;        /* idle 请求位                 */
  uint32_t    idle_mask;       /* idle 应答/状态位            */
  uint32_t    clk_ungate_mask; /* 握手期间要解除门控的时钟    */

  /* 以下是 NPU 域加入时补的字段，前面的域都取 0 / false */

  uint32_t    pwr_offset;      /* PWR 寄存器偏移：0x0 或 0x4  */
  uint32_t    status_mask;     /* 没有 repair 位的域看 STATUS */
  uint32_t    delay_us;        /* 撤销 idle 后的等待          */
  bool        mem_reset;       /* 按厂商流程做 mem 复位       */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 逐项对应 pm-domains.c 的 rk3576_pm_domains[]。
 * 只收录本端口会用到的域，用不到的留空以免误用未经验证的参数。
 */

static const struct rk3576_pd_info_s g_pd_info[] =
{
  /* 字段顺序：name, pwr, repair, req_offset, req, idle, clk_ungate,
   *           pwr_offset, status, delay_us, mem_reset
   * 对应 DOMAIN_RK3576(name, p_offset, pwr, status, r_status,
   *                    r_offset, req, idle, g_mask, delay, wakeup)
   */

  /* DOMAIN_RK3576("npu", 0x0, BIT(0), BIT(0), 0, 0x0, 0, 0, 0, 0, false)
   * 整个 VD_NPU 的总开关。电压 vdd_npu_s0 = RK806 DCDC2，要先有电。
   */

  [RK3576_PD_NPU] =
  {
    "npu",    1u << 0,  0,        0x0, 0,        0,        0,
    0x0, 1u << 0, 0, false
  },

  /* DOMAIN_RK3576("nputop", 0x4, BIT(6), 0, BIT(22), 0x0, 0x18, 0x18,
   *   0x18, 15, false)
   */

  [RK3576_PD_NPUTOP] =
  {
    "nputop", 1u << 6,  1u << 22, 0x0, 0x18,     0x18,     0x18,
    0x4, 0, 15, true
  },

  /* DOMAIN_RK3576("npu0", 0x4, BIT(7), 0, BIT(23), 0x0, BIT(1), BIT(1),
   *   0x1a, 0, false)
   */

  [RK3576_PD_NPU0] =
  {
    "npu0",   1u << 7,  1u << 23, 0x0, 1u << 1,  1u << 1,  0x1a,
    0x4, 0, 0, true
  },

  /* DOMAIN_RK3576("npu1", 0x4, BIT(8), 0, BIT(24), 0x0, BIT(2), BIT(2),
   *   0x1c, 0, false)
   */

  [RK3576_PD_NPU1] =
  {
    "npu1",   1u << 8,  1u << 24, 0x0, 1u << 2,  1u << 2,  0x1c,
    0x4, 0, 0, true
  },

  /* DOMAIN_RK3576("nvm", 0x0, BIT(6), 0, BIT(6), 0x4, BIT(2), BIT(18),
   *   BIT(2))
   */

  [RK3576_PD_NVM] =
  {
    "nvm",   1u << 6,  1u << 6,  0x4, 1u << 2,  1u << 18, 1u << 2
  },

  /* DOMAIN_RK3576("sdgmac", 0x0, BIT(7), 0, BIT(7), 0x4, BIT(1), BIT(17),
   *   0x6)
   */

  [RK3576_PD_SDGMAC] =
  {
    "sdgmac", 1u << 7, 1u << 7, 0x4, 1u << 1,  1u << 17, 0x6
  },

  /* DOMAIN_RK3576("php", 0x0, BIT(9), 0, BIT(9), 0x0, BIT(15), BIT(15),
   *   BIT(15), 0, false)
   * USB3OTG1（DRD1，板上 GL3510 hub 的上游）在这个域里。
   */

  [RK3576_PD_PHP] =
  {
    "php",   1u << 9,  1u << 9,  0x0, 1u << 15, 1u << 15, 1u << 15
  },

  /* DOMAIN_RK3576("audio", 0x0, BIT(8), 0, BIT(8), 0x4, BIT(0), BIT(16),
   *   BIT(0))
   */

  [RK3576_PD_AUDIO] =
  {
    "audio", 1u << 8,  1u << 8,  0x4, 1u << 0,  1u << 16, 1u << 0
  },

  /* DOMAIN_RK3576("vo0", 0x0, BIT(15), 0, BIT(15), 0x0, BIT(11), BIT(11),
   *   0x6800)
   */

  [RK3576_PD_VO0] =
  {
    "vo0",   1u << 15, 1u << 15, 0x0, 1u << 11, 1u << 11, 0x6800
  },

  /* DOMAIN_RK3576("vop", 0x0, BIT(11), 0, BIT(11), 0x0, 0x6000, 0x6000,
   *   0x6000)
   */

  [RK3576_PD_VOP] =
  {
    "vop",   1u << 11, 1u << 11, 0x0, 0x6000,   0x6000,   0x6000
  },
};

#define RK3576_PD_COUNT (sizeof(g_pd_info) / sizeof(g_pd_info[0]))

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t pmu_getreg(uint32_t off)
{
  return getreg32(PMU_BASE + off);
}

static inline void pmu_putreg(uint32_t off, uint32_t val)
{
  putreg32(val, PMU_BASE + off);
}

/****************************************************************************
 * Name: pmu_write_masked
 *
 * Description:
 *   带写使能掩码的位写。set 为 true 时置位，false 时清位。
 *
 ****************************************************************************/

static void pmu_write_masked(uint32_t off, uint32_t mask, bool set)
{
  pmu_putreg(off, (mask << 16) | (set ? mask : 0));
}

/****************************************************************************
 * Name: pmu_set_idle_request
 *
 * Description:
 *   向 NIU 发/撤 idle 请求，并等待应答到位。
 *
 *   上电流程里这一步是"撤销"（idle=false），必须在电源打开之后做；
 *   顺序颠倒会让总线在无电时收到事务。
 *
 ****************************************************************************/

static int pmu_set_idle_request(const struct rk3576_pd_info_s *pd, bool idle)
{
  uint32_t target;
  int us;

  if (pd->req_mask == 0)
    {
      return OK;
    }

  /* ★ req 寄存器地址 = 全局 req_offset + 本域各自的偏移。
   * 漏掉后半截时，req_offset 为 0 的域（VOP/VO0）恰好正确，
   * 而 0x4 的域（NVM/AUDIO）会写到别的域的请求位上。
   */

  pmu_write_masked(PMU_REQ_OFFSET + pd->req_offset, pd->req_mask, idle);

  target = idle ? pd->idle_mask : 0;
  for (us = 0; us < PMU_POLL_TIMEOUT_US; us++)
    {
      if ((pmu_getreg(PMU_ACK_OFFSET) & pd->idle_mask) == target)
        {
          return OK;
        }

      up_udelay(1);
    }

  syslog(LOG_ERR, "PMU: %s idle%s 应答超时 ACK=0x%08" PRIx32 "\n",
         pd->name, idle ? "请求" : "撤销", pmu_getreg(PMU_ACK_OFFSET));
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: pmu_poll
 *
 * Description:
 *   等 off 寄存器里 mask 位变成 set（true=全 1，false=全 0），上界
 *   PMU_POLL_TIMEOUT_US。mask 为 0 时改为等 rk3576_power_is_on(domain)。
 *
 ****************************************************************************/

static int pmu_poll(uint32_t off, uint32_t mask, bool set, bool on,
                    int domain)
{
  int us;

  for (us = 0; us < PMU_POLL_TIMEOUT_US; us++)
    {
      if (mask == 0 ? rk3576_power_is_on(domain) == on :
          ((pmu_getreg(off) & mask) == (set ? mask : 0)))
        {
          return OK;
        }

      up_udelay(1);
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: pmu_mem_reset
 *
 * Description:
 *   照抄厂商 rockchip_pmu_domain_mem_reset()：等电源链通（CHAIN_STATUS
 *   置位）→ 60us → 关 mem → 等 MEM_STATUS 置位（1=关）→ 开 mem → 等清零。
 *   mem 的位与 repair_status 同一个（厂商宏里 mem_status = r_status）。
 *
 ****************************************************************************/

static int pmu_mem_reset(const struct rk3576_pd_info_s *pd)
{
  const char *step;
  int ret;

  step = "电源链";
  ret = pmu_poll(PMU_CHAIN_STATUS_OFFSET, pd->repair_mask, true, true, 0);
  if (ret == OK)
    {
      up_udelay(60);
      pmu_write_masked(PMU_MEM_PWR_OFFSET + pd->pwr_offset, pd->pwr_mask,
                       true);
      step = "mem 关";
      ret = pmu_poll(PMU_MEM_STATUS_OFFSET, pd->repair_mask, true, true, 0);
    }

  if (ret == OK)
    {
      pmu_write_masked(PMU_MEM_PWR_OFFSET + pd->pwr_offset, pd->pwr_mask,
                       false);
      step = "mem 开";
      ret = pmu_poll(PMU_MEM_STATUS_OFFSET, pd->repair_mask, false, true, 0);
    }

  if (ret < 0)
    {
      syslog(LOG_ERR, "PMU: %s mem 复位卡在%s CHAIN=0x%08" PRIx32
             " MEM=0x%08" PRIx32 "\n", pd->name, step,
             pmu_getreg(PMU_CHAIN_STATUS_OFFSET),
             pmu_getreg(PMU_MEM_STATUS_OFFSET));
    }

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

bool rk3576_power_is_on(int domain)
{
  if (domain < 0 || domain >= RK3576_PD_COUNT ||
      g_pd_info[domain].pwr_mask == 0)
    {
      return false;
    }

  /* repair_status 是正逻辑：1 = 已上电。
   * 注意不要用 PMU_STATUS_OFFSET —— 那个是反逻辑（0 才是已上电），
   * 两者搞混会得到完全相反的结论。只有没有 repair 位的域（PD_NPU）
   * 才退而看 STATUS。
   */

  if (g_pd_info[domain].repair_mask == 0)
    {
      return (pmu_getreg(PMU_STATUS_OFFSET) &
              g_pd_info[domain].status_mask) == 0;
    }

  return (pmu_getreg(PMU_REPAIR_STATUS_OFFSET) &
          g_pd_info[domain].repair_mask) != 0;
}

int rk3576_power_on(int domain)
{
  const struct rk3576_pd_info_s *pd;
  bool mem_on;
  int ret;

  if (domain < 0 || domain >= RK3576_PD_COUNT ||
      g_pd_info[domain].pwr_mask == 0)
    {
      syslog(LOG_ERR, "PMU: 电源域 %d 未收录\n", domain);
      return -EINVAL;
    }

  pd = &g_pd_info[domain];

  if (rk3576_power_is_on(domain))
    {
      syslog(LOG_INFO, "PMU: %s 已上电\n", pd->name);
      return OK;
    }

  /* 1) 握手期间解除相关时钟的门控 —— NIU 要有时钟才能应答 */

  if (pd->clk_ungate_mask != 0)
    {
      pmu_write_masked(PMU_CLK_UNGATE_OFFSET + pd->req_offset,
                       pd->clk_ungate_mask, true);
    }

  /* 2) 开电。电源位是反逻辑：写 0 为上电。 */

  mem_on = pd->mem_reset &&
           (pmu_getreg(PMU_MEM_STATUS_OFFSET) & pd->repair_mask) == 0;
  pmu_write_masked(PMU_PWR_OFFSET + pd->pwr_offset, pd->pwr_mask, false);

  /* 2.5) mem 复位（只有 mem_reset 的域）：等电源链通，mem 先关再开 */

  ret = mem_on ? pmu_mem_reset(pd) : OK;

  /* 3) 等上电完成 */

  if (ret == OK)
    {
      ret = pmu_poll(PMU_REPAIR_STATUS_OFFSET, 0, 0, true, domain);
    }

  if (ret < 0)
    {
      syslog(LOG_ERR, "PMU: %s 上电超时 REPAIR=0x%08" PRIx32
             " STATUS=0x%08" PRIx32 "\n", pd->name,
             pmu_getreg(PMU_REPAIR_STATUS_OFFSET),
             pmu_getreg(PMU_STATUS_OFFSET));
    }
  else
    {
      /* 4) 上电成功后才撤销 idle 请求，让总线开始接受事务 */

      ret = pmu_set_idle_request(pd, false);
      if (ret == OK && pd->delay_us != 0)
        {
          up_udelay(pd->delay_us);
        }
    }

  /* 5) 恢复时钟门控 */

  if (pd->clk_ungate_mask != 0)
    {
      pmu_write_masked(PMU_CLK_UNGATE_OFFSET + pd->req_offset,
                       pd->clk_ungate_mask, false);
    }

  if (ret == OK)
    {
      syslog(LOG_INFO, "PMU: %s 上电完成\n", pd->name);
    }

  return ret;
}
