/****************************************************************************
 * board/kickpi-k7/src/kickpi_k7_gpio.c
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

/* 把板上引出的 GPIO 注册成 /dev/gpioN，可用 nsh 的 gpio 命令读写。
 *
 * 只注册 U-Boot 已经复用成 GPIO 的引脚 —— SoC 层的 rk3576_gpio.c 不配
 * pinmux，原因见该文件顶部说明。板子 dts 里声明为 gpio-leds / gpio-fan
 * 的引脚满足这个前提。
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <syslog.h>

#include <nuttx/ioexpander/gpio.h>
#include <arch/board/board.h>

#include "rk3576_gpio.h"
#include "rk3576_pinmux.h"
#include "hardware/rk3576_gpio.h"
#include "kickpi_k7.h"

#ifdef CONFIG_DEV_GPIO

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct kickpi_gpout_s
{
  struct gpio_dev_s   gpio;      /* 必须是第一个成员 */
  uint8_t             bank;
  uint8_t             pin;
  const char         *name;      /* 仅用于日志 */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int kickpi_gpout_read(FAR struct gpio_dev_s *dev, FAR bool *value);
static int kickpi_gpout_write(FAR struct gpio_dev_s *dev, bool value);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct gpio_operations_s g_gpout_ops =
{
  .go_read       = kickpi_gpout_read,
  .go_write      = kickpi_gpout_write,
  .go_attach     = NULL,        /* 中断暂不支持，见文件末尾 TODO */
  .go_enable     = NULL,
  .go_setpintype = NULL,
  .go_setdebounce = NULL,
  .go_setmask    = NULL,
};

/* 板上可用的输出引脚。引脚号来自 include/board.h，出处是厂商 dts。 */

/****************************************************************************
 * 输入引脚
 *
 * ★ cmocka_driver_gpio 需要一个输入设备（默认 /dev/gpio2），此前板上
 *   只注册了两个输出脚，用例的中断子项因 fd_in < 0 失败。
 *
 *   选 TP_INT_L（GPIO0_C5）作输入：它是触摸中断脚，板上有 10K 上拉到
 *   3V3（原理图 R5106），读它不会干扰任何东西；而且触摸驱动此时尚未
 *   接管中断，两者不冲突。
 */

struct kickpi_gpin_s
{
  struct gpio_dev_s gpio;
  uint8_t           bank;
  uint8_t           pin;
  uint8_t           minor;
  const char       *name;
  pin_interrupt_t   callback;

  /* setpintype 定下的触发方式，enable 时照搬。分开存是必须的：
   * 上层的调用顺序是 setpintype -> attach -> enable，如果 enable
   * 自己再配一遍，setpintype 选的类型就被丢掉了。
   */

  bool              irq_rising;
  bool              irq_level;
};

static int kickpi_gpin_read(FAR struct gpio_dev_s *dev, FAR bool *value)
{
  FAR struct kickpi_gpin_s *p = (FAR struct kickpi_gpin_s *)dev;

  if (value == NULL)
    {
      return -EINVAL;
    }

  *value = rk3576_gpio_read(p->bank, p->pin) != 0;
  return OK;
}

/****************************************************************************
 * ★ 输入脚不能只实现 go_read。
 *
 *   cmocka_driver_gpio 的中断子项会通过 GPIOC_SETPINTYPE 把引脚切成
 *   中断类型，上层在调用前用 DEBUGASSERT 检查回调非空 —— 缺哪个就直接
 *   断言失败并打印栈回溯，而不是返回错误码。所以 setpintype / attach /
 *   enable 三个必须一起给出。
 *
 *   底层中断能力本来就有（触摸用的就是它），这里只是把它接到 gpio
 *   上层的接口上。
 ****************************************************************************/

static int kickpi_gpin_attach(FAR struct gpio_dev_s *dev,
                              pin_interrupt_t callback)
{
  FAR struct kickpi_gpin_s *p = (FAR struct kickpi_gpin_s *)dev;

  p->callback = callback;
  return OK;
}

static int kickpi_gpin_isr(int irq, FAR void *context, FAR void *arg)
{
  FAR struct kickpi_gpin_s *p = (FAR struct kickpi_gpin_s *)arg;

  if (p->callback != NULL)
    {
      p->callback(&p->gpio, p->minor);
    }

  return OK;
}

static int kickpi_gpin_enable(FAR struct gpio_dev_s *dev, bool enable)
{
  FAR struct kickpi_gpin_s *p = (FAR struct kickpi_gpin_s *)dev;

  if (enable)
    {
      /* 只挂接，不重配触发方式 —— 那是 setpintype 的职责。
       * 这里再调一次 irq_config 会把上层刚选好的类型覆盖掉，
       * 而且不报错：默认恰好是下降沿时看不出来，选电平或上升沿
       * 就会静默失效。
       */

      rk3576_gpio_irq_attach(p->bank, p->pin, kickpi_gpin_isr, p);
    }

  return rk3576_gpio_irq_enable(p->bank, p->pin, enable);
}

