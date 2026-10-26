/* SPDX-License-Identifier: Apache-2.0 */

/* Host regression harness: actual driver functions, simulated registers. */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include <time.h>
#include <syslog.h>

#include "hardware/rk3576_dwmmc.h"

#define FAR
#define OK 0
#define UNUSED(x) (void)(x)
#define CONFIG_SDIO_BLOCKSETUP 1
#define RK3576_DWMMC_SD_BASE 0x2a310000
#define RK3576_DWMMC_SDIO_BASE 0x2a320000
#define SDIO_STATUS_PRESENT 1
#define SDIO_CAPS_DMABEFOREWRITE 1
#define SDIO_CAPS_DMASUPPORTED 2
#define MMCSD_CMDIDX_MASK 0x3f
#define MMCSD_RESPONSE_MASK 0xf00
#define MMCSD_NO_RESPONSE 0
#define MMCSD_R2_RESPONSE 0x200
#define MMCSD_R3_RESPONSE 0x300
#define MMCSD_R4_RESPONSE 0x400
#define MMCSD_DATAXFR_MASK 0x3000
#define MMCSD_NODATAXFR 0
#define MMCSD_WRXFR 0x2000
#define MMC_CMD1 1
#define MMCSD_CMD0 0
#define SDIO_CMD5 (5 | MMCSD_R4_RESPONSE)
#define SD_CMD3 3
#define MMCSD_CMD7S 7
#define SD_ACMD52 52
#define SD_ACMD52ABRT (52 | 0x4000)
#define SD_ACMD53RD (53 | 0x1000)
#define SD_ACMD53WR (53 | MMCSD_WRXFR)
#define SDIOWAIT_TRANSFERDONE 4
#define SDIOWAIT_TIMEOUT 8
#define SDIOWAIT_ERROR 16
#define SDIO_CCCR_BUS_IF 7
#define SDIO_CCCR_BUS_IF_WIDTH_MASK 3
#define SDIO_CCCR_BUS_IF_4_BITS 2
#define SDIO_FBR_SHIFT 8
#define SDIO_CCCR_FN0_BLKSIZE_0 0x10
#define SDIO_CCCR_FN0_BLKSIZE_1 0x11
#define SDIO_CCCR_IOEN 2
#define SDIO_CCCR_IORDY 3
#define SDIO_CCCR_INTEN 4
#define MSEC2TICK(x) (x)
#define begin_packed_struct
#define end_packed_struct __attribute__((packed))
#define wlerr(...) ((void)0)
#define wlinfo(...) ((void)0)
#define syslog(...) ((void)0)

typedef unsigned int sdio_eventset_t;
typedef unsigned int sdio_statset_t;
typedef unsigned int sdio_capset_t;

struct sdio_dev_s
{
  int mutex;
  int (*sendcmd)(struct sdio_dev_s *, uint32_t, uint32_t);
  int (*waitresponse)(struct sdio_dev_s *, uint32_t);
  int (*recvshort)(struct sdio_dev_s *, uint32_t, uint32_t *);
  void (*blocksetup)(struct sdio_dev_s *, unsigned int, unsigned int);
  void (*waitenable)(struct sdio_dev_s *, sdio_eventset_t, uint32_t);
  int (*sendsetup)(struct sdio_dev_s *, const uint8_t *, size_t);
  int (*recvsetup)(struct sdio_dev_s *, uint8_t *, size_t);
  sdio_eventset_t (*eventwait)(struct sdio_dev_s *);
  int (*cancel)(struct sdio_dev_s *);
  sdio_capset_t caps;
};

#define SDIO_SENDCMD(d,c,a) ((d)->sendcmd(d,c,a))
#define SDIO_WAITRESPONSE(d,c) ((d)->waitresponse(d,c))
#define SDIO_RECVR5(d,c,r) ((d)->recvshort(d,c,r))
#define SDIO_RECVR1 SDIO_RECVR5
#define SDIO_RECVR4 SDIO_RECVR5
#define SDIO_RECVR6 SDIO_RECVR5
#define SDIO_BLOCKSETUP(d,b,n) ((d)->blocksetup(d,b,n))
#define SDIO_WAITENABLE(d,e,t) ((d)->waitenable(d,e,t))
#define SDIO_SENDSETUP(d,b,n) ((d)->sendsetup(d,b,n))
#define SDIO_RECVSETUP(d,b,n) ((d)->recvsetup(d,b,n))
#define SDIO_EVENTWAIT(d) ((d)->eventwait(d))
#define SDIO_CANCEL(d) ((d)->cancel(d))
#define SDIO_CAPABILITIES(d) ((d)->caps)
#define SDIO_WIDEBUS(d,w) ((void)0)
#ifdef CONFIG_SDIO_DMA
#  define SDIO_DMASENDSETUP(d,b,n) mock_dma_setup(d, true, b, n)
#  define SDIO_DMARECVSETUP(d,b,n) mock_dma_setup(d, false, b, n)
#else
static int __attribute__((unused)) mock_no_dma(void) { return -ENOSYS; }
#  define SDIO_DMASENDSETUP(d,b,n) mock_no_dma()
#  define SDIO_DMARECVSETUP(d,b,n) mock_no_dma()
#endif

