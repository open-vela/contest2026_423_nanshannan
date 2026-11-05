/****************************************************************************
 * chip/rk3576/rk3576_boot.c
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

#include <stdint.h>
#include <assert.h>
#include <debug.h>

#include <nuttx/cache.h>
#ifdef CONFIG_LEGACY_PAGING
#  include <nuttx/page.h>
#endif

#include <arch/chip/chip.h>

#ifdef CONFIG_SMP
#include "arm64_smp.h"
#endif

#include "arm64_arch.h"
#include "arm64_internal.h"
#include "arm64_mmu.h"
#include "rk3576_boot.h"
#include "rk3576_serial.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* HCR_EL2.TGE (Trap General Exceptions), bit[27]
 *
 * arch/arm64 通用层的 arm64_arch.h 只定义了 FMO/IMO/AMO/RW/ATA 几个位，
 * 没有 TGE。这里在 SoC 层补一个本地定义，避免改动公共头文件。
 */

#define HCR_TGE_BIT                 BIT(27)

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* MMU 映射表。
 *
 * DEVICE_REGION 必须覆盖本端口访问的全部外设寄存器（GIC、UART、CRU、
 * GRF …）。漏掉一段的表现是访问时直接取指/数据异常，而不是报错，
 * 所以填地址时要先把 DEVICEIO 的范围算准。
 */

static const struct arm_mmu_region g_mmu_regions[] =
{
  MMU_REGION_FLAT_ENTRY("DEVICE_REGION",
                        CONFIG_DEVICEIO_BASEADDR, CONFIG_DEVICEIO_SIZE,
                        MT_DEVICE_NGNRNE | MT_RW | MT_SECURE),

  MMU_REGION_FLAT_ENTRY("DRAM0_S0",
                        CONFIG_RAMBANK1_ADDR, CONFIG_RAMBANK1_SIZE,
                        MT_NORMAL | MT_RW | MT_SECURE),

  /* U-Boot 的帧缓冲。
   *
   * ★ 为什么要额外映射这一段
   *
   *   它在 0xfdf00000，落在 NuttX 的 64MB 之外，CPU 默认访问不到。
   *   而这块内存有个别处没有的优点：**U-Boot 用它把 logo 正常显示出来
   *   过**，也就是说"VOP2 能从这里正确取数"是被实测验证过的，不是假设。
   *
   *   我们自己在堆里分配的缓冲，VOP2 取出来的内容是错乱的，原因尚未
   *   查清。与其继续猜，不如先用这块已知可用的内存把画面显示出来 ——
   *   把"能不能显示"和"能不能用任意内存显示"拆成两个问题。
   */

  MMU_REGION_FLAT_ENTRY("UBOOT_FB",
                        0xfdf00000, 0x400000,
                        MT_NORMAL | MT_RW | MT_SECURE),

#ifdef CONFIG_RK3576_RPTUN

  /* AMP 与 Linux 共享的 vring + rpmsg 缓冲池。
   *
   * ★ 为什么是 NORMAL_NC 而不是 NORMAL
   *
   *   Linux 那边用 ioremap() 映这片内存（rockchip_rpmsg_mbox.c 里还显式
   *   清掉 RPMSG_CACHED_VRING），也就是**不过 cache**。我们要是映成
   *   cacheable，同一块物理内存就有了两种互不相容的属性，描述符的可见性
   *   要靠我们每次读写都做 cache 维护来保证 —— 漏掉一处的症状是偶发丢包
   *   或者读到半新半旧的描述符，极难定位。直接不缓存，把这一整类问题去掉。
   *
   *   代价是 vring 访问慢一点。vring 里只有描述符（每条 16 字节），
   *   真正的负载在缓冲池里按 512 字节搬，这点开销无所谓。
   */

  MMU_REGION_FLAT_ENTRY("AMP_SHM",
                        CONFIG_RK3576_RPTUN_SHM_BASE, 0x400000,
                        MT_NORMAL_NC | MT_RW | MT_SECURE),
#endif

#ifdef CONFIG_RK3576_FRAME_SHM

  /* 与 Linux 共享的帧区（amp/include/k7_frame.h）。
   *
   * 和上面的 AMP_SHM 不同，这里映成 cacheable：一帧 3.7MB，界面要整帧
   * 拷贝、核对要逐像素读，不缓存太慢。代价是每次读之前必须 invalidate
   * （ampctl frame、kickpi_ui 都这么做）。本端从不写这块，所以不需要
   * clean。Linux 那边映成 write-combine，写完不需要 cache 维护。
   */

  MMU_REGION_FLAT_ENTRY("FRAME_SHM",
                        CONFIG_RK3576_FRAME_SHM_BASE,
                        CONFIG_RK3576_FRAME_SHM_SIZE,
                        MT_NORMAL | MT_RW | MT_SECURE),
#endif
};

