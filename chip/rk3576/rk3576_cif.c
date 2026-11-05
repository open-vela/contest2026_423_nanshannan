/****************************************************************************
 * chip/rk3576/rk3576_cif.c
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

/* RK3576 VICAP/CIF —— 取图链路的最后一级：把 CSI-2 收到的像素写进 DDR。
 *
 * ★ 五路输入共用一套寄存器，靠偏移区分
 *
 *   dtb 里有 rkcif_mipi_lvds0..4 五个节点，硬件却只有一个 CIF
 *   @0x27c10000。厂商驱动按 csi_host_idx 加偏移：
 *
 *     idx < 2 : off = idx * 0x200
 *     idx >= 2: off = 0x100 + idx * 0x100
 *
 *   注意前两路的间距是 0x200、之后是 0x100，不是一条等差数列 ——
 *   按等差算的话 idx=3 会差 0x100，写到隔壁那一路上去，不报错。
 *   本板传感器在 mipi3，偏移 0x400。
 *
 * ★ 选非压缩（UNCOMPACT）而不是压缩格式
 *
 *   压缩 RAW12 把两个像素塞进三字节，省 1/4 内存；非压缩每像素占满
 *   16 位。首次点亮选非压缩：显示时取高字节就是灰度，不需要位解包。
 *   少一个环节就少一个可能出错的地方，而 4.2MB 与 3.2MB 的差别在
 *   63MB 的系统里无关紧要。等确认能出图，再谈省内存。
 *
 * ★ 乒乓缓冲两个地址都要给
 *
 *   硬件在 FRM0/FRM1 之间轮换。只配 FRM0 的话第二帧会写到地址 0 ——
 *   那是 DDR 起始，会把别的东西冲掉，而且不报错。即便只想要一帧，
 *   两个地址也都要填成合法值。
 *
 * 寄存器与配置流程出处：厂商
 * drivers/media/platform/rockchip/cif/{regs.h,capture.c,dev.c,hw.c}
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/init.h>
#include <nuttx/signal.h>
#include <debug.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <syslog.h>

#include <nuttx/arch.h>

#include "arm64_internal.h"
#include "rk3576_cru.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_cif.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CIF_BASE                 0x27c10000

/* MIPI0 那一组的寄存器偏移，其余各路在此基础上加 csi_offset */

#define CIF_MIPI_ID0_CTRL0       0x100
#define CIF_MIPI_ID0_CTRL1       0x104
#define CIF_MIPI_CTRL            0x120
#define CIF_MIPI_FRM0_ADDR_Y_ID0 0x124
#define CIF_MIPI_FRM1_ADDR_Y_ID0 0x128
#define CIF_MIPI_VLW_ID0         0x134
#define CIF_MIPI_INTEN           0x174
#define CIF_MIPI_INTSTAT         0x178

/* ID0_CTRL0 位域 —— 出处 RK3576 TRM 6.4 VICAP_MIPIn_ID0_CTRL0。
 *
 * ★ 不能照抄厂商驱动里那套 CSI_* 宏
 *
 *   那套宏是老芯片的位布局，RK3576 重排过。照抄的后果不是编不过，
 *   而是**每一位都落在别的字段上**：第一版我把 bit2 置了 1，老布局里
 *   那是数据格式的一部分，RK3576 上却是 sw_drop_frame_en —— 等于命令
 *   硬件把每一帧都丢掉，且不报任何错。parse_type 也因此填成了 raw10。
 */

#define CIF_ID_CAP_EN            (1u << 0)    /* sw_cap_en_id0        */
#define CIF_ID_CROP_EN           (1u << 1)    /* sw_crop_en_id0       */
#define CIF_ID_DROP_FRAME_EN     (1u << 2)    /* sw_drop_frame_en_id0 */
#define CIF_ID_DMA_EN            (1u << 3)    /* sw_dma_en_id0        */
#define CIF_ID_PARSE_SHIFT       4            /* sw_parse_type_id0    */
#define CIF_ID_PARSE_RAW12       0x2
#define CIF_ID_WRDDR_SHIFT       8            /* sw_wrddr_type_id0    */
#define CIF_ID_WRDDR_UNCOMPACT   0x1
#define CIF_ID_WRDDR_COMPACT     0x0   /* 打包 RAW12，未使用；见下面的实验记录 */