static uint32_t g_regs[256];
static uint8_t g_wire[1024];
static size_t g_fifo_bytes;
static uint32_t g_r5;
static uint32_t g_data_arg;
static uint8_t g_latch[4];
static uint32_t g_cmd_error;
static uint32_t g_late_error;
static int g_lock_error;
static int g_lock_depth;
static int g_setup_error;
static int g_send_error;
static int g_fifo_resets;
static int g_dma_calls;
static int g_cru[2][4];
static int g_cru_count;
static bool g_stall;
static clock_t g_ticks;
static bool g_enum;
static unsigned int g_cmd5_count;
static unsigned int g_ready_after;
static unsigned int g_cmd3_count;
static uint32_t g_ocr;
static bool g_poll_error;

static uint32_t dw_getreg(uint32_t base, uint32_t reg)
{
  uint32_t word;
  if (reg == DWMMC_DATA_240A)
    {
      memcpy(&word, g_wire + g_fifo_bytes, 4);
      g_fifo_bytes += 4;
      if (g_fifo_bytes >= g_regs[DWMMC_BYTCNT / 4])
        {
          g_regs[DWMMC_RINTSTS / 4] |= DWMMC_INT_DATA_OVER | g_late_error;
        }

      return word;
    }

  if (reg == DWMMC_STATUS)
    {
      return g_stall ? DWMMC_STATUS_FIFO_FULL : (1 << 17);
    }

  return g_regs[reg / 4];
}

static void dw_putreg(uint32_t base, uint32_t reg, uint32_t value)
{
  if (reg == DWMMC_RINTSTS)
    {
      g_regs[reg / 4] &= ~value;
    }
  else if (reg == DWMMC_CMD)
    {
      uint32_t arg = g_regs[DWMMC_CMDARG / 4];
      uint32_t address = (arg >> 9) & 0x1ffff;
      if ((value & 63) == 53) g_data_arg = arg;
      if ((value & 63) == 52 && (arg & (1u << 31)) &&
          address >= 0x15c && address <= 0x15f)
        g_latch[address - 0x15c] = arg & 0xff;
      g_regs[reg / 4] = value & ~DWMMC_CMD_START;
      g_regs[DWMMC_RINTSTS / 4] |= DWMMC_INT_CMD_DONE | g_cmd_error;
      g_regs[DWMMC_RESP0 / 4] = (value & 63) == 53 ? g_r5 : 0;
      if (g_enum && (value & 63) == 5)
        {
          g_cmd5_count++;
          g_regs[DWMMC_RESP0 / 4] = g_ocr;
          if (g_cmd5_count >= g_ready_after)
            g_regs[DWMMC_RESP0 / 4] |= 1u << 31;
          if (g_poll_error && g_cmd5_count > 1)
            g_regs[DWMMC_RINTSTS / 4] |= DWMMC_INT_RCRC;
          if (g_cmd5_count > 1)
            assert(arg == (g_ocr & 0x00ffffff));
        }
      if (g_enum && (value & 63) == 3)
        {
          assert(g_cmd5_count >= g_ready_after);
          g_cmd3_count++;
          g_regs[DWMMC_RESP0 / 4] = 0x10000;
        }
    }
  else if (reg == DWMMC_DATA_240A)
    {
      memcpy(g_wire + g_fifo_bytes, &value, 4);
      g_fifo_bytes += 4;
      if (g_fifo_bytes >= g_regs[DWMMC_BYTCNT / 4])
        {
          g_regs[DWMMC_RINTSTS / 4] |= DWMMC_INT_DATA_OVER | g_late_error;
        }
    }
  else
    {
      g_regs[reg / 4] = value;
    }
}

