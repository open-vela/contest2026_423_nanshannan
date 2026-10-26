/****************************************************************************
 * arch/arm64/src/rk3576/rk3576_reboot.c
 *
 * 复位与「重启进下载模式」。
 *
 * ★ 为什么值得单独做这件事：
 *
 *   每烧一次固件都要手按 recovery 键让板子进 loader 模式，一轮调试
 *   下来要按几十次。Rockchip 的引导器本来就支持软件触发 —— 往一个
 *   复位后仍保留的寄存器写魔数，U-Boot 启动时读到就进下载模式。
 *
 *   依据来自原厂 dtb 的 syscon-reboot-mode 节点：
 *
 *     syscon@26024000   compatible = "rockchip,rk3576-pmu0-grf"
 *     reboot-mode { offset = <64>;              // 0x40
 *                   mode-normal   = <0x5242C300>;
 *                   mode-loader   = <0x5242C301>;
 *                   mode-recovery = <0x5242C303>; ... }
 *
 *   复位手段用 CRU 的全局软复位（TRM Part1）：
 *     CRU_GLB_SRST_FST_VAL  0x0C08，写 0xfdb9 触发
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <syslog.h>

#include "arm64_internal.h"
#include "rk3576_reboot.h"
#include "hardware/rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* PMU0 GRF 里的重启原因寄存器。这个寄存器的内容在软复位后保留，
 * 引导器据此决定进哪种模式。
 */

#define RK3576_PMU0_GRF_ADDR      0x26024000
#define RK3576_REBOOT_MODE_OFF    0x40

#define RK3576_REBOOT_NORMAL      0x5242c300
#define RK3576_REBOOT_LOADER      0x5242c301
#define RK3576_REBOOT_RECOVERY    0x5242c303
#define RK3576_REBOOT_WATCHDOG    0x5242c308

/* ★ 看门狗复位的判定靠这个寄存器，而不是 CRU_GLB_RST_ST（2026-10-25）
 *
 *   CRU_GLB_RST_ST 在 ATF（闭源 BL31）里就被读走清零了 —— 串口上
 *   ATF 打印 "soc warm boot, reset status: 0x1050"，轮到 openvela 读到 0，
 *   xTS 1.3.15 于是把看门狗复位判成 CHIPPOR。
 *
 *   做法：看门狗一使能，就往重启模式寄存器写 BOOT_WATCHDOG（"上弦"）。
 *     - 看门狗把板子复位：寄存器跨复位保留，U-Boot 见到 BOOT_WATCHDOG
 *       只打印 "boot mode: watchdog"、**不清**（boot_mode.c：代表重启
 *       原因的值留给下一级），openvela 开机读到它 -> RWDT。
 *     - 软件重启：rk3576_do_reset() 先写 NORMAL/LOADER，把标记盖掉。
 *     - 上电：寄存器内容不定（10-26 实测 0x7ccb56e6），U-Boot 不清；
 *       不是 0x5242c3xx 就判 CHIPPOR。
 *
 *   不需要复位前软件还能动 —— xTS 的 -r 1/-r 2 是关中断死循环，那时
 *   看门狗中断根本进不来，"在中断里记一笔"的办法行不通。
 *
 *   板上实测：写 0x5242c308 后由看门狗复位，ATF 报 0x1050、U-Boot 报
 *   "boot mode: watchdog"，openvela 读回 0x5242c308。
 */

static uint32_t g_bootmode;
static bool g_bootmode_latched;

