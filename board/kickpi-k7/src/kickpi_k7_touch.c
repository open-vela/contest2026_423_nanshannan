/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_touch.c
 *
 * 5 寸 MIPI 屏（F050008M01）转接板上的电容触摸。
 *
 * ★ 芯片是 FocalTech FT8756 @ I2C0:0x38，不是 Goodix GT9xx。
 *
 *   出处是原厂 Android 的启动日志：
 *     input: fts_ts as /devices/platform/27300000.i2c/i2c-0/0-0038/input/input1
 *     [FTS_TS/I]fts_get_chip_types:verify id:0x8756
 *     [FTS_TS/I]fts_parse_dt:max touch number:10, irq gpio:21, reset gpio:24
 *
 *   之前按 GT9xx 扫 0x5d / 0x14 一直无应答，原因就是型号猜错了。
 *   总线（0x27300000 = I2C0）和两个引脚（IRQ=GPIO0_C5=21、
 *   RST=GPIO0_D0=24）此前从原理图推出的结论则与日志完全一致。
 *
 *   FT8756 的多点触摸寄存器布局与 FT5x06 系列兼容，直接用 NuttX 自带的
 *   ft5x06 驱动。
 *
 * ★ 触摸的复位脚（TP_RST）与面板的复位脚（LCD_RESET）是两根不同的线，
 *   动 TP_RST 不会影响 U-Boot 已经初始化好的面板。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <inttypes.h>
#include <stdint.h>
#include <debug.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/i2c/i2c_master.h>
#include <nuttx/input/ft5x06.h>

#include "arm64_internal.h"
#include "rk3576_gpio.h"
#include "rk3576_pinmux.h"
#include "rk3576_i2c.h"
#include <sched.h>
#include <unistd.h>
#include <arch/board/board.h>
#include "kickpi_k7.h"

#ifdef CONFIG_INPUT_FT5X06

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TOUCH_I2C_BUS      BOARD_TP_I2C_BUS
#define TOUCH_I2C_ADDR     0x38          /* 原厂日志的 0-0038 */

#define TOUCH_IRQ_BANK     BOARD_TP_INT_BANK
#define TOUCH_IRQ_PIN      BOARD_TP_INT_PIN
#define TOUCH_RST_BANK     BOARD_TP_RST_BANK
#define TOUCH_RST_PIN      BOARD_TP_RST_PIN

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifndef CONFIG_FT5X06_POLLMODE
static int kickpi_touch_attach(const struct ft5x06_config_s *config,
                               xcpt_t isr, void *arg)
{
  UNUSED(config);
  return rk3576_gpio_irq_attach(TOUCH_IRQ_BANK, TOUCH_IRQ_PIN, isr, arg);
}

static void kickpi_touch_enable(const struct ft5x06_config_s *config,
                                bool enable)
{
  UNUSED(config);
  rk3576_gpio_irq_enable(TOUCH_IRQ_BANK, TOUCH_IRQ_PIN, enable);
}

static void kickpi_touch_clear(const struct ft5x06_config_s *config)
{
  UNUSED(config);

  /* 本端口的 GPIO bank ISR 在派发给回调之后才写 EOI 清除挂起位
   * （见 rk3576_gpio.c 里对电平触发的说明），因此这里无需再清一次。
   */
}
#endif

static void kickpi_touch_wakeup(const struct ft5x06_config_s *config)
{
  UNUSED(config);

  /* 本板没有单独的 WAKE 脚，靠复位脚唤醒。 */
}

static void kickpi_touch_nreset(const struct ft5x06_config_s *config,
                                bool state)
{
  UNUSED(config);

  /* 低有效：state=false 进复位，state=true 出复位。 */

  rk3576_gpio_write(TOUCH_RST_BANK, TOUCH_RST_PIN, state);
}