static int dw_update_clk(uint32_t base) { return OK; }
static void dw_fifo_reset(uint32_t base) { g_fifo_resets++; }
static void dw_wait_while_busy(uint32_t base) { }
static void up_udelay(unsigned int us) { g_ticks++; }
static void up_mdelay(unsigned int ms) { g_ticks += ms; }
static int nxsig_usleep(unsigned int us)
{ g_ticks += (us + 999) / 1000; return OK; }
static clock_t clock_systime_ticks(void) { return g_ticks; }
static bool up_interrupt_context(void) { return false; }
static bool sched_idletask(void) { return false; }
static void nxmutex_init(int *mutex) { *mutex = 0; }
static int nxmutex_lock(int *mutex)
{
  if (g_lock_error) return g_lock_error;
  assert(g_lock_depth++ == 0);
  return OK;
}
static void nxmutex_unlock(int *mutex) { assert(--g_lock_depth == 0); }
static void rk3576_clk_setmux(int con, int shift, int width, int value)
{
  assert(g_cru_count < 2);
  g_cru[g_cru_count][0] = con;
  g_cru[g_cru_count][1] = shift;
  g_cru[g_cru_count][2] = width;
  g_cru[g_cru_count++][3] = value;
}

#include "host.inc"

static int mock_send(struct sdio_dev_s *dev, uint32_t cmd, uint32_t arg)
{
  return g_send_error ? g_send_error : rk3576_dwmmc_sendcmd(dev, cmd, arg);
}
static int mock_recvsetup(struct sdio_dev_s *dev, uint8_t *buf, size_t n)
{
  return g_setup_error ? g_setup_error : rk3576_dwmmc_recvsetup(dev, buf, n);
}
static int mock_sendsetup(struct sdio_dev_s *dev, const uint8_t *buf, size_t n)
{
  return g_setup_error ? g_setup_error : rk3576_dwmmc_sendsetup(dev, buf, n);
}
#ifdef CONFIG_SDIO_DMA
static int mock_dma_setup(struct sdio_dev_s *dev, bool write,
                          uint8_t *buf, size_t n)
{
  g_dma_calls++;
  return write ? mock_sendsetup(dev, buf, n) : mock_recvsetup(dev, buf, n);
}
#endif
int sdio_set_wide_bus(struct sdio_dev_s *dev);
#include "sdio.inc"
#include "skw.inc"

static struct rk3576_dwmmc_dev_s host(void)
{
  struct rk3576_dwmmc_dev_s p = {0};
  memset(g_regs, 0, sizeof(g_regs));
  memset(g_wire, 0xa5, sizeof(g_wire));
  g_fifo_bytes = 0;
  g_data_arg = 0;
  memset(g_latch, 0, sizeof(g_latch));
  g_r5 = g_cmd_error = g_late_error = 0;
  g_lock_error = g_lock_depth = g_setup_error = g_send_error = 0;
  g_fifo_resets = g_dma_calls = g_cru_count = 0;
  g_stall = false;
  g_ticks = 0;
  g_enum = false;
  g_cmd5_count = g_cmd3_count = 0;
  g_ready_after = 3;
  g_ocr = (1u << 28) | 0x00300000;
  g_poll_error = false;
  p.base = RK3576_DWMMC_SDIO_BASE;
  p.fifo_off = DWMMC_DATA_240A;
  p.dev.sendcmd = mock_send;
  p.dev.waitresponse = rk3576_dwmmc_waitresponse;
  p.dev.recvshort = rk3576_dwmmc_recvshort;
  p.dev.blocksetup = rk3576_dwmmc_blocksetup;
  p.dev.waitenable = rk3576_dwmmc_waitenable;
  p.dev.sendsetup = mock_sendsetup;
  p.dev.recvsetup = mock_recvsetup;
  p.dev.eventwait = rk3576_dwmmc_eventwait;
  p.dev.cancel = rk3576_dwmmc_cancel;
  p.dev.caps = SDIO_CAPS_DMABEFOREWRITE;
  return p;
}

static int transfer(struct rk3576_dwmmc_dev_s *p, bool write,
                    uint8_t *buffer, unsigned int length)
{
  int ret = sdio_io_rw_extended(&p->dev, write, 1, 15, false,
                                buffer, length, 0);
  assert(g_lock_depth == 0);
  return ret;
}

