/****************************************************************************
 * arch/arm64/src/rk3576/rk3576_pinmux.c
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

/* RK3576 引脚复用（IOC）。
 *
 * 只做复用功能选择，不含上下拉、驱动强度、施密特触发 —— 那几项各有
 * 一套独立的寄存器和计算规则，目前的外设用不到，加进来只会扩大出错面。
 *
 * ★ 寄存器布局（出处：Linux drivers/pinctrl/pinctrl-rockchip.c 的
 *   rk3576_pin_banks[]，以及 rk3576.dtsi 里 pinctrl 的 rockchip,grf
 *   指向 ioc_grf: syscon@26040000）：
 *
 *     RK3576_PIN_BANK(0, "gpio0", 0,      0x8,    0x2004, 0x200C)
 *     RK3576_PIN_BANK(1, "gpio1", 0x4020, 0x4028, 0x4030, 0x4038)
 *     RK3576_PIN_BANK(2, "gpio2", 0x4040, 0x4048, 0x4050, 0x4058)
 *     RK3576_PIN_BANK(3, "gpio3", 0x4060, 0x4068, 0x4070, 0x4078)
 *     RK3576_PIN_BANK(4, "gpio4", 0x4080, 0x4088, 0xA390, 0xB398)
 *
 *   每 bank 四个偏移，依次管 pin 0-7、8-15、16-23、24-31。
 *
 * ★ 每个引脚占 4 位（IOMUX_WIDTH_4BIT），一个 32 位寄存器的低 16 位
 *   只装得下 4 个引脚（高 16 位是写使能掩码），所以每 8 个引脚要用
 *   两个相邻寄存器：组内后 4 个引脚落在 offset + 4。
 *
 *   注意 bank0 的第一个偏移是 0，看起来像"没有偏移"，实际就是
 *   IOC 基址本身 —— 不是缺省值，照抄即可。
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <errno.h>
#include <stdint.h>

#include "arm64_internal.h"
#include "rk3576_pinmux.h"
#include "hardware/rk3576_memorymap.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

#define RK3576_PINMUX_NBANKS  5
#define RK3576_PINMUX_NPINS   32

static const uint32_t g_iomux_offset[RK3576_PINMUX_NBANKS][4] =
{
  { 0x0000, 0x0008, 0x2004, 0x200c },   /* gpio0 */
  { 0x4020, 0x4028, 0x4030, 0x4038 },   /* gpio1 */
  { 0x4040, 0x4048, 0x4050, 0x4058 },   /* gpio2 */
  { 0x4060, 0x4068, 0x4070, 0x4078 },   /* gpio3 */
  { 0x4080, 0x4088, 0xa390, 0xb398 },   /* gpio4 */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_pinmux_reg
 *
 * Description:
 *   算出某引脚的 iomux 寄存器地址与位偏移。
 *
 ****************************************************************************/

