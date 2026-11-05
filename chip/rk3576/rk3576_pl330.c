/****************************************************************************
 * chip/rk3576/rk3576_pl330.c
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
 * ARM PL330 (DMA-330) 驱动，实现 NuttX 通用 DMA 接口。
 *
 * ★ 为什么需要它
 *
 *   RK3576 的 SAI 只有三种中断：发送欠载、接收溢出、帧同步错 ——
 *   **没有"FIFO 到水位"中断**。也就是说接收方向没有任何可挂的"数据就绪"
 *   信号：要么忙轮询（32 深的 FIFO 在 48kHz 立体声下 667us 就灌满，轮询
 *   线程稍被抢占就丢样点，而把优先级提上去又会饿死系统），要么上 DMA。
 *   录音要做成能用的，这颗 DMA 绕不过去。
 *
 * ★ PL330 是微码机，不是寄存器机
 *
 *   常见的 DMA 控制器是"填源地址、目的地址、长度，置个使能位"。PL330 不是。
 *   它每条通道跑一段**放在内存里的指令流**，由 DMAC 自己取指执行。所以
 *   本驱动的核心不是写寄存器，而是**生成一段程序**：
 *
 *     DMAMOV CCR/SAR/DAR   设好控制字与两端地址
 *     DMAWFP  peri, BURST  等外设发来突发请求（这一步替代了"数据就绪中断"）
 *     DMALDP  BURST, peri  从外设搬一个突发进内部 FIFO
 *     DMAST                把它写进内存
 *     DMALPEND             回跳，构成循环
 *     DMASEV  ev           一个周期搬完，发事件 → 中断
 *     DMAEND               结束
 *
 *   然后通过调试接口（DBGINST0/1 + DBGCMD）塞一条 DMAGO，让某条通道从这段
 *   程序的地址开始执行。启动路径走调试口这一点很容易看漏 —— 没有别的入口。
 *
 * ★ cache
 *
 *   DMAC 通过自己的 AXI 主口取指和搬数，不经过 CPU 的 D-cache。所以：
 *     - 微码写完必须 up_clean_dcache，否则 DMAC 取到的是旧指令；
 *     - 接收进内存的数据，CPU 读之前必须 up_invalidate_dcache。
 *   本文件只负责微码那一半；数据缓冲的失效由调用方做（通用 dma.h 的注释
 *   也是这么规定的：DMA 模块不做任何 cache 操作）。
 *
 * 出处：ARM DMA-330 TRM（指令编码与状态机），Linux drivers/dma/pl330.c
 *       （寄存器偏移、指令编码、启动时序的可执行参考）。
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/dma/dma.h>
#include <nuttx/mutex.h>
#include <nuttx/spinlock.h>

#include "arm64_internal.h"
#include "rk3576_pl330.h"
#include "rk3576_cru.h"
#include "hardware/rk3576_pl330.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define PL330_MAX_CHAN        8
#define PL330_MCODE_SIZE      128     /* 每条通道的微码缓冲，够用有余 */
#define PL330_CACHE_LINE      64

/* ACLK_DMAC0：CLKGATE_CON(19) bit 1（Linux clk-rk3576.c） */

#define DMAC0_GATE_CON        19
#define DMAC0_GATE_BIT        1

/* DMAWFP / DMALDP / DMASTP 的条件编码 */

#define PL330_COND_SINGLE     0
#define PL330_COND_BURST      1
#define PL330_COND_ALWAYS     2

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_pl330_s;

struct rk3576_pl330_chan_s
{
  struct dma_chan_s        chan;      /* 必须是第一个成员 */
  struct rk3576_pl330_s   *dmac;
  uint8_t                  id;        /* 通道号，也用作事件号 */
  bool                     inuse;
  bool                     cyclic;
  struct dma_config_s      cfg;
  dma_callback_t           callback;
  void                    *arg;
  uintptr_t                membase;   /* 内存侧起始地址（算残余用） */
  size_t                   len;
  size_t                   period;
  uint8_t                 *mcode;
};

