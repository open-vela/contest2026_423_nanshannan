/* SPDX-License-Identifier: Apache-2.0 */

/****************************************************************************
 * RK3576 USB3OTG1 (DRD1, DWC3) host bring-up.
 *
 * 参数全部来自厂商 SDK kernel-6.1：
 *
 *   rk3576.dtsi           usb_drd1_dwc3 / u2phy1 / php_grf
 *   clk-rk3576.c          门控 CLKGATE_CON(34/35/36)
 *   pm_domains.c          RK3576_PD_PHP
 *   phy-rockchip-inno-usb2.c  rk3576_phy_cfgs[1]（reg 0x2000）与
 *                             rk3576_usb2phy_tuning()
 *   dwc3/core.c           dwc3_phy_setup() 对 DTS quirk 的处理
 *
 * Linux 侧内核没有 USB（CONFIG_USB_SUPPORT is not set），整个 DRD1 归
 * openvela；它的中断 GIC 292 要在 AMP DTS 的 amp-irqs 里路由过来。
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/usb/xhci_pci.h>

#include "arm64_internal.h"
#include "rk3576_cru.h"
#include "rk3576_gpio.h"
#include "rk3576_power.h"
#include "rk3576_usbhost.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define PHP_GRF_ADDR            0x26020000
#define USB2PHY_GRF_ADDR        0x2602e000

/* php_grf：USB3OTG1 的 PIPE phystatus 来源（pipe_phystatus 字段）。
 * 0x0189 = 由 GRF 给出，DWC3 不再等 combphy1；0x1100 = 来自 PHY。
 */

#define PHP_GRF_U3OTG1_CON      0x0038
#define PHP_GRF_PIPE_FROM_GRF   0xffff0189

/* usb2phy_grf 里 u2phy1 的寄存器（phy_cfg->reg = 0x2000） */

#define U2PHY1_SUS              0x2000   /* phy_sus [8:0]：0 正常，0x1d1 挂起 */
#define U2PHY1_CON3             0x200c
#define U2PHY1_CON4             0x2010   /* bit13 SIDDQ */
#define U2PHY1_SIDDQ            (1u << 13)

/* CRU 门控（RK3576_CLKGATE_CON(n), bit） */

#define GATE_PCLK_PHP_ROOT      34, 0
#define GATE_ACLK_PHP_ROOT      34, 7
#define GATE_ACLK_USB3OTG1      35, 3
#define GATE_CLK_REF_USB3OTG1   35, 4
#define GATE_CLK_SUSP_USB3OTG1  35, 5
#define GATE_ACLK_MMU1          35, 14
#define GATE_ACLK_SLV_MMU1      36, 0

/* 复位号（dt-bindings/clock/rockchip,rk3576-cru.h） */

#define SRST_A_USB3OTG1         563
#define SRST_P_USBPHY_GRF_1     524298
#define SRST_OTGPHY_1           524312

/* VBUS：vcc5v0_host 由 GPIO3_D6 高有效使能 */

#define VBUS_BANK               3
#define VBUS_PIN                (3 * 8 + 6)

/* DWC3 全局寄存器（相对控制器基址） */

#define DWC3_GCTL               0xc110
#define   GCTL_PRTCAPDIR_MASK   (3u << 12)
#define   GCTL_PRTCAP_HOST      (1u << 12)
#define   GCTL_CORESOFTRESET    (1u << 11)
#define   GCTL_SCALEDOWN_MASK   (3u << 4)
#define DWC3_GUCTL1             0xc11c
#define   GUCTL1_TX_IPGAP_DIS   (1u << 28)
#define   GUCTL1_PARKMODE_SS    (1u << 17)
#define   GUCTL1_PARKMODE_HS    (1u << 16)
#define DWC3_GSNPSID            0xc120
#define DWC3_GUSB2PHYCFG        0xc200
#define   U2CFG_PHYSOFTRST      (1u << 31)
#define   U2CFG_FREECLK_EXISTS  (1u << 30)
#define   U2CFG_USBTRDTIM_MASK  (0xfu << 10)
#define   U2CFG_USBTRDTIM_16BIT (5u << 10)
#define   U2CFG_ENBLSLPM        (1u << 8)
#define   U2CFG_SUSPHY          (1u << 6)
#define   U2CFG_PHYIF_16BIT     (1u << 3)
#define DWC3_GUSB3PIPECTL       0xc2c0
#define   U3PIPE_PHYSOFTRST     (1u << 31)
#define   U3PIPE_DISRXDETINP3   (1u << 28)
#define   U3PIPE_DEPOCHANGE     (1u << 18)
#define   U3PIPE_SUSPHY         (1u << 17)

