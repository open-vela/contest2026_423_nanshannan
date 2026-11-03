/****************************************************************************
 * arch/arm/src/s32k3xx/rk3576_eth_emac.c
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

/* Copyright 2022 NXP */

/****************************************************************************
 * Included Files
 ****************************************************************************
 *
 * 移植自 arch/arm/src/s32k3xx/s32k3xx_emac.c。
 *
 * RK3576 的 GMAC 与 S32K3XX 的 EMAC 是同一套 Synopsys DWMAC 4.x：
 * 寄存器偏移逐个核对完全一致（MAC_CONFIGURATION 0x0000、MAC_VERSION
 * 0x0110、MAC_MDIO_ADDRESS 0x0200、MTL 0x0c00、DMA_MODE 0x1000、
 * DMA_CH0 0x1100），板上读到 MAC_VERSION=0x3051（snpsver 5.10a）。
 *
 * 描述符环的管理（txhead/txtail/inflight、recvframe/freeframe）保持原样 ——
 * 这部分逻辑微妙，重写比移植更容易出错。
 *
 * 与原驱动的分工不同之处：时钟、GRF、引脚复用、RGMII 延时线、PHY 的
 * 厂商初始化全部由 rk3576_gmac_probe() 完成（板级初始化时调用），
 * 本文件只负责 MAC/DMA 与网络设备本身。
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <assert.h>
#include <debug.h>
#include <errno.h>

#include <sys/param.h>

#include <arpa/inet.h>

#include <nuttx/wdog.h>
#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include <nuttx/spinlock.h>
#include <nuttx/wqueue.h>
#include <nuttx/signal.h>
#include <nuttx/net/mii.h>
#include <nuttx/net/phy.h>
#include <nuttx/net/ip.h>
#include <nuttx/net/netdev.h>

#ifdef CONFIG_NET_PKT
#  include <nuttx/net/pkt.h>
#endif

#include "arm64_internal.h"
#include "chip.h"
#include "hardware/rk3576_emac.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_gmac.h"
#include "rk3576_eth.h"

#ifdef CONFIG_RK3576_ETH

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* If processing is not done at the interrupt level, then work queue support
 * is required.
 */

#if !defined(CONFIG_SCHED_WORKQUEUE)
#  error Work queue support is required
#else

/* Select work queue.  Always use the LP work queue if available.  If not,
 * then LPWORK will re-direct to the HP work queue.
 *
 * NOTE:  However, the network should NEVER run on the high priority work
 * queue!  That queue is intended only to service short back end interrupt
 * processing that never suspends.  Suspending the high priority work queue
 * may bring the system to its knees!
 */

#  define ETHWORK LPWORK
#endif

/* CONFIG_RK3576_ETH_NETHIFS determines the number of physical interfaces
 * that will be supported.
 */

#if CONFIG_RK3576_ETH_NETHIFS != 1
#  error "CONFIG_RK3576_ETH_NETHIFS must be one for now"
#endif

#if CONFIG_RK3576_ETH_NTXBUFFERS < 1
#  error "Need at least one TX buffer"
#endif

#if CONFIG_RK3576_ETH_NRXBUFFERS < 1
#  error "Need at least one RX buffer"
#endif

#define NENET_NBUFFERS \
  (CONFIG_RK3576_ETH_NTXBUFFERS+CONFIG_RK3576_ETH_NRXBUFFERS)

/* Add 4 to the configured buffer size to account for the 2 byte checksum
 * memory needed at the end of the maximum size packet.  Buffer sizes must
 * be an even multiple of 4, 8, or 16 bytes (depending on buswidth).  We
 * will use the 16-byte alignment in all cases.
 */

#define OPTIMAL_ETH_BUFSIZE ((CONFIG_NET_ETH_PKTSIZE + 4 + 15) & ~15)

#ifdef CONFIG_RK3576_EMAC_BUFSIZE
#  define ETH_BUFSIZE CONFIG_RK3576_EMAC_BUFSIZE
#else
#  define ETH_BUFSIZE OPTIMAL_ETH_BUFSIZE
#endif

#if ETH_BUFSIZE > EMAC_TDES2_B1L_MASK
#  error "ETH_BUFSIZE is too large"
#endif

#if (ETH_BUFSIZE & 15) != 0
#  error "ETH_BUFSIZE must be aligned"
#endif

#if ETH_BUFSIZE != OPTIMAL_ETH_BUFSIZE
#  warning "You are using an incomplete/untested configuration"
#endif

/* We need at least one more free buffer than transmit buffers */

#define RK3576_EMAC_NFREEBUFFERS (CONFIG_RK3576_ETH_NTXBUFFERS+1)

/* Buffers used for DMA access must begin on an address aligned with the
 * D-Cache line and must be an even multiple of the D-Cache line size.
 * These size/alignment requirements are necessary so that D-Cache flush
 * and invalidate operations will not have any additional effects.
 *
 * The TX and RX descriptors are 16 bytes in size
 */

#define DMA_BUFFER_MASK    (64 - 1)
#define DMA_ALIGN_UP(n)    (((n) + DMA_BUFFER_MASK) & ~DMA_BUFFER_MASK)
#define DMA_ALIGN_DOWN(n)  ((n) & ~DMA_BUFFER_MASK)

#define DESC_SIZE           16
#define DESC_PADSIZE        DMA_ALIGN_UP(DESC_SIZE)
#define ALIGNED_BUFSIZE     DMA_ALIGN_UP(ETH_BUFSIZE)

#define RXTABLE_SIZE        (CONFIG_RK3576_ETH_NRXBUFFERS)
#define TXTABLE_SIZE        (CONFIG_RK3576_ETH_NTXBUFFERS)

#define RXBUFFER_SIZE       (CONFIG_RK3576_ETH_NRXBUFFERS * ALIGNED_BUFSIZE)
#define RXBUFFER_ALLOC      (RXBUFFER_SIZE)

#define TXBUFFER_SIZE       (RK3576_EMAC_NFREEBUFFERS * ALIGNED_BUFSIZE)
#define TXBUFFER_ALLOC      (TXBUFFER_SIZE)

/* TX timeout = 1 minute */

#define RK3576_TXTIMEOUT   (60*CLK_TCK)
#define MII_MAXPOLLS      (0x1ffff)
#define LINK_WAITUS       (500*1000)
#define LINK_NLOOPS       (10)

/* Estimate the MII_SPEED in order to get an MDC close to 2.5MHz,
 * based on the internal module (ENET) clock:
 *
 *   MII_SPEED = ENET_FREQ/5000000 -1
 *
 * For example, if ENET_FREQ_MHZ=120 (MHz):
 *
 *   MII_SPEED = 120000000/5000000 -1
 *             = 23
 */

#define RK3576_MII_SPEED  0x0f /* 100Mbs. Revisit and remove hardcoded value */
#if RK3576_MII_SPEED > 63
#  error "RK3576_MII_SPEED is out-of-range"
#endif

/* Interrupt bit sets *******************************************************/

/* All interrupts in the normal and abnormal interrupt summary.  Early
 * transmit interrupt (ETI) is excluded from the abnormal set because it
 * causes too many interrupts and is not interesting.
 */

#define ETH_DMAINT_NORMAL                                               \
  (EMAC_DMA_CH0_INTERRUPT_ENABLE_TIE | EMAC_DMA_CH0_INTERRUPT_ENABLE_TBUE | \
   EMAC_DMA_CH0_INTERRUPT_ENABLE_RIE | EMAC_DMA_CH0_INTERRUPT_ENABLE_ERIE)

#define ETH_DMAINT_ABNORMAL                                             \
  (EMAC_DMA_CH0_INTERRUPT_ENABLE_TXSE | EMAC_DMA_CH0_INTERRUPT_ENABLE_RBUE | \
   EMAC_DMA_CH0_INTERRUPT_ENABLE_RSE | EMAC_DMA_CH0_INTERRUPT_ENABLE_RWTE | \
   /* EMAC_DMA_CH0_INTERRUPT_ENABLE_ETIE | */ EMAC_DMA_CH0_INTERRUPT_ENABLE_FBEE)

/* Normal receive, transmit, error interrupt enable bit sets */

#define ETH_DMAINT_RECV_ENABLE    (ETH_DMACIER_NIE | ETH_DMACIER_RIE)
#define ETH_DMAINT_XMIT_ENABLE    (ETH_DMACIER_NIE | ETH_DMACIER_TIE)
#define ETH_DMAINT_XMIT_DISABLE   (ETH_DMACIER_TIE)

#ifdef CONFIG_DEBUG_NET
#  define ETH_DMAINT_ERROR_ENABLE (ETH_DMACIER_AIE | ETH_DMAINT_ABNORMAL)
#else
#  define ETH_DMAINT_ERROR_ENABLE (0)
#endif

/* Helpers ******************************************************************/

/* This is a helper pointer for accessing the contents of the Ethernet
 * header
 */

#define BUF ((struct eth_hdr_s *)priv->dev.d_buf)

/* PHY definitions.
 *
 * The selected PHY must be selected from the drivers/net/Kconfig PHY menu.
 * A description of the PHY must be provided here.  That description must
 * include:
 *
 * 1. BOARD_PHY_NAME: A PHY name string (for debug output),
 * 2. BOARD_PHYID1 and BOARD_PHYID2: The PHYID1 and PHYID2 values (from
 *    include/nuttx/net/mii.h)
 * 3. BOARD_PHY_STATUS:  The address of the status register to use when
 *    querying link status (from include/nuttx/net/mii.h)
 * 4. BOARD_PHY_ADDRThe PHY broadcast address of 0 is selected.  This
 *    should be fine as long as there is only a single PHY.
 * 5. BOARD_PHY_10BASET:  A macro that can convert the status register
 *    value into a boolean: true=10Base-T, false=Not 10Base-T
 * 6. BOARD_PHY_100BASET:  A macro that can convert the status register
 *    value into a boolean: true=100Base-T, false=Not 100Base-T
 * 7. BOARD_PHY_ISDUPLEX:  A macro that can convert the status register
 *    value into a boolean: true=duplex mode, false=half-duplex mode
 *
 * The imxrt1050-evk board uses a KSZ8081 PHY
 * The Versiboard2 uses a LAN8720 PHY
 * The Teensy-4.1 board uses a DP83825I PHY
 *
 * ...and further PHY descriptions here.
 */

#if defined(CONFIG_ETH0_PHY_KSZ8081)
#  define BOARD_PHY_NAME        "KSZ8081"
#  define BOARD_PHYID1          MII_PHYID1_KSZ8081
#  define BOARD_PHYID2          MII_PHYID2_KSZ8081
#  define BOARD_PHY_STATUS      MII_KSZ8081_PHYCTRL1
#  define BOARD_PHY_ADDR        (0)
#  define BOARD_PHY_10BASET(s)  (((s) & MII_PHYCTRL1_MODE_10HDX) != 0)
#  define BOARD_PHY_100BASET(s) (((s) & MII_PHYCTRL1_MODE_100HDX) != 0)
#  define BOARD_PHY_ISDUPLEX(s) (((s) & MII_PHYCTRL1_MODE_DUPLEX) != 0)
#elif defined(CONFIG_ETH0_PHY_LAN8720)
#  define BOARD_PHY_NAME        "LAN8720"
#  define BOARD_PHYID1          MII_PHYID1_LAN8720
#  define BOARD_PHYID2          MII_PHYID2_LAN8720
#  define BOARD_PHY_STATUS      MII_LAN8720_SCSR
#  define BOARD_PHY_ADDR        (1)
#  define BOARD_PHY_10BASET(s)  (((s)&MII_LAN8720_SPSCR_10MBPS) != 0)
#  define BOARD_PHY_100BASET(s) (((s)&MII_LAN8720_SPSCR_100MBPS) != 0)
#  define BOARD_PHY_ISDUPLEX(s) (((s)&MII_LAN8720_SPSCR_DUPLEX) != 0)
#elif defined(CONFIG_ETH0_PHY_LAN8742A)
#  define BOARD_PHY_NAME        "LAN8742A"
#  define BOARD_PHYID1          MII_PHYID1_LAN8742A
#  define BOARD_PHYID2          MII_PHYID2_LAN8742A
#  define BOARD_PHY_STATUS      MII_LAN8740_SCSR
#  define BOARD_PHY_ADDR        (0)
#  define BOARD_PHY_10BASET(s)  (((s)&MII_LAN8720_SPSCR_10MBPS) != 0)
#  define BOARD_PHY_100BASET(s) (((s)&MII_LAN8720_SPSCR_100MBPS) != 0)
#  define BOARD_PHY_ISDUPLEX(s) (((s)&MII_LAN8720_SPSCR_DUPLEX) != 0)
#elif defined(CONFIG_ETH0_PHY_DP83825I)
#  define BOARD_PHY_NAME        "DP83825I"
#  define BOARD_PHYID1          MII_PHYID1_DP83825I
#  define BOARD_PHYID2          MII_PHYID2_DP83825I
#  define BOARD_PHY_STATUS      MII_DP83825I_PHYSTS
#  define BOARD_PHY_ADDR        (0)
#  define BOARD_PHY_10BASET(s)  (((s) & MII_DP83825I_PHYSTS_SPEED) != 0)
#  define BOARD_PHY_100BASET(s) (((s) & MII_DP83825I_PHYSTS_SPEED) == 0)
#  define BOARD_PHY_ISDUPLEX(s) (((s) & MII_DP83825I_PHYSTS_DUPLEX) != 0)
#elif defined(CONFIG_ETH0_PHY_TJA1103)
#  define BOARD_PHY_NAME        "TJA1103"
#  define BOARD_PHYID1          MII_PHYID1_TJA1103
#  define BOARD_PHYID2          MII_PHYID2_TJA1103
#  define BOARD_PHY_STATUS      MII_TJA110X_BSR
#  define BOARD_PHY_ADDR        (18)
#  define BOARD_PHY_10BASET(s)  0 /* PHY only supports 100BASE-T1 */
#  define BOARD_PHY_100BASET(s) 1 /* PHY only supports 100BASE-T1 */
#  define BOARD_PHY_ISDUPLEX(s) 1 /* PHY only supports fullduplex */

#  ifdef CONFIG_NETDEV_IOCTL
#    define CLAUSE45            1
#  endif
#  define MMD1                  1
#  define MMD1_PMA_STATUS1      1
#  define MMD1_PS1_RECEIVE_LINK_STATUS (1 << 2)
#  define MMD30_VEND1           30
#  define VEND1_PHY_IRQ_ACK     0x80A0
#  define VEND1_PHY_IRQ_EN      0x80A1
#  define VEND1_PHY_IRQ_STATUS  0x80A2
#  define PHY_IRQ_LINK_EVENT    (1 << 1)
#elif defined(CONFIG_RK3576_ETH_PHY_MAE0621A)

/* Maxio MAE0621A-Q2C。KICKPI-K7 板上两路 GMAC 用的都是它。
 *
 * 它的链路状态不在标准寄存器里，而在分页空间 0xa43 的 0x1a，
 * 位定义出处 kernel-6.1/drivers/net/phy/maxio.c。
 *
 * 厂商初始化（含"时钟模式=晶振"）在 rk3576_gmac_probe() 里做，
 * 不在本驱动 —— 那一步必须早于链路，见 rk3576_gmac.c。
 */

#  define BOARD_PHY_NAME        "MAE0621A"
#  define BOARD_PHYID1          0x7b74
#  define BOARD_PHYID2          0x4411
#  define BOARD_PHY_ADDR        (0)

#  define MAXIO_PAGE_SELECT     0x1f
#  define MAXIO_PHYSR_PAGE      0xa43
#  define MAXIO_PHYSR_REG       0x1a
#  define MAXIO_PHY_LINK        (1 << 2)
#  define MAXIO_PHY_DUPLEX      (1 << 3)
#  define MAXIO_PHY_SPEED_MASK  (3 << 4)
#  define MAXIO_PHY_1000M       0x20
#  define MAXIO_PHY_100M        0x10

#  define BOARD_PHY_STATUS      MAXIO_PHYSR_REG
#  define BOARD_PHY_ISDUPLEX(s) (((s) & MAXIO_PHY_DUPLEX) != 0)
#  define BOARD_PHY_1000BASET(s) \
          (((s) & MAXIO_PHY_SPEED_MASK) == MAXIO_PHY_1000M)
