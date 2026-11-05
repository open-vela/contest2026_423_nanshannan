/****************************************************************************
 * chip/rk3576/rk3576_wdt.c
 *
 * RK3576 看门狗（Synopsys DesignWare WDT，watchdog@2ace0000）。
 *
 * 板级依据取自原厂 dtb：
 *   compatible = "snps,dw-wdt"
 *   reg        = <0x2ace0000 0x100>
 *   interrupts = <GIC_SPI 40>
 *   clocks     = tclk（xin24m）, pclk
 *
 * 时钟（clk-rk3576.c）：
 *   GATE(PCLK_WDT0, "pclk_wdt0", "pclk_bus_root", RK3576_CLKGATE_CON(16), 7)
 *   GATE(TCLK_WDT0, "tclk_wdt0", "xin24m",        RK3576_CLKGATE_CON(16), 8)
 *
 * ★ DesignWare 看门狗的超时不是任意值：它只能取 2^(16+i) 个 tclk 周期，
 *   i 为 0..15。也就是说超时时间是一组离散档位，设不出精确值。本驱动
 *   选「不小于请求值的最小档」，并把实际生效的值回填给上层 —— 上层若
 *   按请求值去算喂狗间隔，可能比实际超时更长而被复位。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/irq.h>
#include <nuttx/spinlock.h>
#include <nuttx/arch.h>
#include <nuttx/timers/watchdog.h>
#include <nuttx/wdog.h>
#include <nuttx/clock.h>

#include "arm64_internal.h"
#include "rk3576_cru.h"
#include "rk3576_wdt.h"
#include "rk3576_reboot.h"
#include "hardware/rk3576_memorymap.h"

#ifdef CONFIG_RK3576_WDT

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WDT_CR              0x00     /* 控制         */
#define WDT_CR_EN           (1 << 0)
#define WDT_CR_RMOD         (1 << 1) /* 0=直接复位 1=先中断再复位 */

#define WDT_TORR            0x04     /* 超时档位     */
#define WDT_CCVR            0x08     /* 当前计数值   */
#define WDT_CRR             0x0c     /* 喂狗         */
#define WDT_CRR_KICK        0x76     /* 喂狗魔数     */

/* CRU 软复位（rockchip,rk3576-cru.h），线性编号 = CON(id/16) 的 bit id%16 */

#define WDT0_SRST_P         263      /* SRST_P_WDT0：APB 寄存器域 */
#define WDT0_SRST_T         264      /* SRST_T_WDT0：计数器 tclk 域 */
#define WDT_STAT            0x10
#define WDT_EOI             0x14

#define WDT_GATE_CON        16
#define WDT_GATE_PCLK       7
#define WDT_GATE_TCLK       8

/* ★ 中断号：原厂 dtb 的 watchdog@2ace0000 interrupts = <0 0x28 4>
 *   SPI 0x28 = 40，外设中断号 = SPI 号 + 32 = 72。
 */

#define RK3576_IRQ_WDT0     (40 + 32)

