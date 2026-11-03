/****************************************************************************
 * vendor/rockchip/boards/rk3576/kickpi-k7/scripts/dramboot.ld
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

#include <nuttx/config.h>

OUTPUT_ARCH(aarch64)

ENTRY(__start)

PHDRS
{
  text PT_LOAD ;
}

SECTIONS
{
  /* 链接地址 0x40480000 —— 必须等于 U-Boot 实际搬运后的执行地址。
   *
   * ★ 板上实测的完整加载链（2026-08-31）：
   *
   *     mmc read -> 0x40400000        (kernel_addr_r，eMMC uboot 分区
   *                                    明文环境变量读出；主线
   *                                    rk3576_common.h 写 0x42000000，
   *                                    板厂改过，以实测为准)
   *     U-Boot 按 ARM64 Linux Image 协议搬运：
   *         DRAM 基址 0x40000000 + Image 头 text_offset 0x480000
   *       = 0x40480000                ← CPU 真正开始取指的地址
   *
   *   串口原文：
   *     == DO RELOCATE == Kernel from 0x40400000 to 0x40480000
   *
   *   text_offset 位于 Image 头偏移 8（8 字节小端），NuttX 在
   *   arch/arm64/src/common/arm64_head.S 里硬编码为 0x480000。
   *
   * ★★ 曾经踩过的坑：一度以为「Rockchip U-Boot 2017.09 不搬运，
   *    原地执行」，于是把链接地址填成 0x40400000。表面上能跑完
   *    汇编启动和 arm64_chip_boot()，因为搬运后 0x40400000 上还残留
   *    着一份内容完全相同的原始副本，所有 .data/.bss/向量表的绝对
   *    引用都打在那份副本上，看起来一切正常。
   *    真正的爆点在 nx_start()：堆从 _ebss（按 0x40400000 算，约
   *    0x4044C000）起分配，而正在执行的代码就在 0x40480000 —— 堆一
   *    扩张就把自己覆盖掉，表现为 C 运行时静默挂死、无任何异常报告。
   *    结论：链接地址要填搬运后的地址，不是 mmc read 的地址。
   *
   * ★★★ RK3576 的 DRAM 从 0x40000000 起，与 RK3568（从 0 起，
   *    kernel_addr_r=0x02000000）和 RK3399（0x02080000）都不同。
   *    RK3576 把外设放在低位地址（0x22000000~0x2b060000），
   *    照抄 RK3568 的 0x02000000 会落进外设区，表现是完全没有输出。
   *
   *   BootROM -> TPL(ddr.bin) -> SPL -> U-Boot -> booti -> 搬运 -> 本镜像
   *
   * ★ 2026-09-12 起这里直接用 CONFIG_RAM_START，不再写字面量。
   *
   *   本文件在链接前会过一遍 cpp（arch/arm64/src/Makefile 里
   *   `$(addsuffix .tmp,$(ARCHSCRIPT))` 那条规则），所以能 include
   *   nuttx/config.h。以前链接地址在三处各写一遍（chip.h 的
   *   CONFIG_LOAD_BASE、defconfig 的 CONFIG_RAM_START、本文件），
   *   靠 scripts/check-addr.sh 事后比对；现在只剩一处，"改漏一个地方"
   *   这一整类故障从根上没有了。
   *
   *   这件事在 AMP 阶段变得必要：openvela 单跑时链在 0x40480000，
   *   要和 Linux 共存时得挪到保留区，两个配置之间只应该差 defconfig
   *   里的一行，不该还要改链接脚本。
   *
   * 地址填错的表现是串口完全没有输出且无任何报错 —— CPU 跳到的位置
   * 根本不是你的代码。上板后先用 md 确认镜像落点再 booti。
   */
  . = CONFIG_RAM_START;
  _start = .;
  .text : {
        _stext = .;            /* Text section */
       *(.start .start.*)      /* Place __start here */
       *(.text)
       *(.text.cold)
       *(.text.unlikely)
       *(.fixup)
       *(.gnu.warning)
  } :text = 0x9090

  . = ALIGN(4096);

  .init_section : {
        _sinit = ABSOLUTE(.);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array EXCLUDE_FILE(*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o) .ctors))
        _einit = ABSOLUTE(.);
  }

  . = ALIGN(4096);

  .vector : {
        _vector_start = .;
        KEEP(*(.exc_vector_table))
        KEEP(*(".exc_vector_table.*"))
        KEEP(*(.vectors))
       _vector_end = .;
  } :text
  . = ALIGN(4096);
  _etext = .; /* End_1 of .text */
  _sztext = _etext - _stext;

  . = ALIGN(4096);
  .rodata : {
        _srodata = .;          /* Read-only data */
       *(.rodata)
       *(.rodata.*)
       *(.data.rel.ro)
       *(.data.rel.ro.*)

       /* ★ C++ 异常的栈展开表必须保留。
        *
        *   这里原本把 .eh_frame 放在下面的 /DISCARD/ 里 —— 纯 C 的移植
        *   用不到它，所以一直没暴露。开了 C++ 异常之后，
        *   .gcc_except_table（着陆点表）还在，而 _Unwind_RaiseException
        *   要走的 CFI 描述没了，`throw` 的瞬间展开器找不到信息，直接崩
        *   在 sched_dumpstack。
        *
        *   现象是"cxxtest 前面 vector/map/RTTI 都过，到 Test Exception
        *   崩溃"，很容易被当成 C++ 库没配好 —— 实际上库是全的，是链接
        *   脚本把展开表扔了。**丢弃一个自己用不到的段，代价要等到别人
        *   用到时才显现。**
        */

       *(.eh_frame_hdr)
       __EH_FRAME_BEGIN__ = .;   /* 栈展开表起始（手动注册用） */
       KEEP(*(.eh_frame))

       /* ★ 表尾必须有一个全零的 length 字段作终止符。
        *
        *   __register_frame_info() 注册的是一张**以零长度项结尾**的 FDE
        *   链表，正常由工具链的 crtend.o 提供（它里面就是一个 LONG(0)）。
        *   裸机链接不带 crtbegin/crtend，没人放这个 0 —— 展开器会一路扫
        *   过表尾读到垃圾，表现是异常抛出时崩在展开器内部，和"表根本
        *   不存在"几乎分不开。
        */

       LONG(0);
       __EH_FRAME_END__ = .;     /* 栈展开表结束 */
  } :text
  . = ALIGN(4096);
  _erodata = .;                /* End of read-only data */
  _szrodata = _erodata - _srodata;
  _eronly = .;  /* End of read-only data */

  . = ALIGN(4096);
  .data : {                    /* Data */
        _sdata = .;
       *(.data.page_aligned)
       *(.data)
       . = ALIGN(8);
       *(.data.rel)
       *(.data.rel.*)
       CONSTRUCTORS
  } :text
  _edata = .; /* End+1 of .data */

  .bss : {                     /* BSS */
       . = ALIGN(8);
       _sbss = .;
       *(.bss)
       . = ALIGN(8);
  } :text
  . = ALIGN(4096);
  _ebss = .;
  _szbss = _ebss - _sbss;

  .initstack (NOLOAD) : {    /* INIT STACK */
       _s_initstack = .;
       *(.initstack)
       . = ALIGN(16);
  } :text
  . = ALIGN(4096);
  _e_initstack = . ;
  g_idle_topstack = . ;

  _szdata = _e_initstack - _sdata;

  /* Sections to be discarded */
  /DISCARD/ : {
       *(.exit.text)
       *(.exit.data)
       *(.exitcall.exit)
  }

  /* Stabs debugging sections.  */
  .stab 0 : { *(.stab) }
  .stabstr 0 : { *(.stabstr) }
  .stab.excl 0 : { *(.stab.excl) }
  .stab.exclstr 0 : { *(.stab.exclstr) }
  .stab.index 0 : { *(.stab.index) }
  .stab.indexstr 0 : { *(.stab.indexstr) }
  .comment 0 : { *(.comment) }
}