#define USB_BASE                RK3576_USB3OTG1_ADDR

/****************************************************************************
 * Private Data
 ****************************************************************************/

static bool g_usbhost_started;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t dwc3_getreg(uint32_t off)
{
  return getreg32(USB_BASE + off);
}

static inline void dwc3_putreg(uint32_t off, uint32_t val)
{
  putreg32(val, USB_BASE + off);
}

static void dwc3_modreg(uint32_t off, uint32_t clr, uint32_t set)
{
  dwc3_putreg(off, (dwc3_getreg(off) & ~clr) | set);
}

/****************************************************************************
 * Name: rk3576_u2phy1_init
 *
 * Description:
 *   rockchip_usb2phy_init() + rk3576_usb2phy_tuning() 里与 reg 0x2000
 *   有关的部分，再加 port init 时的 phy_sus 解除。
 *
 ****************************************************************************/

static void rk3576_u2phy1_init(void)
{
  /* APB 复位先撤，GRF 才能写 */

  rk3576_reset(SRST_P_USBPHY_GRF_1, false);
  up_udelay(10);

  /* 退出 IDDQ 后必须复位一次 PHY，否则模拟部分不出 480M */

  if ((getreg32(USB2PHY_GRF_ADDR + U2PHY1_CON4) & U2PHY1_SIDDQ) != 0)
    {
      putreg32(U2PHY1_SIDDQ << 16, USB2PHY_GRF_ADDR + U2PHY1_CON4);
    }

  rk3576_reset(SRST_OTGPHY_1, true);
  up_udelay(10);
  rk3576_reset(SRST_OTGPHY_1, false);
  up_udelay(200);

  /* HS 直流电平 +5.89%，预加重 2x —— 与厂商 tuning 相同 */

  putreg32(0x0f000900, USB2PHY_GRF_ADDR + U2PHY1_CON3);
  putreg32(0x00180010, USB2PHY_GRF_ADDR + U2PHY1_CON4);

  /* 端口退出挂起 */

  putreg32(0x01ff0000, USB2PHY_GRF_ADDR + U2PHY1_SUS);

  /* 等 PHY 输出 UTMI 时钟（厂商驱动在 power_on 后等 1-2ms） */

  up_mdelay(2);
}

/****************************************************************************
 * Name: rk3576_dwc3_host
 *
 * Description:
 *   DWC3 核心复位并切 host。quirk 对应 usb_drd1_dwc3 节点的 snps,* 属性。
 *
 ****************************************************************************/

