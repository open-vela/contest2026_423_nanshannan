/****************************************************************************
 * chip/rk3576/rk3576_smp.c
 *
 * 双系统下补发丢失的调度 SGI。
 *
 * ★ 现象（2026-09 起，10-24 查清）：
 *
 *   双系统下 CPU1 偶尔"不接任务"：绑到它上面的线程永远停在 Ready，
 *   CPU1 的 IDLE 一直在跑。单系统下从不出现。
 *
 * ★ 机制：
 *
 *   NuttX 把任务投给别的核时，先置 g_delivertasks[目标核]，再发 SGI1；
 *   目标核在 SGI 处理函数里清标志、切任务。nxsched_deliver_task() 看到
 *   标志已经是这次的值，就认为"中断已在路上"，**不再发 SGI**。
 *
 *   所以只要丢过一个 SGI，标志就永远清不掉，这个核从此再也不会被叫醒。
 *   板上实测：卡住时 g_delivertasks[1] = 1（SWITCH_HIGHER），手动往
 *   GICD_SGIR 写一次 0x00020001（CPU 接口 1、SGI1），标志立即清零，
 *   卡住的线程马上跑完 —— CPU1 本身完全正常。
 *
 *   丢 SGI 发生在启动阶段。双系统和单系统唯一的区别是 Linux 在同一个
 *   GIC 分发器上做 gic_dist_init()（先关 GICD_CTLR 再重配），所以怀疑
 *   SGI 落在了那个窗口里；这一点未单独验证。
 *
 *   09 月的绕行（启动时让所有任务避开 CPU1）之所以有效，只是因为启动
 *   窗口里没有任务投给 CPU1，标志从未被置上。
 *
 * ★ 做法：
 *
 *   不去猜丢在哪里，而是兜底：CPU0 每 100ms 看一次各核的标志，连续两次
 *   都不是 SWITCH_NONE 就补发一次 SGI。正常投递几微秒内就会清标志，
 *   所以只有真丢了 SGI 才会补发；多发一个也无害 —— 处理函数取到
 *   SWITCH_NONE 时什么都不做。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/wdog.h>

#include "sched/sched.h"
#include "rk3576_smp.h"

#ifdef CONFIG_SMP

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RESCUE_PERIOD   MSEC2TICK(100)

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* 累计补发次数与最后一次补发时的系统节拍，k7diag cpus 会打印 */

volatile uint32_t g_rk3576_sgi_rescued;
volatile clock_t  g_rk3576_sgi_rescue_tick;

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct wdog_s g_rescue_wdog;
static bool g_seen[CONFIG_SMP_NCPUS];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void rk3576_sgi_rescue(wdparm_t arg)
{
  irqstate_t flags;
  int me;
  int cpu;

  UNUSED(arg);

  flags = enter_critical_section();
  me    = this_cpu();

  for (cpu = 0; cpu < CONFIG_SMP_NCPUS; cpu++)
    {
      bool pending = cpu != me &&
                     per_cpu_var_smp(g_delivertasks, cpu) != SWITCH_NONE;

      if (pending && g_seen[cpu])
        {
          up_send_smp_sched(cpu);
          g_rk3576_sgi_rescued++;
          g_rk3576_sgi_rescue_tick = clock_systime_ticks();
          pending = false;
        }

      g_seen[cpu] = pending;
    }

  leave_critical_section(flags);

  wd_start(&g_rescue_wdog, RESCUE_PERIOD, rk3576_sgi_rescue, 0);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void rk3576_sgi_rescue_start(void)
{
  wd_start(&g_rescue_wdog, RESCUE_PERIOD, rk3576_sgi_rescue, 0);
}

#endif /* CONFIG_SMP */