/* ★ 帧尾截断与数据量无关 —— 用 COMPACT 做过对照实验。
 *
 *   把写 DDR 的格式从 UNCOMPACT（16 位/像素，3864 字节/行）换成 COMPACT
 *   （打包 RAW12，2898 字节/行），数据量少 25%，而截断行**一行不差地
 *   停在 768**。
 *
 *   这条否定结果比任何一个正面猜测都有价值：它把"带宽/吞吐不足"整类
 *   解释一次性排除掉了。CIF 是以固定速率在走 —— 每约 2.06 个传感器行
 *   周期写一行，与每行搬多少字节无关。要找的是一个**速率分频**，
 *   不是一条更宽的通路。
 */
#define CIF_ID_ALIGN_HIGH        (1u << 11)   /* raw12 落在 [15:4]    */

/* ID0_CTRL1 位域。★ 它**不是**宽高寄存器。
 *
 *   厂商驱动里有一句 write(CTRL1, width | height << 16)，那是老芯片的
 *   布局。RK3576 的 CTRL1 是虚通道(1:0)与数据类型(7:2)；把宽高写进去
 *   等于设了个乱数据类型、还顺手开了 HDR 模式。
 *
 *   ★ 但"CTRL1 不是宽高寄存器"不等于"没有宽高寄存器"。
 *
 *     这里原先据此推断 RK3576 靠 DMA 自适应、只要跨距和地址就够了。
 *     那是错的，而且错得很贵：DMA 不知道该在第几行收手，就一直往下写，
 *     冲出 kmm 分配的那块缓冲、踩烂堆的空闲链表。现象是**下一条要
 *     malloc 的命令**崩在 mm_malloc 里 —— 崩的地方离肇事者十万八千里。
 *
 *     厂商内核 capture.c 里写得很清楚，RK3576 只是把这个寄存器挪了地方：
 *
 *       if (dev->chip_id < CHIP_RK3576_CIF)
 *           write(get_reg_index_of_id_ctrl1(id), width | height << 16);
 *       else
 *           write(CIF_REG_MIPI_SET_SIZE_ID0 + id, width | height << 16);
 *
 *     regs.h: CSI_MIPI0_SET_FRAME_SIZE_ID0_RK3576 = 0x1A0
 *             CSI_MIPI0_ID0_CROP_START_RK3576     = 0x190
 */

#define CIF_ID_VC_SHIFT          0            /* sw_vc_id0, 2 位 */
#define CIF_ID_DT_SHIFT          2            /* sw_dt_id0, 6 位 */

/* RK3576 专有：裁剪起点与帧尺寸。厂商在 RK3576 上无条件开 CROP_EN
 * 并写这两个寄存器，DMA 的搬运边界就是由它们定的。
 */

#define CIF_MIPI_ID0_CROP_START  0x190
#define CIF_MIPI_SET_SIZE_ID0    0x1a0

/* 通路级控制 VICAP_MIPIn_CTRL：bit0 是这条通路的总开关，复位为 0。
 * 不置位的话各 id 自己的 cap_en 都不起作用。
 */

#define CIF_PATH_CAP_EN          (1u << 0)

/* ★ DMA 的水位流控。缺了它高帧率下会丢帧尾。
 *
 *   出处 kernel-6.1 .../cif/capture.c，RK3576 走的是这一支：
 *
 *     if (dev->chip_id > CHIP_RK3562_CIF) {
 *         val = CIF_MIPI_LVDS_SW_WATER_LINE_ENABLE          // bit0
 *             | (CIF_MIPI_LVDS_SW_WATER_LINE_25 << 19);     // bits[21:20]
 *         rkcif_write_register(dev, CIF_REG_MIPI_LVDS_CTRL, val);
 *     }
 *
 *   regs.h: CIF_MIPI_LVDS_SW_WATER_LINE_25 = (0x2 << 1)，再左移 19 位
 *   落在 [21:20]。它告诉 CIF：FIFO 占用超过这个水位就向总线发紧急请求。
 *
 *   ★ 不设它的后果只在高带宽下才显现，非常有迷惑性：
 *
 *     30fps（127MB/s）  整帧写满，实测 0% 零像素、写到第 1095 行
 *     60fps（254MB/s）  固定截断在第 768 行，后面 30% 全是零
 *
 *     屏上就是"画面区下部一块黑，而且怎么调都在"。因为它不是随机的
 *     半帧，是每一帧都停在同一行 —— 我一度以为是抓到了残帧，往"丢弃
 *     首帧"的方向修，方向完全错了。**固定的截断位置说明是带宽/流控，
 *     随机的截断位置才是时序。**
 */