static int rk3576_dwc3_host(void)
{
  uint32_t id;

  id = dwc3_getreg(DWC3_GSNPSID);
  if ((id & 0xffff0000) != 0x55330000 && (id & 0xffff0000) != 0x33310000)
    {
      syslog(LOG_ERR, "USB: GSNPSID=0x%08" PRIx32 "，不是 DWC3\n", id);
      return -ENODEV;
    }

  /* 核心与两个 PHY 接口一起复位 */

  dwc3_modreg(DWC3_GCTL, 0, GCTL_CORESOFTRESET);
  dwc3_modreg(DWC3_GUSB3PIPECTL, 0, U3PIPE_PHYSOFTRST);
  dwc3_modreg(DWC3_GUSB2PHYCFG, 0, U2CFG_PHYSOFTRST);
  up_mdelay(100);

  dwc3_modreg(DWC3_GUSB3PIPECTL, U3PIPE_PHYSOFTRST, 0);
  dwc3_modreg(DWC3_GUSB2PHYCFG, U2CFG_PHYSOFTRST, 0);
  up_mdelay(100);

  dwc3_modreg(DWC3_GCTL, GCTL_CORESOFTRESET, 0);

  /* snps,dis-del-phy-power-chg-quirk / snps,dis_rxdet_inp3_quirk；
   * u3 susphy 保持默认打开
   */

  dwc3_modreg(DWC3_GUSB3PIPECTL, U3PIPE_DEPOCHANGE,
              U3PIPE_DISRXDETINP3 | U3PIPE_SUSPHY);

  /* phy_type = "utmi_wide"；snps,dis_enblslpm_quirk；
   * snps,dis-u2-freeclk-exists-quirk
   */

  dwc3_modreg(DWC3_GUSB2PHYCFG,
              U2CFG_USBTRDTIM_MASK | U2CFG_ENBLSLPM | U2CFG_FREECLK_EXISTS,
              U2CFG_PHYIF_16BIT | U2CFG_USBTRDTIM_16BIT | U2CFG_SUSPHY);

  /* snps,dis-tx-ipgap-linecheck-quirk / snps,parkmode-disable-{hs,ss} */

  dwc3_modreg(DWC3_GUCTL1, 0,
              GUCTL1_TX_IPGAP_DIS | GUCTL1_PARKMODE_HS |
              GUCTL1_PARKMODE_SS);

  /* host 模式 */

  dwc3_modreg(DWC3_GCTL, GCTL_PRTCAPDIR_MASK | GCTL_SCALEDOWN_MASK,
              GCTL_PRTCAP_HOST);

  syslog(LOG_INFO, "USB: DWC3 0x%08" PRIx32 " GCTL=0x%08" PRIx32
         " U2CFG=0x%08" PRIx32 "\n", id, dwc3_getreg(DWC3_GCTL),
         dwc3_getreg(DWC3_GUSB2PHYCFG));
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int rk3576_usbhost_initialize(void)
{
  int ret;

  if (g_usbhost_started)
    {
      return -EALREADY;
    }

  ret = rk3576_power_on(RK3576_PD_PHP);
  if (ret < 0)
    {
      return ret;
    }

  rk3576_clk_gate(GATE_PCLK_PHP_ROOT, true);
  rk3576_clk_gate(GATE_ACLK_PHP_ROOT, true);
  rk3576_clk_gate(GATE_ACLK_MMU1, true);
  rk3576_clk_gate(GATE_ACLK_SLV_MMU1, true);
  rk3576_clk_gate(GATE_ACLK_USB3OTG1, true);
  rk3576_clk_gate(GATE_CLK_REF_USB3OTG1, true);
  rk3576_clk_gate(GATE_CLK_SUSP_USB3OTG1, true);

  /* 只走 USB2：PIPE phystatus 由 GRF 给 */

  putreg32(PHP_GRF_PIPE_FROM_GRF, PHP_GRF_ADDR + PHP_GRF_U3OTG1_CON);

  rk3576_u2phy1_init();

  /* 控制器复位脉冲，之后寄存器才可信 */

  rk3576_reset(SRST_A_USB3OTG1, true);
  up_udelay(10);
  rk3576_reset(SRST_A_USB3OTG1, false);
  up_udelay(100);

  ret = rk3576_dwc3_host();
  if (ret < 0)
    {
      return ret;
    }

  /* VBUS 最后开：hub 上电后才去探测端口 */

  rk3576_gpio_setdir(VBUS_BANK, VBUS_PIN, true);
  rk3576_gpio_write(VBUS_BANK, VBUS_PIN, true);

  ret = usbhost_xhci_platform_init(USB_BASE, RK3576_USB3OTG1_IRQ);
  if (ret < 0)
    {
      syslog(LOG_ERR, "USB: xHCI 初始化失败 %d\n", ret);
      return ret;
    }

  g_usbhost_started = true;
  syslog(LOG_INFO, "USB: xHCI 已启动，等待设备\n");
  return OK;
}

bool rk3576_usbhost_running(void)
{
  return g_usbhost_started;
}