#  define BOARD_PHY_100BASET(s) \
          (((s) & MAXIO_PHY_SPEED_MASK) == MAXIO_PHY_100M)
#  define BOARD_PHY_10BASET(s)  (((s) & MAXIO_PHY_SPEED_MASK) == 0)
#else
#  error "Unrecognized or missing PHY selection"
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* This union type forces the allocated size of TX&RX descriptors to be the
 * padded to a exact multiple of the Cortex-M7 D-Cache line size.
 */

union rk3576_eth_desc_u
{
  uint8_t             pad[DESC_PADSIZE];
  struct eth_desc_s   desc;
};

/* The rk3576_eth_driver_s encapsulates all state information for a single
 * hardware interface
 */

struct rk3576_eth_driver_s
{
  bool bifup;                  /* true:ifup false:ifdown */
  uint32_t dmasr;              /* ISR 抓下来的 DMA 状态，交给 worker 用 */
  uint8_t phyaddr;             /* Selected PHY address */
  struct wdog_s txtimeout;     /* TX timeout timer */
  struct work_s irqwork;       /* For deferring interrupt work to the work queue */
  struct work_s pollwork;      /* For deferring poll work to the work queue */

  /* This holds the information visible to the NuttX network */

  struct net_driver_s dev;     /* Interface understood by the network */

  /* Used to track transmit and receive descriptors */

  struct eth_desc_s *txhead;        /* Next available TX descriptor */
  struct eth_desc_s *rxhead;        /* Next available RX descriptor */

  struct eth_desc_s *txchbase;      /* TX descriptor ring base address */
  struct eth_desc_s *rxchbase;      /* RX descriptor ring base address */

  struct eth_desc_s *txtail;        /* First "in_flight" TX descriptor */
  uint16_t             inflight;    /* Number of TX transfers "in_flight" */
  sq_queue_t           freeb;       /* The free buffer list */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_eth_driver_s g_enet[CONFIG_RK3576_ETH_NETHIFS];
static spinlock_t g_eth_irq_lock = SP_UNLOCKED;

/* Descriptor allocations */

static union rk3576_eth_desc_u g_rxtable[RXTABLE_SIZE]
aligned_data(64);
static union rk3576_eth_desc_u g_txtable[TXTABLE_SIZE]
aligned_data(64);

/* Buffer allocations */

static uint8_t g_rxbuffer[RXBUFFER_ALLOC]
aligned_data(64);
static uint8_t g_txbuffer[TXBUFFER_ALLOC]
aligned_data(64);

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Free buffer management */

static void rk3576_eth_initbuffer(struct rk3576_eth_driver_s *priv,
                             uint8_t *txbuffer);
static inline uint8_t *rk3576_eth_allocbuffer(struct rk3576_eth_driver_s *priv);
static inline void rk3576_eth_freebuffer(struct rk3576_eth_driver_s *priv,
                                    uint8_t *buffer);
static inline bool rk3576_eth_isfreebuffer(struct rk3576_eth_driver_s *priv);

/* Common TX logic */

static int  rk3576_eth_transmit(struct rk3576_eth_driver_s *priv);
static int  rk3576_eth_txpoll(struct net_driver_s *dev);

/* Interrupt handling */

static void rk3576_eth_enableint(struct rk3576_eth_driver_s *priv,
                              uint32_t ierbit);
static void rk3576_eth_disableint(struct rk3576_eth_driver_s *priv,
                               uint32_t ierbit);

static void rk3576_eth_freesegment(struct rk3576_eth_driver_s *priv,
                              struct eth_desc_s *rxfirst, int segments);
static int  rk3576_eth_recvframe(struct rk3576_eth_driver_s *priv);
static void rk3576_eth_receive(struct rk3576_eth_driver_s *priv);
static void rk3576_eth_freeframe(struct rk3576_eth_driver_s *priv);
static void rk3576_eth_txdone(struct rk3576_eth_driver_s *priv);

static void rk3576_eth_interrupt_work(void *arg);
static int  rk3576_eth_enet_interrupt(int irq, void *context, void *arg);

/* Watchdog timer expirations */

static void rk3576_eth_txtimeout_work(void *arg);
static void rk3576_eth_txtimeout_expiry(wdparm_t arg);

/* NuttX callback functions */

static int  rk3576_eth_ifup(struct net_driver_s *dev);
static int  rk3576_eth_ifdown(struct net_driver_s *dev);

static void rk3576_eth_txavail_work(void *arg);
static int  rk3576_eth_txavail(struct net_driver_s *dev);

/* Internal ifup function that allows phy reset to be optional */

static int rk3576_eth_ifup_action(struct net_driver_s *dev, bool resetphy);

#ifdef CONFIG_NET_MCASTGROUP
static int  rk3576_eth_addmac(struct net_driver_s *dev, const uint8_t *mac);
static int  rk3576_eth_rmmac(struct net_driver_s *dev, const uint8_t *mac);
#endif

#ifdef CONFIG_NETDEV_IOCTL
static int  rk3576_eth_ioctl(struct net_driver_s *dev, int cmd,
            unsigned long arg);
#endif

/* PHY/MII support */

#if defined(CONFIG_NETDEV_PHY_IOCTL) && defined(CONFIG_ARCH_PHY_INTERRUPT)
static int rk3576_eth_phyintenable(struct rk3576_eth_driver_s *priv);
#endif
#if defined(CONFIG_NETDEV_PHY_IOCTL)
static int rk3576_eth_writemii(struct rk3576_eth_driver_s *priv, uint8_t phyaddr,
             uint8_t regaddr, uint16_t data);
#endif
static int rk3576_eth_readmii(struct rk3576_eth_driver_s *priv, uint8_t phyaddr,
             uint8_t regaddr, uint16_t *data);
static int rk3576_eth_initphy(struct rk3576_eth_driver_s *priv, bool renogphy);
#if defined(CLAUSE45)
static int rk3576_eth_readmmd(struct rk3576_eth_driver_s *priv, uint8_t phyaddr,
                         uint8_t mmd, uint16_t regaddr, uint16_t *data);
static int rk3576_eth_writemmd(struct rk3576_eth_driver_s *priv, uint8_t phyaddr,
                         uint8_t mmd, uint16_t regaddr, uint16_t data);
#endif

/* Initialization */

static void rk3576_eth_initbuffers(struct rk3576_eth_driver_s *priv,
                                union rk3576_eth_desc_u *txtable,
                                union rk3576_eth_desc_u *rxtable,
                                uint8_t *rxbuffer);
static void rk3576_eth_initdma(struct rk3576_eth_driver_s *priv);
static void rk3576_eth_initmtl(struct rk3576_eth_driver_s *priv);
static uint32_t rk3576_eth_reset(struct rk3576_eth_driver_s *priv);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Function: rk3576_eth_initbuffer
 *
 * Description:
 *   Initialize the free buffer list.
 *
 * Parameters:
 *   priv     - Reference to the driver state structure
 *   txbuffer - DMA memory allocated for TX buffers.
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Called during early driver initialization before Ethernet interrupts
 *   are enabled.
 *
 ****************************************************************************/

static void rk3576_eth_initbuffer(struct rk3576_eth_driver_s *priv,
                               uint8_t *txbuffer)
{
  uint8_t *buffer;
  int i;

  /* Initialize the head of the free buffer list */

  sq_init(&priv->freeb);

  /* Add all of the pre-allocated buffers to the free buffer list */

  for (i = 0, buffer = txbuffer;
       i < RK3576_EMAC_NFREEBUFFERS;
       i++, buffer += ALIGNED_BUFSIZE)
    {
      sq_addlast((sq_entry_t *)buffer, &priv->freeb);
    }
}

/****************************************************************************
 * Function: rk3576_eth_allocbuffer
 *
 * Description:
 *   Allocate one buffer from the free buffer list.
 *
 * Parameters:
 *   priv  - Reference to the driver state structure
 *
 * Returned Value:
 *   Pointer to the allocated buffer on success; NULL on failure
 *
 * Assumptions:
 *   May or may not be called from an interrupt handler.  In either case,
 *   global interrupts are disabled, either explicitly or indirectly through
 *   interrupt handling logic.
 *
 ****************************************************************************/

static inline uint8_t *rk3576_eth_allocbuffer(struct rk3576_eth_driver_s *priv)
{
  /* Allocate a buffer by returning the head of the free buffer list */

  return (uint8_t *)sq_remfirst(&priv->freeb);
}

/****************************************************************************
 * Function: rk3576_eth_freebuffer
 *
 * Description:
 *   Return a buffer to the free buffer list.
 *
 * Parameters:
 *   priv  - Reference to the driver state structure
 *   buffer - A pointer to the buffer to be freed
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   May or may not be called from an interrupt handler.  In either case,
 *   global interrupts are disabled, either explicitly or indirectly through
 *   interrupt handling logic.
 *
 ****************************************************************************/

static inline void rk3576_eth_freebuffer(struct rk3576_eth_driver_s *priv,
                                    uint8_t *buffer)
{
  /* Free the buffer by adding it to the end of the free buffer list */

  sq_addlast((sq_entry_t *)buffer, &priv->freeb);
}

/****************************************************************************
 * Function: rk3576_eth_isfreebuffer
 *
 * Description:
 *   Return TRUE if the free buffer list is not empty.
 *
 * Parameters:
 *   priv  - Reference to the driver state structure
 *
 * Returned Value:
 *   True if there are one or more buffers in the free buffer list;
 *   false if the free buffer list is empty
 *
 * Assumptions:
 *   None.
 *
 ****************************************************************************/

static inline bool rk3576_eth_isfreebuffer(struct rk3576_eth_driver_s *priv)
{
  /* Return TRUE if the free buffer list is not empty */

  return !sq_empty(&priv->freeb);
}

/****************************************************************************
 * Function: rk3576_eth_get_next_txdesc
 *
 * Description:
 *   Returns the next tx descriptor in the list
 *
 * Parameters:
 *   priv - Reference to the driver state structure
 *   curr - Pointer to a tx descriptor
 *
 * Returned Value:
 *   pointer to the next tx descriptor for the current interface
 *
 ****************************************************************************/

static struct eth_desc_s *rk3576_eth_get_next_txdesc(
    struct rk3576_eth_driver_s *priv,
    struct eth_desc_s * curr)
{
  union rk3576_eth_desc_u *first =
    &g_txtable[0];
  union rk3576_eth_desc_u *last =
    &g_txtable[CONFIG_RK3576_ETH_NTXBUFFERS - 1];
  union rk3576_eth_desc_u *next = ((union rk3576_eth_desc_u *)curr) + 1;

  if (next > last)
    {
      next = first;
    }