/* ★ 直接回 BootROM 的下载模式（maskrom），等价于按住 recovery 键上电。
 *
 *   出处：U-Boot 的 `rbrom` 命令，cmd/boot.c do_reboot_brom()
 *       writel(BOOT_BROM_DOWNLOAD, CONFIG_ROCKCHIP_BOOT_MODE_REG);
 *       do_reset(...);
 *   魔数在 arch/arm/include/asm/arch-rockchip/boot_mode.h，
 *   寄存器就是上面这个 0x26024040（本板 .config 里
 *   CONFIG_ROCKCHIP_BOOT_MODE_REG=0x26024040，和 mode-normal 同一个）。
 *   读它的是 SPL（arch/arm/mach-rockchip/spl.c 的 brom_download()），
 *   读到就 back_to_bootrom()。
 *
 * ★ 为什么要它，而不是用已有的 LOADER
 *
 *   LOADER 进的是 **U-Boot 自己的 rockusb gadget**。它复位没问题，但那个
 *   gadget 在本机枚举不出来 —— 本项目早就记过"板子进了 loader 但主机看
 *   不见它"，这次又复现了一遍。
 *
 *   MASKROM 用的是 **BootROM 自己的 USB**，不经过 U-Boot 的 gadget。
 *   整场调试里它每次都枚举成功。
 *
 *   有了它，烧写流程就不再需要"复位后抢 U-Boot 提示符再敲 rbrom"这种
 *   有竞争的动作：nsh> reboot 2 一条命令直达。
 */

#define RK3576_REBOOT_MASKROM     0xef08a53c

/* CRU 全局软复位。写入 0xfdb9 触发第一级全局复位。 */

#define RK3576_CRU_GLB_SRST_FST   0x0c08
#define RK3576_GLB_SRST_MAGIC     0xfdb9

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void rk3576_do_reset(uint32_t mode)
{
  putreg32(mode, RK3576_PMU0_GRF_ADDR + RK3576_REBOOT_MODE_OFF);

  /* 读回确认写进去了。这个寄存器要跨复位保留，写不进去的话下面复位
   * 完就是一次普通重启，而不是进下载模式 —— 两者现象差别很大，
   * 事先确认比事后猜省事。
   */

  syslog(LOG_INFO, "重启模式寄存器 = 0x%08lx（期望 0x%08lx）\n",
         (unsigned long)getreg32(RK3576_PMU0_GRF_ADDR +
                                 RK3576_REBOOT_MODE_OFF),
         (unsigned long)mode);

  /* 让串口把上面这行发完再复位 */

  up_mdelay(50);

  putreg32(RK3576_GLB_SRST_MAGIC,
           RK3576_CRU_ADDR + RK3576_CRU_GLB_SRST_FST);

  /* 复位是异步的，这里等它发生 */

  for (; ; )
    {
      up_udelay(1000);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void rk3576_reboot_loader(void)
{
  syslog(LOG_INFO,
         "即将重启进下载模式，无需按 recovery 键。\n"
         "USB 会断开重连，之后可直接用 rkdeveloptool 烧写。\n");
  rk3576_do_reset(RK3576_REBOOT_LOADER);
}

void rk3576_reboot_normal(void)
{
  syslog(LOG_INFO, "正在重启\n");
  rk3576_do_reset(RK3576_REBOOT_NORMAL);
}

void rk3576_reboot_maskrom(void)
{
  syslog(LOG_INFO,
         "即将重启进 maskrom（BootROM 下载模式），无需按 recovery 键。\n"
         "之后直接 rkdeveloptool db + wl 烧写。\n");
  rk3576_do_reset(RK3576_REBOOT_MASKROM);
}

uint32_t rk3576_bootmode_latch(void)
{
  if (!g_bootmode_latched)
    {
      g_bootmode = getreg32(RK3576_PMU0_GRF_ADDR + RK3576_REBOOT_MODE_OFF);
      g_bootmode_latched = true;

      /* 读过就还原成 NORMAL：没开看门狗的配置不会再上弦，留着
       * BOOT_WATCHDOG 会让下一次与看门狗无关的复位也被判成看门狗。
       */

      if (g_bootmode == RK3576_REBOOT_WATCHDOG)
        {
          putreg32(RK3576_REBOOT_NORMAL,
                   RK3576_PMU0_GRF_ADDR + RK3576_REBOOT_MODE_OFF);
        }
    }

  return g_bootmode;
}

bool rk3576_bootmode_is_watchdog(void)
{
  return rk3576_bootmode_latch() == RK3576_REBOOT_WATCHDOG;
}

void rk3576_bootmode_arm_watchdog(void)
{
  /* 先锁存本次启动的原因，再覆盖 */

  rk3576_bootmode_latch();
  putreg32(RK3576_REBOOT_WATCHDOG,
           RK3576_PMU0_GRF_ADDR + RK3576_REBOOT_MODE_OFF);
}
