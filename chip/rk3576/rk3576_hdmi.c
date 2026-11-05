/****************************************************************************
 * chip/rk3576/rk3576_hdmi.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * RK3576 HDMI TX（Synopsys DesignWare HDMI 2.1 "QP" 控制器 + Rockchip
 * HDPTX PHY）。
 *
 * ★ 硬件参数全部出自官方 SDK 的 dtsi 与驱动，不是从 RK3588 猜的：
 *
 *     kernel-6.1/arch/arm64/boot/dts/rockchip/rk3576.dtsi
 *       hdmi@27da0000        reg = 0x27da0000(64K) + 0x27db0000(64K)
 *                            interrupts = SPI 338/339/340/341/367
 *                            power-domains = PD_VO0
 *                            rockchip,grf = ioc_grf
 *                            rockchip,vo0_grf = vo0_grf
 *       hdmiphy@2b000000     rockchip,rk3576-hdptx-phy-hdmi,
 *                            **兼容 rockchip,rk3588-hdptx-phy-hdmi**
 *       vo0_grf@2601a000     hdptxphy_grf@26032000
 *
 *     u-boot/drivers/video/drm/rockchip_dw_hdmi_qp.c   （寄存器与 GRF 定义）
 *     u-boot/drivers/video/drm/phy-rockchip-samsung-hdptx-hdmi.c （PHY/PLL）
 *
 *   PHY 的 compatible 第二项是 rk3588 —— 所以 RK3588 的 PHY 驱动本身就是
 *   RK3576 的实现，这不是"照抄同厂商别的型号"，是同一份驱动。
 *
 * ★ 为什么参考 U-Boot 而不是内核：U-Boot 那份是裸机形态、单线程、不依赖
 *   DRM 框架，而且它本来就要把 HDMI 拉起来显示开机画面 —— 和我们要做的
 *   事情形状一致。内核那份的大半篇幅是 DRM 对接。
 *
 * 本文件当前只实现**探测**：把电源域和时钟备好，读 ID/版本/HPD。
 * 先自检再下结论 —— 电源或时钟没到位时，后面所有配置都是在猜。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <debug.h>
#include <inttypes.h>

#include <nuttx/arch.h>
#include <arch/board/board.h>

#include "arm64_internal.h"
#include "rk3576_cru.h"
#include "rk3576_power.h"
#include "rk3576_hdmi.h"

#ifdef CONFIG_RK3576_HDMI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define HDMI_BASE            0x27da0000    /* dw-hdmi-qp 控制器 */
#define HDPTX_PHY_BASE       0x2b000000    /* HDPTX PHY */
#define IOC_GRF_BASE         0x26040000
#define VO0_GRF_BASE         0x2601a000
#define HDPTXPHY_GRF_BASE    0x26032000

/* dw_hdmi_qp 的标识寄存器（u-boot/drivers/video/drm/dw_hdmi_qp.h） */

#define HDMI_CORE_ID         0x0000
#define HDMI_VER_NUMBER      0x0004
#define HDMI_VER_TYPE        0x0008
#define HDMI_CONFIG_REG      0x000c

/* 热插拔检测在 IOC 里，不在 HDMI 控制器里
 * （rockchip_dw_hdmi_qp.c: RK3576_IOC_HDMITX_HPD_STATUS）
 */

#define IOC_MISC_CON0        0xa400
#define IOC_HPD_STATUS       0xa440
#define HPD_PORT_LEVEL       (1 << 6)      /* 1 = 检测到接收端 */
#define HPD_LOW_MORETHAN100MS (1 << 7)

/* VO0 GRF 里与 HDMI 相关的几个（同上） */

#define VO0_GRF_SOC_CON1     0x0004
#define VO0_GRF_SOC_CON8     0x0020
#define VO0_GRF_SOC_CON14    0x0038

/* 时钟门控。出处：kernel-6.1/drivers/clk/rockchip/clk-rk3576.c
 *
 *   GATE(PCLK_HDMITX0,    ... RK3576_CLKGATE_CON(64), 7)
 *   GATE(CLK_HDMITX0_REF, ... RK3576_CLKGATE_CON(64), 9)
 *   GATE(CLK_HDMITXHPD,   ... RK3576_PMU_CLKGATE_CON(1), 13)
 */