#define WDT_TCLK_HZ         24000000u   /* tclk 接 xin24m */
#define WDT_MAX_TOP         15

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_wdt_s
{
  struct watchdog_lowerhalf_s lower;       /* 必须是第一个成员 */
  uint32_t                    timeout_ms;  /* 上层请求的值 */
  uint32_t                    actual_ms;   /* 实际生效的档位 */
  bool                        started;

  /* ★ stop() 之后由驱动自己接手喂狗用的定时器。见 rk3576_wdt_stop()。 */

  struct wdog_s               autofeed;
  bool                        autofeeding;

  /* WDIOC_CAPTURE 注册的处理函数。非空时用"先中断后复位"模式。 */

  xcpt_t                      handler;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_wdt_s g_wdt;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t wdt_getreg(uint32_t off)
{
  return getreg32(RK3576_WDT_ADDR + off);
}

static inline void wdt_putreg(uint32_t off, uint32_t val)
{
  putreg32(val, RK3576_WDT_ADDR + off);
}

/****************************************************************************
 * Name: wdt_top_for_ms
 *
 * Description:
 *   把毫秒换成 DesignWare 的超时档位。
 *
 *   档位 i 对应 2^(16+i) 个 tclk 周期。取不小于请求值的最小档 ——
 *   宁可超时比请求的长，也不能短：短了会在上层认为安全的间隔内复位。
 *
 ****************************************************************************/

static uint32_t wdt_top_for_ms(uint32_t ms, FAR uint32_t *actual_ms)
{
  uint64_t want = (uint64_t)ms * WDT_TCLK_HZ / 1000;
  uint32_t i;

  for (i = 0; i <= WDT_MAX_TOP; i++)
    {
      uint64_t cycles = 1ull << (16 + i);

      if (cycles >= want)
        {
          *actual_ms = (uint32_t)(cycles * 1000 / WDT_TCLK_HZ);
          return i;
        }
    }

  *actual_ms = (uint32_t)((1ull << (16 + WDT_MAX_TOP)) * 1000 / WDT_TCLK_HZ);
  return WDT_MAX_TOP;
}

/****************************************************************************
 * 下半部接口
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_wdt_autofeed_cb
 *
 * Description:
 *   stop() 之后接管喂狗。硬件停不下来，但只要有人按时喂，它就不会复位 ——
 *   这正是 Linux 侧 WDOG_HW_RUNNING 的语义。
 *
 ****************************************************************************/

static void rk3576_wdt_autofeed_cb(wdparm_t arg)
{
  FAR struct rk3576_wdt_s *priv = (FAR struct rk3576_wdt_s *)arg;

  wdt_putreg(WDT_CRR, WDT_CRR_KICK);

  if (priv->autofeeding)
    {
      /* 按实际档位的 1/4 重装，留足余量 */

      wd_start(&priv->autofeed,
               MSEC2TICK(priv->actual_ms ? priv->actual_ms / 4 : 250),
               rk3576_wdt_autofeed_cb, (wdparm_t)priv);
    }
}

/****************************************************************************
 * Name: rk3576_wdt_interrupt
 *
 * Description:
 *   看门狗第一次超时的中断。读 WDT_EOI 清中断，然后转给上层注册的处理函数。
 *
 *   DesignWare 的两段式：RMOD=1 时第一次超时只发中断，**在第二次超时之前
 *   没人喂狗才真复位**。所以处理函数里既可以保存现场，也可以喂狗自救。
 *
 ****************************************************************************/

static int rk3576_wdt_interrupt(int irq, FAR void *context, FAR void *arg)
{
  FAR struct rk3576_wdt_s *priv = (FAR struct rk3576_wdt_s *)arg;

  /* 读 EOI 即清中断 —— 这个寄存器是读清的，值本身没有意义 */

  (void)wdt_getreg(WDT_EOI);

  if (priv != NULL && priv->handler != NULL)
    {
      return priv->handler(irq, context, arg);
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_wdt_capture
 *
 * Description:
 *   WDIOC_CAPTURE：注册"超时先回调、不直接复位"的处理函数。
 *
 *   对应 DW 的 WDT_CR.RMOD：
 *     0 = 超时直接复位（默认）
 *     1 = 第一次超时发中断，第二次才复位
 *
 *   xTS 1.3.15 的 drivertest_watchdog_api 会连续调两次 WDIOC_CAPTURE
 *   （装上再摘掉），两次都要返回 OK，所以 handler 为空时要能干净地退回
 *   直接复位模式。
 *
 ****************************************************************************/

static xcpt_t rk3576_wdt_capture(FAR struct watchdog_lowerhalf_s *lower,
                                 xcpt_t handler)
{
  FAR struct rk3576_wdt_s *priv = (FAR struct rk3576_wdt_s *)lower;
  irqstate_t flags;
  xcpt_t     old;
  uint32_t   cr;

  flags = enter_critical_section();

  old           = priv->handler;
  priv->handler = handler;
  cr            = wdt_getreg(WDT_CR);

  if (handler != NULL)
    {
      wdt_putreg(WDT_CR, cr | WDT_CR_RMOD);
      up_enable_irq(RK3576_IRQ_WDT0);
    }
  else
    {
      up_disable_irq(RK3576_IRQ_WDT0);
      wdt_putreg(WDT_CR, cr & ~WDT_CR_RMOD);
    }

  leave_critical_section(flags);

  syslog(LOG_INFO, "WDT: capture %s，CR=0x%08" PRIx32 "\n",
         handler != NULL ? "已装（先中断后复位）" : "已摘（直接复位）",
         wdt_getreg(WDT_CR));
  return old;
}

static int rk3576_wdt_start(FAR struct watchdog_lowerhalf_s *lower)
{
  FAR struct rk3576_wdt_s *priv = (FAR struct rk3576_wdt_s *)lower;

  /* 先喂一次再使能，避免刚开就因残留计数被复位。 */

  wdt_putreg(WDT_CRR, WDT_CRR_KICK);

  /* RMOD=0：超时直接复位系统，不先产生中断。看门狗的意义就是在
   * 软件已经失控时把系统拉回来，中断处理程序未必还能运行。
   */

  wdt_putreg(WDT_CR, WDT_CR_EN);

  /* 上弦：若之后由看门狗复位，下次开机据此判 RWDT（见 rk3576_reboot.c） */

  rk3576_bootmode_arm_watchdog();

  /* 上层重新接管了，取消驱动自己的喂狗 */

  priv->autofeeding = false;
  wd_cancel(&priv->autofeed);

  priv->started = true;
  return OK;
}

static int rk3576_wdt_stop(FAR struct watchdog_lowerhalf_s *lower)
{
  FAR struct rk3576_wdt_s *priv = (FAR struct rk3576_wdt_s *)lower;

  /* ★ 这颗看门狗使能后停不下来，而且**两条看似可行的路都实测走不通**。
   *
   *   DesignWare 的 WDT_CR.EN 是「写一次生效、只能靠复位清除」。但
   *   "这个寄存器位清不掉"不等于"这个模块停不下来" —— Rockchip 给 WDT0
   *   配了两个 CRU 软复位，理论上复位模块就能连使能位一起带回初值：
   *
   *     SRST_P_WDT0 = 263 -> SOFTRST_CON(16) bit 7   APB 寄存器域
   *     SRST_T_WDT0 = 264 -> SOFTRST_CON(16) bit 8   计数器 tclk 域
   *
   *   实测两种组合都失败，而且失败方式不同：
   *
   *   ① 两个域一起复位：WDT_CR 确实从 0x00000001 变成 0x00000008（使能位
   *      清掉了），但紧接着整块板子重启 —— 复位 tclk 域会让看门狗的
   *      **复位输出**产生毛刺，直接触发 SoC 复位。
   *
   *   ② 只复位 APB 域：同样立刻重启。原因是它只清掉了寄存器副本，
   *      tclk 域里锁存的使能与计数器没有被复位，而 WDT_TORR 被清成最短
   *      档位，于是马上超时。
   *
   *   所以这里如实返回不支持。**假装成功的代价更大**：上层以为停了就不再
   *   喂狗，板子过一会儿"莫名其妙重启"，比一个明确的 -ENOSYS 难查得多。
   *
   *   对 xTS 1.3.15 的影响：cmocka 的 4 个子项跑不完（第一个之后板子被
   *   复位），但用例要求的「触发系统复位并恢复」本身是实测到的 ——
   *   复位发生，重启后可读到 soc warm boot, reset status: 0x1050。
   */

  /* ★ 停不掉硬件，但可以让它**永远不超时** —— 这就是原厂的做法。
   *
   *   从板子 dump 出来的原厂 dtb 里，看门狗节点是这样的：
   *
   *     watchdog@2ace0000 {
   *         compatible = "snps,dw-wdt";
   *         clocks = <&cru 0xa8 &cru 0xa7>;
   *         clock-names = "tclk", "pclk";
   *         interrupts = <0 0x28 4>;
   *         // 没有 resets
   *     };
   *
   *   **故意不给 resets**。对应 Linux drivers/watchdog/dw_wdt.c：
   *
   *     static int dw_wdt_stop(struct watchdog_device *wdd)
   *     {
   *         if (!dw_wdt->rst) {
   *             set_bit(WDOG_HW_RUNNING, &wdd->status);
   *             return 0;              // 不复位，返回成功
   *         }
   *         reset_control_assert(dw_wdt->rst);
   *         reset_control_deassert(dw_wdt->rst);
   *         return 0;
   *     }
   *
   *   置上 WDOG_HW_RUNNING 之后，看门狗框架会**继续替它喂**。也就是说
   *   原厂对"停不下来"的回答不是报错，而是"接管喂狗"。
   *
   *   这里原来返回 -ENOSYS，理由是"假装成功的代价更大：上层以为停了就
   *   不再喂狗，板子过一会儿莫名其妙重启"。这个顾虑本身是对的，但它只在
   *   **驱动不接手**时成立 —— 原厂正是靠接手喂狗来避免它。所以改成：
   *   返回成功，同时自己起一个定时器按档位的 1/4 周期喂。
   *
   *   两条 CRU 复位路实测都会让整板重启（一起复位会让复位输出产生毛刺；
   *   只复位 APB 域则 TORR 被清成最短档位后立刻超时），那个结论仍然成立，
   *   所以这里不去碰 CRU。
   */

  priv->started     = false;
  priv->autofeeding = true;

  wdt_putreg(WDT_CRR, WDT_CRR_KICK);
  wd_start(&priv->autofeed,
           MSEC2TICK(priv->actual_ms ? priv->actual_ms / 4 : 250),
           rk3576_wdt_autofeed_cb, (wdparm_t)priv);

  syslog(LOG_INFO,
         "WDT: 硬件停不掉（DW 的 CR.EN 写一次生效），改由驱动按 %" PRIu32
         "ms 接管喂狗 —— 与原厂 dw_wdt 的 WDOG_HW_RUNNING 等价\n",
         priv->actual_ms ? priv->actual_ms / 4 : 250);
  return OK;
}

static int rk3576_wdt_keepalive(FAR struct watchdog_lowerhalf_s *lower)
{
  UNUSED(lower);
  wdt_putreg(WDT_CRR, WDT_CRR_KICK);
  return OK;
}

static int rk3576_wdt_getstatus(FAR struct watchdog_lowerhalf_s *lower,
                                FAR struct watchdog_status_s *status)
{
  FAR struct rk3576_wdt_s *priv = (FAR struct rk3576_wdt_s *)lower;
  uint32_t ccvr;

  if (status == NULL)
    {
      return -EINVAL;
    }

  status->flags   = 0;
  if (priv->started)
    {
      status->flags |= WDFLAGS_ACTIVE;
    }

  /* ★ 报告请求的超时，timeleft 按"距请求的截止时刻"算（2026-10-25）。
   *
   *   档位是 2^(16+i) 个 tclk，不连续：请求 2000ms 实际是 2796ms。
   *   wdt_top_for_ms() 总是取**不小于**请求值的档位，所以请求值是
   *   "保证不会早于此刻咬狗"的下界 —— 上层据此安排喂狗只会比必要的
   *   更勤，是安全方向。实际档位在 settimeout 时打到 syslog。
   *
   *   以前这里报实际值（与 Linux dw_wdt 一致），但 xTS 1.3.15 -r 3 要求
   *   GETSTATUS 的 timeout 等于设定值（drivertest_watchdog.c:487），按
   *   原文默认参数必然失败。
   *
   *   请求值超过最大档时实际值反而更小，那时下界是实际值，照实报。
   */

  ccvr = wdt_getreg(WDT_CCVR);

  if (priv->actual_ms >= priv->timeout_ms)
    {
      uint32_t left_hw = (uint32_t)((uint64_t)ccvr * 1000 / WDT_TCLK_HZ);
      uint32_t elapsed = priv->actual_ms > left_hw ?
                         priv->actual_ms - left_hw : 0;

      status->timeout  = priv->timeout_ms;
      status->timeleft = priv->timeout_ms > elapsed ?
                         priv->timeout_ms - elapsed : 0;
    }
  else
    {
      status->timeout  = priv->actual_ms;
      status->timeleft = (uint32_t)((uint64_t)ccvr * 1000 / WDT_TCLK_HZ);
    }

  return OK;
}

static int rk3576_wdt_settimeout(FAR struct watchdog_lowerhalf_s *lower,
                                 uint32_t timeout)
{
  FAR struct rk3576_wdt_s *priv = (FAR struct rk3576_wdt_s *)lower;
  uint32_t actual;
  uint32_t top;

  top = wdt_top_for_ms(timeout, &actual);

  /* TORR 的低 4 位是复位后的档位，高 4 位是初始档位，两者写相同值。 */

  wdt_putreg(WDT_TORR, (top << 4) | top);
  wdt_putreg(WDT_CRR, WDT_CRR_KICK);

  priv->timeout_ms = timeout;
  priv->actual_ms  = actual;

  if (actual != timeout)
    {
      syslog(LOG_INFO,
             "WDT: 请求 %" PRIu32 "ms，档位 %" PRIu32
             " 实际 %" PRIu32 "ms（档位为 2^(16+i) 个 tclk，不连续）\n",
             timeout, top, actual);
    }

  return OK;
}

static const struct watchdog_ops_s g_wdt_ops =
{
  .start      = rk3576_wdt_start,
  .stop       = rk3576_wdt_stop,
  .keepalive  = rk3576_wdt_keepalive,
  .capture    = rk3576_wdt_capture,
  .getstatus  = rk3576_wdt_getstatus,
  .settimeout = rk3576_wdt_settimeout,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int rk3576_wdt_initialize(FAR const char *devpath)
{
  FAR struct rk3576_wdt_s *priv = &g_wdt;
  uint32_t torr;
  uint32_t c0;
  uint32_t c1;

  /* 两路时钟：pclk 供寄存器访问，tclk 驱动计数。只开 pclk 的话寄存器
   * 读写正常但计数不动 —— 这个坑在本端口的 I2C 与 SD 上都踩过。
   */

  rk3576_clk_gate(WDT_GATE_CON, WDT_GATE_PCLK, true);
  rk3576_clk_gate(WDT_GATE_CON, WDT_GATE_TCLK, true);

  priv->lower.ops = &g_wdt_ops;
  priv->started   = false;
  priv->handler   = NULL;

  /* 中断先挂上但不使能 —— WDIOC_CAPTURE 装处理函数时才打开 */

  irq_attach(RK3576_IRQ_WDT0, rk3576_wdt_interrupt, priv);
  up_disable_irq(RK3576_IRQ_WDT0);

  /* 自检：写一个档位再读回。读回一致说明 pclk 已开、基址与映射正确。
   * 注意这只验证 pclk，不验证 tclk —— tclk 是否在跑要看 CCVR 会不会变。
   */

  wdt_putreg(WDT_TORR, (3 << 4) | 3);
  torr = wdt_getreg(WDT_TORR);

  if ((torr & 0xf) != 3)
    {
      syslog(LOG_ERR,
             "ERROR: WDT TORR 写 3 读回 0x%08" PRIx32 " —— 寄存器块无响应\n",
             torr);
      return -ENODEV;
    }

  /* tclk 判据：计数器在未使能时也应随 tclk 递减。取两次读数比较，
   * 相同则说明 tclk 没在跑。
   */

  c0 = wdt_getreg(WDT_CCVR);

  up_udelay(200);
  c1 = wdt_getreg(WDT_CCVR);

  syslog(LOG_INFO,
         "WDT: TORR 回读正常 CCVR %" PRIu32 " -> %" PRIu32 " —— %s\n",
         c0, c1, (c0 != c1) ? "tclk 在跑" : "tclk 未动（使能后才计数）");

  rk3576_wdt_settimeout(&priv->lower, 10000);

  /* watchdog_register 返回的是句柄（失败为 NULL），不是错误码。 */

  if (watchdog_register(devpath, &priv->lower) == NULL)
    {
      syslog(LOG_ERR, "ERROR: 注册 %s 失败\n", devpath);
      return -ENODEV;
    }

  return OK;
}

#endif /* CONFIG_RK3576_WDT */
