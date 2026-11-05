/****************************************************************************
 * chip/rk3576/include/chip.h
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

#ifndef __ARCH_ARM64_INCLUDE_RK3576_CHIP_H
#define __ARCH_ARM64_INCLUDE_RK3576_CHIP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* ==========================================================================
 * 地址来源：Linux 主线 arch/arm64/boot/dts/rockchip/rk356x-base.dtsi
 *          与 U-Boot 主线 include/configs/rk3576_common.h
 *
 * 两者均为上游正式合入、经过验证的实现，比原厂 SDK 更可靠。
 * dtsi 的许可证是 (GPL-2.0+ OR MIT)，可按 MIT 分支使用，与本项目的
 * Apache-2.0 兼容。
 *
 * 仍未确证的项见 notes/RK3576-ADDR-TODO.md，均已在下方就地标注。
 * ==========================================================================
 */

/* Number of bytes in x kibibytes/mebibytes/gibibytes */

#define KB(x)           ((x) << 10)
#define MB(x)           (KB(x) << 10)
#define GB(x)           (MB(UINT64_C(x)) << 10)

/* ★★ Rockchip RK3576 Generic Interrupt Controller —— GIC-400，即 GICv2。
 *
 * ★ 与 RK3568/RK3399/RK3588 不同，务必不要照抄它们的值和版本。
 *   RK3568 是 GIC-500、RK3399 是 GIC-500、RK3588 是 GIC-600，全是 GICv3；
 *   RK3576 是 GIC-400，是 GICv2。
 *
 * 取值来源：Linux 主线 arch/arm64/boot/dts/rockchip/rk3576.dtsi
 *
 *   gic: interrupt-controller@2a701000 {
 *           compatible = "arm,gic-400";
 *           reg = <0x0 0x2a701000 0 0x10000>,   // GICD 分发器
 *                 <0x0 0x2a702000 0 0x10000>,   // GICC CPU 接口
 *                 <0x0 0x2a704000 0 0x10000>,   // GICH 虚拟化控制
 *                 <0x0 0x2a706000 0 0x10000>;   // GICV 虚拟 CPU 接口
 *   };
 *
 * 校验：四段 reg 是 GICv2 的标志（GICv3 只有 GICD + GICR 两段）。
 *
 * ★ 在 GICv2 代码路径（arch/arm64/src/common/arm64_gicv2.c）下，
 *   CONFIG_GICR_BASE 承载的是 GICC（CPU 接口）地址，不是 GICv3 的
 *   redistributor。参照 boards/arm64/zynq-mpsoc/zcu111 的做法，
 *   它同样是 GIC-400：GICD 0xf9010000 / GICC 0xf9020000。
 *
 * ★ GICv2 没有 per-CPU redistributor，因此不定义 CONFIG_GICR_OFFSET。
 *
 * ★ defconfig 中必须有 CONFIG_ARM64_GIC_VERSION=2，否则会走 GICv3 路径，
 *   表现为中断完全不触发（轮询发送仍正常，即"能打印但收不到输入"）。
 */

#define CONFIG_GICD_BASE          0x2a701000
#define CONFIG_GICR_BASE          0x2a702000

/* Rockchip RK3576 Memory Map: RAM and Device I/O
 *
 * RAMBANK1  : DDR 的起始地址与本端口可用的大小。
 *             LogicPi A1 实际 DDR 容量以板卡规格为准。
 * DEVICEIO  : 外设寄存器区，需覆盖 GIC、UART、CRU、GRF 等全部用到的
 *             控制器；这段会被 rk3576_boot.c 建成一整块 device 映射。
 */

/* ★★ RK3576 的 DDR 物理起始地址是 0x40000000，不是 0。
 *
 * 来源：U-Boot 主线 include/configs/rk3576_common.h（Rockchip 官方提交）
 *   #define CFG_SYS_SDRAM_BASE   0x40000000
 *   #define CFG_IRAM_BASE        0x3ff80000
 *
 * ★ 这是 RK3576 与 RK3568 最容易踩的差异：RK3568 的 DDR 从 0 起、外设在
 *   0xfxxxxxxx；RK3576 反过来 —— 外设占了低位（0x22000000~0x2b060000），
 *   DDR 被推到 0x40000000。照抄 RK3568 的 0x02000000 会落在外设区里，
 *   表现是上板完全没有输出、也不报错。
 *
 * 交叉验证：dtsi 中 sram@3ff88000 与 U-Boot 的 CFG_IRAM_BASE 0x3ff80000
 * 吻合，且 scmi-shmem@4010f000 落在 0x40000000 之后，与 DDR 基址自洽。
 *
 * 这里沿用 RK3568/RK3399 端口的做法，从镜像加载地址开始映射，
 * 不映射加载地址之前的 ATF/OP-TEE 保留区。
 *
 * TODO(M0)：512MB 是保守取值，KICKPI-K7 有 4/8/16GB 版本，实际容量需按
 * 板卡规格确认。偏小只是用不满内存，不影响启动；偏大会映射到不存在的
 * 物理地址。
 */