  return &next->desc;
}

/****************************************************************************
 * Function: rk3576_eth_transmit
 *
 * Description:
 *   Start hardware transmission.  Called either from the txdone interrupt
 *   handling or from watchdog based polling.
 *
 * Input Parameters:
 *   priv  - Reference to the driver state structure
 *
 * Returned Value:
 *   OK on success; a negated errno on failure
 *
 * Assumptions:
 *   May or may not be called from an interrupt handler.  In either case,
 *   global interrupts are disabled, either explicitly or indirectly through
 *   interrupt handling logic.
 *
 ****************************************************************************/

static int rk3576_eth_transmit(struct rk3576_eth_driver_s *priv)
{
  struct eth_desc_s *txdesc;
  struct eth_desc_s *txfirst;

  /* The internal (optimal) network buffer size may be configured to be
   * larger than the Ethernet buffer size.
   */

#if OPTIMAL_ETH_BUFSIZE > ALIGNED_BUFSIZE
  uint8_t *buffer;
  int bufcount;
  int lastsize;
  int i;
#endif

  /* Verify that the hardware is ready to send another packet.  If we get
   * here, then we are committed to sending a packet; Higher level logic
   * must have assured that there is no transmission in progress.
   */

  txdesc  = priv->txhead;
  txfirst = txdesc;

  ninfo("d_len: %d d_buf: %p txhead: %p tdes3: %08" PRIx32 "\n",
        priv->dev.d_len, priv->dev.d_buf, txdesc, txdesc->des3);

  DEBUGASSERT(txdesc);

  /* Flush the contents of the TX buffer into physical memory */

  up_clean_dcache((uintptr_t)priv->dev.d_buf,
                  (uintptr_t)priv->dev.d_buf + priv->dev.d_len);

  /* Is the size to be sent greater than the size of the Ethernet buffer? */

  DEBUGASSERT(priv->dev.d_len > 0 && priv->dev.d_buf != NULL);

#if OPTIMAL_ETH_BUFSIZE > ALIGNED_BUFSIZE
  if (priv->dev.d_len > ALIGNED_BUFSIZE)
    {
      /* Yes... how many buffers will be need to send the packet? */

      bufcount = (priv->dev.d_len + (ALIGNED_BUFSIZE - 1)) / ALIGNED_BUFSIZE;
      lastsize = priv->dev.d_len - (bufcount - 1) * ALIGNED_BUFSIZE;

      ninfo("bufcount: %d lastsize: %d\n", bufcount, lastsize);

      /* Set the first segment bit in the first TX descriptor */

      txdesc->des3 = EMAC_TDES3_FD_MASK;

      /* Set up all but the last TX descriptor */

      buffer = priv->dev.d_buf;

      for (i = 0; i < bufcount; i++)
        {
          DEBUGASSERT((txdesc->des3 & EMAC_TDES3_OWN_MASK) == 0);

          /* Set the Buffer1 address pointer */

          txdesc->des0 = (uint32_t)(uintptr_t)buffer;

          /* Set the Buffer2 address pointer */

          txdesc->des1 = 0;

          /* Set the buffer size in all TX descriptors */

          if (i == (bufcount - 1))
            {
              /* This is the last segment.  Set the last segment bit in the
               * last TX descriptor
               */

              txdesc->des3 |= EMAC_TDES3_LD_MASK;

              /* This segment is, most likely, of fractional buffersize */

              /* ask for an interrupt when this segment transfer completes. */

              txdesc->des2 = lastsize | EMAC_TDES2_IOC_MASK;
              buffer        += lastsize;
            }
          else
            {
              /* This is not the last segment.  We don't want an interrupt
               * when this segment transfer completes.
               */

              /* The size of the transfer is the whole buffer */

              txdesc->des2  = ALIGNED_BUFSIZE;
              buffer        += ALIGNED_BUFSIZE;
            }

          /* Give the descriptor to DMA */

          txdesc->des3 |= EMAC_TDES3_OWN_MASK;

          /* Flush the contents of the modified TX descriptor into physical
           * memory.
           */

          up_clean_dcache((uintptr_t)txdesc,
                          (uintptr_t)txdesc + sizeof(struct eth_desc_s));

          /* Point to the next available TX descriptor */

          txdesc = rk3576_eth_get_next_txdesc(priv, txdesc);
        }
    }
  else
#endif
    {
      DEBUGASSERT((txdesc->des3 & EMAC_TDES3_OWN_MASK) == 0);

      /* Set the Buffer1 address pointer */

      txdesc->des0 = (uint32_t)(uintptr_t)priv->dev.d_buf;

      /* Set the Buffer2 address pointer */

      txdesc->des1 = 0;

      /* Set frame size, and we do
       * want an interrupt when the transfer completes.
       */

      DEBUGASSERT(priv->dev.d_len <= CONFIG_NET_ETH_PKTSIZE);
      txdesc->des2 = priv->dev.d_len | EMAC_TDES2_IOC_MASK
                     | EMAC_TDES2_TTSE_MASK;

      /* The single descriptor is both the first and last segment. */

      /* Set OWN bit of the TX descriptor des3.  This gives the buffer to
       * Ethernet DMA
       */

      txdesc->des3 = EMAC_TDES3_FD_MASK | EMAC_TDES3_LD_MASK |
                   (uint32_t)priv->dev.d_len | EMAC_TDES3_OWN_MASK;

      /* Flush the contents of the modified TX descriptor into physical
       * memory.
       */

      up_clean_dcache((uintptr_t)txdesc,
                      (uintptr_t)txdesc + sizeof(struct eth_desc_s));

      /* Point to the next available TX descriptor */

      txdesc = rk3576_eth_get_next_txdesc(priv, txdesc);
    }

  /* Remember where we left off in the TX descriptor chain */

  priv->txhead = txdesc;

  /* Detach the buffer from priv->dev structure.  That buffer is now
   * "in-flight".
   */

  priv->dev.d_buf = NULL;
  priv->dev.d_len = 0;

  /* If there is no other TX buffer, in flight, then remember the location
   * of the TX descriptor.  This is the location to check for TX done events.
   */

  if (!priv->txtail)
    {
      DEBUGASSERT(priv->inflight == 0);
      priv->txtail = txfirst;
    }

  /* Increment the number of TX transfer in-flight */

  priv->inflight++;

  ninfo("txhead: %p txtail: %p inflight: %d\n",
        priv->txhead, priv->txtail, priv->inflight);

  /* If all TX descriptors are in-flight, then we have to disable receive
   * interrupts too.  This is because receive events can trigger more un-
   * stoppable transmit events.
   */

  if (priv->inflight >= CONFIG_RK3576_ETH_NTXBUFFERS)
    {
      rk3576_eth_disableint(priv, EMAC_DMA_CH0_INTERRUPT_ENABLE_RIE);
    }

  UP_MB();

  /* Enable TX interrupts */

  rk3576_eth_enableint(priv, EMAC_DMA_CH0_INTERRUPT_ENABLE_TIE);

  /* Setup the TX timeout watchdog (perhaps restarting the timer) */

  wd_start(&priv->txtimeout, RK3576_TXTIMEOUT,
           rk3576_eth_txtimeout_expiry, (wdparm_t)priv);

  /* Update the tx descriptor tail pointer register to start the DMA */

  putreg32((uintptr_t)txdesc, RK3576_EMAC_DMA_CH0_TXDESC_TAIL_POINTER);

  return OK;
}

/****************************************************************************
 * Function: rk3576_eth_txpoll
 *
 * Description:
 *   The transmitter is available, check if the network has any outgoing
 *   packets ready to send.  This is a callback from devif_poll().
 *   devif_poll() may be called:
 *
 *   1. When the preceding TX packet send is complete,
 *   2. When the preceding TX packet send timesout and the interface is reset
 *   3. During normal TX polling
 *
 * Input Parameters:
 *   dev  - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   OK on success; a negated errno on failure
 *
 * Assumptions:
 *   May or may not be called from an interrupt handler.  In either case,
 *   global interrupts are disabled, either explicitly or indirectly through
 *   interrupt handling logic.
 *
 ****************************************************************************/

static int rk3576_eth_txpoll(struct net_driver_s *dev)
{
  struct rk3576_eth_driver_s *priv = (struct rk3576_eth_driver_s *)dev->d_private;

  DEBUGASSERT(priv->dev.d_buf != NULL);

  /* Send the packet */

  rk3576_eth_transmit(priv);
  DEBUGASSERT(dev->d_len == 0 && dev->d_buf == NULL);

  /* Check if the next TX descriptor is owned by the Ethernet DMA or
   * CPU.  We cannot perform the TX poll if we are unable to accept
   * another packet for transmission.
   *
   * In a race condition, ETH_TDES3_OWN may be cleared BUT still
   * not available because rk3576_eth_freeframe() has not yet run. If
   * rk3576_eth_freeframe() has run, the buffer1 pointer (tdes2) will be
   * nullified (and inflight should be < CONFIG_RK3576_ETH_NTXBUFFERS).
   */

  if ((priv->txhead->des3 & EMAC_TDES3_OWN_MASK) != 0 ||
      priv->txhead->des0 != 0)
    {
      /* We have to terminate the poll if we have no more descriptors
       * available for another transfer.
       */

      nerr("No tx descriptors available");

      return -EBUSY;
    }

  /* We have the descriptor, we can continue the poll. Allocate a new
   * buffer for the poll.
   */

  dev->d_buf = rk3576_eth_allocbuffer(priv);

  /* We can't continue the poll if we have no buffers */

  if (dev->d_buf == NULL)
    {
      /* Terminate the poll. */

      nerr("No tx buffer available");

      return -ENOMEM;
    }

  /* If zero is returned, the polling will continue until all connections
   * have been examined.
   */

  return 0;
}

/****************************************************************************
 * Function: rk3576_eth_dopoll
 *
 * Description:
 *   The function is called in order to perform an out-of-sequence TX poll.
 *   This is done:
 *
 *   1. After completion of a transmission (rk3576_eth_txdone),
 *   2. When new TX data is available (rk3576_eth_txavail_process), and
 *   3. After a TX timeout to restart the sending process
 *      (rk3576_eth_txtimeout_process).
 *
 * Parameters:
 *   priv  - Reference to the driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Global interrupts are disabled by interrupt handling logic.
 *
 ****************************************************************************/

static void rk3576_eth_dopoll(struct rk3576_eth_driver_s *priv)
{
  struct net_driver_s *dev = &priv->dev;

  /* Check if the next TX descriptor is owned by the Ethernet DMA or
   * CPU.  We cannot perform the TX poll if we are unable to accept
   * another packet for transmission.
   *
   * In a race condition, EMAC_TDES3_OWN_MASK may be cleared BUT still
   * not available because rk3576_eth_freeframe() has not yet run. If
   * rk3576_eth_freeframe() has run, the buffer1 pointer (des0) will be
   * nullified (and inflight should be < CONFIG_RK3576_ETH_NTXBUFFERS).
   */

  if ((priv->txhead->des3 & EMAC_TDES3_OWN_MASK) == 0 &&
      priv->txhead->des0 == 0)
    {
      /* If we have the descriptor, then poll the network for new XMIT data.
       * Allocate a buffer for the poll.
       */

      DEBUGASSERT(dev->d_len == 0 && dev->d_buf == NULL);
      dev->d_buf = rk3576_eth_allocbuffer(priv);

      /* We can't poll if we have no buffers */

      if (dev->d_buf)
        {
          devif_poll(dev, rk3576_eth_txpoll);

          /* We will, most likely end up with a buffer to be freed.  But it
           * might not be the same one that we allocated above.
           */

          if (dev->d_buf)
            {
              DEBUGASSERT(dev->d_len == 0);
              rk3576_eth_freebuffer(priv, dev->d_buf);
              dev->d_buf = NULL;
            }
        }
      else
        {
          nerr("No tx buffers");
        }
    }
  else
    {
      nerr("No tx descriptors\n");
    }
}

/****************************************************************************
 * Function: rk3576_eth_enableint
 *
 * Description:
 *   Enable a "normal" interrupt
 *
 * Parameters:
 *   priv  - Reference to the driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Global interrupts are disabled by interrupt handling logic.
 *
 ****************************************************************************/

static void rk3576_eth_enableint(struct rk3576_eth_driver_s *priv, uint32_t ierbit)
{
  uint32_t regval;

  /* Enable the specified "normal" interrupt */

  regval  = getreg32(RK3576_EMAC_DMA_CH0_INTERRUPT_ENABLE);
  regval |= (EMAC_DMA_CH0_INTERRUPT_ENABLE_NIE | ierbit);
  putreg32(regval, RK3576_EMAC_DMA_CH0_INTERRUPT_ENABLE);
}

/****************************************************************************
 * Function: rk3576_eth_disableint
 *
 * Description:
 *   Disable a normal interrupt.
 *
 * Parameters:
 *   priv  - Reference to the driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Global interrupts are disabled by interrupt handling logic.
 *
 ****************************************************************************/

static void rk3576_eth_disableint(struct rk3576_eth_driver_s *priv,
                               uint32_t ierbit)
{
  uint32_t regval;

  /* Disable the "normal" interrupt */

  regval  = getreg32(RK3576_EMAC_DMA_CH0_INTERRUPT_ENABLE);
  regval &= ~ierbit;

  /* Are all "normal" interrupts now disabled? */

  if ((regval & ETH_DMAINT_NORMAL) == 0)
    {
      /* Yes.. disable normal interrupts */

      regval &= ~EMAC_DMA_CH0_INTERRUPT_ENABLE_NIE;
    }

  putreg32(regval, RK3576_EMAC_DMA_CH0_INTERRUPT_ENABLE);
}

/****************************************************************************
 * Function: rk3576_eth_get_next_rxdesc
 *
 * Description:
 *   Returns the next rx descriptor in the list
 *
 * Parameters:
 *   priv - Reference to the driver state structure
 *   curr - Pointer to a rx descriptor
 *
 * Returned Value:
 *   pointer to the next rx descriptor for the current interface
 *
 ****************************************************************************/

static struct eth_desc_s *rk3576_eth_get_next_rxdesc(
    struct rk3576_eth_driver_s *priv,
    struct eth_desc_s * curr)
{
  union rk3576_eth_desc_u *first = &g_rxtable[0];
  union rk3576_eth_desc_u *last =
    &g_rxtable[CONFIG_RK3576_ETH_NRXBUFFERS - 1];
  union rk3576_eth_desc_u *next = ((union rk3576_eth_desc_u *)curr) + 1;

  if (next > last)
    {
      next = first;
    }

  return &next->desc;
}

/****************************************************************************
 * Function: rk3576_eth_freesegment
 *
 * Description:
 *   The function is called when a frame is received using the DMA receive
 *   interrupt.  It scans the RX descriptors to the received frame.
 *
 * Parameters:
 *   priv  - Reference to the driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Global interrupts are disabled by interrupt handling logic.
 *
 ****************************************************************************/

static void rk3576_eth_freesegment(struct rk3576_eth_driver_s *priv,
                              struct eth_desc_s *rxfirst, int segments)
{
  struct eth_desc_s *rxdesc;
  int i;

  ninfo("rxfirst: %p segments: %d\n", rxfirst, segments);

  /* Give the freed RX buffers back to the Ethernet MAC to be refilled */

  rxdesc = rxfirst;
  for (i = 0; i < segments; i++)
    {
      /* Set OWN bit in RX descriptors.  This gives the buffers back to DMA */

      rxdesc->des3 = EMAC_RDES3_OWN_MASK | EMAC_RDES3_INTE_MASK |
                     EMAC_RDES3_BUF1V_MASK;

      /* Make sure that the modified RX descriptor is written to physical
       * memory.
       */

      up_clean_dcache((uintptr_t)rxdesc,
                      (uintptr_t)rxdesc + sizeof(struct eth_desc_s));

      /* Get the next RX descriptor in the chain */

      rxdesc = rk3576_eth_get_next_rxdesc(priv, rxdesc);

      /* Update the tail pointer */

      putreg32((uintptr_t)rxdesc, RK3576_EMAC_DMA_CH0_RXDESC_TAIL_POINTER);
    }

  /* Check if the RX Buffer unavailable flag is set */

