/****************************************************************************
 * chip/rk3576/rk3576_gmac.c
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

/* RK3576 GMAC（Synopsys DWMAC 4.20a）—— 前置链路 + MDIO。
 *
 * ★★ 当前状态：DMA 软复位不完成，功能未打通。
 *
 *   现象：写 DMA_MODE.SWR 后该位永不自清（板上实测 DMA_MODE 上电即为
 *   0x1，即从未复位成功）。而 MAC_VERSION=0x3051、HW_FEATURE0=0x1a1173f7
 *   均可读，MAC 块与 DMA 块的纯数据寄存器均可写（已用
 *   DMA_CH0_TXDESC_RL 与 MAC_ADDR0_HIGH 写回读验证）。
 *
 *   已按 dtsi 逐条核对并全部配齐的六项前置条件：
 *     1. 电源域 PD_SDGMAC
 *     2. 五路时钟：125M_SRC（含 5 位分频，cpll/8=125MHz）、RMII_CRU、
 *        PCLK、ACLK、PTP_REF
 *     3. 模块软复位 SRST_A_GMAC0 / SRST_P_GMAC0 撤销
 *     4. PHY 硬件复位释放（板级 gpio2-13，低有效）
 *     5. SDGMAC_GRF 接口模式（RGMII）与 gmii_clk_sel（DIV1_125M）
 *     6. RGMII 引脚复用 14 脚（含 RXCLK g3-25 / TXCLK g3-14）
 *
 *   六项配齐后仍不通，说明还缺一项尚未识别的条件。Synopsys 手册指出
 *   软复位需要**所有时钟域**就绪，其中 RX 时钟由 PHY 提供 —— 下一步
 *   应实测 PHY 是否真的在输出时钟（把 RXCLK 脚切回 GPIO 读电平，
 *   或用示波器），以区分"SoC 侧还缺配置"与"PHY 侧没起来"。
 *
 *   排查全过程见 notes/DEBUG-CASES.md 案例 10。
 *
 * 已完成且可复用的部分：全部寄存器定义、六项前置条件的配置代码、
 * MDIO 读时序。收发路径可参照 arch/arm/src/stm32h7/stm32_ethernet.c。
 *
 * ★ NuttX 里有同 IP 家族的成熟实现：arch/arm/src/stm32h7/stm32_ethernet.c
 *   STM32H7 用 DWC_ether_qos，寄存器布局与 4.20a 一致（已核对
 *   MACCR=0x0000、MTL=0x0C00、DMA=0x1000 三处标志性偏移）。
 *   后续的描述符环、收发路径、中断处理都照它的结构来。
 *
 * ★ 本文件先只做到"能读出 PHY ID"。
 *
 *   理由与 eMMC/SAI 相同：电源域、时钟、引脚复用任一没配好，表现都是
 *   寄存器读全 0 或 MDIO 超时，与收发逻辑写错无法区分。先把这条链路
 *   验证掉，写描述符环才有可靠的地基。
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <syslog.h>

#include <nuttx/arch.h>

#include "arm64_internal.h"
#include "rk3576_gmac.h"
#include "rk3576_cru.h"
#include "rk3576_gpio.h"
#include "rk3576_pinmux.h"
#include "rk3576_power.h"
#include "hardware/rk3576_gmac.h"
#include "hardware/rk3576_memorymap.h"

#ifdef CONFIG_RK3576_GMAC

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 时钟。出处：clk-rk3576.c
 *   GATE(ACLK_GMAC0, ... CLKGATE_CON(42), 7)
 *   GATE(PCLK_GMAC0, ... CLKGATE_CON(42), 9)
 *   GMAC1 在同一寄存器的相邻位
 */

/* ★ 四路时钟都要开，缺一路 DMA 就不动。
 *
 *   ACLK/PCLK 只管总线与寄存器访问；真正驱动 MAC 状态机的是
 *   CLK_GMACn_125M_SRC（dtsi 里名为 "stmmaceth"）。缺它时寄存器
 *   读写完全正常、MAC_VERSION 也读得到，但 DMA 软复位的自清位
 *   永远不归零 —— 本端口实测就是这个现象。
 *
 *   与 I2C 的"PCLK 通而功能时钟没开"、SAI 的"三路时钟都要开"
 *   是同一类陷阱：寄存器能访问不代表外设能工作。
 */

#define GMAC_GATE_CON        42
#define GMAC0_GATE_ACLK      7
#define GMAC0_GATE_PCLK      9
#define GMAC1_GATE_ACLK      8
#define GMAC1_GATE_PCLK      10

/* CLKGATE_CON(3)：CLK_GMACn_125M_SRC，MAC 主应用时钟 */

#define GMAC_GATE_CON_125M   3
#define GMAC0_GATE_125M      6
#define GMAC1_GATE_125M      7

/* CLKGATE_CON(5)：CLK_GMACn_RMII_CRU */

#define GMAC_GATE_CON_RMII   5
#define GMAC0_GATE_RMII      13
#define GMAC1_GATE_RMII      14

/* PTP 参考时钟。dtsi 的 clocks 共五路，此前只开了四路。
 *   GATE(CLK_GMAC1_PTP_REF, ... CLKGATE_CON(42), 13)
 *   CLK_GMAC0_PTP_REF_SRC    ... CLKGATE_CON(43), 0
 */

#define GMAC0_GATE_PTP_CON   43
#define GMAC0_GATE_PTP       0
#define GMAC1_GATE_PTP_CON   42
#define GMAC1_GATE_PTP       13

/* 复位号。出处：include/dt-bindings/reset/rockchip,rk3576-cru.h
 *   SRST_A_GMAC0 = 264   SRST_A_GMAC1 = 265
 *   SRST_P_GMAC0 = 266   SRST_P_GMAC1 = 267
 */

#define SRST_A_GMAC0         264
#define SRST_A_GMAC1         265
#define SRST_P_GMAC0         266
#define SRST_P_GMAC1         267

/* ★ SDGMAC_GRF —— SoC 层的接口与时钟选择，不配的话 MAC 拿不到有效
 *   的 TX/RX 时钟，DMA 软复位的 SWR 位永不自清。
 *
 *   出处：Linux drivers/net/ethernet/stmicro/stmmac/dwmac-rk.c 的
 *   rk3576_ops / rk3576_init()：
 *       RK3576_GRF_GMAC_CON0 = 0x0020   （GMAC0）
 *       RK3576_GRF_GMAC_CON1 = 0x0024   （GMAC1）
 *       gmac_rmii_mode_mask     = BIT(3)      清零选 RGMII
 *       clock.io_clksel_io_mask = BIT(7)
 *       clock.gmii_clk_sel_mask = GENMASK(6,5)
 *   GRF 基址取自主线 rk3576.dtsi 的 sdgmac_grf: syscon@26038000。
 *
 *   GRF 寄存器同样是高 16 位写使能掩码。
 */

/* ★ CLK_GMACn_125M_SRC 带分频器，只开门控不设分频得不到 125MHz。
 *
 *   COMPOSITE_NOMUX(CLK_GMAC0_125M_SRC, "clk_gmac0_125m_src", "cpll", 0,
 *                   RK3576_CLKSEL_CON(30), 10, 5, DFLAGS,
 *                   RK3576_CLKGATE_CON(3), 6, GFLAGS)
 *
 *   父时钟 cpll = 1000MHz（dtsi 的 assigned-clock-rates），
 *   1000/125 = 8，寄存器写 8-1 = 7。
 *
 *   ★ GMAC1 不在 CON(30) 的相邻位域，而是**另一个寄存器 CON(31) 的
 *     [4:0]**。出处 u-boot/arch/arm/include/asm/arch-rockchip/cru_rk3576.h：
 *       CLK_GMAC0_125M_DIV_SHIFT = 10（con30）
 *       CLK_GMAC1_125M_DIV_SHIFT =  0（con31）
 *     此处原先按"相邻位域"猜测，并且整段只在 port==0 时执行 ——
 *     GMAC1 的 125M 分频从来没被设过。
 */

