/* SPDX-License-Identifier: Apache-2.0 */

/****************************************************************************
 * SKW6621S first firmware boot over SDIO (SDIO v2 / SV6160LITE).
 *
 * Wire format follows the SDK seekwaveplatform_lite driver, see
 * docs/skw6621s-boot-protocol.md:
 *   skw_sdio_boot_cp      0x165 / 0x167, DRAM then IRAM, 0x160 = 1
 *   skw_sdio_dt_write     latch address at F0 0x15c, CMD53 to F1 0x0f
 *   skw_sdio_rx_thread    SDMA read from F1 0x20, nsize in the last word
 *   skw_sdio2_handle_packet  LOOPCHECK: BSPREADY -> RDVERSION -> trunk_W
 *
 * The RX side polls CCCR INT pending instead of taking the SDIO interrupt;
 * this file only proves the CP boots, it is not the runtime transport.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <sys/stat.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/fs/fs.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/sdio.h>

#include "skw6621s_sdio.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SKW_FUNCTION            1
#define SKW_MAX_IMAGE           (1024 * 1024)

/* F0 vendor registers (skw_boot.h) */

#define SKW_ADDR_LATCH          0x15c
#define SKW_DL_DONE             0x160    /* first boot: write BIT(0) */
#define SKW_DMA_TYPE            0x165    /* SDK always writes ADMA here */
#define SKW_SLP_SWITCH          0x167    /* 1: CP sleep disabled */
#define SKW_DMA_TYPE_ADMA       1

#define SKW_CCCR_INTEN          0x04
#define SKW_CCCR_INTPEND        0x05
#define SKW_SDIO_INT_EXT        0x16     /* bit1: async interrupt */

/* F1 windows */

#define SKW_DT_WINDOW           0x0f     /* direct memory, incrementing */
#define SKW_PK_WINDOW           0x20     /* packet FIFO, fixed address */

/* SDIO v2 packets */

#define SKW_PAC_SIZE            0x600
#define SKW_BLK_SIZE            512
#define SKW_RX_MAX_PAC          8
#define SKW_RX_BUF_SIZE         (SKW_PAC_SIZE * SKW_RX_MAX_PAC + SKW_BLK_SIZE)
#define SKW_LOOPCHECK_CH        1
#define SKW_BT_CMD_CH           2        /* SDIO2_BT_CMD_PORT */
#define SKW_BT_DATA_CH          5        /* SDIO2_BT_DATA_PORT */
#define SKW_AP2CP_IRQ           0x1b0    /* service start/stop bits */
#define SKW_BT_START_BIT        0x04     /* 1 << ((BT_SERVICE << 1) | START) */

#define SKW_FLAG_VERSION        (1 << 0)
#define SKW_FLAG_BTREADY        (1 << 1)
#define SKW_FLAG_ASSERT         (1 << 2)

