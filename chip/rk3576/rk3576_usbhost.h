/* SPDX-License-Identifier: Apache-2.0 */

/****************************************************************************
 * RK3576 USB3OTG1 (DRD1, DWC3) host bring-up.
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_USBHOST_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_USBHOST_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RK3576_USB3OTG1_ADDR   0x23400000   /* xHCI 在 +0，DWC3 全局寄存器在 +0xc100 */
#define RK3576_USB3OTG1_IRQ    292          /* SPI 260 */

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_usbhost_initialize
 *
 * Description:
 *   给 DRD1 上电、开时钟、撤复位，配置 u2phy1，把 DWC3 切到 host 模式，
 *   然后把 xHCI 交给 NuttX 驱动（usbhost_xhci_platform_init）。
 *
 *   只走 USB2：USB3 PIPE 的 phystatus 改由 GRF 提供，combphy1 不碰。
 *   板上 GL3510 hub 因此以高速 hub 枚举，三个 Type-A 口都经它出来。
 *
 *   调用方须先注册 class 驱动（hub、MSC、HID）。只能调用一次。
 *
 ****************************************************************************/

int rk3576_usbhost_initialize(void);

/* true once rk3576_usbhost_initialize() has succeeded.  Before that PD_PHP
 * may be off and any access to the controller hangs the bus.
 */

bool rk3576_usbhost_running(void);

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_USBHOST_H */