/****************************************************************************
 * 触摸中断脚的常驻采样
 *
 * ★ 为什么要常驻，而不是"跑一条命令采样 N 秒"
 *
 *   第一版诊断是 `k7diag tp 45`：在板上采样 45 秒，同时请人在这 45 秒里
 *   去按屏幕。这个设计把**两边对时**变成了实验的前提 —— 实测有一次人
 *   没按，采出来全是 0，那一轮就白跑了，而且从数据上看不出"是没按"
 *   还是"真的没中断"。
 *
 *   观测手段不该给使用者加同步负担。改成开机就起一个低优先级线程一直
 *   采，任何时候按、任何时候读，两件事互不相干。
 *
 *   代价：200Hz 的读寄存器 + 睡眠，实测对启动和交互没有可见影响。
 ****************************************************************************/

static volatile uint32_t g_tp_samples;
static volatile uint32_t g_tp_lows;      /* 引脚为低的次数           */
static volatile uint32_t g_tp_lowraw;    /* 低且 RAWSTATUS 也置位    */
static volatile uint32_t g_tp_edges;     /* 电平跳变次数             */
static volatile uint32_t g_tp_heals;     /* 被别人屏蔽后放开的次数   */

void kickpi_touch_stat(uint32_t *samples, uint32_t *lows,
                       uint32_t *lowraw, uint32_t *edges)
{
  if (samples != NULL) *samples = g_tp_samples;
  if (lows    != NULL) *lows    = g_tp_lows;
  if (lowraw  != NULL) *lowraw  = g_tp_lowraw;
  if (edges   != NULL) *edges   = g_tp_edges;
}

uint32_t kickpi_touch_heals(void)
{
  return g_tp_heals;
}

/* ★ Linux 会把触摸中断屏蔽掉，这里发现了就放开。
 *
 *   GPIO0 的中断路由给了 openvela（amp-irqs 里的 185），但 GPIO0 控制器
 *   本身 Linux 也认：它的 gpio-rockchip 是编进内核的，probe 时对每个
 *   bank 都写一遍 int_mask = int_en = 0xffffffff（gpio-rockchip.c
 *   rockchip_interrupts_register）。触摸脚就此被屏蔽，FT8756 拉低 INT 也
 *   进不了中断；而 ft5x06 只在**中断之后**的 worker 里才会重新放开 ——
 *   没有中断就没有 worker，触摸从此永久失灵，系统其它部分一切正常。
 *
 *   实测（2026-11-01）：ISR 停在 176 次不再涨，同一段时间引脚被按低了
 *   上千次、RAW 状态也置位了；INT_EN_H 读回 0xffff（bank0 高半全使能），
 *   正是 Linux 那段初始化留下的样子。Linux 起得比界面晚时才会撞上，所以
 *   时好时坏。
 *
 *   ft5x06 自己也会屏蔽：从中断到 worker 读完数据，只有一次 I2C 的时间
 *   （1~2ms）。所以"连续 100ms 都屏蔽着"只可能是别人干的，这时按正常
 *   路径放开（先清挂起再解屏蔽）；引脚若正压着低，电平触发会立刻补进
 *   中断。不等引脚变低再放：那样 Linux 起来后的第一下触摸总要白等
 *   100ms。没人打开 /dev/input0 时 ft5x06 也会屏蔽（shutdown），这里
 *   照样放开 —— 代价只是有人碰屏时 worker 多读一次、没人取，无害。
 *
 *   根治是让 Linux 不碰 GPIO0 的中断寄存器；在那之前靠这里兜底，Linux
 *   哪一刻、来几次都能恢复。
 */

#define TP_HEAL_SAMPLES  20      /* 20 x 5ms = 100ms */