#define GMAC0_CLKSEL_125M_CON   30
#define GMAC0_CLKSEL_125M_SHIFT 10
#define GMAC0_CLKSEL_125M_WIDTH 5
#define GMAC1_CLKSEL_125M_CON   31
#define GMAC1_CLKSEL_125M_SHIFT 0
#define GMAC1_CLKSEL_125M_WIDTH 5
#define GMAC_125M_DIV_VALUE     (8 - 1)

/* ★ PHY 的 25MHz 参考时钟由 SoC 送出，板上没有晶振。
 *
 *   出处：rk3576-kickpi-ethernet-gmac0.dtsi 的 PHY 节点
 *     rgmii_phy0: phy@1 {
 *       reg = <0x0>; clocks = <&cru REFCLKO25M_GMAC0_OUT>;
 *     };
 *   dwmac-rk.c 的 gmac_clk_enable() 里 clk_prepare_enable(clk_phy)，
 *   位置在 set_to_rgmii() 与放 PHY 复位**之前**。
 *
 *   ★ 此前我看到 dtsi 的 pinctrl-0 把 &ethm0_clk0_25m_out 注释掉了，
 *     就断定"PHY 自带晶振、不需要 SoC 送时钟"—— 那是错的。被注释掉的
 *     只是引脚组，时钟本身是通过 PHY 节点的 clocks 属性经 CRU 使能的。
 *     没有它，PHY 的模拟部分不工作，链路永远建不起来。
 *
 *   ★ 同时纠正一个因果倒置：RGMII 的 RXCLK 是 PHY **在链路建立之后**
 *     才输出的，所以"RXCLK 恒低"是链路建不起来的*结果*，不是根因。
 *     照着"先修 RXCLK"去查，方向就反了。
 *
 *   COMPOSITE(REFCLKO25M_GMAC0_OUT, "refclko25m_gmac0_out", gpll_cpll_p, 0,
 *             RK3576_CLKSEL_CON(36),  7, 1, MFLAGS, 0, 7, DFLAGS,
 *             RK3576_CLKGATE_CON(5), 10, GFLAGS)
 *   COMPOSITE(REFCLKO25M_GMAC1_OUT, ...
 *             CON(36), 15, 1, MFLAGS, 8, 7, DFLAGS,
 *             RK3576_CLKGATE_CON(5), 11, GFLAGS)
 *
 *   mux 1 = CPLL = 1000MHz，1000/40 = 25MHz 整除；
 *   mux 0 = GPLL = 1188MHz，1188/25 = 47.52 除不尽，只能选 CPLL。
 */

#define GMAC_REFCLK25M_CON       36
#define GMAC_REFCLK25M_GATE_CON  5
#define GMAC0_REFCLK25M_SHIFT    0
#define GMAC1_REFCLK25M_SHIFT    8
#define GMAC_REFCLK25M_DIV_WIDTH 7
#define GMAC0_REFCLK25M_GATE     10
#define GMAC1_REFCLK25M_GATE     11
#define GMAC_REFCLK25M_DIV       (40 - 1)
#define GMAC_REFCLK25M_SEL_CPLL  1

/* 25M 输出引脚。出处 rk3576-pinctrl.dtsi：
 *   ethm0_clk0_25m_out = <3 RK_PA4 3>  → bank3 pin4  功能3
 *   ethm0_clk1_25m_out = <2 RK_PD6 2>  → bank2 pin30 功能2
 */

#define GMAC0_CLK25M_BANK  3
#define GMAC0_CLK25M_PIN   4
#define GMAC0_CLK25M_FUNC  3
#define GMAC1_CLK25M_BANK  2
#define GMAC1_CLK25M_PIN   30
#define GMAC1_CLK25M_FUNC  2

/* ★ 每路 25M 还有一个 m1 备选脚：
 *     ethm1_clk0_25m_out = <2 RK_PD7 3>  → bank2 pin31 功能3
 *     ethm1_clk1_25m_out = <1 RK_PD5 1>  → bank1 pin29 功能1
 *
 *   之前只配了 m0，并据"焊盘上量到跳变"宣布 25M 就绪 —— 但那只证明
 *   时钟离开了 SoC 的那个引脚，**没有证明它到达了 PHY**。而 dtsi 把
 *   引脚组整个注释掉，很可能就是因为这块板走的不是 m0 那条。
 *
 *   两条都打开。驱动一个没有走线的引脚是无害的，而漏掉正确的那条
 *   会让整条链路一直查不出原因。
 */

#define GMAC0_CLK25M_BANK_ALT  2
#define GMAC0_CLK25M_PIN_ALT   31
#define GMAC0_CLK25M_FUNC_ALT  3
#define GMAC1_CLK25M_BANK_ALT  1
#define GMAC1_CLK25M_PIN_ALT   29
#define GMAC1_CLK25M_FUNC_ALT  1

/* ★ RGMII 的 TX/RX 延时线在 ioc_grf，不在 sdgmac_grf。
 *
 *   出处 dwmac-rk.c 的 rk3576_set_to_rgmii()：
 *     rockchip,php_grf = <&ioc_grf>   ioc_grf @ 0x26040000
 *     GMAC0 -> VCCIO0_1_3_IOC_CON2 (0x6408)，同时写 CON3 (0x640c)
 *     GMAC1 -> VCCIO0_1_3_IOC_CON4 (0x6410)，同时写 CON5 (0x6414)
 *   两条都写，是因为 m0/m1 两组引脚各有一份延时配置。
 *
 *   位域（hiword，高 16 位是写使能）：
 *     bit15    RXCLK 延时使能
 *     bit7     TXCLK 延时使能
 *     [14:8]   RX 延时值
 *     [6:0]    TX 延时值
 *
 *   本板 phy-mode = "rgmii-rxid"，即 RX 延时由 PHY 内部做，SoC 侧关掉；
 *   kernel 传的是 set_to_rgmii(tx_delay, -1)。
 *   tx_delay：GMAC0 = 0x21，GMAC1 = 0x20（出处 kickpi 的 ethernet dtsi）。
 */

#define IOC_GRF_ADDR         0x26040000
#define IOC_GMAC0_DLY_CON    0x6408
#define IOC_GMAC1_DLY_CON    0x6410

#define GMAC_DLY_ENABLE      0x80800080u   /* TX 延时开、RX 延时关 */
#define GMAC0_TX_DELAY       0x21
#define GMAC1_TX_DELAY       0x20
#define GMAC_DLY_VALUE(tx)   (0x007f0000u | (uint32_t)(tx))

#define SDGMAC_GRF_ADDR      0x26038000
#define GRF_GMAC_CON0        0x0020
#define GRF_GMAC_CON1        0x0024

#define GRF_RMII_MODE        (1u << 3)    /* 1=RMII 0=RGMII */
#define GRF_IO_CLKSEL_IO     (1u << 7)
#define GRF_GMII_CLK_SEL_MASK (3u << 5)

/* gmii_clk_sel 的取值，出处 dwmac-rk.c */

#define GMAC_CLK_DIV1_125M    0u    /* 1000Mbps */
#define GMAC_CLK_DIV5_25M     1u    /*  100Mbps */
#define GMAC_CLK_DIV50_2_5M   2u    /*   10Mbps */

/* ★ RGMII 引脚复用。
 *
 *   出处：原厂 dtb 的 ethernet@2a220000 的 pinctrl-0，五组共 14 个引脚，
 *   功能号均为 3：
 *       eth0m0-miim        g3-6  g3-5              MDC / MDIO
 *       eth0m0-rx_bus2     g3-7  g3-10 g3-9        RXD0/1 RXDV
 *       eth0m0-tx_bus2     g3-11 g3-13 g3-12       TXD0/1 TXEN
 *       eth0m0-rgmii_clk   g3-25 g3-14             RXCLK / TXCLK
 *       eth0m0-rgmii_bus   g3-27 g3-26 g3-19 g3-18 RXD2/3 TXD2/3
 *
 *   不配复用时 MAC 的时钟脚没接到 PHY，收不到 RX 时钟，DMA 软复位的
 *   SWR 位永不自清 —— 与"功能时钟没开""模块复位没撤""PHY 被摁着"
 *   "GRF 没配"表现完全一致。本端口在这五者之间排除了五轮。
 */

