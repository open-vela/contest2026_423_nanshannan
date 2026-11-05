/****************************************************************************
 * chip/rk3576/rk3576_dwmmc.h
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_DWMMC_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_DWMMC_H

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_dwmmc_probe
 *
 * Description:
 *   SD 卡控制器（DesignWare MSHC）前置链路与自检：电源域、两路时钟、
 *   模块复位、引脚复用，然后用三个互相独立的读数确认状态 ——
 *   VERID（寄存器块是否响应）、软复位自清（时钟是否真的在跑）、
 *   CDETECT（卡是否在位）。
 *
 *   返回 OK 表示控制器就绪且检测到卡；-ENODEV 表示卡不在位。
 *
 ****************************************************************************/

/* 板上两个 dw-mshc 控制器。出处：原厂 dtb
 *
 *   /mmc@2a310000  dw-mshc 4bit 可插拔                    -> TF 卡
 *   /mmc@2a320000  dw-mshc 4bit + cap-sdio-irq + 不可插拔 -> WiFi(AP6256)
 *
 * （0x2a330000 是 dwcmshc 8bit 的 eMMC，不是这个驱动管的。）
 */

#define RK3576_DWMMC_SD_BASE    0x2a310000
#define RK3576_DWMMC_SDIO_BASE  0x2a320000

int rk3576_dwmmc_probe(uint32_t base);

/****************************************************************************
 * Name: rk3576_dwmmc_initialize
 *
 * Description:
 *   返回可交给 mmcsd_slotinitialize() 的 sdio_dev_s。
 *   必须在 rk3576_dwmmc_probe() 返回 OK 之后调用。
 *
 ****************************************************************************/

struct sdio_dev_s;
struct sdio_dev_s *rk3576_dwmmc_initialize(uint32_t base);

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_DWMMC_H */
