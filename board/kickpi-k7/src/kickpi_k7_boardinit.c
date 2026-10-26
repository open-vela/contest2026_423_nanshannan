/****************************************************************************
 * vendor/rockchip/boards/rk3576/kickpi-k7/src/kickpi_k7_boardinit.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <syslog.h>
#include <sys/boardctl.h>
#include <stdint.h>
#include <nuttx/board.h>
#include <errno.h>

#include "arm64_internal.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_reboot.h"
#include "rk3576_smp.h"
#include "kickpi_k7.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_memory_initialize
 *
 * Description:
 *   All RK3576 boards must provide the following entry point.  It is called
 *   early in the initialization before memory has been configured.
 *
 *   Logic here must be careful to avoid using any global variables because
 *   those will be uninitialized at the time this function is called.
 *
 ****************************************************************************/

void rk3576_memory_initialize(void)
{
  /* DDR 已由启动链路上游完成初始化：
   *   BootROM -> TPL(ddr.bin, Rockchip 闭源) -> SPL -> U-Boot -> 本镜像
   *
   * OS 侧不做 DDR 初始化。RK3576 的 DRAM 物理基址为 0x40000000
   * (U-Boot CFG_SYS_SDRAM_BASE)。
   */
}

/****************************************************************************
 * Name: rk3576_board_initialize
 *
 * Description:
 *   All RK3576 boards must provide the following entry point.  It is called
 *   after rk3576_memory_initialize and after all memory has been configured
 *   and mapped, but before any devices have been initialized.
 *
 ****************************************************************************/

void rk3576_board_initialize(void)
{
#ifdef CONFIG_ARCH_LEDS
  /* TODO(M1+)：板载 LED 初始化，需原理图确认 GPIO 编号。 */
#endif
}

/****************************************************************************
 * Name: board_late_initialize
 ****************************************************************************/

#ifdef CONFIG_HAVE_CXXINITIALIZE
/****************************************************************************
 * Name: kickpi_k7_register_eh_frame
 *
 * Description:
 *   把 C++ 异常的栈展开表注册给 libgcc 的展开器。
 *
 *   ★ 为什么裸机上非做不可。
 *
 *     `throw` 时 _Unwind_RaiseException 要靠 _Unwind_Find_FDE 找到描述
 *     每个栈帧如何回退的 CFI 表（.eh_frame）。libgcc 找它有两条路：
 *
 *       a) 走 dl_iterate_phdr 查 PT_GNU_EH_FRAME 程序头 —— 需要动态加载器
 *       b) 查 __register_frame_info() 注册过的对象链表
 *
 *     裸机 NuttX 没有 (a)，而 (b) 平时由 crtbegin.o 的构造函数完成 ——
 *     可我们不链接 crtbegin/crtend。于是表在镜像里、展开器却找不到，
 *     现象是 vector/map/RTTI 全过、一 throw 就崩在展开器内部。
 *
 *     上游 NuttX 的每一块 arm64 板子都把 .eh_frame 丢进 /DISCARD/，
 *     所以这不是本移植特有的问题 —— 只是纯 C 的板子碰不到。
 *
 *     必须在任何 C++ 异常之前注册，因此放在 board_late_initialize()。
 *
 ****************************************************************************/

extern void __register_frame_info(FAR const void *begin, FAR void *ob);
extern uint8_t __EH_FRAME_BEGIN__[];

/* libgcc 的 struct object 是不透明的，这里给足空间让它存放注册信息。
 * 它必须在整个运行期存活，所以是 static。
 */

static uintptr_t g_eh_object[8];

static void kickpi_k7_register_eh_frame(void)
{
  __register_frame_info(__EH_FRAME_BEGIN__, g_eh_object);
  syslog(LOG_INFO, "C++: 栈展开表已注册 @%p\n", __EH_FRAME_BEGIN__);
}
#endif

/* 启动时锁存的 CRU_GLB_RST_ST，board_reset_cause() 用 */

static uint32_t g_glb_rst_st;

#ifdef CONFIG_BOARD_LATE_INITIALIZE
/* ★ 启动时锁存复位原因，并把原值打出来。
 *
 *   两个来源都在这里读一次：
 *     - CRU_GLB_RST_ST：本应记录看门狗复位，但实测在 ATF 里就被清掉了，
 *       openvela 读到恒为 0。仍然读，ATF 哪天不清了就能直接用。
 *     - 重启模式寄存器（PMU0 GRF OS_REG16）：看门狗使能时写入
 *       BOOT_WATCHDOG，见 rk3576_reboot.c。**必须在看门狗注册之前锁存**，
 *       board_late_initialize 早于 board_app_initialize，满足。
 */

static void kickpi_latch_reset_status(void)
{
  g_glb_rst_st = getreg32(RK3576_CRU_ADDR + 0x0c04);

  /* 写 1 清，免得残留到下一次与它无关的复位 */

  putreg32(g_glb_rst_st, RK3576_CRU_ADDR + 0x0c04);

  syslog(LOG_INFO,
         "复位状态: GLB_RST_ST=0x%08" PRIx32 " 重启模式=0x%08" PRIx32 "\n",
         g_glb_rst_st, rk3576_bootmode_latch());
}