static const uint8_t g_gmac0_pins[] =
{
  6, 5,                    /* miim       */
  7, 10, 9,                /* rx_bus2    */
  11, 13, 12,              /* tx_bus2    */
  25, 14,                  /* rgmii_clk  */
  27, 26, 19, 18           /* rgmii_bus  */
};

/* ★ GMAC1 有自己的一整套引脚，和 GMAC0 完全不在一个 bank。
 *
 *   出处 rk3576-pinctrl.dtsi 的 eth1m0-* 组，本板 gmac1 dtsi 选的正是 m0：
 *
 *     eth1m0_miim        2 PD4 PD5           MDC / MDIO
 *     eth1m0_rx_bus2     2 PD3 PD1 PD2       RXD0/1 RXDV
 *     eth1m0_tx_bus2     2 PD0 PC6 PC7       TXD0/1 TXEN
 *     eth1m0_rgmii_clk   2 PC2 PC5           RXCLK / TXCLK
 *     eth1m0_rgmii_bus   2 PC0 PC1 PC3 PC4   RXD2/3 TXD2/3
 *
 *   全部 bank 2、功能 2（GMAC0 是 bank 3、功能 3）。
 *
 *   ★ 之前只有一张表，probe(1) 会把 GMAC0 的引脚**再复用一遍** —— 不但
 *     GMAC1 没配上，还会扰动已经配好的 GMAC0。日志里两个口都报
 *     "RXCLK g3-25" 就是这个原因：GMAC1 在采样 GMAC0 的脚。
 *
 *     一个只对 port 0 正确的静态表，被一个带 port 参数的函数使用 ——
 *     参数化了控制寄存器却没参数化引脚表，是很容易漏掉的一半。
 */

static const uint8_t g_gmac1_pins[] =
{
  28, 29,                  /* miim       PD4 PD5         */
  27, 25, 26,              /* rx_bus2    PD3 PD1 PD2     */
  24, 22, 23,              /* tx_bus2    PD0 PC6 PC7     */
  18, 21,                  /* rgmii_clk  PC2 PC5         */
  16, 17, 19, 20           /* rgmii_bus  PC0 PC1 PC3 PC4 */
};

#define GMAC0_PIN_BANK       3
#define GMAC0_PIN_FUNC       3
#define GMAC1_PIN_BANK       2
#define GMAC1_PIN_FUNC       2

#define GMAC_PHY_ADDR        0      /* dtb: mdio/phy@1 的 reg = <0> */
#define GMAC_MDIO_TIMEOUT_US 100000

/* Maxio PHY 的页选择寄存器 */

#define MAXIO_PAGE_SELECT    0x1f

/****************************************************************************
 * Private Types
 ****************************************************************************/

#ifdef CONFIG_RK3576_GMAC_DIAG
struct gmac_clkpin_s
{
  const char *name;
  uint8_t     pin;
};
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const uintptr_t g_gmac_base[2] =
{
  RK3576_GMAC0_ADDR, RK3576_GMAC1_ADDR
};

#ifdef CONFIG_RK3576_GMAC_DIAG
/* 时钟脚采样用的 GMAC0 引脚（GMAC1 的在代码里按端口另选） */

static const struct gmac_clkpin_s g_gmac_clkpins[] =
{
  {
    "RXCLK(PHY 送来)", 25
  },
  {
    "TXCLK(SoC 送出)", 14
  },
};
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t gmac_getreg(int port, uint32_t off)
{
  return getreg32(g_gmac_base[port] + off);
}

static inline void gmac_putreg(int port, uint32_t off, uint32_t val)
{
  putreg32(val, g_gmac_base[port] + off);
}

/****************************************************************************
 * Name: gmac_mdio_read
 *
 * Description:
 *   通过 MDIO 读 PHY 的一个寄存器。
 *
 *   ★ CR 分频必须让 MDC 落在 2.5MHz 以下（IEEE 802.3 规定）。
 *     这里取 CR=4（÷102），CSR 时钟按 100MHz 算得 MDC≈1MHz。
 *     分频太快时表现为读回 0xffff —— 与"PHY 不存在"完全一样，
 *     所以读不到 PHY 时要先怀疑分频，再怀疑硬件。
 *
 ****************************************************************************/

static int gmac_mdio_write(int port, uint8_t phyaddr, uint8_t regaddr,
                           uint16_t value)
{
  uint32_t addr;
  int us;

  for (us = 0; us < GMAC_MDIO_TIMEOUT_US; us++)
    {
      if ((gmac_getreg(port, RK3576_GMAC_MAC_MDIO_ADDR) &
           GMAC_MDIO_ADDR_GB) == 0)
        {
          break;
        }

      up_udelay(1);
    }

  if (us >= GMAC_MDIO_TIMEOUT_US)
    {
      return -EBUSY;
    }

  /* 数据要先于命令写入 —— GB 一置位硬件就开始搬 MDIO_DATA。 */

  gmac_putreg(port, RK3576_GMAC_MAC_MDIO_DATA, value);

  addr = ((uint32_t)phyaddr << GMAC_MDIO_ADDR_PA_SHIFT) |
         ((uint32_t)regaddr << GMAC_MDIO_ADDR_RDA_SHIFT) |
         (4u << GMAC_MDIO_ADDR_CR_SHIFT) |
         GMAC_MDIO_ADDR_GOC_WRITE |
         GMAC_MDIO_ADDR_GB;

  gmac_putreg(port, RK3576_GMAC_MAC_MDIO_ADDR, addr);

  for (us = 0; us < GMAC_MDIO_TIMEOUT_US; us++)
    {
      if ((gmac_getreg(port, RK3576_GMAC_MAC_MDIO_ADDR) &
           GMAC_MDIO_ADDR_GB) == 0)
        {
          return OK;
        }

      up_udelay(1);
    }

  return -ETIMEDOUT;
}

static int gmac_mdio_read(int port, uint8_t phyaddr, uint8_t regaddr,
                          uint16_t *value);

/****************************************************************************
 * Name: maxio_write_paged
 *
 * Description:
 *   Maxio MAE0621A 的分页寄存器写。
 *
 *   这颗 PHY 的绝大多数配置不在 IEEE 标准寄存器里，而在分页空间：
 *   先把页号写进 0x1f，再访问目标寄存器，最后把页号写回去。
 *   出处 kernel-6.1/drivers/net/phy/maxio.c 的 maxio_write_paged()。
 *
 ****************************************************************************/

void maxio_write_paged(int port, uint16_t page, uint8_t reg,
                              uint16_t val)
{
  uint16_t oldpage = 0;

  gmac_mdio_read(port, GMAC_PHY_ADDR, MAXIO_PAGE_SELECT, &oldpage);
  gmac_mdio_write(port, GMAC_PHY_ADDR, MAXIO_PAGE_SELECT, page);
  gmac_mdio_write(port, GMAC_PHY_ADDR, reg, val);
  gmac_mdio_write(port, GMAC_PHY_ADDR, MAXIO_PAGE_SELECT, oldpage);
}

/****************************************************************************
 * Name: maxio_mae0621a_init
 *
 * Description:
 *   Maxio MAE0621A-Q2C 的厂商初始化。不做这一步，PHY 的模拟前端不工作：
 *   MDIO 读写完全正常（MDC 由 MAC 提供，与 PHY 自身时钟无关），
 *   但自协商永远不完成、收不到对端的 FLP、网口灯不亮。
 *
 *   ★ 关键是第一条 write_paged(0xd92, 0x02, 0x200a) —— 内核源码在它
 *     后面紧跟着打印 "clkmode(crystal)"，即**把 PHY 的时钟模式设成
 *     晶振**。板上 PHY 用的是自己的晶振，而复位默认值不是这个模式。
 *
 *   其余是模拟前端整定值，逐条照抄 maxio.c 的 maxio_mae0621a_probe()
 *   与 maxio_mae0621a_config_init()，顺序不能改。
 *
 *   PHY 型号是从 Android 的启动日志认出来的：
 *     PHY [stmmac-0:00] driver [MAE0621A-Q2C Gigabit Ethernet]
 *   驱动里 .phy_id = 0x7b744411，与我们 MDIO 读到的完全一致 ——
 *   也就是说前面几轮怀疑"MDIO 不可靠"是错的，读数一直是真的。
 *
 ****************************************************************************/