static int kickpi_gpin_setpintype(FAR struct gpio_dev_s *dev,
                                  enum gpio_pintype_e pintype)
{
  FAR struct kickpi_gpin_s *p = (FAR struct kickpi_gpin_s *)dev;

  switch (pintype)
    {
      case GPIO_INPUT_PIN:
      case GPIO_INPUT_PIN_PULLUP:
      case GPIO_INPUT_PIN_PULLDOWN:
        rk3576_gpio_irq_enable(p->bank, p->pin, false);
        rk3576_gpio_setdir(p->bank, p->pin, false);
        break;

      case GPIO_INTERRUPT_PIN:
      case GPIO_INTERRUPT_HIGH_PIN:
      case GPIO_INTERRUPT_LOW_PIN:
      case GPIO_INTERRUPT_RISING_PIN:
      case GPIO_INTERRUPT_FALLING_PIN:
      case GPIO_INTERRUPT_BOTH_PIN:
        p->irq_rising = (pintype == GPIO_INTERRUPT_RISING_PIN ||
                         pintype == GPIO_INTERRUPT_HIGH_PIN);
        p->irq_level  = (pintype == GPIO_INTERRUPT_HIGH_PIN ||
                         pintype == GPIO_INTERRUPT_LOW_PIN);

        rk3576_gpio_setdir(p->bank, p->pin, false);
        rk3576_gpio_irq_config(p->bank, p->pin, p->irq_rising,
                               p->irq_level);
        break;

      default:
        return -EINVAL;
    }

  dev->gp_pintype = pintype;
  return OK;
}

static const struct gpio_operations_s g_gpin_ops =
{
  .go_read       = kickpi_gpin_read,
  .go_attach     = kickpi_gpin_attach,
  .go_enable     = kickpi_gpin_enable,
  .go_setpintype = kickpi_gpin_setpintype,
};

static struct kickpi_gpin_s g_gpins[] =
{
  {
    .bank = BOARD_TESTPIN_BANK,       /* GPIO4_A6，排针第 7 脚 */
    .pin  = BOARD_TESTPIN_PIN,
    .name = "testpin",
  },
};

#define KICKPI_NGPIN (sizeof(g_gpins) / sizeof(g_gpins[0]))

static struct kickpi_gpout_s g_gpouts[] =
{
  {
    .bank = BOARD_LED_WORK_BANK,      /* GPIO0_B4，dts 默认 heartbeat */
    .pin  = BOARD_LED_WORK_PIN,
    .name = "work-led",
  },
  {
    .bank = BOARD_FAN_PWR_BANK,       /* GPIO2_B3，风扇电源，默认 on */
    .pin  = BOARD_FAN_PWR_PIN,
    .name = "fan-pwr",
  },
  /* 与 testpin 配对的输出脚，供 cmocka_driver_gpio 的中断子项使用。
   *
   * ★ 放在最后一个，让它拿到 /dev/gpio2
   *
   *   那个用例会往输出脚写 0。之前把 /dev/gpio1（风扇电源）当输出脚，
   *   跑一次测试就把风扇断掉了 —— 用例并不知道它拉的是什么，
   *   所以能被它拉动的脚必须是拉了也无所谓的脚。
   */

  {
    .bank = BOARD_TESTPIN_OUT_BANK,   /* GPIO4_A4，排针第 5 脚 */
    .pin  = BOARD_TESTPIN_OUT_PIN,
    .name = "testpin-out",
  },
};

#define KICKPI_NGPOUT (sizeof(g_gpouts) / sizeof(g_gpouts[0]))

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int kickpi_gpout_read(FAR struct gpio_dev_s *dev, FAR bool *value)
{
  FAR struct kickpi_gpout_s *priv = (FAR struct kickpi_gpout_s *)dev;
  int ret;

  DEBUGASSERT(priv != NULL && value != NULL);

  /* ★ 输出脚读回给的是"我驱动成了什么"，取自输出数据寄存器。
   *
   *   原来读 EXT_PORT（引脚实际电平），想法是"两者不一致就说明被外部
   *   驱动着"。但本板 GPIO4_A4 配成输出后 EXT_PORT 恒读 0 —— 引脚其实
   *   有在驱动（同一根线上的中断子项收得到边沿），是该脚做输出时输入
   *   缓冲不工作。xTS 1.3.6 的 drivertest_gpio_rw 写 1 立刻读回，
   *   拿到 0 而失败（cmocka: 49 != 48）。
   */

  ret = rk3576_gpio_read_output(priv->bank, priv->pin);
  if (ret < 0)
    {
      return ret;
    }

  *value = (ret != 0);
  return OK;
}