#define CIF_PATH_WATER_LINE_EN   (1u << 0)
#define CIF_PATH_WATER_LINE_25   (0x2u << 20)

/* VLW：低 20 位是跨距，bit31 是地址强制更新（自清）。 */

#define CIF_VLW_MASK             0x000fffffu
#define CSI_ALL_ERROR_INTEN      (0x1f << 16)

/* 帧结束中断位。出处 kernel-6.1 .../cif/regs.h：
 *
 *   #define CSI_FRAME0_END_INTEN(id)  (0x1 << ((id) * 2 + 8))
 *   #define CSI_FRAME1_END_INTEN(id)  (0x1 << ((id) * 2 + 9))
 *   #define CSI_ALL_FRAME_END_INTEN   (0xff << 8)
 *
 * ★ 原先 INTEN 只写了 CSI_ALL_ERROR_INTEN，也就是**只解开了错误位**。
 *   帧结束位在 8..15，一直被屏蔽着，于是 INTSTAT 永远读出 0 —— 那不是
 *   "帧没有结束"，是我们没让硬件报。取图只好盲等一个 mdelay(300)，
 *   既不知道等到了没有，也对不齐帧边界。
 */

#define CIF_INT_FRAME0_END(id)   (1u << ((id) * 2 + 8))
#define CIF_INT_FRAME1_END(id)   (1u << ((id) * 2 + 9))
#define CIF_INT_FRAME_END_ID0    (CIF_INT_FRAME0_END(0) | CIF_INT_FRAME1_END(0))

/* MIPI CSI-2 标准数据类型 */

#define MIPI_CSI2_DT_RAW12       0x2c

/* 时钟门控，逐条对着 clk-rk3576.c 抄：
 *
 *   GATE(ACLK_VICAP,      ... CLKGATE_CON(53), 7)
 *   GATE(HCLK_VICAP,      ... CLKGATE_CON(53), 8)
 *   GATE(CLK_VICAP_I0CLK, ... CLKGATE_CON(59), 1)   ← 每路一个，
 *   GATE(CLK_VICAP_I1CLK, ... CLKGATE_CON(59), 2)      从 bit1 起顺排
 *   ...
 *   GATE(CLK_VICAP_I4CLK, ... CLKGATE_CON(59), 5)
 *
 * ★ 总线时钟和输入时钟不在同一个 CON 里，别想当然。
 *   开错路的输入时钟，现象是"配置全对但一帧不来"，与"线上没数据"
 *   完全一样，事后分不开。
 */

#define CIF_BUS_GATE_CON         53
#define CIF_BUS_GATE_ACLK        7
#define CIF_BUS_GATE_HCLK        8
#define CIF_ICLK_GATE_CON        59

/* aclk_vi_root：CLKSEL_CON(128)，mux 在 bit5 宽 3，分频在 bit0 宽 5。
 * 父时钟表 gpll_spll_isppvtpll_bpll_lpll_p 的第 0 项是 gpll（1188MHz）。
 */

#define CIF_ACLK_SEL_CON         128
#define CIF_ACLK_MUX_SHIFT       5
#define CIF_ACLK_MUX_WIDTH       3
#define CIF_ACLK_MUX_GPLL        0
#define CIF_ACLK_DIV_SHIFT       0
#define CIF_ACLK_DIV_WIDTH       5
#define CIF_ACLK_DIV_2           2

/* dclk_vicap：CLKSEL_CON(129)，mux 在 bit5 宽 1（0=gpll 1=cpll），
 * 分频在 bit0 宽 5，门控 CLKGATE_CON(53) bit6。
 *
 * ★ 这是 CIF 的数据/像素时钟，决定它每秒能处理多少像素 —— 与 aclk
 *   （AXI 总线时钟）是两回事，两个都要配。
 */