static int kickpi_touch_monitor(int argc, char *argv[])
{
  int prev = 1;
  int stuck = 0;

  UNUSED(argc);
  UNUSED(argv);

  for (; ; )
    {
      uint32_t raw;
      int cur;

      cur = rk3576_gpio_read(TOUCH_IRQ_BANK, TOUCH_IRQ_PIN) ? 1 : 0;
      raw = rk3576_gpio_rawstatus(TOUCH_IRQ_BANK);

      g_tp_samples++;

      if (cur == 0)
        {
          g_tp_lows++;

          if ((raw >> TOUCH_IRQ_PIN) & 1)
            {
              g_tp_lowraw++;
            }
        }

      if (cur != prev)
        {
          g_tp_edges++;
        }

      prev = cur;

#ifndef CONFIG_FT5X06_POLLMODE
      if (rk3576_gpio_irq_masked(TOUCH_IRQ_BANK, TOUCH_IRQ_PIN))
        {
          if (++stuck >= TP_HEAL_SAMPLES)
            {
              rk3576_gpio_irq_enable(TOUCH_IRQ_BANK, TOUCH_IRQ_PIN, true);
              stuck = 0;

              /* 只记前几次：Linux 每次启动屏蔽一次，再多就是别的问题 */

              if (++g_tp_heals <= 4)
                {
                  syslog(LOG_WARNING, "触摸: 中断被外部屏蔽（已持续 "
                         "100ms），已重新放开，第 %" PRIu32 " 次\n",
                         g_tp_heals);
                }
            }
        }
      else
        {
          stuck = 0;
        }
#endif

      usleep(5000);
    }

  return 0;
}

static const struct ft5x06_config_s g_touch_config =
{
  .address   = TOUCH_I2C_ADDR,
  .frequency = 400000,
#ifndef CONFIG_FT5X06_POLLMODE
  .attach    = kickpi_touch_attach,
  .enable    = kickpi_touch_enable,
  .clear     = kickpi_touch_clear,
#endif
  .wakeup    = kickpi_touch_wakeup,
  .nreset    = kickpi_touch_nreset,
};

/****************************************************************************
 * Name: kickpi_touch_probe
 *
 * Description:
 *   读 FT8756 的芯片 ID 确认器件在总线上应答。
 *
 *   ★ 注册函数不访问器件，注册成功不等于芯片存在 —— 这个坑在 GT9xx
 *     那版踩过一次。这里读 0xA3（chip id 高字节）与 0x9F（低字节），
 *     原厂日志报的是 0x8756。
 *
 ****************************************************************************/

static int kickpi_touch_probe(struct i2c_master_s *i2c)
{
  struct i2c_msg_s msg[2];
  uint8_t reg;
  uint8_t val;
  uint8_t idh;
  uint8_t idl;
  int ret;
  int i;

  static const uint8_t regs[2] =
  {
    0xa3,    /* chip id 高字节 */
    0x9f     /* chip id 低字节 */
  };

  uint8_t got[2] =
  {
    0, 0
  };

  for (i = 0; i < 2; i++)
    {
      reg = regs[i];

      msg[0].frequency = 400000;
      msg[0].addr      = TOUCH_I2C_ADDR;
      msg[0].flags     = 0;
      msg[0].buffer    = &reg;
      msg[0].length    = 1;

      msg[1].frequency = 400000;
      msg[1].addr      = TOUCH_I2C_ADDR;
      msg[1].flags     = I2C_M_READ;
      msg[1].buffer    = &val;
      msg[1].length    = 1;

      ret = I2C_TRANSFER(i2c, msg, 2);
      if (ret < 0)
        {
          return ret;
        }

      got[i] = val;
    }

  idh = got[0];
  idl = got[1];

  syslog(LOG_INFO,
         "触摸: FT 系列应答 芯片ID=0x%02x%02x（原厂日志为 0x8756）\n",
         idh, idl);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int kickpi_k7_touch_initialize(void)
{
  struct i2c_master_s *i2c;
  int ret;

  i2c = rk3576_i2cbus_initialize(TOUCH_I2C_BUS);
  if (i2c == NULL)
    {
      syslog(LOG_ERR, "ERROR: 触摸所在 I2C%d 初始化失败\n", TOUCH_I2C_BUS);
      return -ENODEV;
    }

  /* 复位时序。TP_RST 与面板复位无关，可以放心操作。 */

  rk3576_pinmux_set(TOUCH_RST_BANK, TOUCH_RST_PIN, RK3576_PINMUX_GPIO);
  rk3576_gpio_setdir(TOUCH_RST_BANK, TOUCH_RST_PIN, true);
  rk3576_gpio_write(TOUCH_RST_BANK, TOUCH_RST_PIN, false);
  up_mdelay(10);
  rk3576_gpio_write(TOUCH_RST_BANK, TOUCH_RST_PIN, true);
  up_mdelay(200);              /* FT8756 上电到可通信约需 200ms */

  ret = kickpi_touch_probe(i2c);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: 触摸 0x%02x 无应答(%d)，不注册输入设备\n",
             TOUCH_I2C_ADDR, ret);
      return -ENODEV;
    }