static int kickpi_gpout_write(FAR struct gpio_dev_s *dev, bool value)
{
  FAR struct kickpi_gpout_s *priv = (FAR struct kickpi_gpout_s *)dev;

  DEBUGASSERT(priv != NULL);
  return rk3576_gpio_write(priv->bank, priv->pin, value);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_gpio_initialize
 *
 * Description:
 *   注册板上 GPIO 输出引脚为 /dev/gpioN。
 *
 * Returned Value:
 *   成功返回 OK。单个引脚注册失败只记录日志、继续注册其余引脚，
 *   因为一个引脚不可用不应拖垮整个板级初始化。
 *
 ****************************************************************************/

int kickpi_k7_gpio_initialize(void)
{
  uint32_t verid;
  int ret;
  int i;

  /* 自检：读 GPIO0 的 VER_ID。这一个读操作同时验证基址、PCLK 时钟、
   * MMU 映射三件事 —— 三者任一不对，总线都会返回 0 或 0xffffffff，
   * 而不会是一个像模像样的版本号。
   *
   * ★ 只排除这两个无效值，不做版本白名单匹配。
   *   最初按 Linux gpio-rockchip 里的 V2/V2_1 两个常量做精确匹配，
   *   而本板实测是 0x010219c8（第三个取值），结果把一块完全正常的
   *   控制器判成了故障。版本号会随修订递增，白名单是错的判据。
   */

  verid = rk3576_gpio_verid(0);
  if (verid == RK3576_GPIO_VER_INVALID_0 ||
      verid == RK3576_GPIO_VER_INVALID_1)
    {
      syslog(LOG_ERR,
             "ERROR: GPIO0 VER_ID = 0x%08" PRIx32 " —— 读不到控制器，"
             "基址、PCLK 或 MMU 映射有问题\n", verid);
      return -ENODEV;
    }

  syslog(LOG_INFO, "GPIO: 控制器版本 0x%08" PRIx32 "%s\n", verid,
         verid == RK3576_GPIO_VER_K7_OBSERVED ? "（与实测一致）" : "");

  for (i = 0; i < KICKPI_NGPOUT; i++)
    {
      FAR struct kickpi_gpout_s *p = &g_gpouts[i];

      p->gpio.gp_pintype = GPIO_OUTPUT_PIN;
      p->gpio.gp_ops     = &g_gpout_ops;

      /* 显式设成 GPIO 功能。前两脚 U-Boot 已经配好，第三脚（排针上的
       * testpin-out）U-Boot 不认识，不设的话方向寄存器写了也不起作用。
       */

      rk3576_pinmux_set(p->bank, p->pin, RK3576_PINMUX_GPIO);

      ret = rk3576_gpio_setdir(p->bank, p->pin, true);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: %s (gpio%u-%u) 设方向失败: %d\n",
                 p->name, p->bank, p->pin, ret);
          continue;
        }

      ret = gpio_pin_register(&p->gpio, i);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: %s 注册 /dev/gpio%d 失败: %d\n",
                 p->name, i, ret);
          continue;
        }

      syslog(LOG_INFO, "GPIO: /dev/gpio%d = %s (gpio%u-%u)\n",
             i, p->name, p->bank, p->pin);
    }

  /* 输入脚接在输出脚之后，编号顺延（当前为 /dev/gpio3）。 */

  for (i = 0; i < KICKPI_NGPIN; i++)
    {
      FAR struct kickpi_gpin_s *p = &g_gpins[i];
      int minor = KICKPI_NGPOUT + i;

      p->gpio.gp_pintype = GPIO_INPUT_PIN;
      p->gpio.gp_ops     = &g_gpin_ops;
      p->minor           = minor;

      rk3576_pinmux_set(p->bank, p->pin, RK3576_PINMUX_GPIO);

      /* ★ 必须给内部上拉。
       *
       *   这一脚在 40 针扩展口上、没有接任何器件，也就没有外部上下拉 ——
       *   悬空输入会随环境噪声抖动。测试用例把它配成双边沿中断后，
       *   抖动就变成持续的中断风暴，把 CPU 占满，现象是整块板子失去
       *   响应（既不报错也不复位），很容易误判成驱动挂死。
       *
       *   "选一个没接器件的引脚"解决了占用冲突，却引入了悬空问题 ——
       *   空闲和悬空是两回事。
       */

      rk3576_pinmux_setpull(p->bank, p->pin, RK3576_PULL_UP);

      ret = rk3576_gpio_setdir(p->bank, p->pin, false);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: %s (gpio%u-%u) 设为输入失败: %d\n",
                 p->name, p->bank, p->pin, ret);
          continue;
        }

      ret = gpio_pin_register(&p->gpio, minor);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: %s 注册 /dev/gpio%d 失败: %d\n",
                 p->name, minor, ret);
          continue;
        }

      syslog(LOG_INFO, "GPIO: /dev/gpio%d = %s (gpio%u-%u, 输入=%d)\n",
             minor, p->name, p->bank, p->pin,
             rk3576_gpio_read(p->bank, p->pin));
    }

  return OK;
}

/* TODO(M3)：GPIO 中断。需要在 SoC 层补 INT_EN/INT_TYPE/INT_POLARITY 的
 * 配置和 GIC 中断挂接，五个 bank 各占一个 SPI。届时可支持
 * GPIO_INTERRUPT_PIN。
 */

#endif /* CONFIG_DEV_GPIO */