#define CIF_DCLK_SEL_CON         129
#define CIF_DCLK_MUX_SHIFT       5
#define CIF_DCLK_MUX_WIDTH       1
#define CIF_DCLK_MUX_GPLL        0
#define CIF_DCLK_DIV_SHIFT       0
#define CIF_DCLK_DIV_WIDTH       5
#define CIF_DCLK_DIV             2      /* gpll 1188 / 2 = 594MHz，与复位值一致 */
#define CIF_DCLK_GATE_BIT        6
#define CIF_ICLK_GATE_BIT(n)     (1 + (n))

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: cif_csi_offset
 *
 * Description:
 *   某一路 CSI 输入在 CIF 寄存器空间里的偏移。
 *   ★ 前两路间距 0x200、之后 0x100，不是等差数列。
 *
 ****************************************************************************/

static uint32_t cif_csi_offset(int host)
{
  return (host < 2) ? (uint32_t)host * 0x200
                    : 0x100 + (uint32_t)host * 0x100;
}

static inline void cif_putreg(uint32_t off, uint32_t val)
{
  putreg32(val, CIF_BASE + off);
}

static inline uint32_t cif_getreg(uint32_t off)
{
  return getreg32(CIF_BASE + off);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_cif_start
 ****************************************************************************/

int rk3576_cif_start(int host, uintptr_t buf0, uintptr_t buf1,
                     int width, int height)
{
  uint32_t off;
  uint32_t ctrl0;
  uint32_t stride;
  uint32_t before;

  if (host < 0 || host > 4 || width <= 0 || height <= 0 ||
      buf0 == 0 || buf1 == 0)
    {
      return -EINVAL;
    }

  off = cif_csi_offset(host);

  /* 1) 时钟：VICAP 的总线时钟 + 这一路对应的输入时钟。
   *    输入时钟按路分（i0clk..i4clk），开错路的现象是"配置都对但
   *    一帧都不来" —— 与"没数据"分不开，所以这里按 host 号来开。
   */

  rk3576_clk_gate(CIF_BUS_GATE_CON, CIF_BUS_GATE_ACLK, true);
  rk3576_clk_gate(CIF_BUS_GATE_CON, CIF_BUS_GATE_HCLK, true);
  rk3576_clk_gate(CIF_ICLK_GATE_CON, CIF_ICLK_GATE_BIT(host), true);

  /* ★ CIF 的 AXI 时钟必须自己配，只开门控是不够的。
   *
   *   ACLK_VICAP 只是 aclk_vi_root 上的一个门控，频率由 aclk_vi_root 决定：
   *
   *     clk-rk3576.c:
   *       COMPOSITE(ACLK_VI_ROOT, "aclk_vi_root",
   *                 gpll_spll_isppvtpll_bpll_lpll_p,
   *                 CLK_IS_CRITICAL,
   *                 RK3576_CLKSEL_CON(128), 5, 3, MFLAGS, 0, 5, DFLAGS,
   *                 RK3576_CLKGATE_CON(53), 0, GFLAGS)
   *
   *   我们一直用的是复位默认值。实测下来 CIF 的写入带宽只有约 179MB/s
   *   （每行 3864 字节要 21.6us，而传感器每 10.53us 就送一行），于是
   *   帧周期短于 23.7ms 时帧尾直接被截断：
   *
   *     VMAX 3165（33.3ms）  写满 1096 行
   *     VMAX 2110（22.2ms）  只到 1033 行
   *     VMAX 1583（16.7ms）  只到 768 行
   *
   *   截断位置随帧周期线性变化、且完全可复现 —— 这正是"写得不够快"的
   *   指纹。随机的截断才是时序问题，固定的截断是带宽问题。
   *
   *   选 gpll(=1188MHz) 二分频 594MHz。把当前值和设定值都打出来，
   *   免得下次又把"默认值"当成"配置过了"。
   */

  before = getreg32(RK3576_CRU_ADDR +
                    RK3576_CRU_CLKSEL_CON(CIF_ACLK_SEL_CON));

  rk3576_clk_setmux(CIF_ACLK_SEL_CON, CIF_ACLK_MUX_SHIFT,
                    CIF_ACLK_MUX_WIDTH, CIF_ACLK_MUX_GPLL);
  rk3576_clk_setmux(CIF_ACLK_SEL_CON, CIF_ACLK_DIV_SHIFT,
                    CIF_ACLK_DIV_WIDTH, CIF_ACLK_DIV_2 - 1);

  syslog(LOG_INFO,
         "CIF: aclk_vi_root CLKSEL_CON(%d) 0x%08" PRIx32 " -> 0x%08" PRIx32
         "（选 gpll/%d）\n",
         CIF_ACLK_SEL_CON, before,
         getreg32(RK3576_CRU_ADDR +
                  RK3576_CRU_CLKSEL_CON(CIF_ACLK_SEL_CON)),
         CIF_ACLK_DIV_2);

  /* ★ dclk_vicap —— CIF 的数据时钟，与 aclk 是两回事。
   *
   *   ★ 实测结论：这两个时钟都不是瓶颈，复位值本来就是对的。
   *
   *     aclk_vi_root  CLKSEL_CON(128)=0x201 → gpll/2 = 594MHz
   *     dclk_vicap    CLKSEL_CON(129)=0x041 → gpll/2 = 594MHz
   *
   *     我一度把 dclk 改成 gpll/3=396MHz 想验证它是不是限制项 —— 截断
   *     位置一点没变（768/548 完全一致），反过来证明了它有富余。这里
   *     显式写成与复位值相同的 594MHz 并把前后值打出来，是为了让"已经
   *     确认过、不是这里"这个结论留在代码里，下次不必重查。
   */

  before = getreg32(RK3576_CRU_ADDR +
                    RK3576_CRU_CLKSEL_CON(CIF_DCLK_SEL_CON));

  rk3576_clk_gate(CIF_BUS_GATE_CON, CIF_DCLK_GATE_BIT, true);
  rk3576_clk_setmux(CIF_DCLK_SEL_CON, CIF_DCLK_MUX_SHIFT,
                    CIF_DCLK_MUX_WIDTH, CIF_DCLK_MUX_GPLL);
  rk3576_clk_setmux(CIF_DCLK_SEL_CON, CIF_DCLK_DIV_SHIFT,
                    CIF_DCLK_DIV_WIDTH, CIF_DCLK_DIV - 1);

  syslog(LOG_INFO,
         "CIF: dclk_vicap CLKSEL_CON(%d) 0x%08" PRIx32 " -> 0x%08" PRIx32
         "（选 gpll/%d = %dMHz）\n",
         CIF_DCLK_SEL_CON, before,
         getreg32(RK3576_CRU_ADDR +
                  RK3576_CRU_CLKSEL_CON(CIF_DCLK_SEL_CON)),
         CIF_DCLK_DIV, 1188 / CIF_DCLK_DIV);

  /* 2) 非压缩 RAW12：每像素 16 位，行跨距 width*2。 */

  /* ★ 行跨距要对齐到 256 字节。
   *
   *   出处 kernel-6.1 .../cif/capture.c：
   *     channel->virtual_width =
   *       ALIGN(channel->width * fmt->raw_bpp / 8, 256);
   *
   *   我们原来直接写 width * 2 = 3864。3864 / 256 = 15.09 —— 每一行的
   *   起始地址都落在 256 边界之外，DMA 的每笔突发都跨边界，效率大约
   *   减半。实测正是如此：CIF 只能以 21.6us/行 的速度写，而传感器每
   *   10.53us 就送一行，帧周期短于 23.7ms 就写不完、帧尾被截断。
   *
   *   ★ 硬件不会为此报错。CIF 有 BANDWIDTH_LACK 中断位（bit19，我们也
   *     使能了），但它一次都没置位 —— 因为这不是"带宽不够"，是每笔传输
   *     都在做无用功。**没有报错不等于没有问题**：我一度因为它不报错
   *     就把方向从带宽转开了。
   *
   *   对齐后每行 4096 字节（有 232 字节填充），缓冲要按这个跨距分配，
   *   读取方也要按它索引 —— 跨距一旦不等于宽度，所有"行号 x 宽度"的
   *   算法都要改成"行号 x 跨距"。
   *
   *   ★ 实测：对齐本身没有解决帧尾截断（1095/768/548 与对齐前逐行相同）。
   *     这个改动仍然保留 —— 它与厂商一致、是正确的做法 —— 但截断另有
   *     原因，不要因为"改了跨距"就以为这条已经解决。
   *
   *     顺带否定了我自己"跨距不对齐导致突发跨界、吞吐减半"的推断：
   *     推断听起来合理、数量级也对得上（正好慢一倍），但实测不成立。
   *     **能解释现象的假说不止一个，只有实验能选出哪个是真的。**
   */

  stride = ((uint32_t)width * 2 + 255) & ~255u;
  if ((stride & ~CIF_VLW_MASK) != 0)
    {
      return -EINVAL;
    }

  /* 3) 乒乓的两个地址都要给，见文件头说明。TRM 要求双字对齐。 */

  cif_putreg(off + CIF_MIPI_FRM0_ADDR_Y_ID0, (uint32_t)buf0);
  cif_putreg(off + CIF_MIPI_FRM1_ADDR_Y_ID0, (uint32_t)buf1);
  cif_putreg(off + CIF_MIPI_VLW_ID0, stride);

  /* 4) 虚通道与数据类型。RAW12 的 CSI-2 数据类型是 0x2c。
   *    这里没有宽高要配，原因见 CIF_ID_VC_SHIFT 上方的说明。
   */

  cif_putreg(off + CIF_MIPI_ID0_CTRL1,
             (0u << CIF_ID_VC_SHIFT) |
             ((uint32_t)MIPI_CSI2_DT_RAW12 << CIF_ID_DT_SHIFT));

  /* ★ 帧尺寸必须写，它是 DMA 的**边界**。
   *
   *   不写的话 DMA 不知道在第几行收手，会一直写到缓冲外面去 ——
   *   而缓冲是 kmm 分配的，越界就是把堆的空闲链表写烂。
   *
   *   裁剪起点取 (0,0)，即整帧不裁；但 CROP_EN 仍要置位，厂商在
   *   RK3576 上是无条件开的，尺寸寄存器要靠它生效。
   */

  cif_putreg(off + CIF_MIPI_ID0_CROP_START, 0);
  cif_putreg(off + CIF_MIPI_SET_SIZE_ID0,
             (uint32_t)width | ((uint32_t)height << 16));

  /* 5) 错误中断使能。不挂 ISR、靠轮询读 INTSTAT，但使能位必须打开，
   *    否则状态位不会置起来，读到的永远是 0，会被当成"没有错误"。
   */

  cif_putreg(off + CIF_MIPI_INTEN,
             CSI_ALL_ERROR_INTEN | CIF_INT_FRAME_END_ID0);

  /* 6) 先配 id0 的格式，再开通路总开关。反过来的话通路已经在收，
   *    而 id0 的格式还没配好，头几帧会按错误格式落盘。
   */

  ctrl0 = CIF_ID_CAP_EN | CIF_ID_DMA_EN | CIF_ID_CROP_EN |
          CIF_ID_ALIGN_HIGH |
          ((uint32_t)CIF_ID_PARSE_RAW12     << CIF_ID_PARSE_SHIFT) |
          ((uint32_t)CIF_ID_WRDDR_UNCOMPACT << CIF_ID_WRDDR_SHIFT);

  cif_putreg(off + CIF_MIPI_ID0_CTRL0, ctrl0);
  cif_putreg(off + CIF_MIPI_CTRL,
             CIF_PATH_CAP_EN | CIF_PATH_WATER_LINE_EN |
             CIF_PATH_WATER_LINE_25);

  /* ★ 这里不做回读。
   *
   *   TRM 把 VICAP 这批寄存器全部标注为 "W"（只写），读回来的值没有
   *   意义。我一开始加了回读，看到 CTRL1 高 16 位是 0 就断定"高度没写
   *   进去" —— 那个结论是假的，读到的根本不是我写进去的内容，差点顺着
   *   这条错线索去查 DMA。
   *
   *   只写寄存器只能靠**行为**（有没有数据落盘）来验证，不能靠回读。
   */

  syslog(LOG_INFO,
         "CIF: host%d off=0x%03" PRIx32 " %dx%d stride=%" PRIu32
         " buf=0x%08lx/0x%08lx CTRL0=0x%08" PRIx32 "\n",
         host, off, width, height, stride,
         (unsigned long)buf0, (unsigned long)buf1, ctrl0);

  return OK;
}

/****************************************************************************
 * Name: rk3576_cif_stop
 ****************************************************************************/

int rk3576_cif_stop(int host)
{
  uint32_t off;

  if (host < 0 || host > 4)
    {
      return -EINVAL;
    }

  off = cif_csi_offset(host);
  cif_putreg(off + CIF_MIPI_ID0_CTRL0, 0);
  cif_putreg(off + CIF_MIPI_INTEN, 0);
  return OK;
}

/****************************************************************************
 * Name: rk3576_cif_clear_status
 *
 * Description:
 *   清中断状态位（写 1 清）。
 *
 *   ★ CIF 的中断是电平触发的（dtsi: IRQ_TYPE_LEVEL_HIGH），在 ISR 里
 *     不清标志中断线就一直拉着、立刻重入。轮询取图（wait_frame）时
 *     清标志是顺带做的，走中断路径就必须有这个独立接口。
 *
 ****************************************************************************/

void rk3576_cif_clear_status(int host, uint32_t status)
{
  if (host < 0 || host > 4)
    {
      return;
    }

  cif_putreg(cif_csi_offset(host) + CIF_MIPI_INTSTAT, status);
}

/****************************************************************************
 * Name: rk3576_cif_status
 *
 * Description:
 *   读中断状态并清掉。返回值是清掉之前的原值。
 *
 *   ★ 读完就清，是为了让下一次读反映的是"这段时间内"的情况。
 *     不清的话状态位一直挂着，第二次读还是那个值，会误以为错误在
 *     持续发生。
 *
 ****************************************************************************/

uint32_t rk3576_cif_status(int host)
{
  uint32_t off;
  uint32_t stat;

  if (host < 0 || host > 4)
    {
      return 0;
    }

  off  = cif_csi_offset(host);
  stat = cif_getreg(off + CIF_MIPI_INTSTAT);
  cif_putreg(off + CIF_MIPI_INTSTAT, stat);

  /* ★ 只报错误位。这个函数在 V4L2 的帧结束中断里每帧调一次，
   *   无条件打印就是每秒 30 行中断上下文的 syslog —— 板上实测把
   *   agent 的拍照线程饿住了（串口被刷满，工具调用一直不返回）。
   */

  if ((stat & CSI_ALL_ERROR_INTEN) != 0)
    {
      syslog(LOG_WARNING,
             "CIF: host%d INTSTAT=0x%08" PRIx32 "（含错误位）\n",
             host, stat);
    }

  return stat;
}

/****************************************************************************
 * Name: rk3576_cif_wait_frame
 *
 * Description:
 *   轮询 INTSTAT 等一帧写完，返回刚写完的是哪个缓冲（0 或 1）。
 *
 *   ★ 为什么不是延时一段时间就读
 *
 *     盲等的问题不在于等得够不够久，而在于**不知道等到的是什么**：
 *     读到的可能是写了一半的一帧，而撕裂的画面看起来很像图像处理写错
 *     了，会把排查引到完全无关的方向去。等帧结束标志则是确定的 ——
 *     标志置位就说明那一帧整帧都已经落到 DDR 了。
 *
 *     顺带还得到一个盲等给不了的东西：**是哪个缓冲写完的**。硬件在
 *     FRM0/FRM1 之间乒乓，写完 0 号就立刻开始写 1 号；读错一个，读到的
 *     就是正在被 DMA 改写的那块。
 *
 *   ★ 这里用轮询而不是接中断。
 *
 *     取一帧是个同步操作，调用方本来就在等；接中断要多一个 ISR、一个
 *     信号量和一套生命周期管理，而收益仅仅是这段等待里 CPU 可以做别的。
 *     等连续预览真的需要了再上中断，现在上是在没有需求的地方增加状态。
 *
 * Input Parameters:
 *   host       - CSI host 号
 *   want       - 只等哪一个缓冲的帧结束：0、1，或 -1 表示两个都行
 *   timeout_ms - 最长等多久
 *   status     - 非空时返回消费掉的 INTSTAT 原值（含错误位）

 *
 *   ★ want 存在的理由
 *
 *     实测发现 FRAME1_END 会在 1 号缓冲**并没有被写过**的情况下置位：
 *     那一轮读回来整块全是 memset 留下的 0，而同样的配置下 FRAME0_END
 *     那几轮数据都是好的。乒乓的换页时机与标志语义还没查清楚。
 *
 *     但单帧抓取本来就不需要乒乓 —— 指定只等 0 号，就绕开了这块没搞懂
 *     的地方，而且更合理：我们读 0 号的时候硬件正在写 1 号，天然不冲突。
 *     等真要做连续预览时再回来把乒乓弄明白，那时才有需求撑着。
 *
 * Returned Value:
 *   0 或 1 表示刚写完的缓冲序号；-ETIMEDOUT 表示超时没等到。
 *
 ****************************************************************************/

int rk3576_cif_wait_frame(int host, int want, int timeout_ms,
                          uint32_t *status)
{
  uint32_t off;
  uint32_t stat;
  uint32_t mask;
  int elapsed;

  if (host < 0 || host > 4 || timeout_ms <= 0 || want < -1 || want > 1)
    {
      return -EINVAL;
    }

  mask = (want == 0) ? CIF_INT_FRAME0_END(0) :
         (want == 1) ? CIF_INT_FRAME1_END(0) : CIF_INT_FRAME_END_ID0;

  off = cif_csi_offset(host);

  /* 先把旧的标志清掉，否则等到的可能是**上一帧**留下的置位，
   * 那就等于没等 —— 而且会稳定地读到一个更旧的缓冲。
   */

  cif_putreg(off + CIF_MIPI_INTSTAT,
             cif_getreg(off + CIF_MIPI_INTSTAT));

  for (elapsed = 0; elapsed < timeout_ms; elapsed++)
    {
      stat = cif_getreg(off + CIF_MIPI_INTSTAT);

      if ((stat & mask) != 0)
        {
          cif_putreg(off + CIF_MIPI_INTSTAT, stat);

          if (status != NULL)
            {
              *status = stat;
            }

          if (want >= 0)
            {
              return want;
            }

          return (stat & CIF_INT_FRAME1_END(0)) ? 1 : 0;
        }

      /* ★ 任务上下文里要真睡，不能 up_mdelay 空转。
       *
       *   空转的等待线程从不让出 CPU。界面相机页的取帧线程和 LVGL 主循环
       *   同优先级（RR），挤到同一个核上时 LVGL 要等满一个时间片
       *   （CONFIG_RR_INTERVAL=200ms）才轮到 —— 实测取帧 30fps、界面只刷
       *   5fps，主循环 3 秒只转 15 次。
       */

      if (OSINIT_OS_READY() && !up_interrupt_context())
        {
          nxsig_usleep(1000);
        }
      else
        {
          up_mdelay(1);
        }
    }

  if (status != NULL)
    {
      *status = cif_getreg(off + CIF_MIPI_INTSTAT);
    }

  syslog(LOG_ERR,
         "CIF: host%d 等帧结束超时 %d ms，INTSTAT=0x%08" PRIx32 "\n",
         host, timeout_ms, cif_getreg(off + CIF_MIPI_INTSTAT));
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3576_cif_set_buffer
 *
 * Description:
 *   在 DMA 运行中把某一个乒乓槽的目标地址换掉。
 *
 *   ★ 为什么必须能换
 *
 *     只有两块缓冲、且不换页时，硬件写完 FRM0 就立刻开始写 FRM1，写完
 *     FRM1 又回头写 FRM0 —— 而我们渲染一帧要比传感器出一帧慢，于是
 *     还没读完，硬件已经开始覆盖同一块内存了。读出来的就是半新半旧的
 *     撕裂画面，而撕裂看起来很像图像处理写错了。
 *
 *     厂商内核（capture.c 的 rkcif_assign_new_buffer_pingpong）正是在每
 *     次帧结束中断里把刚完成的那个槽换成队列里的另一块，让正在被消费
 *     的缓冲永远不是 DMA 的目标。这里提供同样的能力，由上层维护第三块
 *     备用缓冲。
 *
 *   ★ 可以在运行中写。
 *
 *     这批寄存器不是影子寄存器（TRM 标注为只写，没有 CFG_DONE 之类的
 *     提交机制），厂商也是在中断上下文里直接写。换的是**下一次**才会
 *     用到的那个槽，当前正在写的那个槽不受影响。
 *
 * Input Parameters:
 *   host - CSI host 号
 *   slot - 0 或 1，对应 FRM0 / FRM1
 *   addr - 新的缓冲物理地址
 *
 ****************************************************************************/

int rk3576_cif_set_buffer(int host, int slot, uintptr_t addr)
{
  uint32_t off;

  if (host < 0 || host > 4 || slot < 0 || slot > 1 || addr == 0)
    {
      return -EINVAL;
    }

  off = cif_csi_offset(host);

  cif_putreg(off + (slot == 0 ? CIF_MIPI_FRM0_ADDR_Y_ID0
                              : CIF_MIPI_FRM1_ADDR_Y_ID0),
             (uint32_t)addr);
  return OK;
}