#define GATE_CON_HDMI        64
#define GATE_BIT_PCLK        7
#define GATE_BIT_REF         9
#define PMU_GATE_CON_HPD     1
#define PMU_GATE_BIT_HPD     13

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_hdmi_prepare
 *
 * Description:
 *   把 HDMI 控制器**可被访问**所需的最小条件备好：电源域 + PCLK。
 *
 *   ★ 必须在读任何 HDMI 寄存器之前调用。
 *
 *     这颗 SoC 上访问一个没上电或没给时钟的外设是**总线挂死**，
 *     不是读回 0 —— 板子会当场停住，串口没有任何输出，只能按 RESET。
 *     所以顺序不能反，也不能"先读一下看看"。
 *
 ****************************************************************************/

int rk3576_hdmi_prepare(void)
{
  int ret;

  if (!rk3576_power_is_on(RK3576_PD_VO0))
    {
      ret = rk3576_power_on(RK3576_PD_VO0);
      if (ret < 0)
        {
          syslog(LOG_ERR, "HDMI: PD_VO0 上电失败: %d\n", ret);
          return ret;
        }

      syslog(LOG_INFO, "HDMI: PD_VO0 已上电\n");
    }

  rk3576_clk_gate(GATE_CON_HDMI, GATE_BIT_PCLK, true);
  rk3576_clk_gate(GATE_CON_HDMI, GATE_BIT_REF, true);
  rk3576_pmu_clk_gate(PMU_GATE_CON_HPD, PMU_GATE_BIT_HPD, true);

  return OK;
}

/****************************************************************************
 * Name: rk3576_hdmi_probe
 *
 * Description:
 *   读控制器标识与 HPD 状态。这是往下做任何事情之前的判据。
 *
 ****************************************************************************/

int rk3576_hdmi_probe(void)
{
  uint32_t id;
  uint32_t ver;
  uint32_t type;
  uint32_t cfg;
  uint32_t hpd;
  int ret;

  ret = rk3576_hdmi_prepare();
  if (ret < 0)
    {
      return ret;
    }

  id   = getreg32(HDMI_BASE + HDMI_CORE_ID);
  ver  = getreg32(HDMI_BASE + HDMI_VER_NUMBER);
  type = getreg32(HDMI_BASE + HDMI_VER_TYPE);
  cfg  = getreg32(HDMI_BASE + HDMI_CONFIG_REG);

  printf("HDMI 控制器 @0x%08x\n", HDMI_BASE);
  printf("  CORE_ID    = 0x%08" PRIx32 "\n", id);
  printf("  VER_NUMBER = 0x%08" PRIx32 "\n", ver);
  printf("  VER_TYPE   = 0x%08" PRIx32 "\n", type);
  printf("  CONFIG     = 0x%08" PRIx32 "\n", cfg);

  if (id == 0 || id == 0xffffffff)
    {
      printf("  ★ 读不到有效 ID —— 电源域或时钟没到位，先查这个，\n");
      printf("    不要往下配时序（后面全是在猜）\n");
      return -ENODEV;
    }

  hpd = getreg32(IOC_GRF_BASE + IOC_HPD_STATUS);
  printf("HPD (IOC+0x%04x) = 0x%08" PRIx32 " -> %s\n",
         IOC_HPD_STATUS, hpd,
         (hpd & HPD_PORT_LEVEL) ? "检测到接收端" : "没有接收端");

  printf("VO0_GRF: CON1=0x%08" PRIx32 " CON8=0x%08" PRIx32
         " CON14=0x%08" PRIx32 "\n",
         getreg32(VO0_GRF_BASE + VO0_GRF_SOC_CON1),
         getreg32(VO0_GRF_BASE + VO0_GRF_SOC_CON8),
         getreg32(VO0_GRF_BASE + VO0_GRF_SOC_CON14));

  return OK;
}

#endif /* CONFIG_RK3576_HDMI */
