/****************************************************************************
 * chip/rk3576/hardware/rk3576_dwmmc.h
 *
 * Synopsys DesignWare Mobile Storage Host Controller（dw_mmc / MSHC）。
 *
 * ★ 与 eMMC 用的那个不是同一种 IP。
 *
 *   原厂 dtb：
 *     mmc@2a330000  compatible = "rockchip,rk3576-dwcmshc"  <- eMMC，SDHCI
 *     mmc@2a310000  compatible = "rockchip,rk3576-dw-mshc"  <- SD 卡，本文件
 *
 *   名字只差两个字母，寄存器布局与状态机却完全不同：SDHCI 是标准
 *   SD Host Controller，dw_mmc 是 Synopsys 自己的一套。rk3576_sdhci.c
 *   的代码一行都套不上，必须另写。
 *
 *   寄存器定义取自 Linux drivers/mmc/host/dw_mmc.h。
 *
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_DWMMC_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_DWMMC_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 寄存器偏移 */

#define DWMMC_CTRL          0x000
#define DWMMC_PWREN         0x004
#define DWMMC_CLKDIV        0x008
#define DWMMC_CLKSRC        0x00c
#define DWMMC_CLKENA        0x010
#define DWMMC_TMOUT         0x014
#define DWMMC_CTYPE         0x018
#define DWMMC_BLKSIZ        0x01c
#define DWMMC_BYTCNT        0x020
#define DWMMC_INTMASK       0x024
#define DWMMC_CMDARG        0x028
#define DWMMC_CMD           0x02c
#define DWMMC_RESP0         0x030
#define DWMMC_RESP1         0x034
#define DWMMC_RESP2         0x038
#define DWMMC_RESP3         0x03c
#define DWMMC_MINTSTS       0x040
#define DWMMC_RINTSTS       0x044
#define DWMMC_STATUS        0x048
#define DWMMC_FIFOTH        0x04c
#define DWMMC_CDETECT       0x050
#define DWMMC_WRTPRT        0x054
#define DWMMC_TCBCNT        0x05c
#define DWMMC_TBBCNT        0x060
#define DWMMC_DEBNCE        0x064
#define DWMMC_USRID         0x068
#define DWMMC_VERID         0x06c
#define DWMMC_HCON          0x070
#define DWMMC_UHS_REG       0x074
#define DWMMC_RST_N         0x078
#define DWMMC_BMOD          0x080
#define DWMMC_CDTHRCTL      0x100

/* FIFO 数据窗口。
 *
 * ★ 偏移由 **IP 版本**决定，与数据总线宽度无关：
 *     verid <  0x240A  ->  0x100
 *     verid >= 0x240A  ->  0x200
 *
 *   出处 dw_mmc.c：
 *     else if (host->verid < DW_MMC_240A)
 *             host->fifo_reg = host->regs + DATA_OFFSET;      // 0x100
 *     else    host->fifo_reg = host->regs + DATA_240A_OFFSET; // 0x200
 *
 *   同一份代码里还有一句注释点明了后果：240A 之后 0x100 变成了
 *   CDTHRCTL 寄存器（"that register offset is in the FIFO region"）。
 *   也就是说版本判断错了的话，数据会被写进 CDTHRCTL —— 不报错，
 *   但一个字节也到不了卡上（TBBCNT 恒为 0）。
 *
 *   本板 VERID=0x5342270a，版本号取低 16 位 = 0x270a，用 0x200。
 */

#define DWMMC_VERID_240A    0x240a
#define DWMMC_GET_VERID(x)  ((x) & 0xffff)

#define DWMMC_DATA_OLD      0x100
#define DWMMC_DATA_240A     0x200

/* CTRL */

#define DWMMC_CTRL_RESET        (1u << 0)
#define DWMMC_CTRL_FIFO_RESET   (1u << 1)
#define DWMMC_CTRL_DMA_RESET    (1u << 2)
#define DWMMC_CTRL_INT_ENABLE   (1u << 4)
#define DWMMC_CTRL_DMA_ENABLE   (1u << 5)
#define DWMMC_CTRL_USE_IDMAC    (1u << 25)

#define DWMMC_CTRL_ALL_RESET    (DWMMC_CTRL_RESET | \
                                 DWMMC_CTRL_FIFO_RESET | \
                                 DWMMC_CTRL_DMA_RESET)

/* CTYPE —— 总线宽度 */

#define DWMMC_CTYPE_1BIT        0
#define DWMMC_CTYPE_4BIT        (1u << 0)
#define DWMMC_CTYPE_8BIT        (1u << 16)

/* 中断/状态位。RINTSTS 与 INTMASK 共用这套位定义。 */