  if ((getreg32(RK3576_EMAC_DMA_CH0_STATUS) & EMAC_DMA_CH0_STATUS_RBU) != 0)
    {
      /* Clear the RBU flag */

      putreg32(EMAC_DMA_CH0_STATUS_RBU, RK3576_EMAC_DMA_CH0_STATUS);

      nerr("EMAC_DMA_CH0_STATUS_RBU\n");

      /* To resume processing Rx descriptors, the application should change
       * the ownership of the descriptor and issue a Receive Poll Demand
       * command. If this command is not issued, the Rx process resumes when
       * the next recognized incoming packet is received. In ring mode, the
       * application should advance the Receive Descriptor Tail Pointer
       * register of a channel. This bit is set only when the DMA owns the
       * previous Rx descriptor.
       */
    }
}

/****************************************************************************
 * Function: rk3576_eth_recvframe
 *
 * Description:
 *   The function is called when a frame is received using the DMA receive
 *   interrupt.  It scans the RX descriptors of the received frame.
 *
 *   NOTE: This function will silently discard any packets containing errors.
 *
 * Parameters:
 *   priv  - Reference to the driver state structure
 *
 * Returned Value:
 *   OK if a packet was successfully returned; -EAGAIN if there are no
 *   further packets available
 *
 * Assumptions:
 *   Global interrupts are disabled by interrupt handling logic.
 *
 ****************************************************************************/

static int rk3576_eth_recvframe(struct rk3576_eth_driver_s *priv)
{
  struct eth_desc_s *rxdesc;
  struct eth_desc_s *rxcurr = NULL;
  uint8_t *buffer;
  int i;

  ninfo("rxhead: %p\n", priv->rxhead);

  /* Check if there are free buffers.  We cannot receive new frames in this
   * design unless there is at least one free buffer.
   */

  if (!rk3576_eth_isfreebuffer(priv))
    {
      nerr("ERROR: No free buffers\n");
      return -ENOMEM;
    }

  /* Scan descriptors owned by the CPU.  Scan until:
   *
   *   1) We find a descriptor still owned by the DMA,
   *   2) We have examined all of the RX descriptors, or
   *   3) All of the TX descriptors are in flight.
   *
   * This last case is obscure.  It is due to that fact that each packet
   * that we receive can generate an unstoppable transmisson.  So we have
   * to stop receiving when we can not longer transmit.  In this case, the
   * transmit logic should also have disabled further RX interrupts.
   */

  rxdesc = priv->rxhead;

  /* Forces the first RX descriptor to be re-read from physical memory */

  up_invalidate_dcache((uintptr_t)rxdesc,
                       (uintptr_t)rxdesc + sizeof(struct eth_desc_s));

  for (i = 0;
       (rxdesc->des3 & EMAC_RDES3_OWN_MASK) == 0 &&
         i < CONFIG_RK3576_ETH_NRXBUFFERS &&
         priv->inflight < CONFIG_RK3576_ETH_NTXBUFFERS;
       i++)
    {
      /* Check if this is a normal descriptor */

      if (!(rxdesc->des3 & EMAC_RDES3_INTE_MASK))
        {
          rxcurr = rxdesc;

          ninfo("rxhead: %p\n", priv->rxhead);

          /* Check if any errors are reported in the frame */

          if (true) /* FIXME rxdesc does not provide error information find another source */
            {
              struct net_driver_s *dev = &priv->dev;

              /* Get the Frame Length of the received packet: subtract 4
               * bytes of the CRC
               */

              uint32_t pktlen = rxdesc->des3 & EMAC_RDES3_PL_MASK;

              if (pktlen < 4 || pktlen - 4 > ALIGNED_BUFSIZE)
                {
                  /* Drop invalid or oversized frames. */

                  nerr("ERROR: Dropped invalid RX length: %" PRIu32 "\n",
                       pktlen);

                  rk3576_eth_freesegment(priv, rxcurr, 1);
                }
              else
                {
                  dev->d_len = pktlen - 4;

                  /* Get a buffer from the free list.  We don't even
                   * check if this is successful because we already
                   * assure the free list is not empty above.
                   */

                  buffer = rk3576_eth_allocbuffer(priv);

                  /* Take the buffer from the RX descriptor of the first
                   * free segment, put it into the network device
                   * structure, then replace the buffer in the RX
                   * descriptor with the newly allocated buffer.
                   */

                  DEBUGASSERT(dev->d_buf == NULL);
                  dev->d_buf    = (uint8_t *)(uintptr_t)rxcurr->des0;
                  rxcurr->des0 = (uint32_t)(uintptr_t)buffer;

                  /* Make sure that the modified RX descriptor is written
                   * to physical memory.
                   */

                  up_clean_dcache((uintptr_t)rxcurr,
                                  (uintptr_t)rxdesc +
                                  sizeof(struct eth_desc_s));

                  /* Remember where we should re-start scanning and reset
                   * the segment scanning logic
                   */

                  priv->rxhead   = rk3576_eth_get_next_rxdesc(priv, rxdesc);
                  rk3576_eth_freesegment(priv, rxcurr, 1);

                  /* Force the completed RX DMA buffer to be re-read from
                   * physical memory.
                   */

                  up_invalidate_dcache((uintptr_t)dev->d_buf,
                                      (uintptr_t)dev->d_buf +
                                      MIN(dev->d_len, ALIGNED_BUFSIZE));

                  ninfo("rxhead: %p d_buf: %p d_len: %d\n",
                      priv->rxhead, dev->d_buf, dev->d_len);

                  /* Return success */

                  return OK;
                }
            }
          else
            {
              /* Drop the frame that contains the errors, reset the
               * segment scanning logic, and continue scanning with the
               * next frame.
               */

            nwarn("WARNING: DROPPED RX descriptor errors: "
                "%08" PRIx32 "\n",
                rxdesc->des3);
            rk3576_eth_freesegment(priv, rxcurr, 1);
            }
        }
      else
        {
          /* Drop the context descriptors, we are not interested */

          DEBUGASSERT(rxcurr != NULL);
          rk3576_eth_freesegment(priv, rxcurr, 1);
        }

      /* Try the next descriptor */

      rxdesc = rk3576_eth_get_next_rxdesc(priv, rxdesc);

      /* Force the next RX descriptor to be re-read from physical memory */

      up_invalidate_dcache((uintptr_t)rxdesc,
                           (uintptr_t)rxdesc + sizeof(struct eth_desc_s));
    }

  /* We get here after all of the descriptors have been scanned or when
   * rxdesc points to the first descriptor owned by the DMA. Remember
   * where we left off.
   */

  priv->rxhead = rxdesc;

  ninfo("rxhead: %p\n", priv->rxhead);

  return -EAGAIN;
}

/****************************************************************************
 * Function: rk3576_eth_reply
 *
 * Description:
 *   收包处理里产生的回复（ping 应答、ARP 应答）走这里发出。
 *
 *   ★ 必须先看发送环有没有空位。
 *
 *     txpoll 发送前后都检查 txhead 是否空闲，这条路径原来没有：发送环满
 *     时直接覆盖一个还归 DMA 所有、或者还没回收的描述符，inflight 却照样
 *     加一。之后 freeframe 按 inflight 去回收，走到空描述符停不下来。
 *     没空位就丢掉这个回复 —— 对端会重发，比弄乱发送环好得多。
 *
 ****************************************************************************/

static uint32_t g_eth_reply_drops;

static void rk3576_eth_reply(struct rk3576_eth_driver_s *priv)
{
  struct eth_desc_s *txdesc = priv->txhead;

  up_invalidate_dcache((uintptr_t)txdesc,
                       (uintptr_t)txdesc + sizeof(struct eth_desc_s));

  if ((txdesc->des3 & EMAC_TDES3_OWN_MASK) != 0 || txdesc->des0 != 0 ||
      priv->inflight >= CONFIG_RK3576_ETH_NTXBUFFERS)
    {
      if ((g_eth_reply_drops++ % 64) == 0)
        {
          syslog(LOG_WARNING, "ETH: 发送环满，丢弃回复（累计 %" PRIu32
                 "，inflight=%d）\n", g_eth_reply_drops, priv->inflight);
        }

      priv->dev.d_len = 0;
      return;
    }

  rk3576_eth_transmit(priv);
}

/****************************************************************************
 * Function: rk3576_eth_receive
 *
 * Description:
 *   An interrupt was received indicating the availability of a new RX packet
 *
 * Parameters:
 *   priv  - Reference to the driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Global interrupts are disabled by interrupt handling logic.
 *
 ****************************************************************************/

static void rk3576_eth_receive(struct rk3576_eth_driver_s *priv)
{
  struct net_driver_s *dev = &priv->dev;

  /* Loop while while rk3576_eth_recvframe() successfully retrieves valid
   * Ethernet frames.
   */

  while (rk3576_eth_recvframe(priv) == OK)
    {
#ifdef CONFIG_NET_PKT
      /* When packet sockets are enabled, feed the frame into the packet
       * tap
       */

     pkt_input(&priv->dev);
#endif

      /* Check if the packet is a valid size for the network buffer
       * configuration (this should not happen)
       */

      if (dev->d_len > CONFIG_NET_ETH_PKTSIZE)
        {
          nwarn("WARNING: DROPPED Too big: %d\n", dev->d_len);

          /* Free dropped packet buffer */

          if (dev->d_buf)
            {
              rk3576_eth_freebuffer(priv, dev->d_buf);
              dev->d_buf = NULL;
              dev->d_len = 0;
            }

          continue;
        }

      /* We only accept IP packets of the configured type and ARP packets */

#ifdef CONFIG_NET_IPv4
      if (BUF->type == HTONS(ETHTYPE_IP))
        {
          ninfo("IPv4 frame\n");

          /* Receive an IPv4 packet from the network device */

          ipv4_input(&priv->dev);

          /* If the above function invocation resulted in data that should
           * be sent out on the network, the field  d_len will set to a
           * value > 0.
           */

          if (priv->dev.d_len > 0)
            {
              /* And send the packet */

              rk3576_eth_reply(priv);
            }
        }
      else
#endif
#ifdef CONFIG_NET_IPv6
      if (BUF->type == HTONS(ETHTYPE_IP6))
        {
          ninfo("IPv6 frame\n");

          /* Give the IPv6 packet to the network layer */

          ipv6_input(&priv->dev);

          /* If the above function invocation resulted in data that should
           * be sent out on the network, the field  d_len will set to a
           * value > 0.
           */

          if (priv->dev.d_len > 0)
            {
              /* And send the packet */

              rk3576_eth_reply(priv);
            }
        }
      else
#endif
#ifdef CONFIG_NET_ARP
      if (BUF->type == HTONS(ETHTYPE_ARP))
        {
          ninfo("ARP frame\n");

          /* Handle ARP packet */

          arp_input(&priv->dev);

          /* If the above function invocation resulted in data that should
           * be sent out on the network, the field  d_len will set to a
           * value > 0.
           */

          if (priv->dev.d_len > 0)
            {
              rk3576_eth_reply(priv);
            }
        }
      else
#endif
        {
          nwarn("WARNING: DROPPED Unknown type: %04x\n", BUF->type);
        }

      /* We are finished with the RX buffer.  NOTE:  If the buffer is
       * re-used for transmission, the dev->d_buf field will have been
       * nullified.
       */

      if (dev->d_buf)
        {
          /* Free the receive packet buffer */

          rk3576_eth_freebuffer(priv, dev->d_buf);
          dev->d_buf = NULL;
          dev->d_len = 0;
        }
    }
}

/****************************************************************************
 * Function: rk3576_eth_freeframe
 *
 * Description:
 *   Scans the TX descriptors and frees the buffers of completed TX
 *   transfers.
 *
 * Parameters:
 *   priv  - Reference to the driver state structure
 *
 * Returned Value:
 *   None.
 *
 * Assumptions:
 *   Global interrupts are disabled by interrupt handling logic.
 *
 ****************************************************************************/

static void rk3576_eth_freeframe(struct rk3576_eth_driver_s *priv)
{
  struct eth_desc_s *txdesc;
  uint32_t des3_tmp;
  int i;

  ninfo("txhead: %p txtail: %p inflight: %d\n",
        priv->txhead, priv->txtail, priv->inflight);

  /* Scan for "in-flight" descriptors owned by the CPU */

  txdesc = priv->txtail;
  if (txdesc)
    {
      DEBUGASSERT(priv->inflight > 0);

      /* Force re-reading of the TX descriptor for physical memory */

      up_invalidate_dcache((uintptr_t)txdesc,
                           (uintptr_t)txdesc + sizeof(struct eth_desc_s));

      /* ★ 最多走一圈，遇到空描述符立即停。
       *
       *   原来的循环只以"OWN 位为 0"为条件，唯一的出口是遇到 LD 且
       *   inflight 归零。一旦 inflight 与环上真实的帧数对不上（见
       *   rk3576_eth_receive 里直接发送的说明），剩下的空描述符 OWN=0、
       *   LD=0，这里就在 net_lock 里无限打转 —— 板上表现是有人 ping
       *   几下，整机（包括串口）卡死。
       */

      for (i = 0; (txdesc->des3 & EMAC_TDES3_OWN_MASK) == 0; i++)
        {
          if (i >= CONFIG_RK3576_ETH_NTXBUFFERS || txdesc->des0 == 0)
            {
              syslog(LOG_ERR, "ETH: 发送环计数失配 i=%d inflight=%d "
                     "des0=%08" PRIx32 " des3=%08" PRIx32 "，复位计数\n",
                     i, priv->inflight, txdesc->des0, txdesc->des3);
              priv->txtail   = NULL;
              priv->inflight = 0;
              return;
            }

          /* There should be a buffer assigned to all in-flight
           * TX descriptors.
           */

          ninfo("txtail: %p des0: %08" PRIx32
                " des2: %08" PRIx32 " des3: %08" PRIx32 "\n",
                txdesc, txdesc->des0, txdesc->des2, txdesc->des3);

          DEBUGASSERT(txdesc->des0 != 0);

          /* Yes.. Free the buffer */

          rk3576_eth_freebuffer(priv, (uint8_t *)(uintptr_t)txdesc->des0);

          /* In any event, make sure that des0-3 are nullified. */

          txdesc->des0 = 0;
          txdesc->des1 = 0;
          txdesc->des2 = 0;
          des3_tmp = txdesc->des3;
          txdesc->des3 = 0;

          /* Flush the contents of the modified TX descriptor into
           * physical memory.
           */

          up_clean_dcache((uintptr_t)txdesc,
                          (uintptr_t)txdesc + sizeof(struct eth_desc_s));

          /* Check if this was the last segment of a TX frame */

          if ((des3_tmp & EMAC_TDES3_LD_MASK) != 0)
            {
              /* Yes.. Decrement the number of frames "in-flight". */

              priv->inflight--;

              /* If all of the TX descriptors were in-flight,
               * then RX interrupts may have been disabled...
               * we can re-enable them now.
               */

              rk3576_eth_enableint(priv, EMAC_DMA_CH0_INTERRUPT_ENABLE_RIE);

              /* If there are no more frames in-flight, then bail. */

              if (priv->inflight <= 0)
                {
                  priv->txtail   = NULL;
                  priv->inflight = 0;
                  return;
                }
            }

          /* Try the next descriptor in the TX chain */

          txdesc = rk3576_eth_get_next_txdesc(priv, txdesc);

          /* Force re-reading of the TX descriptor for physical memory */

          up_invalidate_dcache((uintptr_t)txdesc,
                               (uintptr_t)txdesc +
                               sizeof(struct eth_desc_s));
        }

      /* We get here if (1) there are still frames "in-flight". Remember
       * where we left off.
       */

      priv->txtail = txdesc;

      ninfo("txhead: %p txtail: %p inflight: %d\n",
            priv->txhead, priv->txtail, priv->inflight);
    }
}

/****************************************************************************
 * Function: rk3576_eth_txdone
 *
 * Description:
 *   An interrupt was received indicating that the last TX packet(s) is done
 *
 * Input Parameters:
 *   priv  - Reference to the driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Global interrupts are disabled by the watchdog logic.
 *   The network is locked.
 *
 ****************************************************************************/

static void rk3576_eth_txdone(struct rk3576_eth_driver_s *priv)
{
  /* ★ 没有在途的帧是合法状态，不是断言条件。
   *
   *   TI 状态位在 ISR 里累积到 priv->dmasr，工作队列处理时可能上一轮已经
   *   把帧回收完、txtail 置空了，这一轮又带着 TI 进来。原来这里是
   *   DEBUGASSERT(txtail != NULL)，ai_agent 跑 HTTPS 时在 lpwork 里
   *   触发（rk3576_eth.c:1759），整机 panic。
   */

  if (priv->txtail != NULL)
    {
      /* Scan the TX descriptor change, returning buffers to free list */

      rk3576_eth_freeframe(priv);
    }

  /* If no further xmits are pending, then cancel the TX timeout */

  if (priv->inflight <= 0)
    {
      /* Cancel the TX timeout */

      wd_cancel(&priv->txtimeout);

      /* And disable further TX interrupts. */

      rk3576_eth_disableint(priv, EMAC_DMA_CH0_INTERRUPT_ENABLE_TIE);
    }

  /* Then poll the network for new XMIT data */

  rk3576_eth_dopoll(priv);
}

/****************************************************************************
 * Function: rk3576_eth_interrupt_work
 *
 * Description:
 *   Perform interrupt related work from the worker thread
 *
 * Input Parameters:
 *   arg - The argument passed when work_queue() was called.
 *
 * Returned Value:
 *   OK on success
 *
 * Assumptions:
 *   The network is locked.
 *
 ****************************************************************************/

static void rk3576_eth_interrupt_work(void *arg)
{
  struct rk3576_eth_driver_s *priv = (struct rk3576_eth_driver_s *)arg;
  uint32_t dmasr;
  irqstate_t flags;

  DEBUGASSERT(priv);

  /* Process pending Ethernet interrupts */

  net_lock();

  /* 用 ISR 抓下来的状态：寄存器已经在 ISR 里被清掉了 */

  flags = spin_lock_irqsave(&g_eth_irq_lock);
  dmasr = priv->dmasr;
  priv->dmasr = 0;
  spin_unlock_irqrestore(&g_eth_irq_lock, flags);

  /* Mask only enabled interrupts.  This depends on the fact that the
   * interrupt related bits (0-16) correspond in these two registers.
   */

  /* Check if there are pending "normal" interrupts */

  if (1)
    {
      /* Yes.. Check if we received an incoming packet, if so, call
       * rk3576_eth_receive()
       */

      if ((dmasr & EMAC_DMA_CH0_STATUS_RI) != 0)
        {
          /* Clear the pending receive interrupt */

          putreg32(EMAC_DMA_CH0_STATUS_RI, RK3576_EMAC_DMA_CH0_STATUS);

          /* Handle the received package */

          rk3576_eth_receive(priv);
        }

      /* Check if a packet transmission just completed.  If so, call
       * rk3576_eth_txdone(). This may disable further TX interrupts if there
       * are no pending transmissions.
       */

      if ((dmasr & EMAC_DMA_CH0_STATUS_TI) != 0)
        {
          /* Clear the pending receive interrupt */

          putreg32(EMAC_DMA_CH0_STATUS_TI, RK3576_EMAC_DMA_CH0_STATUS);

          /* Check if there are pending transmissions */

          rk3576_eth_txdone(priv);

          /* ★ 发送完成后要补收一次。
           *
           *   recvframe 在发送环满（inflight 到顶）或没有空闲缓冲区时
           *   提前返回，已经收进接收环的帧留在那里，而它们的 RI 已经
           *   消费掉了。txdone 腾出了位置，但不会再来新的 RI —— 对端在
           *   等这些帧的 ACK，不再发包。于是接收环里躺着 OWN=0 的帧、
           *   DMA 停在 RBU，连 ping 都不通（2026-10-31，接收环加到 64
           *   后大流量 TCP 必现）。
           */

          up_invalidate_dcache((uintptr_t)priv->rxhead,
                               (uintptr_t)priv->rxhead +
                               sizeof(struct eth_desc_s));

          if ((priv->rxhead->des3 & EMAC_RDES3_OWN_MASK) == 0)
            {
              rk3576_eth_receive(priv);
            }
        }

      /* Clear the pending normal summary interrupt */

      putreg32(EMAC_DMA_CH0_STATUS_NIS, RK3576_EMAC_DMA_CH0_STATUS);
    }

  /* Handle error interrupt only if CONFIG_DEBUG_NET is eanbled */

#ifdef CONFIG_DEBUG_NET
  /* Check if there are pending "abnormal" interrupts */

  if ((dmasr & EMAC_DMA_CH0_STATUS_AIS) != 0)
    {
      /* Just let the user know what happened */

      nerr("ERROR: Abormal event(s): %08x\n", dmasr);

      /* Clear all pending abnormal events */

      putreg32(ETH_DMAINT_ABNORMAL, RK3576_EMAC_DMA_CH0_STATUS);

      /* Clear the pending abnormal summary interrupt */

      putreg32(EMAC_DMA_CH0_STATUS_AIS, RK3576_EMAC_DMA_CH0_STATUS);
    }
#endif

  net_unlock();

}

/****************************************************************************
 * Function: rk3576_eth_enet_interrupt
 *
 * Description:
 *   Three interrupt sources will vector to this function:
 *   1. Ethernet MAC transmit interrupt handler
 *   2. Ethernet MAC receive interrupt handler
 *   3.
 *
 * Input Parameters:
 *   irq     - Number of the IRQ that generated the interrupt
 *   context - Interrupt register state save info (architecture-specific)
 *
 * Returned Value:
 *   OK on success
 *
 * Assumptions:
 *
 ****************************************************************************/

static int rk3576_eth_enet_interrupt(int irq, void *context, void *arg)
{
  register struct rk3576_eth_driver_s *priv = &g_enet[0];
  uint32_t dmasr;
  irqstate_t flags;

  /* Get the DMA interrupt status bits (no MAC interrupts are expected) */

  dmasr = getreg32(RK3576_EMAC_DMA_CH0_STATUS);

  /* ★ 进了中断却读到 DMA 状态为 0：这条电平线是别的源拉的（MAC 层
   *   中断），本函数不会去清它，中断会立刻重入。数一下，风暴时说出来。
   */

  if (dmasr == 0)
    {
      static uint32_t zero_hits;

      if ((++zero_hits % 100000) == 0)
        {
          syslog(LOG_ERR, "ETH: 中断 %d 状态为 0 已 %" PRIu32 " 次，"
                 "MAC_INT=%08" PRIx32 "\n", irq, zero_hits,
                 getreg32(RK3576_EMAC_BASE + 0xb0));
        }
    }

  if (dmasr != 0)
    {
      /* Disable further Ethernet interrupts.  Because Ethernet interrupts
       * are also disabled if the TX timeout event occurs, there can be no
       * race condition here.
       *
       * ★ RK3576 的 GMAC 中断是**电平触发**（dtsi: IRQ_TYPE_LEVEL_HIGH）。
       *   ISR 里既不清 DMA 状态位也不屏蔽中断的话，中断会立刻重入，
       *   ★ 电平触发的中断，**必须在 ISR 里当场把状态位清掉**，
       *     否则中断线一直拉着，ISR 反复重入 —— 表现为看门狗复位。
       *     参考驱动是在工作队列里才清，那是给边沿/脉冲中断写的，
       *     搬过来会死。
       *
       *     光屏蔽 DMA 的中断使能也不够：中断线由状态位驱动，
       *     使能位只决定"要不要产生"，已经拉起的线不会因此落下。
       *
       *   清掉的状态存进 priv->dmasr 交给 worker，worker 不再自己读 ——
       *   否则它读到的是已经被清空的寄存器，什么都处理不了。
       */

      putreg32(dmasr, RK3576_EMAC_DMA_CH0_STATUS);
      flags = spin_lock_irqsave(&g_eth_irq_lock);
      priv->dmasr |= dmasr;
      spin_unlock_irqrestore(&g_eth_irq_lock, flags);

      /* Check if a packet transmission just completed. */

      if ((dmasr & EMAC_DMA_CH0_STATUS_TI) != 0)
        {
          /* If a TX transfer just completed, then cancel the TX timeout so
           * there will be no race condition between any subsequent timeout
           * expiration and the deferred interrupt processing.
           */

          wd_cancel(&priv->txtimeout);
        }

      /* 已经排着一份就不再排 —— 原来这里是 DEBUGASSERT，
       * 开了断言的构建上会直接崩掉。
       */

      if (work_available(&priv->irqwork))
        {
          work_queue(ETHWORK, &priv->irqwork,
                     rk3576_eth_interrupt_work, priv, 0);
        }
    }

  return OK;
}

/****************************************************************************
 * Function: rk3576_eth_txtimeout_work
 *
 * Description:
 *   Perform TX timeout related work from the worker thread
 *
 * Input Parameters:
 *   arg - The argument passed when work_queue() as called.
 *
 * Returned Value:
 *   OK on success
 *
 * Assumptions:
 *
 ****************************************************************************/

static void rk3576_eth_txtimeout_work(void *arg)
{
  struct rk3576_eth_driver_s *priv = (struct rk3576_eth_driver_s *)arg;

  /* Increment statistics and dump debug info */

  net_lock();
  nerr("Resetting interface\n");

  NETDEV_TXTIMEOUTS(&priv->dev);

  /* Take the interface down and bring it back up.  That is the most
   * aggressive hardware reset.
   */

  rk3576_eth_ifdown(&priv->dev);
  rk3576_eth_ifup_action(&priv->dev, false);

  /* Then poll the network for new XMIT data */

  rk3576_eth_dopoll(priv);
  net_unlock();
}

/****************************************************************************
 * Function: rk3576_eth_txtimeout_expiry
 *
 * Description:
 *   Our TX watchdog timed out.  Called from the timer interrupt handler.
 *   The last TX never completed.  Reset the hardware and start again.
 *
 * Input Parameters:
 *   arg  - The argument
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Global interrupts are disabled by the watchdog logic.
 *
 ****************************************************************************/

static void rk3576_eth_txtimeout_expiry(wdparm_t arg)
{
  struct rk3576_eth_driver_s *priv = (struct rk3576_eth_driver_s *)arg;

  /* Disable further Ethernet interrupts.  This will prevent some race
   * conditions with interrupt work.  There is still a potential race
   * condition with interrupt work that is already queued and in progress.
   */

  up_disable_irq(RK3576_IRQ_EMAC);
  up_disable_irq(RK3576_IRQ_EMAC_CH0);

  /* Schedule to perform the TX timeout processing on the worker thread,
   * canceling any pending interrupt work.
   */

  work_queue(ETHWORK, &priv->irqwork, rk3576_eth_txtimeout_work, priv, 0);
}

/****************************************************************************
 * Function: rk3576_eth_ifup_action
 *
 * Description:
 *   Internal action routine to bring up the Ethernet interface
 *   which makes the resetting of the phy (which takes considerable time)
 *   optional.
 *
 * Input Parameters:
 *   dev      - Reference to the NuttX driver state structure
 *   resetphy - Flag indicating if Phy is to be reset. If not then the
 *              phy configuration is just re-loaded into the ethernet
 *              interface
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

static int rk3576_eth_ifup_action(struct net_driver_s *dev, bool resetphy)
{
  struct rk3576_eth_driver_s *priv = (struct rk3576_eth_driver_s *)dev->d_private;
  uint8_t *mac = dev->d_mac.ether.ether_addr_octet;
  uint32_t regval;
  int ret;

  ninfo("Bringing up: %u.%u.%u.%u\n",
        ip4_addr1(dev->d_ipaddr), ip4_addr2(dev->d_ipaddr),
        ip4_addr3(dev->d_ipaddr), ip4_addr4(dev->d_ipaddr));

  /* Initialize the free buffer list */