void maxio_mae0621a_init(int port)
{
  /* probe()：时钟模式 = 晶振 */

  maxio_write_paged(port, 0xd92, 0x02, 0x200a);
  gmac_mdio_write(port, GMAC_PHY_ADDR, MAXIO_PAGE_SELECT, 0x0);
  up_mdelay(100);

  /* config_init()：模拟前端整定 */

  maxio_write_paged(port, 0xd92, 0x02, 0x200a);
  maxio_write_paged(port, 0xda0, 0x10, 0x0c13);
  maxio_write_paged(port, 0x000, 0x0d, 0x0007);
  maxio_write_paged(port, 0x000, 0x0e, 0x003c);
  maxio_write_paged(port, 0x000, 0x0d, 0x4007);
  maxio_write_paged(port, 0x000, 0x0e, 0x0000);
  maxio_write_paged(port, 0xd96, 0x13, 0x07bc);
  maxio_write_paged(port, 0xd8f, 0x08, 0x2500);
  maxio_write_paged(port, 0xd90, 0x02, 0x1555);
  maxio_write_paged(port, 0xd90, 0x05, 0x2b15);
  maxio_write_paged(port, 0xd92, 0x14, 0x000a);
  maxio_write_paged(port, 0xd91, 0x07, 0x5b00);
  maxio_write_paged(port, 0xd8f, 0x00, 0x0300);
  maxio_write_paged(port, 0xd92, 0x0a, 0x8506);
  maxio_write_paged(port, 0xd91, 0x06, 0x6870);
  maxio_write_paged(port, 0xd91, 0x01, 0x0940);
  maxio_write_paged(port, 0xda0, 0x13, 0x1303);
  maxio_write_paged(port, 0xd97, 0x0c, 0x0177);
  maxio_write_paged(port, 0xd97, 0x0b, 0x09a9);
  maxio_write_paged(port, 0xa42, 0x12, 0x0028);
  maxio_write_paged(port, 0x000, 0x04, 0x0de1);
  maxio_write_paged(port, 0x000, 0x00, 0x9140);

  gmac_mdio_write(port, GMAC_PHY_ADDR, MAXIO_PAGE_SELECT, 0x0);

  /* self_check() 的收尾两条（ADC 校准通过后执行的那一段） */

  maxio_write_paged(port, 0xd96, 0x02, 0x0fff);
  maxio_write_paged(port, 0x000, 0x00, 0x9140);
  gmac_mdio_write(port, GMAC_PHY_ADDR, MAXIO_PAGE_SELECT, 0x0);
  up_mdelay(100);

  syslog(LOG_INFO, "GMAC%d: MAE0621A 厂商初始化完成（时钟模式=晶振）\n",
         port);
}