#define DWMMC_INT_CD            (1u << 0)   /* 卡插拔           */
#define DWMMC_INT_RESP_ERR      (1u << 1)
#define DWMMC_INT_CMD_DONE      (1u << 2)
#define DWMMC_INT_DATA_OVER     (1u << 3)
#define DWMMC_INT_TXDR          (1u << 4)
#define DWMMC_INT_RXDR          (1u << 5)
#define DWMMC_INT_RCRC          (1u << 6)
#define DWMMC_INT_DCRC          (1u << 7)
#define DWMMC_INT_RTO           (1u << 8)   /* 响应超时         */
#define DWMMC_INT_DRTO          (1u << 9)
#define DWMMC_INT_HTO           (1u << 10)
#define DWMMC_INT_FRUN          (1u << 11)
#define DWMMC_INT_HLE           (1u << 12)  /* 硬件锁写错误     */
#define DWMMC_INT_SBE           (1u << 13)
#define DWMMC_INT_ACD           (1u << 14)
#define DWMMC_INT_EBE           (1u << 15)

/* ★ 清中断要清满 32 位，不能只清 0x1ffff。
 *
 *   RINTSTS 的高 16 位是各张卡的 SDIO 中断（bit16 = 卡0），只清低 17 位
 *   的话高位会一直挂着。实测 SDIO 实例上 bit24 恒为 1，而 SD 卡实例上
 *   没有 —— 就是这么来的。
 *
 *   这个项目里已经栽过一次一模一样的坑：GMAC 当初也是要
 *   putreg32(0xffffffff, DMA_CH0_STATUS) 才正常。"清全部"这个名字骗人，
 *   得看它到底覆盖了哪些位。
 */

#define DWMMC_INT_ALL           0xffffffffu

#define DWMMC_INT_CMD_ERROR     (DWMMC_INT_RESP_ERR | DWMMC_INT_RCRC | \
                                 DWMMC_INT_RTO | DWMMC_INT_HLE)
#define DWMMC_INT_DATA_ERROR    (DWMMC_INT_DCRC | DWMMC_INT_DRTO | \
                                 DWMMC_INT_SBE | DWMMC_INT_EBE | \
                                 DWMMC_INT_HTO | DWMMC_INT_FRUN)

/* CMD */

#define DWMMC_CMD_INDX(n)       ((n) & 0x3f)
#define DWMMC_CMD_RESP_EXP      (1u << 6)
#define DWMMC_CMD_RESP_LONG     (1u << 7)
#define DWMMC_CMD_RESP_CRC      (1u << 8)
#define DWMMC_CMD_DAT_EXP       (1u << 9)
#define DWMMC_CMD_DAT_WR        (1u << 10)
#define DWMMC_CMD_SEND_STOP     (1u << 12)
#define DWMMC_CMD_PRV_DAT_WAIT  (1u << 13)
#define DWMMC_CMD_STOP          (1u << 14)
#define DWMMC_CMD_INIT          (1u << 15)
#define DWMMC_CMD_UPD_CLK       (1u << 21)  /* 只更新时钟，不发命令 */
#define DWMMC_CMD_USE_HOLD_REG  (1u << 29)
#define DWMMC_CMD_START         (1u << 31)

/* STATUS */

#define DWMMC_STATUS_FIFO_RX    (1u << 0)
#define DWMMC_STATUS_FIFO_TX    (1u << 1)
#define DWMMC_STATUS_FIFO_EMPTY (1u << 2)
#define DWMMC_STATUS_FIFO_FULL  (1u << 3)
#define DWMMC_STATUS_BUSY       (1u << 9)   /* data busy         */

/* ★ STATUS[7:4] 是命令状态机，只有 0 是 Idle（TRM Part1）。
 *
 *   其中 4'he 叫 "Cmd path wait NCC" —— SD 规范要求相邻两条命令之间
 *   至少间隔 NCC（8 个时钟）。控制器会自己走完这段，但**软件必须等**，
 *   在它回到 Idle 之前发下一条命令会丢。
 *
 *   这是本端口踩过的一个隐蔽陷阱：打开命令 trace 时每条命令会打一行
 *   日志，串口输出的耗时恰好盖住了这段等待，于是"开 trace 认得卡、
 *   关掉就认不出"。调试输出成了功能的一部分，最容易被误判成
 *   "关掉某个开关就坏了"。
 */

#define DWMMC_STATUS_CMD_FSM_SHIFT  4
#define DWMMC_STATUS_CMD_FSM_MASK   (0xfu << 4)
#define DWMMC_STATUS_CMD_FSM_IDLE   0
#define DWMMC_STATUS_MC_BUSY    (1u << 10)
#define DWMMC_GET_FCNT(x)       (((x) >> 17) & 0x1fff)

/* HCON */

#define DWMMC_GET_HDATA_WIDTH(x) (((x) >> 7) & 0x7)

/* CLKENA */

#define DWMMC_CLKENA_ENABLE     (1u << 0)
#define DWMMC_CLKENA_LOWPWR     (1u << 16)

/* CDETECT：位 0 为 0 表示卡在位（低有效） */

#define DWMMC_CDETECT_PRESENT(x) (((x) & 1) == 0)

/* FIFOTH: (msize << 28) | (rx_wmark << 16) | tx_wmark */

#define DWMMC_SET_FIFOTH(m, r, t) ((((m) & 0x7) << 28) | \
                                   (((r) & 0xfff) << 16) | \
                                   ((t) & 0xfff))

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_DWMMC_H */