const struct arm_mmu_config g_mmu_config =
{
  .num_regions = nitems(g_mmu_regions),
  .mmu_regions = g_mmu_regions,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#ifdef CONFIG_ARCH_HAVE_MULTICPU

/****************************************************************************
 * Name: arm64_get_mpid
 *
 * Description:
 *   逻辑 CPU 号 -> MPIDR_EL1。
 *
 *   RK3576 是 big.LITTLE，两个簇的 Aff1 不同：
 *
 *     cpu_l0..l3  Cortex-A53  MPIDR 0x000 0x001 0x002 0x003  （Aff1=0）
 *     cpu_b0..b3  Cortex-A72  MPIDR 0x100 0x101 0x102 0x103  （Aff1=1）
 *
 *   出处：kernel-6.1/arch/arm64/boot/dts/rockchip/rk3576.dtsi 的 cpus 节点，
 *   八个核 enable-method 都是 "psci"，psci 节点 method = "smc"、
 *   compatible = "arm,psci-1.0"。
 *
 *   ★ 当前只用 **A53 簇（Aff1=0）的 4 个核**，因此可以直接用通用层
 *     按 Aff0 换算的 CORE_TO_MPID/MPID_TO_CORE。启动核就是这个簇的
 *     core 0（MPIDR 0x0），编号连续，映射是恒等的。
 *
 *   ★ 要扩到 8 核就不能再用默认宏了：通用层的 MPID_TO_CORE 只取 Aff0，
 *     A72 簇会算出 0..3，与 A53 簇撞号。届时需要同时处理 Aff1
 *     （cpu = (Aff1 << 2) | Aff0）并自定义 MPID_TO_CORE。
 *     另外 A72 簇也是 AMP 阶段准备划给 Linux 的那一半，所以先只占
 *     A53 簇，两件事不打架。
 *
 ****************************************************************************/

uint64_t arm64_get_mpid(int cpu)
{
  return CORE_TO_MPID(cpu, 0);
}

/****************************************************************************
 * Name: arm64_get_cpuid
 *
 * Description:
 *   MPIDR_EL1 -> 逻辑 CPU 号。仅对 A53 簇成立，理由见上。
 *
 ****************************************************************************/

int arm64_get_cpuid(uint64_t mpid)
{
  return MPID_TO_CORE(mpid);
}

#endif /* CONFIG_ARCH_HAVE_MULTICPU */

/****************************************************************************
 * Name: arm64_el_init
 *
 * Description:
 *   The function called from arm64_head.S at very early stage for these
 * platform, it's use to:
 *   - Handling special hardware initialize routine which is need to
 *     run at high ELs
 *   - Initialize system software such as hypervisor or security firmware
 *     which is need to run at high ELs
 *
 ****************************************************************************/

void arm64_el_init(void)
{
  /* ★ 清除 U-Boot 遗留的 HCR_EL2 残留位：TGE 与 FMO/IMO/AMO。
   *
   * 现象：从 U-Boot 的 booti 进入后，汇编启动到 EL2->EL1 的 eret 就崩，
   *       U-Boot 的 EL2 向量报 esr 0x3a000000（EC=0xE，Illegal Execution
   *       State），且早期打印能正常输出到 "- Boot from EL2"。
   *
   * 根因：Rockchip 的 U-Boot 2017.09 在 EL2 运行时置了 HCR_EL2.TGE=1。
   *       ARM 架构规定 TGE=1 时禁止 eret 到 EL1
   *       （Illegal Exception Return）。
   *       而 arch/arm64 通用层的 arm64_boot_el2_init() 对 HCR_EL2 用的是
   *       read-modify-write：
   *
   *           reg = read_sysreg(hcr_el2);
   *           reg |= HCR_RW_BIT;
   *           write_sysreg(reg, hcr_el2);
   *
   *       只补 RW、不清任何位，于是把 bootloader 的残留状态带进了 eret。
   *       （Linux 内核在同一位置是覆盖写 HCR_EL2，不保留 bootloader 状态。）
   *
   * 定位：在 arm64_head.S 的 EL 切换路径上插无栈单字符打点，输出为
   *       "12E2- Boot from EL2 abcd2RT*4>e- Boot from EL1 fgh...i"，
   *       其中 T 表示读到 TGE=1，* 之后 eret 立即成功。
   *
   * ★★ 第二个残留位：FMO/IMO/AMO（bit 3/4/5）—— 同源、同样致命。
   *
   * 现象：TGE 修好后 OS 能一路初始化完成（堆、GIC、arch timer、
   *       nx_bringup 全过），进入空闲循环打印出第一个 '.'，
   *       随即崩在 U-Boot 的 EL2 向量：
   *
   *           "Synchronous Abort" handler, esr 0x02000000
   *           * PC = 0000000040200c98        （镜像范围之外的零内存）
   *
   * 定位：在进空闲循环前把三个系统寄存器打出来，全部正常 ——
   *           VBAR_EL1  = 0x404a7000   （等于 _vector_table，向量表没问题）
   *           CurrentEL = 1            （确实在 EL1）
   *           DAIF      = 0x240        （D=1 F=1，但 I=0，IRQ 已打开）
   *       向量表对、异常级对、IRQ 开着，异常却被 EL2 接走 ——
   *       在架构上只有一种可能：HCR_EL2.IMO=1 把物理 IRQ 路由到了 EL2。
   *       U-Boot 在 EL2 运行时置这几位以便自己接管中断，交接时没清。
   *       第一次 arch timer 中断到达即跳进 U-Boot 已失效的 IRQ 路径，
   *       落到一片零内存，再触发同步异常，于是打出上面那份转储。
   *
   * 通用层同样不管这三位（arm64_boot.c 只 |= RW/ATA），所以必须在这里清。
   *
   * ★★★ 这本质上是 arch/arm64 通用层的缺陷，不是 RK3576 的特例：
   *      任何从 EL2 的 bootloader 进入 NuttX 的 arm64 平台都会踩到。
   *      已另备上游补丁 bsp/upstream-hcr-el2.patch，在
   *      arm64_boot_el2_init() 里统一清理这四位。
   *
   *      该补丁已在本板验证：把本函数清空、只保留通用层修复，
   *      仍能正常启动到 NuttShell 并交互（验证分支 verify-upstream-hcr，
   *      uname 显示 118f003d-dirty）。
   *
   *      本处的清理在上游补丁合入后即成冗余，但保留它可使本 BSP
   *      在未打补丁的上游分支上也能独立工作，故暂不移除。
   *
   * 本函数是 arm64_head.S 里 "Platform hook for highest EL" 的回调，
   * 在 switch_el 之前、最高 EL 上执行，正是清理 bootloader 残留的位置。
   */

  uint64_t reg;

  if (arm64_current_el() == MODE_EL2)
    {
      reg = read_sysreg(hcr_el2);
      reg &= ~(HCR_TGE_BIT |                    /* 允许 eret 到 EL1     */
               HCR_FMO_BIT | HCR_IMO_BIT |      /* FIQ / IRQ 交回 EL1   */
               HCR_AMO_BIT);                    /* SError 交回 EL1      */
      write_sysreg(reg, hcr_el2);
      UP_ISB();
    }

  /* RK3576 的基础时钟由启动链路上游（TPL/SPL/U-Boot）配置完毕，
   * M1 阶段无需在此操作 CRU。
   */
}

/****************************************************************************
 * Name: arm64_chip_boot
 *
 * Description:
 *   Complete boot operations started in arm64_head.S
 *
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_mark
 *
 * Description:
 *   启动早期的打点。**不经过任何串口驱动**：直接往 UART0 的 THR 里轮询
 *   写字节（arm64_lowputc 就是干这个的）。
 *
 * ★ 为什么非要有这么一个东西
 *
 *   arm64_chip_boot() 跑在一段很窄的窗口里：MMU 刚要开、串口驱动还没
 *   注册、syslog 还没有后端、异常向量表可能还没生效。这一段里出问题的
 *   表现**统一是"串口最后一行是 arm64_head.S 的 Boot to C runtime，
 *   之后彻底没输出、也没有异常信息"** —— 因为连报异常的路都还没铺好。
 *
 *   AMP 上板第一次就撞在这里。当时能拿到的唯一信息是"没输出"，而候选
 *   原因有四个（MMU 映射、PSCI、板级初始化、串口早期初始化），一条
 *   信息区分不了。排查方向一度跑去查 GIC 共享和 Linux 抢中断，全错。
 *   真正的原因是 chip.h 里 CONFIG_RAMBANK1_ADDR 写死成 0x40480000，
 *   而 amp-dual 把镜像链到了 0x4a400000 —— MMU 一开，正在执行的代码
 *   就不在映射里了。
 *
 *   有了打点，这一段的每一步都有独立的证据，不用再猜。
 *
 *   代价几乎为零：只在 CONFIG_ARCH_EARLY_PRINT 下编进去，一行字符串。
 *
 ****************************************************************************/

#ifdef CONFIG_ARCH_EARLY_PRINT
static void rk3576_mark(const char *s)
{
  while (*s != '\0')
    {
      arm64_lowputc(*s++);
    }
}
#  define MARK(s) rk3576_mark("[boot] " s "\r\n")
#else
#  define MARK(s)
#endif

void arm64_chip_boot(void)
{
  /* MAP IO and DRAM, enable MMU.
   *
   * 打点必须在 mmu_init **前后**各一次：这一步失败的唯一表现就是
   * 之后再没有任何输出，只有"前"那一条能证明我们到过这里。
   */

  MARK("mmu_init 前");
  arm64_mmu_init(true);
  MARK("mmu_init 后（MMU 已开，代码仍在映射内）");

#if defined(CONFIG_ARM64_PSCI)
  arm64_psci_init("smc");
  MARK("psci_init 后");

#endif

  /* Perform board-specific device initialization. This would include
   * configuration of board specific resources such as GPIOs, LEDs, etc.
   */

  rk3576_board_initialize();
  MARK("board_initialize 后");

#ifdef USE_EARLYSERIALINIT
  /* Perform early serial initialization if we are going to use the serial
   * driver.
   */

  arm64_earlyserialinit();
  MARK("earlyserialinit 后（此后走 syslog，波特率见 UART0_BAUD）");

#endif

  MARK("arm64_chip_boot 结束，进 nx_start");
}

#if defined(CONFIG_NET) && !defined(CONFIG_NETDEV_LATEINIT)
void arm64_netinitialize(void)
{
  /* TODO(M4): DW GMAC 驱动接入点 */
}
#endif