  rk3576_eth_initbuffer(priv, &g_txbuffer[0]);

  /* Software reset EMAC */

  rk3576_eth_reset(priv);

  /* Init EMAC DMA controller */

  putreg32(EMAC_DMA_MODE_INTM(1), RK3576_EMAC_DMA_MODE);
  putreg32(EMAC_DMA_SYSBUS_MODE_AAL, RK3576_EMAC_DMA_SYSBUS_MODE);

  /* Initialize EMAC DMA buffers */

  rk3576_eth_initbuffers(priv, &g_txtable[0], &g_rxtable[0], &g_rxbuffer[0]);

  rk3576_eth_initdma(priv);

  rk3576_eth_initmtl(priv);

  /* Set the MAC address */

  putreg32(EMAC_MAC_ADDRESS0_LOW_ADDRLO((mac[3] << 24) | (mac[2] << 16)
                                  | (mac[1] << 8) | mac[0]),
           RK3576_EMAC_MAC_ADDRESS0_LOW);
  putreg32(EMAC_MAC_ADDRESS0_HIGH_ADDRHI((mac[5] << 8) | (mac[4])),
           RK3576_EMAC_MAC_ADDRESS0_HIGH);

  /* Configure the PHY */

  ret = rk3576_eth_initphy(priv, resetphy);
  if (ret < 0)
    {
      nerr("ERROR: Failed to configure the PHY: %d\n", ret);
      return ret;
    }

  putreg32(0, RK3576_EMAC_MAC_Q0_TX_FLOW_CTRL);

  putreg32(EMAC_MAC_INTERRUPT_ENABLE_TXSTSIE
           | EMAC_MAC_INTERRUPT_ENABLE_RXSTSIE,
           RK3576_EMAC_MAC_INTERRUPT_ENABLE);

  regval = EMAC_MAC_PACKET_FILTER_RA;
#ifdef CONFIG_NET_PROMISCUOUS
  regval |= EMAC_MAC_PACKET_FILTER_PR;
#endif
  putreg32(regval, RK3576_EMAC_MAC_PACKET_FILTER);

  /* Mark the interrupt "up" and enable interrupts at the NVIC */

  priv->bifup = true;

  up_enable_irq(RK3576_IRQ_EMAC);
  up_enable_irq(RK3576_IRQ_EMAC_CH0);

  syslog(LOG_INFO, "ETH: eth0 就绪 MACCR=%08" PRIx32 " IE=%08" PRIx32 "\n",
         getreg32(RK3576_EMAC_MAC_CONFIGURATION),
         getreg32(RK3576_EMAC_DMA_CH0_INTERRUPT_ENABLE));

  /* ★ 必须报告载波已就绪。
   *
   *   IFF_UP 只表示"接口被启用"，而协议栈选路由时查的是 **IFF_RUNNING**
   *   （net/netdev/netdev_findbyaddr.c 的 netdev_prefixlen_findby_lipv4addr
   *   里 IFF_IS_RUNNING(dev->d_flags)）。这个标志在整棵树里只由
   *   netdev_carrier_on() 置位。
   *
   *   不调它的表现很有迷惑性：ifconfig 显示地址、掩码、网关都对，
   *   ifup 也返回 OK，但任何 sendto 都是 ENETUNREACH —— 连 ping
   *   自己的地址都不通，因为根本没有设备被选中。
   *
   *   参考驱动 s32k3xx_emac.c 里没有这两个调用，照搬过来就会踩到。
   */

  netdev_carrier_on(dev);

  return OK;
}

/****************************************************************************
 * Function: rk3576_eth_ifup
 *
 * Description:
 *   NuttX Callback: Bring up the Ethernet interface when an IP address is
 *   provided
 *
 * Input Parameters:
 *   dev  - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

static int rk3576_eth_ifup(struct net_driver_s *dev)
{
  /* The externally available ifup action includes resetting the phy */

  return rk3576_eth_ifup_action(dev, true);
}

/****************************************************************************
 * Function: rk3576_eth_ifdown
 *
 * Description:
 *   NuttX Callback: Stop the interface.
 *
 * Input Parameters:
 *   dev  - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

static int rk3576_eth_ifdown(struct net_driver_s *dev)
{
  struct rk3576_eth_driver_s *priv = (struct rk3576_eth_driver_s *)dev->d_private;
  irqstate_t flags;

  netdev_carrier_off(dev);

  ninfo("Taking down: %u.%u.%u.%u\n",
        ip4_addr1(dev->d_ipaddr), ip4_addr2(dev->d_ipaddr),
        ip4_addr3(dev->d_ipaddr), ip4_addr4(dev->d_ipaddr));

  /* Flush and disable the Ethernet interrupts at the NVIC */

  flags = enter_critical_section();

  up_disable_irq(RK3576_IRQ_EMAC);
  up_disable_irq(RK3576_IRQ_EMAC_CH0);

  {
    irqstate_t lockflags = spin_lock_irqsave(&g_eth_irq_lock);
    priv->dmasr = 0;
    spin_unlock_irqrestore(&g_eth_irq_lock, lockflags);
  }

  /* FIXME clear interrupts */

  /* Cancel the TX timeout timers */

  wd_cancel(&priv->txtimeout);

  /* Put the EMAC in its reset, non-operational state.  This should be
   * a known configuration that will guarantee the rk3576_eth_ifup() always
   * successfully brings the interface back up.
   */

  rk3576_eth_reset(priv);

  /* Mark the device "down" */

  priv->bifup = false;
  leave_critical_section(flags);
  return OK;
}

/****************************************************************************
 * Function: rk3576_eth_txavail_work
 *
 * Description:
 *   Perform an out-of-cycle poll on the worker thread.
 *
 * Input Parameters:
 *   arg - Reference to the NuttX driver state structure (cast to void*)
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Called on the higher priority worker thread.
 *
 ****************************************************************************/

static void rk3576_eth_txavail_work(void *arg)
{
  struct rk3576_eth_driver_s *priv = (struct rk3576_eth_driver_s *)arg;

  /* Ignore the notification if the interface is not yet up */

  net_lock();
  if (priv->bifup)
    {
      /* Poll the network for new XMIT data */

      rk3576_eth_dopoll(priv);
    }

  net_unlock();
}

/****************************************************************************
 * Function: rk3576_eth_txavail
 *
 * Description:
 *   Driver callback invoked when new TX data is available.  This is a
 *   stimulus perform an out-of-cycle poll and, thereby, reduce the TX
 *   latency.
 *
 * Input Parameters:
 *   dev  - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Called in normal user mode
 *
 ****************************************************************************/

static int rk3576_eth_txavail(struct net_driver_s *dev)
{
  struct rk3576_eth_driver_s *priv = (struct rk3576_eth_driver_s *)dev->d_private;

  /* Is our single work structure available?  It may not be if there are
   * pending interrupt actions and we will have to ignore the Tx
   * availability action.
   */

  if (work_available(&priv->pollwork))
    {
      /* Schedule to serialize the poll on the worker thread. */

      work_queue(ETHWORK, &priv->pollwork, rk3576_eth_txavail_work, priv, 0);
    }

  return OK;
}

/****************************************************************************
 * Function: rk3576_eth_calcethcrc
 *
 * Description:
 *   Function to calculate the CRC used by S32K3XX to check an Ethernet frame
 *
 * Input Parameters:
 *   data   - the data to be checked
 *   length - length of the data
 *
 * Returned Value:
 *   crc32
 *
 * Assumptions:
 *
 ****************************************************************************/

#ifdef CONFIG_NET_MCASTGROUP
static uint32_t rk3576_eth_calcethcrc(const uint8_t *data, size_t length)
{
  uint32_t crc    = 0xffffffffu;
  uint32_t count1 = 0;
  uint32_t count2 = 0;

  /* Calculates the CRC-32 polynomial on the multicast group address. */

  for (count1 = 0; count1 < length; count1++)
    {
      uint8_t c = data[count1];

      for (count2 = 0; count2 < 0x08u; count2++)
        {
          if ((c ^ crc) & 1U)
            {
              crc >>= 1U;
              c   >>= 1U;
              crc  ^= 0xedb88320u;
            }
          else
            {
              crc >>= 1U;
              c   >>= 1U;
            }
        }
    }

  return crc;
}
#endif

/****************************************************************************
 * Function: rk3576_eth_enet_hash_index
 *
 * Description:
 *   Function to find the hash index for multicast address filter
 *
 * Input Parameters:
 *   mac  - The MAC address
 *
 * Returned Value:
 *   hash index
 *
 * Assumptions:
 *
 ****************************************************************************/

#ifdef CONFIG_NET_MCASTGROUP
static uint32_t rk3576_eth_enet_hash_index(const uint8_t *mac)
{
  uint32_t crc;
  uint32_t hashindex;

  ninfo("MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  crc = rk3576_eth_calcethcrc(mac, 6);
  hashindex = (crc >> 26) & 0x3f;

  return hashindex;
}
#endif

/****************************************************************************
 * Function: rk3576_eth_addmac
 *
 * Description:
 *   NuttX Callback: Add the specified MAC address to the hardware multicast
 *   address filtering
 *
 * Input Parameters:
 *   dev  - Reference to the NuttX driver state structure
 *   mac  - The MAC address to be added
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

#ifdef CONFIG_NET_MCASTGROUP
static int rk3576_eth_addmac(struct net_driver_s *dev, const uint8_t *mac)
{
  uint32_t crc;
  uint32_t hashindex;
  uint32_t temp;
  uint32_t registeraddress;

  hashindex = rk3576_eth_enet_hash_index(mac);

  /* Add the MAC address to the hardware multicast routing table */

  if (hashindex > 31)
    {
      registeraddress = RK3576_EMAC_MAC_HASH_TABLE_REG1;
      hashindex      -= 32;
    }
  else
    {
      registeraddress = RK3576_EMAC_MAC_HASH_TABLE_REG0;
    }

  temp  = getreg32(registeraddress);
  temp |= 1 << hashindex;
  putreg32(temp, registeraddress);

  return OK;
}
#endif