static uintptr_t rk3576_pinmux_reg(int bank, int pin, int *shift)
{
  uint32_t off = g_iomux_offset[bank][pin / 8];

  /* 组内后 4 个引脚在相邻的下一个寄存器 */

  if ((pin % 8) >= 4)
    {
      off += 4;
    }

  /* ★ RK3576 专有特例：bank0 的 PB4-PB7（pin 12-15）另有一段偏移。
   *
   *   出处：Linux pinctrl-rockchip.c 的 rockchip_set_mux()
   *
   *       if (ctrl->type == RK3576) {
   *         if ((bank->bank_num == 0) && (pin >= RK_PB4) && (pin <= RK_PB7))
   *           reg += 0x1ff4;  // GPIO0_IOC_GPIO0B_IOMUX_SEL_H
   *       }
   *
   *   漏掉它的表现很隐蔽：写入不报错，但落在了别的寄存器上，复用不生效。
   *   本端口就是靠 rk3576_pinmux_get() 的读回值发现的 —— I2C2 的 SDA
   *   (pin 16，不在区间内) 读回正确而 SCL (pin 15) 读回 0，两者一对比
   *   立刻定位。配完复用一定要读回核对，不要假设写进去了。
   */

  if (bank == 0 && pin >= 12 && pin <= 15)
    {
      off += 0x1ff4;
    }

  *shift = (pin % 4) * 4;
  return RK3576_IOC_GRF_ADDR + off;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int rk3576_pinmux_set(int bank, int pin, unsigned int func)
{
  uintptr_t addr;
  int shift;

  if (bank < 0 || bank >= RK3576_PINMUX_NBANKS ||
      pin  < 0 || pin  >= RK3576_PINMUX_NPINS  || func > 0xf)
    {
      return -EINVAL;
    }

  addr = rk3576_pinmux_reg(bank, pin, &shift);

  /* 写使能掩码在高 16 位，因此单次写即原子，无需读改写。 */

  putreg32((0xfu << (shift + 16)) | ((func & 0xf) << shift), addr);
  return OK;
}

int rk3576_pinmux_get(int bank, int pin)
{
  uintptr_t addr;
  int shift;

  if (bank < 0 || bank >= RK3576_PINMUX_NBANKS ||
      pin  < 0 || pin  >= RK3576_PINMUX_NPINS)
    {
      return -EINVAL;
    }

  addr = rk3576_pinmux_reg(bank, pin, &shift);
  return (getreg32(addr) >> shift) & 0xf;
}

/****************************************************************************
 * Name: rk3576_pinmux_setpull
 *
 * Description:
 *   配置引脚的内部上下拉。
 *
 *   为什么需要它：判断"外部上拉是否存在"这类板级问题时，SoC 内部上拉
 *   会把外部的变化整个盖住 —— 引脚无论如何都读到 1，观测失去区分力。
 *   要让这类判据成立，必须先把内部拉关掉。
 *
 *   寄存器排布出处 drivers/pinctrl/pinctrl-rockchip.c
 *   rk3576_calc_pull_reg_and_bit()：每引脚 2 位，每寄存器 8 个引脚，
 *   bank0 以 pin12 为界分成两段（与 iomux 的分段是两码事，偏移也不同）。
 *   RK3576_PIN_BANK selects PULL_TYPE_IO_1 for every bank:
 *   0/2 = disabled, 1 = pull-down, 3 = pull-up. No bus hold.
 *
 ****************************************************************************/

/****************************************************************************
 * Name: pull_reg
 *
 * Description:
 *   算出某引脚的上下拉寄存器偏移与位移。set 与 get 共用同一份计算 ——
 *   分成两份写迟早会漂移，那种错读回来是"一致"的，最难发现。
 *
 ****************************************************************************/

static int pull_reg(int bank, int pin, uint32_t *off, int *shift)
{
  if (bank < 0 || bank >= RK3576_PINMUX_NBANKS ||
      pin  < 0 || pin  >= RK3576_PINMUX_NPINS)
    {
      return -EINVAL;
    }

  switch (bank)
    {
      case 0:
        *off = (pin < 12) ? 0x20 : (0x2028 - 0x4);
        break;

      case 1:
        *off = 0x6110;
        break;

      case 2:
        *off = 0x6120;
        break;

      case 3:
        *off = 0x6130;
        break;

      default:  /* bank 4 也分三段 */
        *off = (pin < 16) ? 0x6140 :
               (pin < 24) ? (0xa148 - 0x8) : (0xb14c - 0xc);
        break;
    }

  *off  += (pin / 8) * 4;
  *shift = (pin % 8) * 2;
  return OK;
}

int rk3576_pinmux_setpull(int bank, int pin, unsigned int pull)
{
  uint32_t off;
  int shift;
  int ret;

  if (pull != RK3576_PULL_NONE && pull != RK3576_PULL_UP &&
      pull != RK3576_PULL_DOWN)
    {
      return -EINVAL;
    }

  ret = pull_reg(bank, pin, &off, &shift);
  if (ret < 0)
    {
      return ret;
    }

  /* 高 16 位写使能掩码，单次写即原子。 */

  putreg32((0x3u << (shift + 16)) | ((pull & 0x3) << shift),
           RK3576_IOC_GRF_ADDR + off);
  return OK;
}

/****************************************************************************
 * Name: rk3576_pinmux_getpull
 *
 * Description:
 *   读回上下拉设置。写进去不等于生效 —— 偏移算错时写入不报错、
 *   读回来却是别处的值，只有把设与读配对才能证明这一环成立。
 *
 * Returned Value:
 *   Raw encoding: 0/2 disabled, 1 pull-down, 3 pull-up.
 *   Invalid bank or pin arguments return -EINVAL.
 *
 ****************************************************************************/

int rk3576_pinmux_getpull(int bank, int pin)
{
  uint32_t off;
  int shift;
  int ret;

  ret = pull_reg(bank, pin, &off, &shift);
  if (ret < 0)
    {
      return ret;
    }

  return (getreg32(RK3576_IOC_GRF_ADDR + off) >> shift) & 0x3;
}