/* ★ 2026-09-12：改成跟着 CONFIG_RAM_START / CONFIG_RAM_SIZE 走，
 *   不再写死。
 *
 *   这里原来硬编码 0x40480000，和 dramboot.ld 里那个字面量是同一类
 *   问题：同一个地址在两个地方各写一遍，靠人记住一起改。
 *
 *   AMP 上它真的炸了。amp-dual 配置把 CONFIG_RAM_START 挪到
 *   0x4a400000（Linux 要用 0x40400000），但 MMU 仍按 0x40480000 建 DRAM
 *   映射 —— 于是 arm64_mmu_init(true) 打开 MMU 的那一拍，**正在执行的
 *   代码不在映射里**，取指立刻异常。
 *
 *   现象极具误导性：串口最后一行是 arm64_head.S 的
 *   "- Boot to C runtime for OS Initialize"，之后全无输出、也没有异常
 *   信息（异常向量表所在页同样没映射）。当时第一反应是去查 GIC 共享、
 *   查 Linux 抢中断 —— 方向完全偏了。
 *
 *   跟着 RAM_START 走之后，这一处再也不可能和链接地址分叉。
 */

#define CONFIG_RAMBANK1_ADDR      CONFIG_RAM_START
#define CONFIG_RAMBANK1_SIZE      CONFIG_RAM_SIZE

/* ⚠️ RAM_START + RAM_SIZE 不能跨过 0x48400000。
 * 板上 bdinfo 显示 DRAM 分两段：
 *   bank0  0x40200000 .. 0x48400000  (130 MB)
 *   bank1  0x49400000 .. 0x100000000
 * 中间 0x48400000..0x49400000 这 16MB 是 OP-TEE(BL32) 的安全保留区，
 * 不属于任何 bank，映射并访问会被 TZC 拦截。
 *
 *   nsh      0x40480000 + 64MB = 0x44480000  落在 bank0 内 ✓
 *   amp-dual 0x4a400000 + 64MB = 0x4e400000  落在 bank1 内 ✓
 */

/* 外设寄存器区。RK3576 的外设都在低位地址段：
 *
 *   0x22000000  PCIe0        0x26000000  GRF 群（sys/ioc/pmu…）
 *   0x23000000  USB DWC3     0x27200000  CRU
 *   0x27300000  I2C0         0x27310000  UART1
 *   0x27320000  GPIO0        0x2a701000  ★ GIC
 *   0x2ac40000  I2C1-8       0x2ad40000  ★ UART0（M1 调试口）
 *   0x2ae10000  GPIO1-4      0x2b000000  各类 PHY（最高 0x2b060000）
 *
 * 取 0x20000000 起 192MB，上界 0x2c000000，把上述全部包住，
 * 且与 DDR（0x40000000 起）不重叠。
 *
 * ★ 新增外设地址时先确认它落在本范围内，否则 MMU 不建映射，
 *   访问时直接异常。用 contest/scripts/check-addr.sh 校验。
 */

#define CONFIG_DEVICEIO_BASEADDR  0x20000000
#define CONFIG_DEVICEIO_SIZE      MB(192)

/* U-Boot loads NuttX at this address (kernel_addr_r)
 *
 * ★★ 已由 KICKPI-K7 板上实测确认（2026-08-31）。
 *
 * 出厂 U-Boot 的环境变量以明文存在 eMMC 的 uboot 分区（mmcblk2p2）中，
 * 在板上 Android 的 root shell 里读出：
 *
 *   $ strings /dev/block/mmcblk2p2 | grep -iE "addr_r|scriptaddr"
 *     scriptaddr=0x40500000
 *     kernel_addr_r=0x40400000      <- 本值
 *     fdt_addr_r=0x48300000
 *     ramdisk_addr_r=0x4a200000
 *
 * ★ 注意：这与 U-Boot 主线 include/configs/rk3576_common.h 里的
 *   kernel_addr_r=0x42000000 不同 —— 板厂改过。先前按主线推断的
 *   0x42000000 是错的，照它刷进去会是「完全没有输出且没有任何报错」，
 *   因为 U-Boot 把镜像放在 0x40400000 而代码按 0x42000000 链接。
 *
 *   同样与 RK3568 的 0x02000000、RK3399 的 0x02080000 都不同。
 *
 * 本值必须与三处保持一致，改一处就要改三处：
 *   1. 这里的 CONFIG_LOAD_BASE
 *   2. vendor/rockchip/boards/rk3576/kickpi-k7/configs/nsh/defconfig
 *      的 CONFIG_RAM_START
 *   3. vendor/rockchip/boards/rk3576/kickpi-k7/scripts/dramboot.ld
 *      的第一行 ". ="
 */

#define CONFIG_LOAD_BASE          0x40480000

#define MPID_TO_CLUSTER_ID(mpid)  ((mpid) & ~0xff)

/****************************************************************************
 * Assembly Macros
 ****************************************************************************/

#ifdef __ASSEMBLY__

.macro  get_cpu_id xreg0
  mrs    \xreg0, mpidr_el1
  ubfx   \xreg0, \xreg0, #0, #8
.endm

#endif /* __ASSEMBLY__ */

#endif /* __ARCH_ARM64_INCLUDE_RK3576_CHIP_H */