/****************************************************************************
 * Function: rk3576_eth_rmmac
 *
 * Description:
 *   NuttX Callback: Remove the specified MAC address from the hardware
 *   multicast address filtering
 *
 * Input Parameters:
 *   dev  - Reference to the NuttX driver state structure
 *   mac  - The MAC address to be removed
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

#ifdef CONFIG_NET_MCASTGROUP
static int rk3576_eth_rmmac(struct net_driver_s *dev, const uint8_t *mac)
{
  uint32_t crc;
  uint32_t hashindex;
  uint32_t temp;
  uint32_t registeraddress;

  /* Remove the MAC address from the hardware multicast routing table */

  hashindex = rk3576_eth_enet_hash_index(mac);

  if (hashindex > 31)
    {
      registeraddress = RK3576_EMAC_MAC_HASH_TABLE_REG1;
      hashindex      -= 32;
    }
  else
    {
      registeraddress = RK3576_EMAC_MAC_HASH_TABLE_REG0;
    }

  temp  = getreg32(registeraddress);
  temp &= ~(1 << hashindex);
  putreg32(temp, registeraddress);

  return OK;
}
#endif

/****************************************************************************
 * Function: rk3576_eth_ioctl
 *
 * Description:
 *   PHY ioctl command handler
 *
 * Input Parameters:
 *   dev  - Reference to the NuttX driver state structure
 *   cmd  - ioctl command
 *   arg  - Argument accompanying the command
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 * Assumptions:
 *
 ****************************************************************************/

#ifdef CONFIG_NETDEV_IOCTL
static int rk3576_eth_ioctl(struct net_driver_s *dev, int cmd,
                         unsigned long arg)
{
#ifdef CONFIG_NETDEV_PHY_IOCTL
  struct rk3576_eth_driver_s *priv = (struct rk3576_eth_driver_s *)dev->d_private;
#endif
  int ret;

  switch (cmd)
    {
#ifdef CONFIG_NETDEV_PHY_IOCTL
#ifdef CONFIG_ARCH_PHY_INTERRUPT
      case SIOCMIINOTIFY: /* Set up for PHY event notifications */
        {
          struct mii_ioctl_notify_s *req =
            (struct mii_ioctl_notify_s *)((uintptr_t)arg);

          ret = phy_notify_subscribe(dev->d_ifname, req->pid, &req->event);
          if (ret == OK)
            {
              /* Enable PHY link up/down interrupts */

              ret = rk3576_eth_phyintenable(priv);
            }
        }
        break;
#endif

      case SIOCGMIIPHY: /* Get MII PHY address */
        {
          struct mii_ioctl_data_s *req =
            (struct mii_ioctl_data_s *)((uintptr_t)arg);
          req->phy_id = priv->phyaddr;
          ret = OK;
        }
        break;

      case SIOCGMIIREG: /* Get register from MII PHY */
        {
          struct mii_ioctl_data_s *req =
            (struct mii_ioctl_data_s *)((uintptr_t)arg);
#if defined(CLAUSE45)
          if (MII_MSR == req->reg_num)
            {
              ret = rk3576_eth_writemmd(priv, priv->phyaddr, MMD30_VEND1,
                                     VEND1_PHY_IRQ_ACK, PHY_IRQ_LINK_EVENT);

              ret = rk3576_eth_readmmd(priv, req->phy_id, MMD1,
                                    MMD1_PMA_STATUS1, &req->val_out);
            }
          else
#endif
            {
              ret = rk3576_eth_readmii(priv, req->phy_id,
                              req->reg_num, &req->val_out);
            }
        }
        break;

      case SIOCSMIIREG: /* Set register in MII PHY */
        {
          struct mii_ioctl_data_s *req =
            (struct mii_ioctl_data_s *)((uintptr_t)arg);
          ret =
            rk3576_eth_writemii(priv, req->phy_id, req->reg_num, req->val_in);
        }
        break;
#endif /* CONFIG_NETDEV_PHY_IOCTL */

      default:
        ret = -ENOTTY;
        break;
    }

  return ret;
}
#endif /* CONFIG_NETDEV_IOCTL */

/****************************************************************************
 * Function: rk3576_eth_phyintenable
 *
 * Description:
 *  Enable link up/down PHY interrupts.  The interrupt protocol is like this:
 *
 *  - Interrupt status is cleared when the interrupt is enabled.
 *  - Interrupt occurs.  Interrupt is disabled (at the processor level) when
 *    is received.
 *  - Interrupt status is cleared when the interrupt is re-enabled.
 *
 * Input Parameters:
 *   priv - A reference to the private driver state structure
 *
 * Returned Value:
 *   OK on success; Negated errno (-ETIMEDOUT) on failure.
 *
 ****************************************************************************/

#if defined(CONFIG_NETDEV_PHY_IOCTL) && defined(CONFIG_ARCH_PHY_INTERRUPT)
static int rk3576_eth_phyintenable(struct rk3576_eth_driver_s *priv)
{
#if defined(CONFIG_ETH0_PHY_KSZ8051) || defined(CONFIG_ETH0_PHY_KSZ8061) || \
    defined(CONFIG_ETH0_PHY_KSZ8081)
  uint16_t phyval;
  int ret;

  /* Read the interrupt status register in order to clear any pending
   * interrupts
   */

  ret = rk3576_eth_readmii(priv, priv->phyaddr, MII_KSZ8081_INT, &phyval);
  if (ret == OK)
    {
      /* Enable link up/down interrupts */

      ret = rk3576_eth_writemii(priv, priv->phyaddr, MII_KSZ8081_INT,
                           (MII_KSZ80X1_INT_LDEN | MII_KSZ80X1_INT_LUEN));
    }

  return ret;
#elif defined(CONFIG_ETH0_PHY_TJA1103)
  uint16_t phyval;
  int ret;

  ret = rk3576_eth_writemmd(priv, priv->phyaddr, MMD30_VEND1, VEND1_PHY_IRQ_EN,
                    PHY_IRQ_LINK_EVENT);

  return ret;
#else
#  error Unrecognized PHY
  return -ENOSYS;
#endif
}
#endif

/****************************************************************************
 * Function: rk3576_eth_writemii
 *
 * Description:
 *   Write a 16-bit value to a PHY register.
 *
 * Input Parameters:
 *   priv - Reference to the private ENET driver state structure
 *   phyaddr - The PHY address
 *   regaddr - The PHY register address
 *   data    - The data to write to the PHY register
 *
 * Returned Value:
 *   Zero on success, a negated errno value on failure.
 *
 ****************************************************************************/
#if defined(CONFIG_NETDEV_PHY_IOCTL)
static int rk3576_eth_writemii(struct rk3576_eth_driver_s *priv, uint8_t phyaddr,
                            uint8_t regaddr, uint16_t data)
{
  int timeout;
  uint32_t regval;

  /* Clear the MDIO */

  regval = getreg32(RK3576_EMAC_MAC_MDIO_ADDRESS);
  regval &= ~(EMAC_MAC_MDIO_ADDRESS_PA_MASK |
              EMAC_MAC_MDIO_ADDRESS_RDA_MASK |
              EMAC_MAC_MDIO_ADDRESS_GOC_0 |
              EMAC_MAC_MDIO_ADDRESS_GOC_1 |
              EMAC_MAC_MDIO_ADDRESS_C45E);
  putreg32(regval, RK3576_EMAC_MAC_MDIO_ADDRESS);

  putreg32((EMAC_MAC_MDIO_DATA_RA(0) | /* No CL45 for now */
           EMAC_MAC_MDIO_DATA_GD(data)),
           RK3576_EMAC_MAC_MDIO_DATA);

  regval = getreg32(RK3576_EMAC_MAC_MDIO_ADDRESS);
  regval |= (EMAC_MAC_MDIO_ADDRESS_GOC_0 |
             EMAC_MAC_MDIO_ADDRESS_PA(phyaddr) |
             EMAC_MAC_MDIO_ADDRESS_RDA(regaddr) |
             EMAC_MAC_MDIO_ADDRESS_GB);
  putreg32(regval, RK3576_EMAC_MAC_MDIO_ADDRESS);

  /* Wait for the transfer to complete */

  for (timeout = 0; timeout < MII_MAXPOLLS; timeout++)
    {
      if ((getreg32(RK3576_EMAC_MAC_MDIO_ADDRESS)
          & EMAC_MAC_MDIO_ADDRESS_GB) == 0)
        {
          break;
        }
    }

  /* Check for a timeout */

  if (timeout == MII_MAXPOLLS)
    {
      return -ETIMEDOUT;
    }

  return OK;
}
#endif

/****************************************************************************
 * Function: rk3576_eth_reademii
 *
 * Description:
 *   Read a 16-bit value from a PHY register.
 *
 * Input Parameters:
 *   priv    - Reference to the private ENET driver state structure
 *   phyaddr - The PHY address
 *   regaddr - The PHY register address
 *   data    - A pointer to the location to return the data
 *
 * Returned Value:
 *   Zero on success, a negated errno value on failure.
 *
 ****************************************************************************/

static int rk3576_eth_readmii(struct rk3576_eth_driver_s *priv, uint8_t phyaddr,
                           uint8_t regaddr, uint16_t *data)
{
  int timeout;
  uint32_t regval;

  /* Clear the MDIO  */

  regval = getreg32(RK3576_EMAC_MAC_MDIO_ADDRESS);
  regval &= ~(EMAC_MAC_MDIO_ADDRESS_PA_MASK |
              EMAC_MAC_MDIO_ADDRESS_RDA_MASK |
              EMAC_MAC_MDIO_ADDRESS_GOC_0 |
              EMAC_MAC_MDIO_ADDRESS_GOC_1 |
              EMAC_MAC_MDIO_ADDRESS_C45E);
  putreg32(regval, RK3576_EMAC_MAC_MDIO_ADDRESS);

  putreg32(EMAC_MAC_MDIO_DATA_RA(0), /* No CL45 for now */
           RK3576_EMAC_MAC_MDIO_DATA);

  regval = getreg32(RK3576_EMAC_MAC_MDIO_ADDRESS);
  regval |= (EMAC_MAC_MDIO_ADDRESS_GOC_0 |
             EMAC_MAC_MDIO_ADDRESS_GOC_1 | /* Indicate read */
             EMAC_MAC_MDIO_ADDRESS_PA(phyaddr) |
             EMAC_MAC_MDIO_ADDRESS_RDA(regaddr) |
             EMAC_MAC_MDIO_ADDRESS_GB);
  putreg32(regval, RK3576_EMAC_MAC_MDIO_ADDRESS);

  /* Wait for the transfer to complete */

  for (timeout = 0; timeout < MII_MAXPOLLS; timeout++)
    {
      if ((getreg32(RK3576_EMAC_MAC_MDIO_ADDRESS)
          & EMAC_MAC_MDIO_ADDRESS_GB) == 0)
        {
          break;
        }
    }

  /* Check for a timeout */

  if (timeout == MII_MAXPOLLS)
    {
      return -ETIMEDOUT;
    }

  *data = EMAC_MAC_MDIO_DATA_GD(getreg32(RK3576_EMAC_MAC_MDIO_DATA));

  return OK;
}

#if defined(CLAUSE45)
/****************************************************************************
 * Function: rk3576_eth_readmmd
 *
 * Description:
 *   Read a 16-bit value from a PHY register.
 *
 * Input Parameters:
 *   priv - Reference to the private ENET driver state structure
 *   phyaddr - The PHY address
 *   mmd     - The Selected MMD Space
 *   regaddr - The PHY register address
 *   data    - A pointer to the location to return the data
 *
 * Returned Value:
 *   Zero on success, a negated errno value on failure.
 *
 ****************************************************************************/

static int rk3576_eth_readmmd(struct rk3576_eth_driver_s *priv, uint8_t phyaddr,
                         uint8_t mmd, uint16_t regaddr, uint16_t *data)
{
  int timeout;
  uint32_t regval;

  /* Clear the MDIO  */

  regval = getreg32(RK3576_EMAC_MAC_MDIO_ADDRESS);
  regval &= ~(EMAC_MAC_MDIO_ADDRESS_PA_MASK |
              EMAC_MAC_MDIO_ADDRESS_RDA_MASK |
              EMAC_MAC_MDIO_ADDRESS_GOC_0 |
              EMAC_MAC_MDIO_ADDRESS_GOC_1 |
              EMAC_MAC_MDIO_ADDRESS_C45E);
  putreg32(regval, RK3576_EMAC_MAC_MDIO_ADDRESS);

  putreg32(EMAC_MAC_MDIO_DATA_RA(regaddr), /* CL45 regaddr */
           RK3576_EMAC_MAC_MDIO_DATA);

  regval = getreg32(RK3576_EMAC_MAC_MDIO_ADDRESS);
  regval |= (EMAC_MAC_MDIO_ADDRESS_GOC_0 |
             EMAC_MAC_MDIO_ADDRESS_GOC_1 | /* Indicate read */
             EMAC_MAC_MDIO_ADDRESS_PA(phyaddr) |
             EMAC_MAC_MDIO_ADDRESS_RDA(mmd) |
             EMAC_MAC_MDIO_ADDRESS_C45E |
             EMAC_MAC_MDIO_ADDRESS_GB);
  putreg32(regval, RK3576_EMAC_MAC_MDIO_ADDRESS);

  /* Wait for the transfer to complete */

  for (timeout = 0; timeout < MII_MAXPOLLS; timeout++)
    {
      if ((getreg32(RK3576_EMAC_MAC_MDIO_ADDRESS)
          & EMAC_MAC_MDIO_ADDRESS_GB) == 0)
        {
          break;
        }
    }

  /* Check for a timeout */

  if (timeout == MII_MAXPOLLS)
    {
      return -ETIMEDOUT;
    }

  *data = EMAC_MAC_MDIO_DATA_GD(getreg32(RK3576_EMAC_MAC_MDIO_DATA));

  return OK;
}

/****************************************************************************
 * Function: rk3576_eth_writemmd
 *
 * Description:
 *   Write a 16-bit value to a PHY register.
 *
 * Input Parameters:
 *   priv - Reference to the private ENET driver state structure
 *   phyaddr - The PHY address
 *   mmd     - The Selected MMD Space
 *   regaddr - The PHY register address
 *   data    - Data
 *
 * Returned Value:
 *   Zero on success, a negated errno value on failure.
 *
 ****************************************************************************/

static int rk3576_eth_writemmd(struct rk3576_eth_driver_s *priv, uint8_t phyaddr,
                         uint8_t mmd, uint16_t regaddr, uint16_t data)
{
  int timeout;
  uint32_t regval;

  /* Clear the MDIO  */

  regval = getreg32(RK3576_EMAC_MAC_MDIO_ADDRESS);
  regval &= ~(EMAC_MAC_MDIO_ADDRESS_PA_MASK |
              EMAC_MAC_MDIO_ADDRESS_RDA_MASK |
              EMAC_MAC_MDIO_ADDRESS_GOC_0 |
              EMAC_MAC_MDIO_ADDRESS_GOC_1 |
              EMAC_MAC_MDIO_ADDRESS_C45E);
  putreg32(regval, RK3576_EMAC_MAC_MDIO_ADDRESS);

  putreg32((EMAC_MAC_MDIO_DATA_RA(regaddr) |
           EMAC_MAC_MDIO_DATA_GD(data)),
           RK3576_EMAC_MAC_MDIO_DATA);

  regval = getreg32(RK3576_EMAC_MAC_MDIO_ADDRESS);
  regval |= (EMAC_MAC_MDIO_ADDRESS_GOC_0 |
             EMAC_MAC_MDIO_ADDRESS_PA(phyaddr) |
             EMAC_MAC_MDIO_ADDRESS_RDA(mmd) |
             EMAC_MAC_MDIO_ADDRESS_C45E |
             EMAC_MAC_MDIO_ADDRESS_GB);
  putreg32(regval, RK3576_EMAC_MAC_MDIO_ADDRESS);

  /* Wait for the transfer to complete */

  for (timeout = 0; timeout < MII_MAXPOLLS; timeout++)
    {
      if ((getreg32(RK3576_EMAC_MAC_MDIO_ADDRESS)
          & EMAC_MAC_MDIO_ADDRESS_GB) == 0)
        {
          break;
        }
    }

  /* Check for a timeout */

  if (timeout == MII_MAXPOLLS)
    {
      return -ETIMEDOUT;
    }

  return OK;
}
#endif