int main(void)
{
  uint8_t buffer[514];
  struct rk3576_dwmmc_dev_s p;
  const unsigned int lengths[] = {1, 2, 3, 4, 5, 15, 16, 255, 511, 512};
  const uint32_t errors[] = {1 << 8, 1 << 9, 1 << 11, 1 << 14, 1 << 15};
  const int results[] = {-EINVAL, -EINVAL, -EIO, -EIO, -EIO};
  size_t i;
  unsigned int j;
  int write;

  assert(dwmmc_hw(RK3576_DWMMC_SDIO_BASE) != NULL);
  assert(clock_systime_ticks() == 0);

  for (write = 0; write <= 1; write++)
    for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++)
      {
        p = host();
        memset(buffer, 0x77, sizeof(buffer));
        if (write) memset(buffer + 1, 0xa5, lengths[i]);
        assert(transfer(&p, write, buffer + 1, lengths[i]) == OK);
        assert(g_fifo_bytes == ((lengths[i] + 3) / 4) * 4);
        assert(buffer[0] == 0x77 && buffer[lengths[i] + 1] == 0x77);
        for (j = 0; j < lengths[i]; j++)
          assert((write ? g_wire[j] : buffer[j + 1]) == 0xa5);
        if (write)
          for (j = lengths[i]; j < g_fifo_bytes; j++) assert(g_wire[j] == 0);
        assert(g_dma_calls == 0);
      }

  for (i = 0; i < sizeof(errors) / sizeof(errors[0]); i++)
    {
      p = host();
      g_r5 = errors[i];
      assert(transfer(&p, false, buffer, 16) == results[i]);
    }

  p = host();
  g_late_error = DWMMC_INT_DCRC;
  assert(transfer(&p, false, buffer, 16) == -EIO);
  p = host();
  g_late_error = DWMMC_INT_DRTO;
  assert(transfer(&p, true, buffer, 16) == -ETIMEDOUT);
  p = host();
  g_cmd_error = DWMMC_INT_RTO;
  assert(transfer(&p, false, buffer, 16) == -ETIMEDOUT);
  assert(g_fifo_bytes == 0);
  p = host();
  g_stall = true;
  assert(transfer(&p, false, buffer, 16) == -ETIMEDOUT);
  assert(g_ticks == 1000);
  p = host();
  g_lock_error = -EINTR;
  assert(transfer(&p, false, buffer, 16) == -EINTR);
  assert(g_fifo_resets == 0);
  p = host();
  g_setup_error = -ENOMEM;
  assert(transfer(&p, true, buffer, 16) == -ENOMEM);
  assert(g_fifo_bytes == 0 && p.buffer == NULL);
  p = host();
  g_send_error = -EIO;
  assert(transfer(&p, false, buffer, 16) == -EIO);
  assert(g_fifo_bytes == 0 && p.buffer == NULL);

  /* Preserve direct-command failure even if stale RECVR5 could succeed. */

  p = host();
  p.last_result = OK;
  g_send_error = -EIO;
  assert(sdio_io_rw_direct(&p.dev, false, 0, 0, 0, buffer) == -EIO);
  assert(g_lock_depth == 0);
  p = host();
  g_lock_error = -EINTR;
  assert(sdio_io_rw_direct(&p.dev, false, 0, 0, 0, buffer) == -EINTR);
  assert(g_lock_depth == 0);

  /* DTO may be set before the final FIFO word is drained. */

  p = host();
  p.buffer = buffer;
  p.remaining = 5;
  p.waittimeout_ms = 1000;
  g_regs[DWMMC_BYTCNT / 4] = 5;
  g_regs[DWMMC_RINTSTS / 4] = DWMMC_INT_DATA_OVER;
  assert(dw_pio_transfer(&p) == OK);
  assert(g_fifo_bytes == 8);

  /* The ordinary SD path has already consumed CMD_DONE before EVENTWAIT. */

  p = host();
  p.base = RK3576_DWMMC_SD_BASE;
  rk3576_dwmmc_blocksetup(&p.dev, 512, 1);
  rk3576_dwmmc_recvsetup(&p.dev, buffer, 512);
  rk3576_dwmmc_waitenable(&p.dev, SDIOWAIT_TRANSFERDONE, 1000);
  assert(rk3576_dwmmc_sendcmd(&p.dev, SD_ACMD53RD, 0) == OK);
  assert(rk3576_dwmmc_waitresponse(&p.dev, SD_ACMD53RD) == OK);
  assert(rk3576_dwmmc_eventwait(&p.dev) == SDIOWAIT_TRANSFERDONE);
  assert(g_fifo_bytes == 512);

  for (i = 0; i < 2; i++)
    {
      p = host();
      p.base = i ? RK3576_DWMMC_SD_BASE : RK3576_DWMMC_SDIO_BASE;
      dw_set_clock_hz(&p, 400000);
      assert(g_cru_count == 2);
      assert(g_cru[0][0] == (i ? 105 : 104));
      assert(g_cru[0][1] == (i ? 13 : 6));
      assert(g_cru[1][1] == (i ? 7 : 0));
      assert(g_cru[1][3] == (i ? 59 : 29));
      assert(p.card_hz == 400000);
      g_regs[DWMMC_CDETECT / 4] = 1;
      assert(rk3576_dwmmc_status(&p.dev) == (i ? 0 : SDIO_STATUS_PRESENT));
    }

  /* SDIO's 50 MHz source request rounds down to 49.5 MHz / 2. */

  p = host();
  dw_set_clock_hz(&p, 25000000);
  assert(g_cru[0][0] == 104);
  assert(g_cru[1][3] == 23);
  assert(p.card_hz == 24750000);
  assert(g_regs[DWMMC_CLKDIV / 4] == 0);
  dw_set_clock_hz(&p, 0);
  assert(p.card_hz == 0);
  assert(g_regs[DWMMC_CLKENA / 4] == 0);