#define SKW_BOOT_TIMEOUT_MS     3000
#define SKW_POLL_MS             5

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct skw_image_s
{
  FAR uint8_t *data;
  size_t size;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int skw_load(FAR const char *dir, FAR const char *name,
                    FAR struct skw_image_s *image)
{
  struct file filep;
  char path[96];
  struct stat st;
  ssize_t nread;
  size_t done = 0;
  int ret;

  snprintf(path, sizeof(path), "%s/%s", dir, name);
  ret = file_open(&filep, path, O_RDONLY);
  if (ret < 0)
    {
      syslog(LOG_ERR, "SKW6621S: open %s: %d\n", path, ret);
      return ret;
    }

  ret = file_fstat(&filep, &st);
  if (ret < 0 || st.st_size <= 0 || st.st_size > SKW_MAX_IMAGE)
    {
      file_close(&filep);
      return ret < 0 ? ret : -EFBIG;
    }

  image->size = st.st_size;
  image->data = kmm_malloc(image->size);
  if (image->data == NULL)
    {
      file_close(&filep);
      return -ENOMEM;
    }

  while (done < image->size)
    {
      nread = file_read(&filep, image->data + done, image->size - done);
      if (nread <= 0)
        {
          break;
        }

      done += nread;
    }

  file_close(&filep);
  if (done != image->size)
    {
      kmm_free(image->data);
      image->data = NULL;
      return -EIO;
    }

  syslog(LOG_INFO, "SKW6621S: %s %zu bytes\n", name, image->size);
  return OK;
}

static uint32_t skw_le32(FAR const uint8_t *p)
{
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
         (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* One 4-byte aligned tag in the first 512 bytes, like the inspector */

static int skw_marker(FAR const struct skw_image_s *img,
                      FAR const char *tag)
{
  size_t len = strlen(tag);
  size_t end = img->size < 512 ? img->size : 512;
  int found = -1;
  size_t pos;

  for (pos = 0; pos + len <= end; pos += 4)
    {
      if (memcmp(img->data + pos, tag, len) == 0)
        {
          if (found >= 0)
            {
              return -EINVAL;
            }

          found = pos;
        }
    }

  return found >= 0 ? found : -ENOENT;
}

/* Read the load addresses and merge the NV CMFG section into IRAM
 * (scripts/skw-firmware-inspect.py prepare()).
 */

static int skw_prepare(FAR struct skw_image_s *iram,
                       FAR const struct skw_image_s *nv,
                       FAR uint32_t *iram_addr, FAR uint32_t *dram_addr)
{
  uint32_t cmfg_off;
  uint32_t cmfg_len;
  int head;
  int tsvn;
  int devn;

  head = skw_marker(iram, "kees0616");
  tsvn = skw_marker(iram, "TSVN");
  devn = skw_marker(iram, "DEVN");
  if (head < 0 || tsvn < 0 || devn < 0 || tsvn + 4 >= devn ||
      nv->size < 32)
    {
      return -EINVAL;
    }

  *iram_addr = skw_le32(iram->data + head + 8);
  *dram_addr = skw_le32(iram->data + head + 12);

  cmfg_off = skw_le32(nv->data + 8);
  cmfg_len = skw_le32(nv->data + 12);
  if (cmfg_off < 32 || cmfg_len == 0 || cmfg_off > nv->size ||
      cmfg_len > nv->size - cmfg_off ||
      cmfg_len > (uint32_t)(devn - tsvn - 4))
    {
      return -EINVAL;
    }

  memcpy(iram->data + tsvn + 4, nv->data + cmfg_off, cmfg_len);
  syslog(LOG_INFO, "SKW6621S: IRAM@0x%08" PRIx32 " DRAM@0x%08" PRIx32
         " CMFG %" PRIu32 " bytes -> IRAM+0x%x\n",
         *iram_addr, *dram_addr, cmfg_len, tsvn + 4);
  return OK;
}

static int skw_f0_write(FAR struct sdio_dev_s *sdio, uint32_t addr,
                        uint8_t val)
{
  return sdio_io_rw_direct(sdio, true, 0, addr, val, NULL);
}

static int skw_f0_read(FAR struct sdio_dev_s *sdio, uint32_t addr,
                       FAR uint8_t *val)
{
  return sdio_io_rw_direct(sdio, false, 0, addr, 0, val);
}

/* skw_sdio_dt_write: latch once, then 512-byte CMD53s that the chip
 * advances internally.  The tail is a byte-mode CMD53.
 */

static int skw_dt_write(FAR struct sdio_dev_s *sdio, uint32_t addr,
                        FAR uint8_t *buf, size_t len)
{
  size_t done = 0;
  size_t n;
  int ret;
  int i;

  for (i = 0; i < 4; i++)
    {
      ret = skw_f0_write(sdio, SKW_ADDR_LATCH + i, (addr >> (8 * i)) & 0xff);
      if (ret < 0)
        {
          return ret;
        }
    }

  while (done < len)
    {
      n = len - done < SKW_BLK_SIZE ? len - done : SKW_BLK_SIZE;
      ret = sdio_io_rw_extended(sdio, true, SKW_FUNCTION, SKW_DT_WINDOW,
                                true, buf + done, n, 0);
      if (ret < 0)
        {
          syslog(LOG_ERR, "SKW6621S: dt write 0x%08" PRIx32 " +%zu: %d\n",
                 addr, done, ret);
          return ret;
        }

      done += n;
    }

  return OK;
}

/* setup_sdio2_packet + skw_sdio_sdma_write */

static int skw_send(FAR struct sdio_dev_s *sdio, uint8_t channel,
                    FAR const char *msg, size_t size)
{
  uint8_t pkt[SKW_BLK_SIZE];
  uint32_t hdr;
  size_t count = (size + 3) & ~3;

  if (count + 8 > sizeof(pkt))
    {
      return -E2BIG;
    }

  memset(pkt, 0, sizeof(pkt));
  hdr = (uint32_t)count | (uint32_t)channel << 24;
  memcpy(pkt, &hdr, 4);
  memcpy(pkt + 4, msg, size);
  hdr = 1u << 23;                                  /* eof */
  memcpy(pkt + 4 + count, &hdr, 4);

  return sdio_io_rw_extended(sdio, true, SKW_FUNCTION, SKW_PK_WINDOW,
                             false, pkt, SKW_BLK_SIZE, 0);
}


/* Driver state kept after boot: the RX buffer and flags are reused by the
 * Bluetooth transport.  All bus access goes through lock.
 */

struct skw_ctx_s
{
  FAR struct sdio_dev_s *sdio;
  FAR uint8_t *rxbuf;
  unsigned int npac;
  uint32_t flags;
  uint32_t acks;
  skw6621s_hci_cb_t cb;
  FAR void *cbarg;
  mutex_t lock;
};

static struct skw_ctx_s g_skw =
{
  .npac = 1,
  .lock = NXMUTEX_INITIALIZER,
};

/* skw_sdio2_sdma_parser + skw_sdio2_handle_packet.  Every non-ack packet
 * carries a 12-byte sub-header after the 4-byte header; the payload is at
 * +16 and is len - 12 bytes.  A 12-byte packet is the CP's TX ack.
 */

static void skw_parse(FAR struct skw_ctx_s *ctx, unsigned int npac)
{
  unsigned int i;

  for (i = 0; i < npac; i++)
    {
      FAR uint8_t *p = ctx->rxbuf + i * SKW_PAC_SIZE;
      uint32_t hdr = skw_le32(p);
      unsigned int len = hdr & 0xffff;
      unsigned int ch = hdr >> 24;
      FAR const uint8_t *pl = p + 16;
      size_t plen;

      if ((hdr >> 23) & 1)
        {
          break;
        }

      if (len < 12 || len > SKW_PAC_SIZE - 4)
        {
          syslog(LOG_WARNING, "SKW6621S: rx bad header 0x%08" PRIx32 "\n",
                 hdr);
          break;
        }

      if (len == 12)
        {
          ctx->acks++;
          continue;
        }

      plen = len - 12;

      if (ch == SKW_LOOPCHECK_CH)
        {
          char cmd[96];
          size_t n = plen < sizeof(cmd) - 1 ? plen : sizeof(cmd) - 1;

          memcpy(cmd, pl, n);
          cmd[n] = '\0';
          syslog(LOG_INFO, "SKW6621S: LOOPCHECK \"%s\"\n", cmd);

          if (strncmp(cmd, "BSPREADY", 8) == 0)
            {
              int ret = skw_send(ctx->sdio, SKW_LOOPCHECK_CH, "RDVERSION", 9);
              syslog(LOG_INFO, "SKW6621S: sent RDVERSION: %d\n", ret);
            }
          else if (strncmp(cmd, "trunk_W", 7) == 0)
            {
              ctx->flags |= SKW_FLAG_VERSION;
            }
          else if (strncmp(cmd, "BTREADY", 7) == 0)
            {
              ctx->flags |= SKW_FLAG_BTREADY;
            }
          else if (strncmp(cmd, "BSPASSERT", 9) == 0)
            {
              ctx->flags |= SKW_FLAG_ASSERT;
            }

          continue;
        }

      if ((ch == SKW_BT_CMD_CH || ch == SKW_BT_DATA_CH) && plen >= 1)
        {
          /* H4: first byte is the packet type; 0x07 is the CP's log */

          if (pl[0] == 0x07)
            {
              continue;
            }

          if (ctx->cb != NULL)
            {
              ctx->cb(ctx->cbarg, pl[0], pl + 1, plen - 1);
            }
          else
            {
              syslog(LOG_INFO, "SKW6621S: ch %u type %02x len %zu\n",
                     ch, pl[0], plen - 1);
            }

          continue;
        }

      syslog(LOG_INFO, "SKW6621S: rx ch %u len %u (dropped)\n", ch, len);
    }
}

/* One RX pass under the lock: read only if F1 interrupt is pending, and
 * keep reading while the CP says more packets follow.
 */

static int skw_rx_once(FAR struct skw_ctx_s *ctx)
{
  uint8_t pend;
  int loops = 0;
  int ret;

  ret = skw_f0_read(ctx->sdio, SKW_CCCR_INTPEND, &pend);
  if (ret < 0 || (pend & 0x02) == 0)
    {
      return ret < 0 ? ret : 0;
    }

  do
    {
      unsigned int n = ctx->npac;
      size_t len = SKW_PAC_SIZE * n + SKW_BLK_SIZE;
      uint32_t nsize;

      ret = sdio_io_rw_extended(ctx->sdio, false, SKW_FUNCTION,
                                SKW_PK_WINDOW, false, ctx->rxbuf,
                                SKW_BLK_SIZE, len / SKW_BLK_SIZE);
      if (ret < 0)
        {
          syslog(LOG_ERR, "SKW6621S: rx read %zu: %d\n", len, ret);
          return ret;
        }

      nsize = skw_le32(ctx->rxbuf + len - 4);
      skw_parse(ctx, n);

      ctx->npac = nsize == 0 ? 1 :
                  nsize > SKW_RX_MAX_PAC ? SKW_RX_MAX_PAC : nsize;
      if (nsize == 0)
        {
          break;
        }
    }
  while (++loops < 16);

  return 1;
}

/* Poll until a flag shows up, bounded */

static int skw_wait_flag(FAR struct skw_ctx_s *ctx, uint32_t flag,
                         unsigned int timeout_ms)
{
  clock_t start = clock_systime_ticks();
  int ret;

  while (TICK2MSEC(clock_systime_ticks() - start) < timeout_ms)
    {
      nxmutex_lock(&ctx->lock);
      ret = skw_rx_once(ctx);
      nxmutex_unlock(&ctx->lock);

      if (ret < 0)
        {
          return ret;
        }

      if (ctx->flags & SKW_FLAG_ASSERT)
        {
          return -EIO;
        }

      if (ctx->flags & flag)
        {
          return OK;
        }

      if (ret == 0)
        {
          up_mdelay(SKW_POLL_MS);
        }
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int skw6621s_boot(FAR struct sdio_dev_s *sdio,
                  FAR const struct skw6621s_probe_s *probe,
                  FAR const char *dir, FAR const char *nvname)
{
  FAR struct skw_ctx_s *ctx = &g_skw;
  struct skw_image_s iram =
  {
    0
  };

  struct skw_image_s dram =
  {
    0
  };

  struct skw_image_s nv =
  {
    0
  };

  uint32_t iram_addr = 0;
  uint32_t dram_addr = 0;
  clock_t start;
  uint8_t reg;
  int ret;

  if (probe->protocol != SKW6621S_SDIO_V2)
    {
      return -ENOTSUP;
    }

  ret = skw_load(dir, "SWT6621S_IRAM_SDIO.bin", &iram);
  if (ret == OK)
    {
      ret = skw_load(dir, "SWT6621S_DRAM_SDIO.bin", &dram);
    }

  if (ret == OK)
    {
      ret = skw_load(dir, nvname, &nv);
    }

  if (ret == OK)
    {
      ret = skw_prepare(&iram, &nv, &iram_addr, &dram_addr);
    }

  if (ret == OK && ctx->rxbuf == NULL)
    {
      ctx->rxbuf = kmm_malloc(SKW_RX_BUF_SIZE);
      ret = ctx->rxbuf == NULL ? -ENOMEM : OK;
    }

  if (ret < 0)
    {
      goto out;
    }

  nxmutex_lock(&ctx->lock);
  ctx->sdio = sdio;
  ctx->npac = 1;
  ctx->flags = 0;
  ctx->acks = 0;

  /* skw_sdio_set_dma_type / skw_sdio_slp_feature_en.  K7 has no
   * host/chip wake GPIOs, so the SDK would disable CP sleep.
   */

  ret = skw_f0_write(sdio, SKW_DMA_TYPE, SKW_DMA_TYPE_ADMA);
  if (ret == OK)
    {
      ret = skw_f0_write(sdio, SKW_SLP_SWITCH, 1);
    }

  /* Function 1 + master interrupt enable, and async interrupt, so that
   * CCCR INT pending reflects CP data even though we only poll it.
   */

  if (ret == OK)
    {
      ret = skw_f0_write(sdio, SKW_CCCR_INTEN, 0x03);
    }

  if (ret == OK && skw_f0_read(sdio, SKW_SDIO_INT_EXT, &reg) == OK)
    {
      ret = skw_f0_write(sdio, SKW_SDIO_INT_EXT, reg | 0x02);
    }

  if (ret < 0)
    {
      nxmutex_unlock(&ctx->lock);
      syslog(LOG_ERR, "SKW6621S: pre-download config: %d\n", ret);
      goto out;
    }

  start = clock_systime_ticks();
  ret = skw_dt_write(sdio, dram_addr, dram.data, dram.size);
  if (ret == OK)
    {
      ret = skw_dt_write(sdio, iram_addr, iram.data, iram.size);
    }

  if (ret == OK)
    {
      syslog(LOG_INFO, "SKW6621S: download done in %u ms\n",
             (unsigned int)TICK2MSEC(clock_systime_ticks() - start));
      ret = skw_f0_write(sdio, SKW_DL_DONE, 1);
    }

  nxmutex_unlock(&ctx->lock);
  if (ret < 0)
    {
      goto out;
    }

  /* Bounded handshake: BSPREADY -> RDVERSION -> trunk_W */

  start = clock_systime_ticks();
  ret = skw_wait_flag(ctx, SKW_FLAG_VERSION, SKW_BOOT_TIMEOUT_MS);
  syslog(LOG_INFO, "SKW6621S: handshake %d, %u ms\n", ret,
         (unsigned int)TICK2MSEC(clock_systime_ticks() - start));

out:
  kmm_free(iram.data);
  kmm_free(dram.data);
  kmm_free(nv.data);
  return ret;
}

int skw6621s_bt_start(void)
{
  FAR struct skw_ctx_s *ctx = &g_skw;
  clock_t start;
  int ret;

  if (ctx->sdio == NULL || (ctx->flags & SKW_FLAG_VERSION) == 0)
    {
      return -ENODEV;
    }

  if (ctx->flags & SKW_FLAG_BTREADY)
    {
      return OK;
    }

  /* send_modem_service_command(BT_SERVICE, SERVICE_START) */

  nxmutex_lock(&ctx->lock);
  ret = skw_f0_write(ctx->sdio, SKW_AP2CP_IRQ, SKW_BT_START_BIT);
  nxmutex_unlock(&ctx->lock);
  if (ret < 0)
    {
      return ret;
    }

  start = clock_systime_ticks();
  ret = skw_wait_flag(ctx, SKW_FLAG_BTREADY, SKW_BOOT_TIMEOUT_MS);
  syslog(LOG_INFO, "SKW6621S: BT service start %d, %u ms\n", ret,
         (unsigned int)TICK2MSEC(clock_systime_ticks() - start));
  return ret;
}

void skw6621s_set_hci_cb(skw6621s_hci_cb_t cb, FAR void *arg)
{
  nxmutex_lock(&g_skw.lock);
  g_skw.cb = cb;
  g_skw.cbarg = arg;
  nxmutex_unlock(&g_skw.lock);
}

int skw6621s_poll(void)
{
  int ret;

  if (g_skw.sdio == NULL)
    {
      return -ENODEV;
    }

  nxmutex_lock(&g_skw.lock);
  ret = skw_rx_once(&g_skw);
  nxmutex_unlock(&g_skw.lock);

  if (ret >= 0 && (g_skw.flags & SKW_FLAG_ASSERT))
    {
      ret = -EIO;
    }

  return ret;
}

int skw6621s_hci_send(uint8_t type, FAR const uint8_t *data, size_t len)
{
  uint8_t h4[SKW_BLK_SIZE - 8];
  uint8_t ch;
  int ret;

  if (g_skw.sdio == NULL || (g_skw.flags & SKW_FLAG_BTREADY) == 0)
    {
      return -ENODEV;
    }

  /* btseekwave_send_frame: commands on the cmd port, ACL on data port */

  if (type == 0x01)
    {
      ch = SKW_BT_CMD_CH;
    }
  else if (type == 0x02)
    {
      ch = SKW_BT_DATA_CH;
    }
  else
    {
      return -EINVAL;
    }

  if (len + 1 > sizeof(h4))
    {
      return -E2BIG;
    }

  h4[0] = type;
  memcpy(h4 + 1, data, len);

  nxmutex_lock(&g_skw.lock);
  ret = skw_send(g_skw.sdio, ch, (FAR const char *)h4, len + 1);
  nxmutex_unlock(&g_skw.lock);
  return ret;
}