/****************************************************************************
 * Function: rk3576_eth_initphy
 *
 * Description:
 *   Configure the PHY
 *
 * Input Parameters:
 *   priv     - Reference to the private ENET driver state structure
 *   renogphy - Flag indicating if to perform negotiation of the link
 *
 * Returned Value:
 *   Zero (OK) returned on success; a negated errno value is returned on any
 *   failure;
 *
 * Assumptions:
 *
 ****************************************************************************/

static inline int rk3576_eth_initphy(struct rk3576_eth_driver_s *priv,
                                  bool renogphy)
{
  uint16_t phydata;
  uint8_t phyaddr    = BOARD_PHY_ADDR;
  int retries;
  int ret;
  uint32_t mac_conf = 0;

  if (renogphy)
    {
      /* Loop (potentially infinitely?) until we successfully communicate
       * with the PHY. This is 'standard stuff' that should work for any PHY
       * - we are not communicating with it's 'special' registers
       * at this point.
       */

      ninfo("%s: Try phyaddr: %u\n", BOARD_PHY_NAME, phyaddr);

      /* Try to read PHYID1 few times using this address */

      retries = 0;
      do
        {
          nxsig_usleep(LINK_WAITUS);

          ninfo("%s: Read PHYID1, retries=%d\n",
                BOARD_PHY_NAME, retries + 1);

          phydata = 0xffff;
          ret     = rk3576_eth_readmii(priv, phyaddr, MII_PHYID1, &phydata);
        }
      while ((ret < 0 || phydata == 0xffff) && ++retries < 3);

      if (retries >= 3)
        {
          nerr("ERROR: Failed to read %s PHYID1 at address %d\n",
               BOARD_PHY_NAME, phyaddr);
          return -ENOENT;
        }

      ninfo("%s: Using PHY address %u\n", BOARD_PHY_NAME, phyaddr);
      priv->phyaddr = phyaddr;

      /* Verify PHYID1.  Compare OUI bits 3-18 */

      ninfo("%s: PHYID1: %04x\n", BOARD_PHY_NAME, phydata);
      if (phydata != BOARD_PHYID1)
        {
          nerr("ERROR: PHYID1=%04x incorrect for %s.  Expected %04x\n",
               phydata, BOARD_PHY_NAME, BOARD_PHYID1);
          return -ENXIO;
        }

      /* Read PHYID2 */

      ret = rk3576_eth_readmii(priv, phyaddr, MII_PHYID2, &phydata);
      if (ret < 0)
        {
          nerr("ERROR: Failed to read %s PHYID2: %d\n", BOARD_PHY_NAME, ret);
          return ret;
        }

      ninfo("%s: PHYID2: %04x\n", BOARD_PHY_NAME, phydata);

      /* Verify PHYID2:  Compare OUI bits 19-24 and the 6-bit model number
       * (ignoring the 4-bit revision number).
       */

      if ((phydata & 0xfff0) != (BOARD_PHYID2 & 0xfff0))
        {
          nerr("ERROR: PHYID2=%04x incorrect for %s.  Expected %04x\n",
               (phydata & 0xfff0), BOARD_PHY_NAME, (BOARD_PHYID2 & 0xfff0));
          return -ENXIO;
        }

#ifdef CONFIG_ETH0_PHY_KSZ8081
      /* Reset PHY */

      rk3576_eth_writemii(priv, phyaddr, MII_MCR, MII_MCR_RESET);

      /* Set RMII mode */

      ret = rk3576_eth_readmii(priv, phyaddr, MII_KSZ8081_PHYCTRL2, &phydata);
      if (ret < 0)
        {
          nerr("ERROR: Failed to read MII_KSZ8081_PHYCTRL2\n");
          return ret;
        }

      /* Indicate 50MHz clock */

      rk3576_eth_writemii(priv, phyaddr, MII_KSZ8081_PHYCTRL2,
                     (phydata | (1 << 7)));

      /* Switch off NAND Tree mode (in case it was set via pinning) */

      ret = rk3576_eth_readmii(priv, phyaddr, MII_KSZ8081_OMSO, &phydata);
      if (ret < 0)
        {
          nerr("ERROR: Failed to read MII_KSZ8081_OMSO: %d\n", ret);
          return ret;
        }

      rk3576_eth_writemii(priv, phyaddr, MII_KSZ8081_OMSO,
                     (phydata & ~(1 << 5)));

      /* Set Ethernet led to green = activity and yellow = link and  */

      ret = rk3576_eth_readmii(priv, phyaddr, MII_KSZ8081_PHYCTRL2, &phydata);
      if (ret < 0)
        {
          nerr("ERROR: Failed to read MII_KSZ8081_PHYCTRL2\n");
          return ret;
        }

      rk3576_eth_writemii(priv, phyaddr, MII_KSZ8081_PHYCTRL2,
                     (phydata | (1 << 4)));

      rk3576_eth_writemii(priv, phyaddr, MII_ADVERTISE,
                     MII_ADVERTISE_100BASETXFULL |
                     MII_ADVERTISE_100BASETXHALF |
                     MII_ADVERTISE_10BASETXFULL |
                     MII_ADVERTISE_10BASETXHALF |
                     MII_ADVERTISE_CSMA);

#elif defined (CONFIG_ETH0_PHY_LAN8720) || defined (CONFIG_ETH0_PHY_LAN8742A)
      /* Make sure that PHY comes up in correct mode when it's reset */

      rk3576_eth_writemii(priv, phyaddr, MII_LAN8720_MODES,
                     MII_LAN8720_MODES_RESV | MII_LAN8720_MODES_ALL |
                     MII_LAN8720_MODES_PHYAD(BOARD_PHY_ADDR));

      /* ...and reset PHY */

      rk3576_eth_writemii(priv, phyaddr, MII_MCR, MII_MCR_RESET);

#elif defined (CONFIG_ETH0_PHY_DP83825I)

      /* Reset PHY */

      rk3576_eth_writemii(priv, phyaddr, MII_MCR, MII_MCR_RESET);

      /* Set RMII mode and Indicate 50MHz clock */

      rk3576_eth_writemii(priv, phyaddr, MII_DP83825I_RCSR,
                    MII_DP83825I_RCSC_ELAST_2 | MII_DP83825I_RCSC_RMIICS);

      rk3576_eth_writemii(priv, phyaddr, MII_ADVERTISE,
                     MII_ADVERTISE_100BASETXFULL |
                     MII_ADVERTISE_100BASETXHALF |
                     MII_ADVERTISE_10BASETXFULL |
                     MII_ADVERTISE_10BASETXHALF |
                     MII_ADVERTISE_CSMA);

#endif
#if !defined(CONFIG_ETH0_PHY_TJA1103)

      /* Start auto negotiation */

      ninfo("%s: Start Autonegotiation...\n",  BOARD_PHY_NAME);
      rk3576_eth_writemii(priv, phyaddr, MII_MCR,
                     (MII_MCR_ANRESTART | MII_MCR_ANENABLE));

      /* Wait for auto negotiation to complete */

      for (retries = 0; retries < LINK_NLOOPS; retries++)
        {
          ret = rk3576_eth_readmii(priv, phyaddr, MII_MSR, &phydata);
          if (ret < 0)
            {
              nerr("ERROR: Failed to read %s MII_MSR: %d\n",
                    BOARD_PHY_NAME, ret);
              return ret;
            }

          if (phydata & MII_MSR_ANEGCOMPLETE)
            {
              break;
            }

          nxsig_usleep(LINK_WAITUS);
        }

      if (phydata & MII_MSR_ANEGCOMPLETE)
        {
          ninfo("%s: Autonegotiation complete\n",  BOARD_PHY_NAME);
          ninfo("%s: MII_MSR: %04x\n", BOARD_PHY_NAME, phydata);
        }
      else
        {
          /* TODO: Autonegotiation has right now failed. Maybe the Eth cable
           * is not connected.  PHY chip have mechanisms to configure link
           * OK. We should leave autconf on, and find a way to re-configure
           * MCU whenever the link is ready.
           */

          /* ★ 用 syslog 而不是 ninfo：没开网络调试时 ninfo 是空的，
           *   ifup 失败在串口上只剩一句 "ifup eth0...Failed"，看不出
           *   是没插网线、插错了口，还是 PHY 没应答。
           */

          syslog(LOG_WARNING, "ETH: 自协商 %d.%d 秒未完成（MSR=%04x），"
                 "网线插在 GMAC%d 这个口上吗？\n",
                 retries * LINK_WAITUS / 1000000,
                 retries * LINK_WAITUS / 100000 % 10, phydata,
                 CONFIG_RK3576_ETH_PORT);

          /* Stop auto negotiation */

          rk3576_eth_writemii(priv, phyaddr, MII_MCR, 0);
        }
#endif
    }

#if !defined(CONFIG_ETH0_PHY_TJA1103)
  /* When we get here we have a (negotiated) speed and duplex. This is also
   * the point we enter if renegotiation is turned off, so have multiple
   * attempts at reading the status register in case the PHY isn't awake
   * properly.
   */

  retries = 0;
  do
    {
      phydata = 0xffff;
#ifdef CONFIG_RK3576_ETH_PHY_MAE0621A
      /* 状态寄存器在分页空间：先选页，读完把页选回去 */

      {
        uint16_t oldpage = 0;

        rk3576_eth_readmii(priv, phyaddr, MAXIO_PAGE_SELECT, &oldpage);
        rk3576_eth_writemii(priv, phyaddr, MAXIO_PAGE_SELECT,
                            MAXIO_PHYSR_PAGE);
        ret = rk3576_eth_readmii(priv, phyaddr, MAXIO_PHYSR_REG, &phydata);
        rk3576_eth_writemii(priv, phyaddr, MAXIO_PAGE_SELECT, oldpage);
      }
#else
      ret = rk3576_eth_readmii(priv, phyaddr, BOARD_PHY_STATUS, &phydata);
#endif
    }
  while ((ret < 0 || phydata == 0xffff) && ++retries < 3);

  /* If we didn't successfully read anything and we haven't tried a physical
   * renegotiation then lets do that
   */

  if (retries >= 3)
    {
      if (renogphy == false)
        {
          /* Give things one more chance with renegotiation turned on */

          return rk3576_eth_initphy(priv, true);
        }
      else
        {
          /* That didn't end well, just give up */

          nerr("ERROR: Failed to read %s BOARD_PHY_STATUS[%02x]: %d\n",
               BOARD_PHY_NAME, BOARD_PHY_STATUS, ret);
          return ret;
        }
    }

  ninfo("%s: BOARD_PHY_STATUS: %04x\n", BOARD_PHY_NAME, phydata);
#endif

  /* Setup half or full duplex */

  if (BOARD_PHY_ISDUPLEX(phydata))
    {
      /* Full duplex */

      ninfo("%s: Full duplex\n",  BOARD_PHY_NAME);
      mac_conf |= EMAC_MAC_CONFIGURATION_DM;
    }
  else
    {
      /* Half duplex */

      ninfo("%s: Half duplex\n",  BOARD_PHY_NAME);
    }

  /* ★ 速率要同时落到两处：MAC_CONFIGURATION 的 PS/FES，以及 GRF 里
   *   RGMII 的 TX 时钟分频。只配一处的话链路是好的、数据不通。
   *
   *   PS  = 1 走 MII/RMII（10/100M），= 0 走 GMII（1000M）
   *   FES = 1 选 100M（仅在 PS=1 时有意义）
   *
   *   原驱动只做 10/100 且把 PS 写死在最后一条 putreg32 里 ——
   *   照抄过来千兆会不通。
   */

#ifdef CONFIG_RK3576_ETH_PHY_MAE0621A
  if (BOARD_PHY_1000BASET(phydata))
    {
      ninfo("%s: 1000 Base-T\n", BOARD_PHY_NAME);
      rk3576_gmac_set_speed(CONFIG_RK3576_ETH_PORT, 1000);
    }
  else
#endif
  if (BOARD_PHY_100BASET(phydata))
    {
      ninfo("%s: 100 Base-T\n",  BOARD_PHY_NAME);
      mac_conf |= EMAC_MAC_CONFIGURATION_FES | EMAC_MAC_CONFIGURATION_PS;
      rk3576_gmac_set_speed(CONFIG_RK3576_ETH_PORT, 100);
    }
  else if (BOARD_PHY_10BASET(phydata))
    {
      ninfo("%s: 10 Base-T\n",  BOARD_PHY_NAME);
      mac_conf |= EMAC_MAC_CONFIGURATION_PS;
      rk3576_gmac_set_speed(CONFIG_RK3576_ETH_PORT, 10);
    }
  else
    {
      syslog(LOG_ERR, "ETH: 速率未识别，自协商可能没完成: PHY STATUS=%04x"
             "（链路没起来，ifup 失败）\n", phydata);
      return -EIO;
    }

  putreg32(mac_conf | EMAC_MAC_CONFIGURATION_RE | EMAC_MAC_CONFIGURATION_TE,
           RK3576_EMAC_MAC_CONFIGURATION);
  return OK;
}

/****************************************************************************
 * Function: rk3576_eth_initmtl
 *
 * Description:
 *   Initialize ENET buffers and descriptors
 *
 * Input Parameters:
 *   priv - Reference to the private ENET driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

static void rk3576_eth_initmtl(struct rk3576_eth_driver_s *priv)
{
  uint32_t regval;

  /* EMAC_MTL_OPERATION_MODE */

  putreg32(EMAC_MTL_OPERATION_MODE_SCHALG_SP,
           RK3576_EMAC_MTL_OPERATION_MODE);

  putreg32(EMAC_MTL_TXQ0_OPERATION_MODE_FTQ |
           EMAC_MTL_TXQ0_OPERATION_MODE_TXQEN_DCB_GEN |
           EMAC_MTL_TXQ0_OPERATION_MODE_TSF |
           EMAC_MTL_TXQ0_OPERATION_MODE_TQS(2),
           RK3576_EMAC_MTL_TXQ0_OPERATION_MODE);

  regval  = getreg32(RK3576_EMAC_MTL_OPERATION_MODE);
  regval &= ~EMAC_MTL_OPERATION_MODE_RAA;
  putreg32(regval, RK3576_EMAC_MTL_OPERATION_MODE);

  putreg32(0, RK3576_EMAC_MTL_RXQ_DMA_MAP0);

  /* EMAC_MTL_RXQ0_OPERATION_MODE */

  putreg32(EMAC_MTL_RXQ0_OPERATION_MODE_RQS(2),
           RK3576_EMAC_MTL_RXQ0_OPERATION_MODE);

  putreg32(EMAC_MAC_RXQ_CTRL0_RXQ0EN_DCB_GEN,
           RK3576_EMAC_MAC_RXQ_CTRL0);
  putreg32(0, RK3576_EMAC_MAC_RXQ_CTRL2);
}