#ifndef CONFIG_FT5X06_POLLMODE
  rk3576_pinmux_set(TOUCH_IRQ_BANK, TOUCH_IRQ_PIN, RK3576_PINMUX_GPIO);
  rk3576_gpio_setdir(TOUCH_IRQ_BANK, TOUCH_IRQ_PIN, false);

  /* ★ 内部上拉必须打开。
   *
   *   FT8756 的 INT 是开漏输出：有数据时把线拉低，没数据时**放开**。
   *   放开之后线靠上拉回到高电平 —— 没有上拉，空闲电平就是不确定的，
   *   多半一直停在低位，于是"从高到低"这件事永远不会发生，一次中断
   *   也收不到。现象就是 `cat /dev/input0` 一个字节都没有，而 I2C 读
   *   芯片 ID 完全正常（那条路不经过 INT 脚）。
   *
   *   厂商 dtsi 明确配了内部上拉，说明板上没有外部上拉：
   *     touch1_gpio: touch-gpio {
   *       rockchip,pins = <0 RK_PC5 RK_FUNC_GPIO &pcfg_pull_up>,
   *                       <0 RK_PD0 RK_FUNC_GPIO &pcfg_pull_none>;
   *     };
   *   出处 kernel-6.1 的 rk3576-kickpi-lcd-mipi-5-720-1280-F050008M01.dtsi。
   */

  rk3576_pinmux_setpull(TOUCH_IRQ_BANK, TOUCH_IRQ_PIN, RK3576_PULL_UP);

  /* ★ 电平触发（低有效），不是边沿。
   *
   *   同一份 dtsi 写的是
   *     focaltech,irq-gpio = <&gpio0 RK_PC5 IRQ_TYPE_LEVEL_LOW>;
   *
   *   这不只是"换个触发方式"：FT 系列在**主机把触摸数据读走之前**一直
   *   把 INT 压着低。用边沿触发时，若有任何一次沿被漏掉（上电时线本来
   *   就是低、或 ISR 里清挂起的时序稍有出入），之后线一直低、再也没有
   *   新的下降沿，中断就永久停了。电平触发没有这个失效模式：只要还压着
   *   低，就会继续进中断，直到数据被读走线被放开。
   */

  ret = rk3576_gpio_irq_config(TOUCH_IRQ_BANK, TOUCH_IRQ_PIN,
                               false, true);   /* 电平触发、低有效 */
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 触摸中断脚配置失败: %d\n", ret);
      return ret;
    }
#endif

  ret = ft5x06_register(i2c, &g_touch_config, 0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 注册 /dev/input0 失败: %d\n", ret);
      return ret;
    }

  /* 常驻采样线程 —— 见上面的说明，目的是把"对时"从实验里去掉。 */

  {
    int pid = task_create("tpmon", 50, 2048, kickpi_touch_monitor, NULL);

    if (pid < 0)
      {
        syslog(LOG_ERR, "ERROR: 触摸采样线程启动失败: %d\n", pid);
      }
  }

  syslog(LOG_INFO,
         "触摸: /dev/input0 就绪（FT8756 @I2C%d:0x%02x, %s）\n",
         TOUCH_I2C_BUS, TOUCH_I2C_ADDR,
#ifdef CONFIG_FT5X06_POLLMODE
         "轮询模式"
#else
         "中断模式"
#endif
        );
  return OK;
}

#endif /* CONFIG_INPUT_FT5X06 */