struct rk3576_pl330_s
{
  struct dma_dev_s            dev;    /* 必须是第一个成员 */
  uintptr_t                   base;
  bool                        ready;
  bool                        ns;     /* 管理线程是否非安全（CR0 bit2） */
  int                         nchans;
  spinlock_t                  lock;
  struct rk3576_pl330_chan_s  chans[PL330_MAX_CHAN];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int      pl330_config(struct dma_chan_s *chan,
                             const struct dma_config_s *cfg);
static int      pl330_start(struct dma_chan_s *chan,
                            dma_callback_t callback, void *arg,
                            uintptr_t dst, uintptr_t src, size_t len);
static int      pl330_start_cyclic(struct dma_chan_s *chan,
                                   dma_callback_t callback, void *arg,
                                   uintptr_t dst, uintptr_t src,
                                   size_t len, size_t period_len);
static int      pl330_stop(struct dma_chan_s *chan);
static int      pl330_pause(struct dma_chan_s *chan);
static int      pl330_resume(struct dma_chan_s *chan);
static size_t   pl330_residual(struct dma_chan_s *chan);

static struct dma_chan_s *pl330_get_chan(struct dma_dev_s *dev,
                                         unsigned int ident);
static void     pl330_put_chan(struct dma_dev_s *dev,
                               struct dma_chan_s *chan);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct dma_ops_s g_pl330_ops =
{
  .config        = pl330_config,
  .start         = pl330_start,
  .start_cyclic  = pl330_start_cyclic,
  .stop          = pl330_stop,
  .pause         = pl330_pause,
  .resume        = pl330_resume,
  .residual      = pl330_residual,
};

static struct rk3576_pl330_s g_dmac0;

/* 一次性诊断标记：确认路径走通即可，不必每次传输都打。 */

static bool g_go_logged  = false;
static bool g_isr_logged = false;

/* 微码缓冲。DMAC 用自己的 AXI 主口取指，所以必须按 cache 行对齐，
 * 且写完要 clean —— 否则指令还留在 CPU 的写回缓存里，DMAC 取到旧内容。
 */

static uint8_t g_mcode[PL330_MAX_CHAN][PL330_MCODE_SIZE]
               aligned_data(PL330_CACHE_LINE);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t pl330_getreg(struct rk3576_pl330_s *dmac,
                                    unsigned int off)
{
  return getreg32(dmac->base + off);
}

static inline void pl330_putreg(struct rk3576_pl330_s *dmac,
                                unsigned int off, uint32_t val)
{
  putreg32(val, dmac->base + off);
}

/****************************************************************************
 * 微码生成
 *
 * 每个 emit_* 把一条指令写进 buf 并返回它占的字节数。编码逐字节照抄
 * DMA-330 TRM 的指令格式；这类编码没有"看起来对不对"的余地，写错了
 * DMAC 只会报 undefined instruction 故障，所以宁可照抄不要推导。
 ****************************************************************************/

static size_t emit_mov(uint8_t *buf, uint8_t dst, uint32_t val)
{
  buf[0] = PL330_CMD_DMAMOV;
  buf[1] = dst;
  buf[2] = (uint8_t)(val);
  buf[3] = (uint8_t)(val >> 8);
  buf[4] = (uint8_t)(val >> 16);
  buf[5] = (uint8_t)(val >> 24);
  return PL330_SZ_DMAMOV;
}

static size_t emit_wfp(uint8_t *buf, int cond, uint8_t peri)
{
  buf[0] = PL330_CMD_DMAWFP;
  if (cond == PL330_COND_SINGLE)
    {
      buf[0] |= (0 << 1) | (0 << 0);
    }
  else if (cond == PL330_COND_BURST)
    {
      buf[0] |= (1 << 1) | (0 << 0);
    }
  else
    {
      buf[0] |= (0 << 1) | (1 << 0);    /* peripheral（由外设定单/突发） */
    }

  buf[1] = (uint8_t)((peri & 0x1f) << 3);
  return PL330_SZ_DMAWFP;
}

static size_t emit_ldp(uint8_t *buf, int cond, uint8_t peri)
{
  buf[0] = PL330_CMD_DMALDP;
  if (cond == PL330_COND_BURST)
    {
      buf[0] |= (1 << 1);
    }

  buf[1] = (uint8_t)((peri & 0x1f) << 3);
  return PL330_SZ_DMALDP;
}

static size_t emit_stp(uint8_t *buf, int cond, uint8_t peri)
{
  buf[0] = PL330_CMD_DMASTP;
  if (cond == PL330_COND_BURST)
    {
      buf[0] |= (1 << 1);
    }

  buf[1] = (uint8_t)((peri & 0x1f) << 3);
  return PL330_SZ_DMASTP;
}

static size_t emit_ld(uint8_t *buf, int cond)
{
  buf[0] = PL330_CMD_DMALD;
  if (cond == PL330_COND_SINGLE)
    {
      buf[0] |= (0 << 1) | (1 << 0);
    }
  else if (cond == PL330_COND_BURST)
    {
      buf[0] |= (1 << 1) | (1 << 0);
    }

  return PL330_SZ_DMALD;
}

static size_t emit_st(uint8_t *buf, int cond)
{
  buf[0] = PL330_CMD_DMAST;
  if (cond == PL330_COND_SINGLE)
    {
      buf[0] |= (0 << 1) | (1 << 0);
    }
  else if (cond == PL330_COND_BURST)
    {
      buf[0] |= (1 << 1) | (1 << 0);
    }

  return PL330_SZ_DMAST;
}

static size_t emit_lp(uint8_t *buf, int loopreg, unsigned int cnt)
{
  buf[0] = PL330_CMD_DMALP;
  if (loopreg)
    {
      buf[0] |= (1 << 1);
    }

  buf[1] = (uint8_t)(cnt - 1);    /* DMAC 内部会 +1 */
  return PL330_SZ_DMALP;
}

/* forever = true 时不看计数器，无条件回跳 —— 这就是环形传输的外层循环。 */

static size_t emit_lpend(uint8_t *buf, int loopreg, bool forever,
                         int cond, uint8_t bjump)
{
  buf[0] = PL330_CMD_DMALPEND;
  if (loopreg)
    {
      buf[0] |= (1 << 2);
    }

  if (!forever)
    {
      buf[0] |= (1 << 4);
    }

  if (cond == PL330_COND_SINGLE)
    {
      buf[0] |= (0 << 1) | (1 << 0);
    }
  else if (cond == PL330_COND_BURST)
    {
      buf[0] |= (1 << 1) | (1 << 0);
    }

  buf[1] = bjump;
  return PL330_SZ_DMALPEND;
}

static size_t emit_sev(uint8_t *buf, uint8_t ev)
{
  buf[0] = PL330_CMD_DMASEV;
  buf[1] = (uint8_t)((ev & 0x1f) << 3);
  return PL330_SZ_DMASEV;
}

static size_t emit_flushp(uint8_t *buf, uint8_t peri)
{
  buf[0] = PL330_CMD_DMAFLUSHP;
  buf[1] = (uint8_t)((peri & 0x1f) << 3);
  return PL330_SZ_DMAFLUSHP;
}

static size_t emit_end(uint8_t *buf)
{
  buf[0] = PL330_CMD_DMAEND;
  return PL330_SZ_DMAEND;
}

/****************************************************************************
 * Name: pl330_dbg_exec
 *
 * Description:
 *   通过调试接口执行一条指令。DMAGO 与 DMAKILL 只能这样发 —— 这是唯一的
 *   启动/终止入口。as_manager 为真表示让管理线程执行（DMAGO），否则由
 *   指定通道线程执行（DMAKILL）。
 *
 ****************************************************************************/

static int pl330_dbg_exec(struct rk3576_pl330_s *dmac, uint8_t insn[],
                          bool as_manager, int chan)
{
  uint32_t val;
  int i;

  for (i = 0; i < 10000; i++)
    {
      if ((pl330_getreg(dmac, PL330_DBGSTATUS) & PL330_DBG_BUSY) == 0)
        {
          break;
        }

      up_udelay(1);
    }

  if (i >= 10000)
    {
      syslog(LOG_ERR, "PL330: 调试口一直忙，DMAC 可能已停摆\n");
      return -EBUSY;
    }

  val = ((uint32_t)insn[0] << 16) | ((uint32_t)insn[1] << 24);
  if (!as_manager)
    {
      val |= (1 << 0) | ((uint32_t)chan << 8);
    }

  pl330_putreg(dmac, PL330_DBGINST0, val);

  val = (uint32_t)insn[2]        | ((uint32_t)insn[3] << 8) |
        ((uint32_t)insn[4] << 16) | ((uint32_t)insn[5] << 24);
  pl330_putreg(dmac, PL330_DBGINST1, val);

  pl330_putreg(dmac, PL330_DBGCMD, 0);
  return OK;
}

/****************************************************************************
 * Name: pl330_build_ccr
 *
 * Description:
 *   由通用配置算出通道控制字。
 *
 *   ★ 突发大小的编码是 log2(字节数)，不是字节数本身。这一位写错了不会
 *     报错，只会让搬运的步长不对 —— 表现是数据"错位"而不是"没有"。
 *
 ****************************************************************************/

static uint32_t pl330_build_ccr(const struct dma_config_s *cfg,
                                unsigned int burst, bool ns)
{
  uint32_t ccr = 0;
  unsigned int ssize = 2;   /* 默认 4 字节 */
  unsigned int dsize = 2;

  switch (cfg->src_width)
    {
      case 1: ssize = 0; break;
      case 2: ssize = 1; break;
      case 4: ssize = 2; break;
      case 8: ssize = 3; break;
      default: break;
    }

  switch (cfg->dst_width)
    {
      case 1: dsize = 0; break;
      case 2: dsize = 1; break;
      case 4: dsize = 2; break;
      case 8: dsize = 3; break;
      default: break;
    }

  ccr |= PL330_CC_SRCBRSTSIZE(ssize) | PL330_CC_SRCBRSTLEN(burst);
  ccr |= PL330_CC_DSTBRSTSIZE(dsize) | PL330_CC_DSTBRSTLEN(burst);
  ccr |= PL330_CC_SRCCCTRL(PL330_CCTRL_NONCACHEABLE);
  ccr |= PL330_CC_DSTCCTRL(PL330_CCTRL_NONCACHEABLE);

  /* ★ 安全态是**一对**，不是一个位：DMAGO 的 ns 和 CCR 的 SRCNS/DSTNS
   *   必须一致，而两者都取自 CR0 bit2（管理线程的启动安全态）。
   *
   *   本平台 OP-TEE/TZASC 把 DDR 与大部分外设配成非安全。CCR 不标 NS 时
   *   DMAC 以安全属性访问，被总线拒掉 —— 实测 FTC=0x80（通道读写错误）
   *   且 CPC 停在微码首地址（imprecise abort）。补上这两位后数据访问恢复
   *   正常。
   *
   *   这里按 CR0 推导而不是写死：管理线程的安全态是硬件配置，换一颗 SoC
   *   或换一个 dmac 实例就可能不同。本 SoC 实测 CR0=0x001ff075，bit2=1。
   */

  if (ns)
    {
      ccr |= PL330_CC_SRCPROT_NS | PL330_CC_DSTPROT_NS;
    }

  /* 地址是否自增：外设一侧固定（FIFO 端口），内存一侧递增。
   * dma_config_s 用 *_step 表达，>0 即递增。
   */

  /* ★ 地址自增：按方向定默认，但 *_step 显式给了就以它为准。
   *
   *   外设一侧固定（FIFO 是一个端口，不能递增），内存一侧递增。
   *   MEM_TO_MEM 原来无条件两侧都递增 —— 那就没法表达"源地址固定"，
   *   而那恰恰是**不带外设握手地去读一个 FIFO 端口**的唯一写法。
   *   用它可以把"DMAC 能不能读到这个地址"和"外设请求线通不通"分开验证：
   *   两者失败的现象一模一样，都是搬回来一片零。
   *
   *   现有调用方（mem2mem 自检）两个 step 都填 4，行为不变。
   */

  switch (cfg->direction)
    {
      case DMA_MEM_TO_DEV:
        ccr |= PL330_CC_SRCINC;          /* 源是内存 */
        break;                           /* 目的是 FIFO，不递增 */

      case DMA_DEV_TO_MEM:
        ccr |= PL330_CC_DSTINC;          /* 目的是内存 */
        break;                           /* 源是 FIFO，不递增 */

      default:                            /* MEM_TO_MEM / DEV_TO_DEV */
        if (cfg->src_step > 0)
          {
            ccr |= PL330_CC_SRCINC;
          }

        if (cfg->dst_step > 0)
          {
            ccr |= PL330_CC_DSTINC;
          }
        break;
    }

  return ccr;
}

/****************************************************************************
 * Name: pl330_build_program
 *
 * Description:
 *   生成一条通道的微码。
 *
 *   一次性传输（cyclic=false）：
 *     MOV CCR / MOV SAR / MOV DAR
 *     LP0 nper { LP1 nburst { [WFP] LD ST } LPEND1 }  LPEND0
 *     SEV ev
 *     END
 *
 *   环形传输（cyclic=true）：外面再套一层无条件回跳，并在回跳点重设内存侧
 *   地址；每搬完一个周期发一次事件。
 *
 *     MOV CCR / MOV SAR
 *   top: MOV DAR(或 SAR)
 *     LP0 nper { LP1 nburst { WFP LDP ST } LPEND1  SEV ev } LPEND0
 *     LPEND forever → top
 *     END
 *
 * Returned Value:
 *   微码字节数，出错返回负的 errno。
 *
 ****************************************************************************/

static ssize_t pl330_build_program(struct rk3576_pl330_chan_s *ch,
                                   uintptr_t dst, uintptr_t src,
                                   size_t len, size_t period, bool cyclic)
{
  const struct dma_config_s *cfg = &ch->cfg;
  unsigned int burst = RK3576_DMA_OPT_GET_BURST(cfg->option);
  unsigned int width;
  unsigned int burstbytes;
  unsigned int nburst;
  unsigned int nper;
  uint8_t     *buf = ch->mcode;
  size_t       off = 0;
  size_t       toploop;
  size_t       innerloop;
  size_t       outerloop;
  uint32_t     ccr;
  bool         ismem2mem = (cfg->direction == DMA_MEM_TO_MEM);
  bool         istx      = (cfg->direction == DMA_MEM_TO_DEV);
  uint8_t      peri      = (uint8_t)(istx ? cfg->dst_drq : cfg->src_drq);

  width = istx ? cfg->dst_width : cfg->src_width;
  if (width == 0)
    {
      width = 4;
    }

  burstbytes = width * burst;
  if (burstbytes == 0)
    {
      return -EINVAL;
    }

  /* ★ 长度必须能被"一个突发的字节数"整除。
   *
   *   PL330 的循环计数器是按**突发**计的，没有"剩下半个突发"这种表达。
   *   除不尽就意味着尾巴会被悄悄丢掉 —— 与其让调用方在数据里发现少了
   *   几十字节，不如在这里直接拒绝。
   */

  if ((period % burstbytes) != 0 || (len % period) != 0)
    {
      syslog(LOG_ERR, "PL330: 长度不整除 len=%zu period=%zu 突发=%u 字节\n",
             len, period, burstbytes);
      return -EINVAL;
    }

  nburst = period / burstbytes;
  nper   = len / period;

  /* LP 的计数器是 8 位，一层最多 256 次。
   *
   *   突发 8 字节宽时 8192B/8B = 1024 次，直接超限；与其让调用方去
   *   拆 period（dma 接口的 start 把 period 固定为整段），不如这里
   *   自动把外层循环翻倍、内层减半：nper x2 / nburst /2，直到放得下。
   *   前提是 period/burstbytes 为 2 的幂，才能一路整除到底。
   */

  while (nburst > 256)
    {
      nper   *= 2;
      nburst /= 2;
    }

  if (nburst == 0 || nper > 256)
    {
      syslog(LOG_ERR, "PL330: 无法拆分循环 nburst=%u nper=%u\n",
             nburst, nper);
      return -EINVAL;
    }

  ccr = pl330_build_ccr(cfg, burst, ch->dmac->ns);

  off += emit_mov(&buf[off], PL330_MOV_CCR, ccr);

  /* ★ 开头这条 FLUSHP 与之前在**末尾**试过的那条作用完全不同。
   *
   *   末尾那条是"搬完再清"，实测会跟完成事件竞争，已删。这条是
   *   Linux 注释明确写的 "clear any stale dma requests before the first
   *   WFP"：外设的请求线是带状态的，上一次传输结束时可能还挂着一个没被
   *   消费的请求。不清掉，第一条 WFP 会立刻被这个陈旧请求放行，于是
   *   DMALDP 去读一个其实还没准备好的 FIFO —— 首个突发数据错位，而且
   *   整条流水从此错开一拍。
   *
   *   放在 toploop 之前，所以环形传输里也只执行一次：如果放在回跳点
   *   之后，每绕一圈都会清一次，正好可能丢掉刚到的那个请求。
   */

  if (!ismem2mem)
    {
      off += emit_flushp(&buf[off], peri);
    }

  /* 环形时，固定的一侧（外设端口）在循环外设一次，
   * 递增的一侧（内存）要在每轮开头重设，所以放在回跳点之后。
   */

  if (cyclic)
    {
      if (istx)
        {
          off += emit_mov(&buf[off], PL330_MOV_DAR, (uint32_t)dst);
          toploop = off;
          off += emit_mov(&buf[off], PL330_MOV_SAR, (uint32_t)src);
        }
      else
        {
          off += emit_mov(&buf[off], PL330_MOV_SAR, (uint32_t)src);
          toploop = off;
          off += emit_mov(&buf[off], PL330_MOV_DAR, (uint32_t)dst);
        }
    }
  else
    {
      off += emit_mov(&buf[off], PL330_MOV_SAR, (uint32_t)src);
      off += emit_mov(&buf[off], PL330_MOV_DAR, (uint32_t)dst);
      toploop = off;
    }

  /* 外层：周期数 */

  off += emit_lp(&buf[off], 0, nper);
  outerloop = off;

  /* 内层：一个周期里的突发数 */

  off += emit_lp(&buf[off], 1, nburst);
  innerloop = off;

  if (ismem2mem)
    {
      off += emit_ld(&buf[off], PL330_COND_ALWAYS);
      off += emit_st(&buf[off], PL330_COND_ALWAYS);
    }
  else
    {
      /* ★ 这一条 DMAWFP 就是"数据就绪中断"的替代品。
       *
       *   SAI 没有 FIFO 水位中断，但它有**外设 DMA 请求线**：FIFO 到达
       *   DMACR 里设的水位就拉请求。DMAWFP 让通道线程停在这里等那条线，
       *   等到了才搬一个突发。CPU 全程不参与，也就不存在"轮询跟不上"。
       *
       *   dtsi 上带 arm,pl330-periph-burst，表示外设发的是突发请求，
       *   所以这里用 BURST 条件，与 DMALDP/DMASTP 保持一致。
       */

      if (istx)
        {
          /* 发送同样用 BURST（原厂 _bursts() 两个方向用同一个条件）。
           * 原来的 SINGLE 下 WFP 不等请求，放音是"按总线速度往 FIFO 里
           * 灌"；改 BURST 后 1 秒测试音照常放完，放音按请求线节拍走。
           */

          off += emit_wfp(&buf[off], PL330_COND_BURST, peri);
          off += emit_ld(&buf[off], PL330_COND_BURST);
          off += emit_stp(&buf[off], PL330_COND_BURST, peri);
        }
      else
        {
          /* ★ 接收必须用 BURST 条件（WFPB / LDPB / STB）。
           *
           *   原先三条都是 SINGLE（与上面注释说的相反）：实测 8192 字节
           *   一个缓冲区 1ms 就"收完"（48k 立体声本该 43ms），数据是大段
           *   0 夹着 0x7f0x、左右声道逐字相同 —— WFPS 没在等请求线，DMA
           *   按总线速度读空 FIFO。录音机"波形动一下就不动、放出来没声"
           *   就是它。
           *
           *   原厂 Linux pl330.c 的 _bursts()：突发长度 > 1 用 BURST，dtsi
           *   带 arm,pl330-periph-burst 时强制 BURST；WFP、load、store
           *   三条用同一个条件。
           *
           */

          off += emit_wfp(&buf[off], PL330_COND_BURST, peri);
          off += emit_ldp(&buf[off], PL330_COND_BURST, peri);
          off += emit_st(&buf[off], PL330_COND_BURST);
        }
    }

  off += emit_lpend(&buf[off], 1, false, PL330_COND_ALWAYS,
                    (uint8_t)(off - innerloop));

  /* 一个周期搬完，通知上层。环形时这是每周期一次的心跳。 */

  if (cyclic)
    {
      off += emit_sev(&buf[off], ch->id);
    }

  off += emit_lpend(&buf[off], 0, false, PL330_COND_ALWAYS,
                    (uint8_t)(off - outerloop));

  if (cyclic)
    {
      off += emit_lpend(&buf[off], 0, true, PL330_COND_ALWAYS,
                        (uint8_t)(off - toploop));
    }
  else
    {
      /* ★ 去掉 FLUSHP：实测搬完后的 FLUSHP 会跟完成事件竞争，
       *   且 LDP 读外设口本身有 DATA_READ_ERR 嫌疑。搬完直接
       *   SEV 通知上层，END 收尾。若 FLUSHP 是故障源，这版应该
       *   全程无故障。
       */

      off += emit_sev(&buf[off], ch->id);
    }

  off += emit_end(&buf[off]);

  DEBUGASSERT(off <= PL330_MCODE_SIZE);

  /* ★ 写完必须刷回内存：DMAC 走自己的 AXI 主口取指，看不见 CPU 的
   *   写回缓存。少了这一步，DMAC 取到的是这块内存上一次的内容。
   */

  up_clean_dcache((uintptr_t)buf, (uintptr_t)buf + off);

  return (ssize_t)off;
}

/****************************************************************************
 * Name: pl330_go
 *
 * Description:
 *   让通道从微码起始地址开始执行。
 *
 ****************************************************************************/

static int pl330_go(struct rk3576_pl330_chan_s *ch)
{
  struct rk3576_pl330_s *dmac = ch->dmac;
  uintptr_t addr = (uintptr_t)ch->mcode;
  uint8_t insn[6];
  int i;

  /* ★ 通道复用清理：上一次传输可能留下 FSC 故障残留、或没回到 STOP。
   *   不清的话这次 DMAGO 后 LDP 会带着旧故障跑（实测首缓冲成功、
   *   后续缓冲全 DATA_READ_ERR 的模式与此吻合）。
   */

  pl330_putreg(dmac, PL330_FSC, 1u << ch->id);
  pl330_putreg(dmac, PL330_INTCLR, 1u << ch->id);

  if (PL330_CS_STATUS(pl330_getreg(dmac, PL330_CS(ch->id))) !=
      PL330_ST_STOP)
    {
      uint8_t kill[6];

      memset(kill, 0, sizeof(kill));
      kill[0] = PL330_CMD_DMAKILL;
      kill[1] = ch->id & 0x7;
      pl330_dbg_exec(dmac, kill, true, ch->id);

      for (i = 0; i < 1000; i++)
        {
          if (PL330_CS_STATUS(pl330_getreg(dmac, PL330_CS(ch->id))) ==
              PL330_ST_STOP)
            {
              break;
            }

          up_udelay(1);
        }
    }

  pl330_putreg(dmac, PL330_INTEN,
               pl330_getreg(dmac, PL330_INTEN) | (1u << ch->id));

  /* ★ ns=1（非安全态）：RK 平台的 dmaengine 客户端（含 SAI）全部以
   *   非安全身份跑，OP-TEE 环境下 DMAC 的访问属性也按非安全走。
   *   实测 DS=0x200 的 bit9 置位，说明管理线程并不在纯安全态；若
   *   DMAGO 的 ns 与管理线程不一致，通道会 abort —— 表现是 DMAGO
   *   返回 OK、CS 恒 STOP、CPC/SA/DA 全 0、无故障，正是最初见到的现象。
   */

  /* ns 从 CR0 读出来，不硬编：管理线程的安全态是硬件配置，
   * 换一颗 SoC 或换一个 dmac 实例就可能不同。
   */

  insn[0] = PL330_CMD_DMAGO | (dmac->ns ? (1 << 1) : 0);
  insn[1] = ch->id & 0x7;
  insn[2] = (uint8_t)(addr);
  insn[3] = (uint8_t)(addr >> 8);
  insn[4] = (uint8_t)(addr >> 16);
  insn[5] = (uint8_t)(addr >> 24);

  /* ★ 一次性：每次 DMAGO 都打，在音频这种每 43ms 一次的场景里会把串口
   *   灌满，反过来成为新的故障源。首次打一条足够确认路径走通。
   */

  if (!g_go_logged)
    {
      g_go_logged = true;
      syslog(LOG_INFO, "PL330: 首次 DMAGO ch=%u ns=%d mc=%p DS=%08" PRIx32
                       "\n",
             ch->id, dmac->ns, ch->mcode,
             pl330_getreg(dmac, PL330_DS));
    }

  return pl330_dbg_exec(dmac, insn, true, ch->id);
}

/****************************************************************************
 * Name: pl330_kill
 ****************************************************************************/

static int pl330_kill(struct rk3576_pl330_chan_s *ch)
{
  uint8_t insn[6];
  int i;

  memset(insn, 0, sizeof(insn));
  insn[0] = PL330_CMD_DMAKILL;

  pl330_dbg_exec(ch->dmac, insn, false, ch->id);

  for (i = 0; i < 1000; i++)
    {
      uint32_t cs = pl330_getreg(ch->dmac, PL330_CS(ch->id));

      if (PL330_CS_STATUS(cs) == PL330_ST_STOP)
        {
          pl330_putreg(ch->dmac, PL330_FSC, 1u << ch->id);
          return OK;
        }

      up_udelay(1);
    }

  syslog(LOG_ERR, "PL330: 通道 %u 停不下来 CS=0x%08" PRIx32 "\n",
         ch->id, pl330_getreg(ch->dmac, PL330_CS(ch->id)));
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: pl330_interrupt
 *
 * Description:
 *   两条中断线接的是同一个处理函数。PL330 的事件到中断线的映射依实现而定，
 *   与其去猜哪条线对应哪个事件，不如两条都接上、统一从 INTSTATUS 读是谁
 *   触发的 —— 这样映射怎么变都不影响正确性。
 *
 ****************************************************************************/

static int pl330_interrupt(int irq, void *context, void *arg)
{
  struct rk3576_pl330_s *dmac = arg;
  uint32_t status;
  uint32_t fault;
  int i;

  static int nisr = 0;

  status = pl330_getreg(dmac, PL330_INTSTATUS);
  pl330_putreg(dmac, PL330_INTCLR, status);

  if (nisr < 24)
    {
      nisr++;
      if (!g_isr_logged)
        {
          g_isr_logged = true;
          syslog(LOG_INFO, "PL330: 首次完成中断 irq=%d INTSTATUS=%08" PRIx32
                           " FSC=%08" PRIx32 "\n",
                 irq, status, fault);
        }
    }

  /* 故障要先看：搬运出错时事件不会来，只有故障位。
   * 少了这一段，现象是"回调再也不来了"，而原因（比如地址没对齐、
   * 微码里有非法指令）完全看不见。
   */

  fault = pl330_getreg(dmac, PL330_FSC);
  if (fault != 0)
    {
      for (i = 0; i < dmac->nchans; i++)
        {
          if ((fault & (1u << i)) != 0)
            {
              syslog(LOG_ERR,
                     "PL330: 通道 %d 故障 FSC=0x%08" PRIx32
                     " FTC=0x%08" PRIx32 " CPC=0x%08" PRIx32
                     " SA=0x%08" PRIx32 " DA=0x%08" PRIx32 "\n",
                     i, fault,
                     pl330_getreg(dmac, PL330_FTC(i)),
                     pl330_getreg(dmac, PL330_CPC(i)),
                     pl330_getreg(dmac, PL330_SA(i)),
                     pl330_getreg(dmac, PL330_DA(i)));

              if (dmac->chans[i].callback != NULL)
                {
                  dmac->chans[i].callback(&dmac->chans[i].chan,
                                          dmac->chans[i].arg, -EIO);
                }

              pl330_kill(&dmac->chans[i]);
            }
        }

      /* ★ FSC 是写 1 清除寄存器。不清的话故障标志残留，下一次任何
       *   事件中断都会误判成故障 —— 表现为第一个缓冲偶尔成功、
       *   后续缓冲全部被 kill（本板实测的故障模式）。
       */

      pl330_putreg(dmac, PL330_FSC, fault);
    }

  for (i = 0; i < dmac->nchans; i++)
    {
      struct rk3576_pl330_chan_s *ch = &dmac->chans[i];

      if ((status & (1u << i)) == 0 || !ch->inuse)
        {
          continue;
        }

      if (ch->callback != NULL)
        {
          ch->callback(&ch->chan, ch->arg,
                       (ssize_t)(ch->cyclic ? ch->period : ch->len));
        }
    }

  return OK;
}

/****************************************************************************
 * Public-facing ops
 ****************************************************************************/

static int pl330_config(struct dma_chan_s *chan,
                        const struct dma_config_s *cfg)
{
  struct rk3576_pl330_chan_s *ch = (struct rk3576_pl330_chan_s *)chan;

  if (cfg == NULL)
    {
      return -EINVAL;
    }

  ch->cfg = *cfg;
  return OK;
}

static int pl330_start_common(struct dma_chan_s *chan,
                              dma_callback_t callback, void *arg,
                              uintptr_t dst, uintptr_t src,
                              size_t len, size_t period, bool cyclic)
{
  struct rk3576_pl330_chan_s *ch = (struct rk3576_pl330_chan_s *)chan;
  irqstate_t flags;
  ssize_t n;
  int ret;

  if (len == 0 || period == 0)
    {
      return -EINVAL;
    }

  ch->callback = callback;
  ch->arg      = arg;
  ch->cyclic   = cyclic;
  ch->len      = len;
  ch->period   = period;
  ch->membase  = (ch->cfg.direction == DMA_MEM_TO_DEV) ? src : dst;

  n = pl330_build_program(ch, dst, src, len, period, cyclic);
  if (n < 0)
    {
      return (int)n;
    }

  flags = spin_lock_irqsave(&ch->dmac->lock);
  ret = pl330_go(ch);
  spin_unlock_irqrestore(&ch->dmac->lock, flags);

  return ret;
}

static int pl330_start(struct dma_chan_s *chan,
                       dma_callback_t callback, void *arg,
                       uintptr_t dst, uintptr_t src, size_t len)
{
  return pl330_start_common(chan, callback, arg, dst, src, len, len, false);
}

static int pl330_start_cyclic(struct dma_chan_s *chan,
                              dma_callback_t callback, void *arg,
                              uintptr_t dst, uintptr_t src,
                              size_t len, size_t period_len)
{
  return pl330_start_common(chan, callback, arg, dst, src,
                            len, period_len, true);
}

static int pl330_stop(struct dma_chan_s *chan)
{
  struct rk3576_pl330_chan_s *ch = (struct rk3576_pl330_chan_s *)chan;

  pl330_putreg(ch->dmac, PL330_INTEN,
               pl330_getreg(ch->dmac, PL330_INTEN) & ~(1u << ch->id));
  pl330_putreg(ch->dmac, PL330_INTCLR, 1u << ch->id);

  ch->callback = NULL;
  return pl330_kill(ch);
}

/* PL330 没有真正的暂停/恢复：通道线程要么在跑，要么被 KILL 掉。
 * 与其伪造一个"看起来能用"的暂停，不如如实返回不支持 —— 上层可以据此
 * 选择停掉再重启，而不是以为自己暂停成功了。
 */

static int pl330_pause(struct dma_chan_s *chan)
{
  UNUSED(chan);
  return -ENOSYS;
}

static int pl330_resume(struct dma_chan_s *chan)
{
  UNUSED(chan);
  return -ENOSYS;
}

static size_t pl330_residual(struct dma_chan_s *chan)
{
  struct rk3576_pl330_chan_s *ch = (struct rk3576_pl330_chan_s *)chan;
  uintptr_t cur;
  size_t done;

  if (ch->cfg.direction == DMA_MEM_TO_DEV)
    {
      cur = (uintptr_t)pl330_getreg(ch->dmac, PL330_SA(ch->id));
    }
  else
    {
      cur = (uintptr_t)pl330_getreg(ch->dmac, PL330_DA(ch->id));
    }

  if (cur < ch->membase || cur > ch->membase + ch->len)
    {
      return 0;
    }

  done = cur - ch->membase;
  return ch->len - done;
}

static struct dma_chan_s *pl330_get_chan(struct dma_dev_s *dev,
                                         unsigned int ident)
{
  struct rk3576_pl330_s *dmac = (struct rk3576_pl330_s *)dev;
  struct dma_chan_s *ret = NULL;
  irqstate_t flags;
  int i;

  UNUSED(ident);

  flags = spin_lock_irqsave(&dmac->lock);

  for (i = 0; i < dmac->nchans; i++)
    {
      if (!dmac->chans[i].inuse)
        {
          dmac->chans[i].inuse = true;
          ret = &dmac->chans[i].chan;
          break;
        }
    }

  spin_unlock_irqrestore(&dmac->lock, flags);

  if (ret == NULL)
    {
      syslog(LOG_ERR, "PL330: 没有空闲通道了（共 %d 条）\n", dmac->nchans);
    }

  return ret;
}

static void pl330_put_chan(struct dma_dev_s *dev, struct dma_chan_s *chan)
{
  struct rk3576_pl330_s *dmac = (struct rk3576_pl330_s *)dev;
  struct rk3576_pl330_chan_s *ch = (struct rk3576_pl330_chan_s *)chan;
  irqstate_t flags;

  if (ch == NULL)
    {
      return;
    }

  pl330_stop(chan);

  flags = spin_lock_irqsave(&dmac->lock);
  ch->inuse    = false;
  ch->callback = NULL;
  spin_unlock_irqrestore(&dmac->lock, flags);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* MEM_TO_MEM 自检：隔离"PL330 DDR 数据通路"与"外设口访问"。
 *   如果这里能搬通，说明 CCR(NS)/LD/ST/中断全链路 OK，
 *   问题就只剩 LDP 读外设口这一环（SAI RX）。
 */

static volatile int g_m2m_done = 0;

static void pl330_m2m_cb(FAR struct dma_chan_s *chan, FAR void *arg,
                         ssize_t len)
{
  g_m2m_done = 1;
}

static void pl330_mem2mem_selftest(struct rk3576_pl330_s *dmac)
{
  static uint32_t src[256] aligned_data(PL330_CACHE_LINE);
  static uint32_t dst[256] aligned_data(PL330_CACHE_LINE);
  struct dma_config_s cfg;
  struct dma_chan_s *ch;
  int i;

  for (i = 0; i < 256; i++)
    {
      src[i] = 0x5a000000u + i;
      dst[i] = 0;
    }

  /* ★ 源要 clean，目的也要 clean —— 后者容易漏，代价是"DMA 报成功但全是零"。
   *
   *   上面刚把 dst[] 写成 0，这些**脏缓存行还在 CPU 的写回缓存里**。
   *   DMA 把真实数据写进 DRAM 之后，那些脏的零行被回写，正好盖掉 DMA
   *   写的内容；事后再 invalidate，读到的就是被盖回去的零。
   *
   *   现象极具误导性：通道跑到 STOPPED、完成事件也到了、FTC 无故障，
   *   一切都说"成功"，只有数据是错的。于是排查会往外设、握手、时钟那边
   *   去 —— 而根因在一条没写的 cache 维护语句上。
   *
   *   规矩：**DMA 之前 clean 两端，DMA 之后 invalidate 目的端。**
   */

  up_clean_dcache((uintptr_t)src, (uintptr_t)src + sizeof(src));
  up_clean_dcache((uintptr_t)dst, (uintptr_t)dst + sizeof(dst));

  ch = pl330_get_chan(&dmac->dev, 0);
  if (ch == NULL)
    {
      syslog(LOG_ERR, "PL330 自检: get_chan 失败\n");
      return;
    }

  memset(&cfg, 0, sizeof(cfg));
  cfg.direction = DMA_MEM_TO_MEM;
  cfg.src_width = 4;
  cfg.dst_width = 4;
  cfg.src_step  = 4;
  cfg.dst_step  = 4;
  cfg.option    = RK3576_DMA_OPT_BURST(4);

  if (ch->ops->config(ch, &cfg) < 0)
    {
      syslog(LOG_ERR, "PL330 自检: config 失败\n");
      return;
    }

  g_m2m_done = 0;
  if (ch->ops->start(ch, pl330_m2m_cb, NULL,
                     (uintptr_t)dst, (uintptr_t)src, 1024) < 0)
    {
      syslog(LOG_ERR, "PL330 自检: start 失败\n");
      ch->ops->stop(ch);
      dmac->dev.put_chan(&dmac->dev, ch);
      return;
    }

  syslog(LOG_INFO, "PL330 自检: go后 CS=%08" PRIx32 " CPC=%08" PRIx32
                   " DBGST=%08" PRIx32 " DS=%08" PRIx32 "\n",
         pl330_getreg(dmac, PL330_CS(0)),
         pl330_getreg(dmac, PL330_CPC(0)),
         pl330_getreg(dmac, PL330_DBGSTATUS),
         pl330_getreg(dmac, PL330_DS));

  /* ★ 判"搬完了没有"用通道状态，不用中断。
   *
   *   两件事必须分开：**数据有没有搬对**，和**完成事件有没有送到 CPU**。
   *   用回调作为唯一判据时，中断路径一坏，数据校验就永远不会执行 ——
   *   而这两类故障的修法完全不同。通道跑到 DMAEND 会进 STOPPED，
   *   这是不依赖中断的、可直接观测的完成标志。
   */

  for (i = 0; i < 500; i++)
    {
      uint32_t cs = pl330_getreg(dmac, PL330_CS(0));

      if (PL330_CS_STATUS(cs) == PL330_ST_STOP)
        {
          break;
        }

      up_mdelay(1);
    }

  up_invalidate_dcache((uintptr_t)dst, (uintptr_t)dst + sizeof(dst));

  if (PL330_CS_STATUS(pl330_getreg(dmac, PL330_CS(0))) != PL330_ST_STOP)
    {
      syslog(LOG_ERR, "PL330 自检: 通道没停 CS=%08" PRIx32
             " CPC=%08" PRIx32 " FTC=%08" PRIx32 "\n",
             pl330_getreg(dmac, PL330_CS(0)),
             pl330_getreg(dmac, PL330_CPC(0)),
             pl330_getreg(dmac, PL330_FTC(0)));
      ch->ops->stop(ch);
      dmac->dev.put_chan(&dmac->dev, ch);
      return;
    }

  if (!g_m2m_done)
    {
      syslog(LOG_ERR, "PL330 自检: 通道已停但**完成中断没到** "
                      "INTEN=%08" PRIx32 " INTSTATUS=%08" PRIx32 "\n",
             pl330_getreg(dmac, PL330_INTEN),
             pl330_getreg(dmac, PL330_INTSTATUS));
    }

  for (i = 0; i < 256; i++)
    {
      if (dst[i] != src[i])
        {
          syslog(LOG_ERR, "PL330 自检: 数据错位 @%d dst=%08" PRIx32
                 " src=%08" PRIx32 "\n", i, dst[i], src[i]);
          return;
        }
    }

  syslog(LOG_INFO, "PL330 自检: MEM_TO_MEM 1024B OK\n");

  ch->ops->stop(ch);
  dmac->dev.put_chan(&dmac->dev, ch);
}

struct dma_dev_s *rk3576_pl330_initialize(int ctrl)
{
  struct rk3576_pl330_s *dmac = &g_dmac0;
  uint32_t cr0;
  uint32_t pid;
  int i;

  if (ctrl != 0)
    {
      syslog(LOG_ERR, "PL330: 目前只接了 dmac0（SAI1 挂在它上面）\n");
      return NULL;
    }

  if (dmac->ready)
    {
      return &dmac->dev;
    }

  dmac->base = RK3576_DMAC0_BASE;
  spin_lock_init(&dmac->lock);

  /* 时钟：ACLK_DMAC0。不开的话下面读 CR0 会读回 0 或挂总线，
   * 所以先开门控再做任何寄存器访问。
   */

  rk3576_clk_gate(DMAC0_GATE_CON, DMAC0_GATE_BIT, true);

  /* ★ PL330 的 ID 寄存器是 8 位宽：在本 SoC 上 32 位读 0xfe0 只回低字节
   *   （PID0=0x30），高 24 位恒 0，拼不出完整 PART/DESIGNER。实测逐字节：
   *   ID0=0x30 ID1=0x13 ID2=0x24 CID0=0x0d —— 标准 ARM PrimeCell。
   *   所以这里用 8 位读再拼装（与 Linux 读 32 位拿 0x241330 等价）。
   */

  pid = getreg8(dmac->base + 0x0fe0)
      | ((uint32_t)getreg8(dmac->base + 0x0fe4) << 8)
      | ((uint32_t)getreg8(dmac->base + 0x0fe8) << 16);
  cr0 = pl330_getreg(dmac, PL330_CR0);

  /* ★ 先确认这里真的是一颗 PL330 再往下走。
   *
   *   PrimeCell 的 PART 号是 0x330、DESIGNER 是 0x41（ARM）。基址抄错或
   *   时钟没开时读回来的是 0 或全 f —— 那种情况下继续初始化，后面所有
   *   现象都会指向错误的方向。
   */

  if (((pid & 0xfff) != 0x330) || (((pid >> 12) & 0xff) != 0x41))
    {
      syslog(LOG_ERR, "PL330: 特征号不对 PERIPH_ID=0x%08" PRIx32
             "（期望 PART=0x330 DESIGNER=0x41）\n", pid);
      return NULL;
    }

  dmac->ns     = (cr0 & PL330_CR0_BOOT_MAN_NS) != 0;
  dmac->nchans = PL330_CR0_NUM_CHANS(cr0);
  if (dmac->nchans > PL330_MAX_CHAN)
    {
      dmac->nchans = PL330_MAX_CHAN;
    }

  dmac->dev.get_chan = pl330_get_chan;
  dmac->dev.put_chan = pl330_put_chan;

  for (i = 0; i < PL330_MAX_CHAN; i++)
    {
      dmac->chans[i].chan.ops = &g_pl330_ops;
      dmac->chans[i].dmac     = dmac;
      dmac->chans[i].id       = (uint8_t)i;
      dmac->chans[i].inuse    = false;
      dmac->chans[i].mcode    = g_mcode[i];
    }

  pl330_putreg(dmac, PL330_INTEN, 0);
  pl330_putreg(dmac, PL330_INTCLR, 0xffffffff);

  irq_attach(RK3576_IRQ_DMAC0_0, pl330_interrupt, dmac);
  irq_attach(RK3576_IRQ_DMAC0_1, pl330_interrupt, dmac);
  up_enable_irq(RK3576_IRQ_DMAC0_0);
  up_enable_irq(RK3576_IRQ_DMAC0_1);

  dmac->ready = true;

  /* ★ 自检必须放在中断挂好之后。
   *
   *   原先它跑在 irq_attach 之前，等的是一个**当时不可能到达的回调** ——
   *   于是每次都报"超时未完成"，而且在超时分支里直接 return，**数据校验
   *   那段从来没执行过**。也就是说这个自检从头到尾没验过任何东西，
   *   却在启动日志里留下一条看起来很像硬件故障的错误信息，
   *   把排查方向引向了 SAI 和外设握手。
   *
   *   一个从不真正检查的自检，比没有自检更糟。
   */

  pl330_mem2mem_selftest(dmac);

  syslog(LOG_INFO,
         "PL330: dmac0 就绪 通道=%d 外设口=%d 事件=%d 安全态=%s "
         "PERIPH_ID=0x%08" PRIx32 "\n",
         dmac->nchans, PL330_CR0_NUM_PERIPH(cr0),
         PL330_CR0_NUM_EVENTS(cr0), dmac->ns ? "非安全" : "安全", pid);

  return &dmac->dev;
}