/****************************************************************************
 * Function: rk3576_eth_initbuffers
 *
 * Description:
 *   Initialize ENET buffers and descriptors
 *
 * Input Parameters:
 *   priv - Reference to the private ENET driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

static void rk3576_eth_initbuffers(struct rk3576_eth_driver_s *priv,
                                union rk3576_eth_desc_u *txtable,
                                union rk3576_eth_desc_u *rxtable,
                                uint8_t *rxbuffer)
{
  struct eth_desc_s *txdesc;
  struct eth_desc_s *rxdesc;
  int i;

  /* priv->txhead will point to the first, available TX descriptor in the
   * chain.  Set the priv->txhead pointer to the first descriptor in the
   * table.
   */

  priv->txhead = &txtable[0].desc;
  priv->txchbase = &txtable[0].desc;

  /* priv->txtail will point to the first segment of the oldest pending
   * "in-flight" TX transfer.  NULL means that there are no active TX
   * transfers.
   */

  priv->txtail   = NULL;
  priv->inflight = 0;

  /* Initialize each TX descriptor */

  for (i = 0; i < CONFIG_RK3576_ETH_NTXBUFFERS; i++)
    {
      txdesc = &txtable[i].desc;

#ifdef CHECKSUM_BY_HARDWARE
#if 0
      /* Enable the checksum insertion for the TX frames TODO! */

      txdesc->des0 |= ETH_TDES0_CIC_ALL;
#endif
#endif

      /* Clear Buffer1 address pointer (buffers will be assigned as they
       * are used)
       */

      txdesc->des0 = 0;

      /* Clear the rest of the descriptor as well */

      txdesc->des1 = 0;
      txdesc->des2 = 0;
      txdesc->des3 = 0;
    }

  /* Flush all of the initialized TX descriptors to physical memory */

  up_clean_dcache((uintptr_t)txtable,
                  (uintptr_t)txtable +
                  TXTABLE_SIZE * sizeof(union rk3576_eth_desc_u));

  /* Set Channel Tx descriptor ring length register
   * TODO: Why -1 is needed? Without this the ring doesn't wrap around
   * properly but the DMACCATXDR advances to outside the descriptor ring
   */

  putreg32(CONFIG_RK3576_ETH_NTXBUFFERS - 1,
           RK3576_EMAC_DMA_CH0_TXDESC_RING_LENGTH);

  /* Set Transmit Descriptor List Address Register */

  putreg32((uint32_t)(uintptr_t)&txtable[0].desc,
           RK3576_EMAC_DMA_CH0_TXDESC_LIST_ADDRESS);

  /* Set Transmit Descriptor Tail pointer */

  putreg32((uint32_t)(uintptr_t)&txtable[0].desc,
           RK3576_EMAC_DMA_CH0_TXDESC_TAIL_POINTER);

  /* priv->rxhead will point to the first,  RX descriptor in the chain.
   * This will be where we receive the first incomplete frame.
   */

  priv->rxhead = &rxtable[0].desc;
  priv->rxchbase = &rxtable[0].desc;

  /* Initialize each RX descriptor */

  for (i = 0; i < CONFIG_RK3576_ETH_NRXBUFFERS; i++)
    {
      rxdesc = &rxtable[i].desc;

      /* Set Buffer1 address pointer */

      rxdesc->des0 = (uint32_t)(uintptr_t)&rxbuffer[i * ALIGNED_BUFSIZE];

      /* Set Buffer1 address high bytes */

      rxdesc->des1 = 0;

      /* Set Buffer2 address high bytes */

      rxdesc->des2 = 0;

      /* Set Own bit of the RX descriptor des3 and buffer address valid */

      rxdesc->des3 = EMAC_RDES3_OWN_MASK | EMAC_RDES3_INTE_MASK |
                     EMAC_RDES3_BUF1V_MASK;
    }

  /* Flush all of the initialized RX descriptors to physical memory */

  up_clean_dcache((uintptr_t)rxtable,
                  (uintptr_t)rxtable +
                  RXTABLE_SIZE * sizeof(union rk3576_eth_desc_u));

  /* Set Receive Descriptor ring length register
   * TODO: Why -1 is needed? Without this the ring doesn't wrap around
   * properly but the DMACCARXDR advances to outside the descriptor ring
   */

  putreg32(CONFIG_RK3576_ETH_NRXBUFFERS - 1,
           RK3576_EMAC_DMA_CH0_RXDESC_RING_LENGTH);

  /* Set Receive Descriptor List Address Register */

  putreg32((uint32_t)(uintptr_t)&rxtable[0].desc,
           RK3576_EMAC_DMA_CH0_RXDESC_LIST_ADDRESS);

  /* Set Receive Descriptor Tail pointer Address */

  putreg32((uint32_t)(uintptr_t)&rxtable[CONFIG_RK3576_ETH_NRXBUFFERS - 1].desc,
           RK3576_EMAC_DMA_CH0_RXDESC_TAIL_POINTER);
}

/****************************************************************************
 * Function: rk3576_eth_initdma
 *
 * Description:
 *   Initialize EMAC DMA controllers
 *
 * Input Parameters:
 *   priv - Reference to the private EMAC driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

static void rk3576_eth_initdma(struct rk3576_eth_driver_s *priv)
{
  uint32_t regval;

  /* ★ 描述符步长必须与软件摆放的间距一致。
   *
   *   DSL（Descriptor Skip Length）告诉 DMA 两个描述符之间要跳过多少，
   *   单位是**总线位宽的字**：32 位 AXI 按 4 字节算，64 位按 8 字节算。
   *   描述符本身 16 字节，所以 DMA 看到的步长 = 16 + DSL * 字宽。
   *
   *   原驱动跑在 Cortex-M7 上，缓存行 32 字节，DESC_PADSIZE 因此是 32，
   *   配 32 位总线的 DSL(4)（跳 16）正好凑成 32。
   *
   *   移植到 arm64 后缓存行变成 64，DESC_PADSIZE 跟着变成 64，
   *   而 DSL 还是 4 —— **软件每 64 字节放一个描述符，DMA 每 32 字节读
   *   一个**。于是除第 0 个以外读到的全是填充区，OWN=0，收到第一帧就
   *   RBU（Receive Buffer Unavailable），接收 DMA 就此挂起：
   *   MMC 计数器显示 MAC 收到了帧、CRC 也没错，但一个中断都不产生。
   *
   *   这里按 64 位 AXI 计算：(64 - 16) / 8 = 6。
   *   若将来改了缓存行或总线位宽，两处必须同步 —— 判据是
   *   DMA_CH0_STATUS 的 RBU 位：一收到帧就置位就是步长对不上。
   */

  putreg32(EMAC_DMA_CH0_CONTROL_DSL((DESC_PADSIZE - DESC_SIZE) / 8),
           RK3576_EMAC_DMA_CH0_CONTROL);

  /* ★ 先清状态、再开中断，最后才启动收发。
   *
   *   原来的顺序是"启动 RX -> 开中断"。DMA_CH0_STATUS 的 RI 是粘滞位，
   *   而**拉中断线的是汇总位 NIS**，NIS 只在事件发生的那一刻、且对应
   *   使能位已经打开时才置。若首帧落在 SR 置位与 IE 写入之间，就会出现
   *   RI=1 而 NIS=0：状态寄存器上看得见收到了包，中断线却从未拉起。
   *
   *   实测正是这个现象：MMC 计数器显示 MAC 收到帧、CRC 零错误，
   *   DMA_CH0_STATUS=0x40（RI），但 ISR 一次都没进过。
   */

  /* ★ DMA_MODE 的 INTM（bits[17:16]）决定通道中断走哪条线：
   *   0 = 全部汇总到 macirq（sbd_intr）；非 0 = 每通道各走一条独立线。
   *   出处 dwmac4_dma.h 的 DMA_BUS_MODE_INTM_MASK，stmmac 只在
   *   multi_msi_en 时才置它。
   *
   *   实测扫描 GIC 的 pending 位图发现：流量过后挂起的是 IRQ 336
   *   （SPI 304），而 dtsi 给 gmac1 的 macirq 是 SPI 301（IRQ 333），
   *   且 SPI 304 在整份 dtsi 里没有任何设备占用 —— 正是"通道中断走了
   *   独立线"的样子。这里显式清零。
   */

  {
    uint32_t v = getreg32(RK3576_EMAC_DMA_MODE);

    v &= ~(3u << 16);
    putreg32(v, RK3576_EMAC_DMA_MODE);
    syslog(LOG_INFO, "ETH: DMA_MODE INTM 清零后=%08" PRIx32 "\n",
           getreg32(RK3576_EMAC_DMA_MODE));
  }

  putreg32(0xffffffff, RK3576_EMAC_DMA_CH0_STATUS);

  putreg32(EMAC_DMA_CH0_INTERRUPT_ENABLE_AIE |
           EMAC_DMA_CH0_INTERRUPT_ENABLE_NIE |
           EMAC_DMA_CH0_INTERRUPT_ENABLE_RIE |
           EMAC_DMA_CH0_INTERRUPT_ENABLE_TIE,
           RK3576_EMAC_DMA_CH0_INTERRUPT_ENABLE);

  /* Set EMAC_DMA_CH0_TX_CONTROL register */

  putreg32(EMAC_DMA_CH0_TX_CONTROL_TXPBL(32) | EMAC_DMA_CH0_TX_CONTROL_OSF,
           RK3576_EMAC_DMA_CH0_TX_CONTROL);

  /* DMA TX start transmitting */

  regval  = getreg32(RK3576_EMAC_DMA_CH0_TX_CONTROL);
  regval |= EMAC_DMA_CH0_TX_CONTROL_ST;
  putreg32(regval, RK3576_EMAC_DMA_CH0_TX_CONTROL);

  /* Set EMAC_DMA_CH0_RX_CONTROL register
   *
   * ★ RBSZ 占 bit 14:1，寄存器值 = 字节数 << 1（Linux stmmac 的
   *   dwmac4_set_bfsize 同样如此）。宏把参数左移 3 位，所以参数要给
   *   字节数 >> 2。原先给的是 PKTSIZE >> 2 = 379，即 1516 字节，装不下
   *   1514 字节的满长帧 + 4 字节 FCS：对端按 MSS 1460 发来的整段全被
   *   丢掉，表现为 TCP 上超过约 1450 字节的消息永远到不了（MQTT 大消息
   *   卡死时查出来的）。按实际分配的 DMA 缓冲大小设。
   */

  putreg32(EMAC_DMA_CH0_RX_CONTROL_RXPBL(32) |
           EMAC_DMA_CH0_RX_CONTROL_RBSZ_13_Y(ALIGNED_BUFSIZE >> 2),
           RK3576_EMAC_DMA_CH0_RX_CONTROL);

  /* DMA RX start receiving */

  regval  = getreg32(RK3576_EMAC_DMA_CH0_RX_CONTROL);
  regval |= EMAC_DMA_CH0_RX_CONTROL_SR;
  putreg32(regval, RK3576_EMAC_DMA_CH0_RX_CONTROL);

}

/****************************************************************************
 * Function: rk3576_eth_reset
 *
 * Description:
 *   Put the EMAC in the non-operational, reset state
 *
 * Input Parameters:
 *   priv - Reference to the private ENET driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

static uint32_t rk3576_eth_reset(struct rk3576_eth_driver_s *priv)
{
  const uint32_t timeout = 1000;
  uint32_t wait_ack;

  /* Reset emac using DMA SWR bit */

  putreg32(EMAC_DMA_MODE_SWR, RK3576_EMAC_DMA_MODE);

  for (wait_ack = 0; wait_ack < timeout; wait_ack++)
    {
      if ((getreg32(RK3576_EMAC_DMA_MODE) & EMAC_DMA_MODE_SWR) == 0)
        {
          return true;
        }

      up_udelay(10);
    }

  return false;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Function: rk3576_eth_netinitialize
 *
 * Description:
 *   Initialize the Ethernet controller and driver
 *
 * Input Parameters:
 *   intf - In the case where there are multiple EMACs, this value
 *          identifies which EMAC is to be initialized.
 *
 * Returned Value:
 *   OK on success; Negated errno on failure.
 *
 * Assumptions:
 *
 ****************************************************************************/

int rk3576_eth_netinitialize(int intf)
{
  struct rk3576_eth_driver_s *priv;
#ifdef CONFIG_NET_ETHERNET
  uint8_t *mac;
#endif
  int ret;

  /* Get the interface structure associated with this interface number. */

  DEBUGASSERT(intf < CONFIG_RK3576_ETH_NETHIFS);
  priv = &g_enet[intf];

  /* ★ 时钟、GRF、引脚复用、RGMII 延时线、PHY 的厂商初始化都不在这里。
   *
   *   它们由 rk3576_gmac_probe() 在板级初始化时完成，那一步同时验证
   *   PHY 应答、等自协商。本函数只负责 MAC/DMA 与网络设备注册 ——
   *   到这里硬件应当已经就绪。
   */

  /* 挂中断。RK3576 的 macirq：GMAC0 = GIC_SPI 293、GMAC1 = GIC_SPI 301，
   * 加 32 得到 IRQ 号（出处 rk3576.dtsi，与本端口其它外设同一换算）。
   */

  if (irq_attach(RK3576_IRQ_EMAC, rk3576_eth_enet_interrupt, NULL))
    {
      nerr("ERROR: 挂 GMAC 中断失败\n");
      return -EAGAIN;
    }

  /* 通道独立线也挂上，两条都指向同一个 ISR。
   * 清了 INTM 之后应当只有 macirq 会来；留着这条是为了在
   * INTM 不生效时仍能工作，ISR 会打印实际触发的号。
   */

  if (irq_attach(RK3576_IRQ_EMAC_CH0, rk3576_eth_enet_interrupt, NULL))
    {
      nerr("ERROR: 挂 GMAC 通道中断失败\n");
      irq_detach(RK3576_IRQ_EMAC);
      return -EAGAIN;
    }

  /* Initialize the driver structure */

  memset(priv, 0, sizeof(struct rk3576_eth_driver_s));
  priv->dev.d_ifup    = rk3576_eth_ifup;     /* I/F up (new IP address) callback */
  priv->dev.d_ifdown  = rk3576_eth_ifdown;   /* I/F down callback */
  priv->dev.d_txavail = rk3576_eth_txavail;  /* New TX data callback */
#ifdef CONFIG_NET_MCASTGROUP
  priv->dev.d_addmac  = rk3576_eth_addmac;   /* Add multicast MAC address */
  priv->dev.d_rmmac   = rk3576_eth_rmmac;    /* Remove multicast MAC address */
#endif
#ifdef CONFIG_NETDEV_IOCTL
  priv->dev.d_ioctl   = rk3576_eth_ioctl;    /* Support PHY ioctl() calls */
#endif
  priv->dev.d_private = g_enet;           /* Used to recover private state from dev */

#ifdef CONFIG_NET_ETHERNET
  /* ★ MAC 地址。
   *
   *   RK3576 没有 S32K 那样可直接读的芯片 UID，这里用一个本地管理的
   *   地址（第一个字节 bit1=1 表示 locally administered、bit0=0 表示
   *   单播），后三字节取芯片的 OTP/随机源会更好，当前先固定，
   *   同一网段接多块板时需要改。
   */

  mac    = priv->dev.d_mac.ether.ether_addr_octet;
  mac[0] = 0x02;
  mac[1] = 0x00;
  mac[2] = 0x35;
  mac[3] = 0x76;
  mac[4] = 0x00;
  mac[5] = CONFIG_RK3576_ETH_PORT + 1;
#endif

#ifdef CONFIG_RK3576_ETH_PHYINIT
  /* Perform any necessary, one-time, board-specific PHY initialization */

  ret = rk3576_eth_phy_boardinitialize(0);
  if (ret < 0)
    {
      nerr("ERROR: Failed to initialize the PHY: %d\n", ret);
      irq_detach(RK3576_IRQ_EMAC_CH0);
      irq_detach(RK3576_IRQ_EMAC);
      return ret;
    }
#endif

  /* Put the interface in the down state.  This usually amounts to resetting
   * the device and/or calling rk3576_eth_ifdown().
   */

  rk3576_eth_ifdown(&priv->dev);

  /* Register the device with the OS so that socket IOCTLs can be performed */

  ret = netdev_register(&priv->dev, NET_LL_ETHERNET);
  if (ret < 0)
    {
      irq_detach(RK3576_IRQ_EMAC_CH0);
      irq_detach(RK3576_IRQ_EMAC);
    }

  return ret;
}

/****************************************************************************
 * Name: arm_netinitialize
 *
 * Description:
 *   Initialize the first network interface.  If there are more than one
 *   interface in the chip, then board-specific logic will have to provide
 *   this function to determine which, if any, Ethernet controllers should
 *   be initialized.
 *
 ****************************************************************************/

#if CONFIG_RK3576_ETH_NETHIFS == 1 && !defined(CONFIG_NETDEV_LATEINIT)
void arm64_netinitialize(void)
{
  rk3576_eth_netinitialize(0);
}
#endif

#endif /* CONFIG_RK3576_ETH */
