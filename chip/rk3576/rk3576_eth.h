/****************************************************************************
 * chip/rk3576/rk3576_eth.h
 *
 * RK3576 GMAC 的网络设备驱动（Synopsys DWMAC 4.x）。
 *
 * 硬件的上电、时钟、GRF、引脚复用、RGMII 延时线与 PHY 的厂商初始化
 * 由 rk3576_gmac_probe() 负责，见 rk3576_gmac.h。本驱动只做 MAC/DMA。
 *
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_ETH_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_ETH_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* macirq。出处 rk3576.dtsi：
 *   gmac0  interrupts = <GIC_SPI 293 IRQ_TYPE_LEVEL_HIGH>, ...
 *   gmac1  interrupts = <GIC_SPI 301 IRQ_TYPE_LEVEL_HIGH>, ...
 * SPI 号加 32 得到 IRQ 号，与本端口其它外设的换算一致。
 */

/* ★ 除了 dtsi 标注的 macirq，DMA 通道在 INTM 非 0 时还会走一条
 * 独立的中断线。实测 gmac1 的通道线是 SPI 304（dtsi 里无人占用）。
 */

#ifdef CONFIG_RK3576_ETH_PORT1
#  define RK3576_IRQ_EMAC       (301 + 32)
#  define RK3576_IRQ_EMAC_CH0   (304 + 32)
#  define CONFIG_RK3576_ETH_PORT 1
#else
#  define RK3576_IRQ_EMAC       (293 + 32)
#  define RK3576_IRQ_EMAC_CH0   (296 + 32)
#  define CONFIG_RK3576_ETH_PORT 0
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifndef __ASSEMBLY__

/****************************************************************************
 * Name: rk3576_eth_netinitialize
 *
 * Description:
 *   初始化 GMAC 并把它注册成网络设备。调用前 rk3576_gmac_probe()
 *   必须已经成功 —— 否则 PHY 不工作，DMA 软复位不会完成。
 *
 ****************************************************************************/

int rk3576_eth_netinitialize(int intf);

#endif /* __ASSEMBLY__ */
#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_ETH_H */