void board_late_initialize(void)
{
  kickpi_latch_reset_status();

#ifdef CONFIG_SMP
  /* 双系统下启动阶段会丢调度 SGI，丢了 NuttX 不会补发 ——
   * 09 月"CPU1 不接任务"就是这个。见 rk3576_smp.c。
   */

  rk3576_sgi_rescue_start();
#endif

#ifdef CONFIG_HAVE_CXXINITIALIZE
  kickpi_k7_register_eh_frame();
#endif
}
#endif /* CONFIG_BOARD_LATE_INITIALIZE */

#ifdef CONFIG_BOARDCTL_RESET_CAUSE
/****************************************************************************
 * Name: board_reset
 *
 * Description:
 *   nsh 的 `reboot` 命令走这里。
 *
 *   ★ 为什么要有它：在此之前板子上没有任何"重启"手段，每次都得走
 *     `loader` -> USB 枚举 -> rkdeveloptool rd 这条链。而那条链的 USB
 *     环节很不稳 —— 一场调试里已经三次卡在"板子进了 loader 但主机
 *     看不见它"，只能人工断电。加上这个命令就绕开了整条 USB 链路。
 *
 *   status 非 0 时进下载模式，便于烧写；0 是普通重启。
 *
 ****************************************************************************/

int board_reset(int status)
{
  /* nsh> reboot      普通重启
   * nsh> reboot 1    U-Boot 的 rockusb 下载模式
   * nsh> reboot 2    maskrom（BootROM 下载模式）★ 烧写用这个
   *
   * ★ 为什么加 2 而不是继续用 1
   *
   *   1 进的是 U-Boot 自己的 rockusb gadget，复位没问题，但那个 gadget
   *   在开发机上枚举不出来（本项目记过多次，这次又复现）。于是烧写只能
   *   退回"复位 -> 抢 U-Boot 提示符 -> 敲 rbrom"，而抢提示符是个竞争
   *   动作，失败了就得人去按板子。
   *
   *   2 写的是 BootROM 的下载魔数，SPL 读到就跳回 BootROM，用的是
   *   BootROM 自己的 USB —— 整场调试里它每次都枚举成功。一条命令直达，
   *   没有竞争。
   */

  switch (status)
    {
      case 0:
        rk3576_reboot_normal();
        break;

      case 1:
        rk3576_reboot_loader();
        break;

      default:
        rk3576_reboot_maskrom();
        break;
    }

  return 0;
}

/****************************************************************************
 * Name: board_reset_cause
 *
 * Description:
 *   报告本次启动的复位原因，同一次启动里多次调用结果相同。
 *
 *   依据（按优先级）：
 *   1. CRU_GLBRST_ST（0x27200000 + 0x0C04，TRM Part1）：
 *        bit15 PMU_WDT  bit14 NPU_WDT  bit13 WDT_S  bit12 WDT_NS
 *        bit11 BUS_WDT  bit10 DDR_WDT  bit6 看门狗复位  bit5 第二级复位
 *      实测被 ATF 清零，一般读到 0。
 *   2. 重启模式寄存器（rk3576_reboot.c）：
 *        BOOT_WATCHDOG -> 看门狗复位
 *        0x5242c3xx（NORMAL 等，软件重启后由 U-Boot 写回）-> 软件复位
 *        其他 -> 上电（寄存器上电值不定，实测 0x7ccb56e6 / 0x7cc356e6）
 *
 *   已知盲区：Linux 侧发起的 PSCI SYSTEM_RESET、以及不掉电的硬件复位，
 *   都不经过 openvela 的重启路径，若此时看门狗已上弦会被判成看门狗。
 *
 ****************************************************************************/

int board_reset_cause(FAR struct boardioc_reset_cause_s *cause)
{
  uint32_t mode;

  if (cause == NULL)
    {
      return -EINVAL;
    }

  mode = rk3576_bootmode_latch();

  if (g_glb_rst_st & ((1u << 6) | (1u << 5) | (0x1fu << 11)))
    {
      cause->cause = BOARDIOC_RESETCAUSE_SYS_RWDT;
      cause->flag  = g_glb_rst_st;
    }
  else if (rk3576_bootmode_is_watchdog())
    {
      cause->cause = BOARDIOC_RESETCAUSE_SYS_RWDT;
      cause->flag  = mode;
    }
  else if ((mode & 0xffffff00) == 0x5242c300)
    {
      /* 只有软件重启会写 0x5242c3xx（NORMAL / LOADER / ...） */

      cause->cause = BOARDIOC_RESETCAUSE_CORE_SOFT;
      cause->flag  = mode;
    }
  else
    {
      /* 上电：寄存器内容不定。10-26 断电上电实测读到 0x7ccb56e6，不是 0 */

      cause->cause = BOARDIOC_RESETCAUSE_SYS_CHIPPOR;
      cause->flag  = mode;
    }

  return OK;
}
#endif
