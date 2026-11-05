/****************************************************************************
 * chip/rk3576/rk3576_hdmi.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __CHIP_RK3576_RK3576_HDMI_H
#define __CHIP_RK3576_RK3576_HDMI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifndef __ASSEMBLY__

/* 备好电源域与 PCLK。读任何 HDMI 寄存器之前必须先调 —— 访问未上电的
 * 外设在这颗 SoC 上是总线挂死，不是读回 0。
 */

int rk3576_hdmi_prepare(void);

/* 读控制器 ID/版本与 HPD 状态 */

int rk3576_hdmi_probe(void);

#endif /* __ASSEMBLY__ */
#endif /* __CHIP_RK3576_RK3576_HDMI_H */