static int gmac_mdio_read(int port, uint8_t phyaddr, uint8_t regaddr,
                          uint16_t *value)
{
  uint32_t addr;
  int us;

  /* 等上一次操作结束 */

  for (us = 0; us < GMAC_MDIO_TIMEOUT_US; us++)
    {
      if ((gmac_getreg(port, RK3576_GMAC_MAC_MDIO_ADDR) &
           GMAC_MDIO_ADDR_GB) == 0)
        {
          break;
        }

      up_udelay(1);
    }

  if (us >= GMAC_MDIO_TIMEOUT_US)
    {
      return -EBUSY;
    }

  addr = ((uint32_t)phyaddr << GMAC_MDIO_ADDR_PA_SHIFT) |
         ((uint32_t)regaddr << GMAC_MDIO_ADDR_RDA_SHIFT) |
         (4u << GMAC_MDIO_ADDR_CR_SHIFT) |
         GMAC_MDIO_ADDR_GOC_READ |
         GMAC_MDIO_ADDR_GB;

  gmac_putreg(port, RK3576_GMAC_MAC_MDIO_ADDR, addr);

  for (us = 0; us < GMAC_MDIO_TIMEOUT_US; us++)
    {
      if ((gmac_getreg(port, RK3576_GMAC_MAC_MDIO_ADDR) &
           GMAC_MDIO_ADDR_GB) == 0)
        {
          *value = (uint16_t)gmac_getreg(port, RK3576_GMAC_MAC_MDIO_DATA);
          return OK;
        }

      up_udelay(1);
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_gmac_set_speed
 *
 * Description:
 *   按协商出的速率配 GRF 里 RGMII 的 TX 时钟分频。
 *
 *   出处 dwmac-rk.c 的 rk3576_set_gmac_speed()：
 *     1000M -> RGMII_DIV1   bits[6:5] = 00
 *      100M -> RGMII_DIV5   bits[6:5] = 11
 *       10M -> RGMII_DIV50  bits[6:5] = 10
 *
 *   ★ 这一处和 MAC_CONFIGURATION 的 PS/FES 必须一致。只配 MAC 不配 GRF
 *     的话，链路照样是 up 的，但 TX 时钟频率不对，数据发不出去 ——
 *     一个"链路好、就是不通"的典型来源。
 *
 ****************************************************************************/

void rk3576_gmac_set_speed(int port, int speed)
{
  uint32_t off = port ? GRF_GMAC_CON1 : GRF_GMAC_CON0;
  uint32_t val;

  switch (speed)
    {
      case 1000:
        val = (3u << 5) << 16;                 /* bits[6:5] = 00 */
        break;

      case 100:
        val = ((3u << 5) << 16) | (3u << 5);   /* bits[6:5] = 11 */
        break;

      default:
        val = ((3u << 5) << 16) | (2u << 5);   /* bits[6:5] = 10 */
        break;
    }

  putreg32(val, SDGMAC_GRF_ADDR + off);
  syslog(LOG_INFO,
         "GMAC%d: 速率 %dM，GRF[0x%02" PRIx32 "]=0x%08" PRIx32 "\n",
         port, speed, off, getreg32(SDGMAC_GRF_ADDR + off));
}

/****************************************************************************
 * Name: rk3576_gmac_refclk25m
 *
 * Description:
 *   打开送给 PHY 的 25MHz 参考时钟，并把它复用到引脚上。
 *
 *   ★ 必须在放 PHY 复位**之前**调用 —— PHY 出复位时就要有参考时钟，
 *     否则它的模拟前端不启动，自协商永远不开始。SDK 里的顺序同样是
 *     gmac_clk_enable(clk_phy) → set_to_rgmii → 放复位。
 *
 ****************************************************************************/

void rk3576_gmac_refclk25m(int port)
{
  int shift = port ? GMAC1_REFCLK25M_SHIFT : GMAC0_REFCLK25M_SHIFT;
  int bank  = port ? GMAC1_CLK25M_BANK : GMAC0_CLK25M_BANK;
  int pin   = port ? GMAC1_CLK25M_PIN  : GMAC0_CLK25M_PIN;
  int func  = port ? GMAC1_CLK25M_FUNC : GMAC0_CLK25M_FUNC;
  int prev;
  int edges;
  int high;
  int k;

  syslog(LOG_INFO, "GMAC%d: 25M 使能前 div=%u sel=%u 引脚 g%d-%d 功能=%d\n",
         port,
         rk3576_clk_getmux(GMAC_REFCLK25M_CON, shift,
                           GMAC_REFCLK25M_DIV_WIDTH),
         rk3576_clk_getmux(GMAC_REFCLK25M_CON,
                           shift + GMAC_REFCLK25M_DIV_WIDTH, 1),
         bank, pin, rk3576_pinmux_get(bank, pin));

  rk3576_clk_setmux(GMAC_REFCLK25M_CON, shift, GMAC_REFCLK25M_DIV_WIDTH,
                    GMAC_REFCLK25M_DIV);
  rk3576_clk_setmux(GMAC_REFCLK25M_CON, shift + GMAC_REFCLK25M_DIV_WIDTH, 1,
                    GMAC_REFCLK25M_SEL_CPLL);
  rk3576_clk_gate(GMAC_REFCLK25M_GATE_CON,
                  port ? GMAC1_REFCLK25M_GATE : GMAC0_REFCLK25M_GATE, true);

  /* ★ 不复用 25M 输出引脚。
   *
   *   Android 在同一块板上把 refclko25m_gmac0/1_out 使能到 25MHz
   *   （clk_summary 可见），但 pinmux-pins 里**没有任何 25m_out 引脚组**
   *   —— 时钟使能了却不接出去。也就是说 PHY 用的是板上晶振，
   *   SoC 这一路是空的。
   *
   *   我先按"dtsi 注释掉引脚组 = PHY 自带晶振"判断（对的），
   *   又因为看到 PHY 节点的 clocks 属性而推翻它（错的），还接连试了
   *   m0/m1 两组引脚。真正的判据一直在参照系统里摆着：
   *   **要证明一个外设怎么配，去看能工作的那个系统实际配了什么。**
   */

  UNUSED(bank);
  UNUSED(pin);
  UNUSED(func);

  syslog(LOG_INFO,
         "GMAC%d: 25M 已开 CPLL/40 回读 div=%u sel=%u 引脚功能=%d\n",
         port,
         rk3576_clk_getmux(GMAC_REFCLK25M_CON, shift,
                           GMAC_REFCLK25M_DIV_WIDTH),
         rk3576_clk_getmux(GMAC_REFCLK25M_CON,
                           shift + GMAC_REFCLK25M_DIV_WIDTH, 1),
         rk3576_pinmux_get(bank, pin));

  /* ★ 证实时钟真的从焊盘出来了，而不是只把寄存器写对了。
   *
   *   "配了寄存器"和"信号出来了"是两件事。这一轮我已经据"回读正确"
   *   宣布过一次 25M 就绪，但那只证明了 CRU 的位写进去了。
   *
   *   复用态下 GPIO 输入缓冲仍能读到焊盘电平（RXCLK/TXCLK 的采样用的
   *   就是这个办法，并给出过有意义的高/低/跳变计数），所以这里同样
   *   采样计跳变：有跳变 = 时钟确实在引脚上；恒定 = 没出来。
   *   25MHz 远快于采样速率，会混叠，但"有没有跳变"这个判据不受影响。
   */

  prev = rk3576_gpio_read(bank, pin);
  edges = 0;
  high = 0;

  for (k = 0; k < 200; k++)
    {
      int v = rk3576_gpio_read(bank, pin);

      if (v)
        {
          high++;
        }

      if (v != prev)
        {
          edges++;
        }

      prev = v;
    }

  syslog(LOG_INFO,
         "GMAC%d: 25M 焊盘 g%d-%d 采样 200 次 高=%d 跳变=%d —— %s\n",
         port, bank, pin, high, edges,
         edges ? "时钟已输出" : "★无跳变，时钟没到引脚");
}

void rk3576_gmac_phy_reset(int bank, int pin, bool active_low)
{
  /* 复位时序取自本板设备树，而不是手册的最小值。
   *
   *   rk3576-kickpi-ethernet-gmac0.dtsi:
   *     snps,reset-delays-us = <0 20000 100000>;
   *     // Reset time is 20ms, 100ms for rtl8211f
   *
   *   三个数分别是"拉低前等待 / 保持复位 / 释放后等待"。原来这里用的是
   *   手册最小值 10ms/30ms —— 能过手册，但比板厂实测值短。厂商会把这类
   *   值往上调，通常是因为板上真的需要（电源建立慢、晶振起振慢）。
   *   **有板级实测值时用板级的，手册最小值只是下界。**
   */

  rk3576_gpio_setdir(bank, pin, true);
  rk3576_gpio_write(bank, pin, active_low ? false : true);
  up_mdelay(20);
  rk3576_gpio_write(bank, pin, active_low ? true : false);
  up_mdelay(100);
}

int rk3576_gmac_probe(int port)
{
  uint32_t version;
  uint16_t id1 = 0xffff;
  uint16_t id2 = 0xffff;
  int ret;
  int us;
  int con;
  int shift;
  int width;
  const uint8_t *pins;
  int bank;
  int func;
  size_t npins;
  size_t i;
  uint32_t grf_off;
  uint32_t val;
  uint32_t dly;
  uint32_t rl_before;
  uint32_t rl_back;
  uint16_t bmsr;
  uint16_t lp;
#ifdef CONFIG_RK3576_GMAC_DIAG
  uint16_t v0;
  uint16_t v;
  int same;
  int k;
  int a;
  int n;
  uint16_t back;
  int wr;
  uint16_t bmcr;
#endif

  if (port < 0 || port > 1)
    {
      return -EINVAL;
    }

  /* 两个 GMAC 同属 PD_SDGMAC */

  ret = rk3576_power_on(RK3576_PD_SDGMAC);
  if (ret < 0)
    {
      syslog(LOG_ERR, "GMAC%d: PD_SDGMAC 上电失败: %d\n", port, ret);
      return ret;
    }

  rk3576_clk_gate(GMAC_GATE_CON,
                  port ? GMAC1_GATE_ACLK : GMAC0_GATE_ACLK, true);
  rk3576_clk_gate(GMAC_GATE_CON,
                  port ? GMAC1_GATE_PCLK : GMAC0_GATE_PCLK, true);
  rk3576_clk_gate(GMAC_GATE_CON_125M,
                  port ? GMAC1_GATE_125M : GMAC0_GATE_125M, true);
  rk3576_clk_gate(GMAC_GATE_CON_RMII,
                  port ? GMAC1_GATE_RMII : GMAC0_GATE_RMII, true);
  rk3576_clk_gate(port ? GMAC1_GATE_PTP_CON : GMAC0_GATE_PTP_CON,
                  port ? GMAC1_GATE_PTP : GMAC0_GATE_PTP, true);

  /* ★ 设 125M 分频。只开门控不设分频，输出不是 125MHz。 */

  con = port ? GMAC1_CLKSEL_125M_CON   : GMAC0_CLKSEL_125M_CON;
  shift = port ? GMAC1_CLKSEL_125M_SHIFT : GMAC0_CLKSEL_125M_SHIFT;
  width = port ? GMAC1_CLKSEL_125M_WIDTH : GMAC0_CLKSEL_125M_WIDTH;

  rk3576_clk_setmux(con, shift, width, GMAC_125M_DIV_VALUE);
  syslog(LOG_INFO, "GMAC%d: 125M 分频 CLKSEL_CON(%d)[%d:%d]=%u（回读 %u）\n",
         port, con, shift + width - 1, shift, GMAC_125M_DIV_VALUE,
         rk3576_clk_getmux(con, shift, width));

  /* ★ 引脚复用。必须在 GRF 与复位之前 —— 时钟线没接出去时，
   * 后面无论怎么配都收不到 RX 时钟。
   */

  pins = port ? g_gmac1_pins : g_gmac0_pins;
  bank = port ? GMAC1_PIN_BANK : GMAC0_PIN_BANK;
  func = port ? GMAC1_PIN_FUNC : GMAC0_PIN_FUNC;
  npins = port ? sizeof(g_gmac1_pins) : sizeof(g_gmac0_pins);

  for (i = 0; i < npins; i++)
    {
      rk3576_pinmux_set(bank, pins[i], func);
    }

  syslog(LOG_INFO,
         "GMAC%d: 复用 %d 脚 bank%d 功能%d（抽查 rxclk g%d-%d→%d）\n",
         port, (int)npins, bank, func, bank, pins[8],
         rk3576_pinmux_get(bank, pins[8]));

  /* ★ GRF：选 RGMII 模式并接通时钟。
   *
   *   这一步不做的话，MAC 侧拿不到有效的 TX/RX 时钟，DMA 软复位
   *   永远完不成 —— 而寄存器读写、MAC_VERSION、HW_FEATURE 全都正常，
   *   现象与"功能时钟没开"、"模块复位没撤"、"PHY 被摁着"完全一致。
   *   本端口在这四者之间逐个排除了四轮。
   */

  grf_off = port ? GRF_GMAC_CON1 : GRF_GMAC_CON0;

  /* 清 RMII_MODE 选 RGMII；io_clksel 用 CRU 出时钟；
   * gmii_clk_sel 按千兆选 DIV1_125M。
   *
   * ★ 该字段是按速率选 TX 时钟分频的，不能一律清零 ——
   *   出处：dwmac-rk.c 的 rk_gmac_rgmii_clk_div()
   *     SPEED_1000 → GMAC_CLK_DIV1_125M
   *     SPEED_100  → GMAC_CLK_DIV5_25M
   *     SPEED_10   → GMAC_CLK_DIV50_2_5M
   */

  val = ((GRF_RMII_MODE | GRF_IO_CLKSEL_IO | GRF_GMII_CLK_SEL_MASK) << 16)
        | (GMAC_CLK_DIV1_125M << 5);
  putreg32(val, SDGMAC_GRF_ADDR + grf_off);

  syslog(LOG_INFO, "GMAC%d: GRF[0x%02" PRIx32 "]=0x%08" PRIx32 "\n",
         port, grf_off, getreg32(SDGMAC_GRF_ADDR + grf_off));

  /* ★ RGMII 延时线。
   *
   *   链路建立不需要它（那是 PHY 自己的事），但**收发数据需要** ——
   *   延时不对时表现为链路正常、丢包或 CRC 错。所以放在这里配，
   *   而不是等出问题再回来找。
   */

  dly = IOC_GRF_ADDR +
        (port ? IOC_GMAC1_DLY_CON : IOC_GMAC0_DLY_CON);
  val = GMAC_DLY_VALUE(port ? GMAC1_TX_DELAY : GMAC0_TX_DELAY);

  putreg32(GMAC_DLY_ENABLE, dly);
  putreg32(GMAC_DLY_ENABLE, dly + 4);
  putreg32(val, dly);
  putreg32(val, dly + 4);

  syslog(LOG_INFO,
         "GMAC%d: RGMII 延时 tx=0x%02x rx=关（回读 0x%08" PRIx32 "）\n",
         port, port ? GMAC1_TX_DELAY : GMAC0_TX_DELAY, getreg32(dly));

  up_udelay(100);

  /* ★ 撤销模块复位。
   *
   *   引导器没用过 GMAC，其复位仍是置位状态。此时寄存器读写正常、
   *   MAC_VERSION 也读得到，但状态机不工作 —— 表现为 DMA 软复位的
   *   自清位永远不归零，与"功能时钟没开"完全一样。
   *   本端口实测：补齐四路时钟后仍超时，撤销复位才通。
   */

  rk3576_reset(port ? SRST_A_GMAC1 : SRST_A_GMAC0, false);
  rk3576_reset(port ? SRST_P_GMAC1 : SRST_P_GMAC0, false);
  up_udelay(100);

  /* 第一层判据：IP 版本。只能证明寄存器块活着。 */

  version = gmac_getreg(port, RK3576_GMAC_MAC_VERSION);
  if (version == 0 || version == 0xffffffff)
    {
      syslog(LOG_ERR,
             "GMAC%d: MAC_VERSION=0x%08" PRIx32
             " —— 电源域/时钟/基址有问题\n", port, version);
      return -ENODEV;
    }

  /* ★ PHY 的厂商初始化必须在这里 —— MAC 已出复位、MDIO 可用，
   *   而且要早于后面任何依赖链路的步骤。
   */

  maxio_mae0621a_init(port);

  /* DMA 软复位。自清位，要等它归零；不等就配 MDIO 会被复位冲掉。 */

  /* ★ 结构性问题先于细节：DMA 寄存器块本身能写吗？
   *
   *   前几轮都假定"SWR 卡住 = 缺某个前置条件"，却从未验证过 DMA 块
   *   是否可写。用一个纯数据寄存器（TXDESC 环长度，无副作用）做
   *   写回读：
   *     能写 → DMA 块活着，SWR 卡住是真的缺前置条件
   *     不能写 → 整块处于复位/无时钟，再补前置条件也没用
   */

  rl_before = gmac_getreg(port, RK3576_GMAC_DMA_CH0_TXDESC_RL);

  gmac_putreg(port, RK3576_GMAC_DMA_CH0_TXDESC_RL, 0x0000003f);
  rl_back = gmac_getreg(port, RK3576_GMAC_DMA_CH0_TXDESC_RL);

  syslog(LOG_INFO,
         "GMAC%d: DMA 块写测试 TXDESC_RL 原值=0x%08" PRIx32
         " 写 0x3f 读回=0x%08" PRIx32 " → %s\n",
         port, rl_before, rl_back,
         (rl_back & 0x3f) == 0x3f ? "可写" : "★不可写（整块无时钟/在复位）");

  /* MAC 块对照：MAC_ADDR0_HIGH 同样是纯数据寄存器 */

  gmac_putreg(port, RK3576_GMAC_MAC_ADDR0_HIGH, 0x00001234);
  syslog(LOG_INFO,
         "GMAC%d: MAC 块写测试 ADDR0_HIGH 写 0x1234 读回=0x%08" PRIx32
         "\n", port,
         gmac_getreg(port, RK3576_GMAC_MAC_ADDR0_HIGH) & 0xffff);

  syslog(LOG_INFO,
         "GMAC%d: 复位前 DMA_MODE=0x%08" PRIx32 " MAC_VERSION=0x%08" PRIx32
         "\n", port, gmac_getreg(port, RK3576_GMAC_DMA_MODE), version);
#ifdef CONFIG_RK3576_GMAC_DIAG

  /* ★ 复位之前先把「时钟到底在不在跑」测出来。
   *
   *   DWMAC 4.20a 的 SWR 只有在**所有时钟域都就绪**后才会自清，其中
   *   包含 PHY 送来的 RX 时钟。因此 SWR 不清有至少四种原因：
   *     a) PHY 没有输出 RX 时钟（没上电／被摁在复位／没插网线）
   *     b) SoC 侧的 TX 时钟没配好
   *     c) CRU 里还有门控没开
   *     d) GRF 的接口选择不对
   *   四者现象完全一样，此前在它们之间来回排除了多轮仍未定位。
   *
   *   直接测量：把时钟脚临时切成 GPIO 输入，连续采样若干次。时钟在跑
   *   的话采样值必然有 0 有 1（125MHz 对 GPIO 读是高度混叠的随机采样）；
   *   恒为同一个值就说明这根线是静止的。
   *
   *   这一步能把 (a)(b) 与 (c)(d) 彻底分开 —— 前者是板级/PHY 问题，
   *   后者是 SoC 配置问题，修法毫不相干。
   */

  for (i = 0; i < 2; i++)
    {
      int ones = 0;
      int j;
      int prev;
      int edges = 0;

      /* 采样的必须是**本端口**的时钟脚。写死 GMAC0 的 bank 时，
       * GMAC1 报出来的是 GMAC0 的引脚状态 —— 一份看起来很具体、
       * 实际张冠李戴的读数，比没有读数更容易误导。
       */

      int cbank = port ? GMAC1_PIN_BANK : GMAC0_PIN_BANK;
      int cpin  = port ? (i ? 21 : 18) : g_gmac_clkpins[i].pin;

      rk3576_pinmux_set(cbank, cpin, RK3576_PINMUX_GPIO);
      rk3576_pinmux_setpull(cbank, cpin, RK3576_PULL_NONE);
      rk3576_gpio_setdir(cbank, cpin, false);
      up_udelay(50);

      prev = rk3576_gpio_read(cbank, cpin);

      for (j = 0; j < 200; j++)
        {
          int lvl = rk3576_gpio_read(cbank, cpin);

          ones += lvl;
          if (lvl != prev)
            {
              edges++;
            }

          prev = lvl;
        }

      syslog(LOG_INFO,
             "GMAC%d: %s g%d-%u 采样 200 次 高=%d 跳变=%d —— %s\n",
             port, g_gmac_clkpins[i].name, cbank, cpin,
             ones, edges,
             edges > 0 ? "有跳变，时钟在跑" :
                         (ones == 0 ? "恒低，无时钟" : "恒高，无时钟"));

      /* 串口在连续输出时会丢字节，这两行是本次测量的全部结论，
       * 丢一行就等于白测，所以留出间隔。
       */

      up_mdelay(20);

      /* 测完还回 RGMII 功能 —— 诊断改了复用必须还原，
       * 否则后面的现象都是自己造出来的（本项目在触摸那次踩过）。
       */

      rk3576_pinmux_set(cbank, cpin,
                        port ? GMAC1_PIN_FUNC : GMAC0_PIN_FUNC);
    }
#endif /* CONFIG_RK3576_GMAC_DIAG */

  gmac_putreg(port, RK3576_GMAC_DMA_MODE, GMAC_DMA_MODE_SWR);

  syslog(LOG_INFO, "GMAC%d: 写 SWR 后立刻回读 DMA_MODE=0x%08" PRIx32 "\n",
         port, gmac_getreg(port, RK3576_GMAC_DMA_MODE));
  for (us = 0; us < 100000; us++)
    {
      if ((gmac_getreg(port, RK3576_GMAC_DMA_MODE) &
           GMAC_DMA_MODE_SWR) == 0)
        {
          break;
        }

      up_udelay(1);
    }

  if (us >= 100000)
    {
      syslog(LOG_ERR,
             "GMAC%d: DMA 软复位超时 DMA_MODE=0x%08" PRIx32
             " SYSBUS=0x%08" PRIx32 " MAC_CFG=0x%08" PRIx32
             " HW_FEAT0=0x%08" PRIx32 " CH0_STAT=0x%08" PRIx32 "\n",
             port,
             gmac_getreg(port, RK3576_GMAC_DMA_MODE),
             gmac_getreg(port, RK3576_GMAC_DMA_SYSBUS_MODE),
             gmac_getreg(port, RK3576_GMAC_MAC_CONFIG),
             gmac_getreg(port, RK3576_GMAC_MAC_HW_FEATURE0),
             gmac_getreg(port, RK3576_GMAC_DMA_CH0_STATUS));

#ifdef CONFIG_RK3576_GMAC_DIAG
      /* ★ 超时了也要先读一次 PHY ID 再返回。
       *
       *   MDC/MDIO 由 MAC 自己驱动，**不依赖 PHY 送来的 RX 时钟** ——
       *   所以即使 DMA 软复位卡住，这条链路仍然可用。而它恰好能把两种
       *   完全不同的情况一句话分开：
       *
       *     读到合法厂商 ID  -> PHY 供电、复位、MDIO 都正常，
       *                         问题只在 RGMII 的时钟/延时通路上
       *     读回 0xffff      -> PHY 根本没应答，先查供电与复位，
       *                         再谈时钟
       *
       *   原来的代码把 PHY ID 的读取放在软复位**之后**，于是软复位一超时
       *   就直接返回，这条最有信息量的判据从来没被执行过 —— 排查了几轮
       *   都在猜 PHY 死没死。**判据要放在它还能执行的位置上**，放在失败
       *   路径之后等于没有。
       */

      bmcr = 0xffff;
      bmsr = 0xffff;

      /* ★ 稳定性判断要在固件里做，不能靠人看串口。
       *
       *   上一轮我据"两次启动读到的 ID1 不同"断言 MDIO 在返回噪声。
       *   但那两个值是从**串口日志**里读的，而串口这一整轮一直在丢字符
       *   —— 我无法区分"MDIO 乱"和"日志乱"。用一个本身不可靠的观测去
       *   判断另一个东西可不可靠，这个循环今天已经绕进去好几次了。
       *
       *   改成同一次启动里连读 8 遍 PHYID1，在固件里比较，只输出
       *   "稳定/不稳定"和取值。这样结论不再经过串口这一层。
       */

      v0 = 0;
      v = 0;
      same = 1;

      for (k = 0; k < 8; k++)
        {
          gmac_mdio_read(port, GMAC_PHY_ADDR, 2, &v);
          if (k == 0)
            {
              v0 = v;
            }
          else if (v != v0)
            {
              same = 0;
            }
        }

      syslog(LOG_ERR,
             "GMAC%d: PHYID1 连读 8 次 %s，值=0x%04x（%s）\n",
             port, same ? "全部相同" : "有差异", v0,
             !same ? "MDIO 不稳定，先修总线" :
             (v0 == 0xffff) ? "无应答，查 PHY 供电/复位/地址" :
             (v0 == 0x0000) ? "读回全 0，查 MDC 是否在跑" :
             "MDIO 可信");
      up_mdelay(3);

      gmac_mdio_read(port, GMAC_PHY_ADDR, 0, &bmcr);   /* BMCR  */
      gmac_mdio_read(port, GMAC_PHY_ADDR, 1, &bmsr);   /* BMSR  */
      gmac_mdio_read(port, GMAC_PHY_ADDR, 2, &id1);    /* PHYID1 */
      gmac_mdio_read(port, GMAC_PHY_ADDR, 3, &id2);    /* PHYID2 */

      /* ★ 判据不能只看"有没有 0xffff"。
       *
       *   上一版只要 ID1/ID2 有一个不是 0xffff 就断言"PHY 活着" ——
       *   那太弱了：MDIO 没人应答时总线被上拉，读回 0xffff；但**一个
       *   寄存器读到别的值也可能只是噪声或时序毛刺**。实测就出现过
       *   ID1=0xffff 而 ID2=0x4411 这种自相矛盾的组合，据此下结论是
       *   不负责任的。
       *
       *   加两条更硬的：BMCR(reg0) 与 BMSR(reg1)。它们有明确的保留位
       *   和固定位模式，全 0xffff 或全 0 都说明总线没在工作；而
       *   BMSR bit2 是链路状态，直接回答"对端接上没有"这个问题 ——
       *   这正是当前最需要知道的。
       */

      syslog(LOG_ERR,
             "GMAC%d: PHY BMCR=0x%04x BMSR=0x%04x ID1=0x%04x ID2=0x%04x\n",
             port, bmcr, bmsr, id1, id2);
      up_mdelay(3);

      syslog(LOG_ERR,
             "GMAC%d: 链路=%s 自协商=%s（BMSR bit2/bit5）\n", port,
             (bmsr != 0xffff && (bmsr & (1u << 2))) ? "已建立" : "未建立",
             (bmsr != 0xffff && (bmsr & (1u << 5))) ? "已完成" : "未完成");
      up_mdelay(3);

      /* ★ 扫描 32 个 MDIO 地址。
       *
       *   这条测量拖了好几轮才做。它一次回答三个问题：地址对不对、
       *   总线上有几颗 PHY、以及读到的 ID 是真器件还是噪声 ——
       *   如果 32 个地址都"应答"同一个值，那就不是 PHY 在说话。
       */

      for (a = 0; a < 32; a++)
        {
          uint16_t s1 = 0xffff;
          uint16_t s2 = 0xffff;

          gmac_mdio_read(port, a, 2, &s1);
          gmac_mdio_read(port, a, 3, &s2);

          if (s1 != 0xffff && s1 != 0x0000)
            {
              syslog(LOG_ERR, "GMAC%d: 扫描 addr=%d ID=%04x%04x\n",
                     port, a, s1, s2);
              up_mdelay(3);
            }
        }

      /* ★ PHY 层实验：软复位 + 强制重启自协商。
       *
       *   两颗 PHY 用网线对接时，自协商是 PHY 之间的事，与 MAC 无关。
       *   既然 PHY 活着、自协商使能、又没 power-down/isolate，却五秒
       *   不完成，那就把它按标准流程重来一遍，并把**链路伙伴**的
       *   通告寄存器读出来 —— reg5 有值说明对端在发 FLP，问题在本端
       *   解析；reg5 全 0 且 reg6.bit0=0 说明**根本没收到对端**，
       *   那问题就在网线/变压器/模拟侧，不在寄存器配置。
       */

      v = 0;

      gmac_mdio_read(port, GMAC_PHY_ADDR, 4, &v);
      syslog(LOG_ERR, "GMAC%d: 本端通告 ANAR(reg4)=0x%04x\n", port, v);
      up_mdelay(3);
      gmac_mdio_read(port, GMAC_PHY_ADDR, 9, &v);
      syslog(LOG_ERR, "GMAC%d: 千兆通告 GBCR(reg9)=0x%04x\n", port, v);
      up_mdelay(3);

      /* ★ 先验证写通道本身，再谈写进去的内容。
       *
       *   上一轮软复位后 BMCR 读回 0x0000、且"0ms 就自清"，这更像
       *   写下去的是 0 而不是 0x8000。在确认写能落地之前，任何
       *   "写了 X 但 PHY 没反应"的结论都不成立。
       *
       *   用 ANAR(reg4) 试：它可读可写、改动无副作用，且刚读到的
       *   原值 0x01e1 已知，写完能复原。
       */

      back = 0xffff;

      wr = gmac_mdio_write(port, GMAC_PHY_ADDR, 4, 0x0141);
      gmac_mdio_read(port, GMAC_PHY_ADDR, 4, &back);
      syslog(LOG_ERR,
             "GMAC%d: 写通道自检 ANAR 写 0x0141 读回 0x%04x（写返回 %d）"
             "%s\n", port, back, wr,
             back == 0x0141 ? " 写通道正常" : " ★写没落地");
      up_mdelay(3);

      gmac_mdio_write(port, GMAC_PHY_ADDR, 4, 0x01e1);  /* 复原 */

      gmac_mdio_write(port, GMAC_PHY_ADDR, 0, 0x8000);  /* 软复位 */

      for (n = 0; n < 200; n++)
        {
          up_mdelay(5);
          gmac_mdio_read(port, GMAC_PHY_ADDR, 0, &v);
          if ((v & 0x8000) == 0)
            {
              break;
            }
        }

      syslog(LOG_ERR, "GMAC%d: PHY 软复位 %s（%d ms 后 BMCR=0x%04x）\n",
             port, (v & 0x8000) ? "未自清" : "已完成", n * 5, v);
      up_mdelay(3);

      /* 自协商使能 + 重启 */

      gmac_mdio_write(port, GMAC_PHY_ADDR, 0, 0x1200);
      gmac_mdio_read(port, GMAC_PHY_ADDR, 0, &v);
      syslog(LOG_ERR, "GMAC%d: 写 BMCR=0x1200 后读回 0x%04x\n", port, v);
      up_mdelay(3);

      for (n = 1; n <= 6; n++)
        {
          uint16_t lpa = 0;
          uint16_t exp = 0;

          up_mdelay(1000);
          gmac_mdio_read(port, GMAC_PHY_ADDR, 1, &v);
          gmac_mdio_read(port, GMAC_PHY_ADDR, 5, &lpa);
          gmac_mdio_read(port, GMAC_PHY_ADDR, 6, &exp);
          syslog(LOG_ERR,
                 "GMAC%d: 重启后 %ds BMSR=0x%04x LP(reg5)=0x%04x "
                 "EXP(reg6)=0x%04x\n", port, n, v, lpa, exp);
          up_mdelay(3);

          if (v & (1u << 2))
            {
              break;
            }
        }

      /* 链路没起来时再多等一会儿重读几次 —— 1000Base-T 自协商可能要
       * 数秒，一次读不到不代表永远读不到。
       */

      /* 失败路径上的复读也从 5 次减到 1 次：它是"再看一眼"，
       * 不是"等着它好"，而每一次都要 1 秒。
       */

      for (i = 0; i < 1 && (bmsr == 0xffff || !(bmsr & (1u << 2))); i++)
        {
          up_mdelay(1000);
          gmac_mdio_read(port, GMAC_PHY_ADDR, 1, &bmsr);
          syslog(LOG_ERR, "GMAC%d:  第 %d 秒 BMSR=0x%04x\n",
                 port, (int)i + 1, bmsr);
          up_mdelay(3);
        }

#endif /* CONFIG_RK3576_GMAC_DIAG */
      return -ETIMEDOUT;
    }

  /* 第二层判据：PHY 厂商 ID。这一步才证明 MDIO 时序、PHY 供电、
   * 连线都对。
   */

  gmac_mdio_read(port, GMAC_PHY_ADDR, 2, &id1);   /* PHYSID1 */
  gmac_mdio_read(port, GMAC_PHY_ADDR, 3, &id2);   /* PHYSID2 */

  syslog(LOG_INFO,
         "GMAC%d: MAC_VERSION=0x%02" PRIx32 " HW_FEATURE0=0x%08" PRIx32
         " PHY@%d ID=%04x:%04x%s\n",
         port, version & 0xff,
         gmac_getreg(port, RK3576_GMAC_MAC_HW_FEATURE0),
         GMAC_PHY_ADDR, id1, id2,
         (id1 == 0xffff && id2 == 0xffff)
           ? "  ← 读不到 PHY（分频/复位/供电/引脚复用）" : "  ← PHY 已应答");

  /* ★ 成功路径上也要报链路状态。
   *
   *   之前链路诊断只写在失败分支里，一旦探测通过反而什么都看不到 ——
   *   而"探测通过"离"网口能用"还差着自协商这一步。判据要放在
   *   两条路径上都能执行的位置。
   */

  bmsr = 0;
  lp = 0;

  /* ★ 只读一次，不再等着链路起来。
   *
   *   这里原来是 `for (i = 1; i <= 8; i++) { up_mdelay(500); ... }` ——
   *   最多等 4 秒，**只为了打一行链路状态**，而且等到与否都 return OK。
   *
   *   带时间戳抓启动日志量出来的代价：GMAC0（没插线）4500ms +
   *   GMAC1 3000ms = 7.5 秒，占当时整个启动的六分之一。
   *
   *   链路状态是有用的诊断，但它是**随时间变化的量**，在启动路径上
   *   阻塞着等一个"迟早会变"的位，本来就不是它该待的地方 —— 自协商
   *   没完成不影响 openvela 继续启动，netinit 线程和 ifconfig 随后
   *   都会再看。所以改成读一次、如实报告"还在协商"。
   */

  gmac_mdio_read(port, GMAC_PHY_ADDR, 1, &bmsr);
  gmac_mdio_read(port, GMAC_PHY_ADDR, 5, &lp);
  syslog(LOG_INFO,
         "GMAC%d: 链路=%s 自协商=%s 对端能力 LP=0x%04x"
         "（探测结束即时读取，未完成不等）\n",
         port, (bmsr & (1u << 2)) ? "已建立" : "未建立",
         (bmsr & (1u << 5)) ? "已完成" : "未完成", lp);

  return OK;
}

/****************************************************************************
 * Name: rk3576_gmac_linkinfo
 *
 * Description:
 *   读一次 PHY 的链路状态并打印（k7diag eth 用）。
 *
 *   ★ BMSR 的链路位是"锁存低"的：掉过一次线就一直读到 0，直到被读一次。
 *     所以连读两次，以第二次为准。
 *
 ****************************************************************************/

int rk3576_gmac_linkinfo(int port)
{
  uint16_t bmsr = 0;
  uint16_t lp = 0;
  int ret;

  if (port < 0 || port > 1)
    {
      return -EINVAL;
    }

  gmac_mdio_read(port, GMAC_PHY_ADDR, 1, &bmsr);
  ret = gmac_mdio_read(port, GMAC_PHY_ADDR, 1, &bmsr);
  gmac_mdio_read(port, GMAC_PHY_ADDR, 5, &lp);
  if (ret < 0)
    {
      return ret;
    }

  return ((bmsr >> 2) & 1) | (((bmsr >> 5) & 1) << 1) | ((int)lp << 2);
}

#endif /* CONFIG_RK3576_GMAC */