#ifdef CONFIG_SDIO_DMA
  p = host();
  p.dev.caps |= SDIO_CAPS_DMASUPPORTED;
  assert(transfer(&p, true, buffer, 16) == OK);
  assert(g_dma_calls == 1);
#endif

  p = host();
  memcpy(g_wire, "SV6160LITE", 10);
  assert(skw_read_identity(&p.dev, buffer) == OK);
  assert(memcmp(buffer, "SV6160LITE", 10) == 0);
  assert(g_latch[0] == 0 && g_latch[1] == 0 &&
         g_latch[2] == 0 && g_latch[3] == 0x40);
  assert((g_data_arg & (1u << 26)) != 0);
  assert(((g_data_arg >> 9) & 0x1ffff) == 0x0f);
  assert(((g_data_arg >> 28) & 7) == 1);
  assert((g_data_arg & 511) == 16);

  p = host();
  g_enum = true;
  assert(sdio_probe(&p.dev) == OK);
  assert(g_cmd5_count == 3 && g_cmd3_count == 1 && g_lock_depth == 0);
  p = host();
  g_enum = true;
  g_ready_after = 10000;
  assert(sdio_probe(&p.dev) == -ETIMEDOUT);
  assert(g_ticks == 1050 && g_cmd3_count == 0 && g_lock_depth == 0);
  p = host();
  g_enum = true;
  g_ocr = 1u << 28;
  assert(sdio_probe(&p.dev) == -ENODEV);
  assert(g_cmd3_count == 0 && g_lock_depth == 0);
  p = host();
  g_enum = true;
  g_ocr = 0x00300000;
  assert(sdio_probe(&p.dev) == -ENODEV);
  assert(g_cmd3_count == 0 && g_lock_depth == 0);
  p = host();
  g_enum = true;
  g_poll_error = true;
  assert(sdio_probe(&p.dev) == -EIO);
  assert(g_cmd3_count == 0 && g_lock_depth == 0);

  p = host();
  assert(sdio_io_rw_direct(&p.dev, false, 8, 0, 0, buffer) == -EINVAL);
  assert(sdio_io_rw_direct(&p.dev, false, 0, 0x20000, 0, buffer) == -EINVAL);
  assert(sdio_io_rw_direct(NULL, false, 0, 0, 0, buffer) == -EINVAL);
  assert(transfer(&p, false, buffer, 0) == -EINVAL);
  assert(transfer(&p, false, buffer, 513) == -EINVAL);
  assert(transfer(&p, false, NULL, 16) == -EINVAL);
  assert(sdio_io_rw_extended(&p.dev, false, 1, 15, false,
                             buffer, 512, 512) == -EINVAL);
  assert(sdio_io_rw_extended(&p.dev, false, 1, 0x1ffff, true,
                             buffer, 2, 0) == -EINVAL);
  assert(g_fifo_resets == 0 && g_lock_depth == 0);

  puts("PASS: CMD53 PIO, tails, R5, failures, deadline, SD and clock isolation");
  return 0;
}
