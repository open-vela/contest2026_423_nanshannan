/* SPDX-License-Identifier: Apache-2.0 */

/****************************************************************************
 * SKW6621S WiFi vendor command channel.
 *
 * Follows the SDK swt6621s_wifi driver (skw_drv_probe):
 *   service start -> SYN_VERSION -> GET_INFO -> PHY_BB_CFG (calibration
 *   file) -> OPEN_DEV (station, inst 0) -> START_SCAN.
 * Every message is a skw_msg header plus parameters on SDIO v2 port 6;
 * the CP answers with CMD_ACK (header, u16 status, return data) and
 * reports scan results as SCAN_REPORT events.  Commands are sent one at a
 * time and each waits (bounded) for its ack.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <sys/stat.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/compiler.h>
#include <nuttx/fs/fs.h>
#include <nuttx/kmalloc.h>

#include "skw6621s_sdio.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SKW_WIFI_CMD_CH             6

/* enum SKW_MSG_TYPE */

#define SKW_MSG_CMD                 0
#define SKW_MSG_CMD_ACK             1
#define SKW_MSG_EVENT               2

/* enum SKW_CMD_ID */

#define SKW_CMD_GET_INFO            1
#define SKW_CMD_SYN_VERSION         2
#define SKW_CMD_OPEN_DEV            3
#define SKW_CMD_START_SCAN          5
#define SKW_CMD_PHY_BB_CFG          50

/* enum SKW_EVENT_ID */

#define SKW_EVENT_NORMAL_SCAN_CMPL  0
#define SKW_EVENT_SCAN_REPORT       11
#define SKW_EVENT_CREDIT_UPDATE     16

#define SKW_STA_MODE                1
#define SKW_MSG_HDR_LEN             8
#define SKW_ACK_HDR_LEN             (SKW_MSG_HDR_LEN + 2)
#define SKW_CALIB_CHUNK             512
#define SKW_CALIB_MAX               16384

#define SKW_CMD_TIMEOUT_MS          1000
#define SKW_CALIB_TIMEOUT_MS        5000
#define SKW_SCAN_TIMEOUT_MS         8000

#define SKW_SCAN_MAX_BSS            48

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* struct skw_chip_info, as returned by GET_INFO */

begin_packed_struct struct skw_chip_info_s
{
  uint16_t enc_capa;
  uint32_t chip_model;
  uint32_t chip_version;
  uint32_t fw_version;
  uint32_t fw_capa;
  uint8_t max_sta_allowed;
  uint8_t max_mc_addr_allowed;
  uint16_t ht_capa;
  uint16_t ht_ext_capa;
  uint16_t ht_ampdu_param;
  uint32_t ht_tx_mcs_maps;
  uint32_t ht_rx_mcs_maps;
  uint32_t vht_capa;
  uint16_t vht_tx_mcs_maps;
  uint16_t vht_rx_mcs_maps;
  uint8_t max_scan_ssids;
  uint8_t he_capa[6];
  uint8_t he_phy_capa[11];
  uint16_t he_tx_mcs_maps;
  uint16_t he_rx_mcs_maps;
  uint8_t mac[6];
  uint8_t abg_rate_num;
  uint8_t abg_rate[15];
  uint32_t fw_bw_capa;
  uint32_t priv;
  uint8_t fw_build_time[32];
  uint8_t fw_plat_ver[16];
  uint8_t fw_wifi_ver[16];
  uint8_t fw_bt_ver[16];
  uint32_t fw_timestamp;
  uint32_t fw_chip_type;
  uint16_t calib_module_id;
  uint16_t resv;
  uint32_t fw_ext_capa[12];
} end_packed_struct;

begin_packed_struct struct skw_calib_param_s
{
  uint8_t seq;
  uint8_t end;
  uint16_t len;
  uint8_t data[SKW_CALIB_CHUNK];
} end_packed_struct;

begin_packed_struct struct skw_open_dev_s
{
  uint16_t mode;
  uint16_t flags;
  uint8_t mac[6];
} end_packed_struct;

/* struct skw_scan_param, followed by skw_scan_chan_info[] and
 * cfg80211_ssid[] (ssid[32] + len)
 */

begin_packed_struct struct skw_scan_param_s
{
  uint16_t flags;
  uint8_t rand_mac[6];
  uint32_t nr_chan;
  uint32_t chan_offset;
  uint32_t n_ssid;
  uint32_t ssid_offset;
  uint32_t ie_len;
  uint32_t ie_offset;
} end_packed_struct;

struct skw_bss_s
{
  uint8_t bssid[6];
  uint8_t chan;
  int16_t signal;
  uint8_t ssid_len;
  char ssid[33];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Pending command, filled by the RX callback */

static uint8_t g_cmd_id;
static uint16_t g_cmd_seq;
static volatile bool g_cmd_acked;
static uint16_t g_cmd_status;
static uint8_t g_cmd_rsp[512];
static size_t g_cmd_rsp_len;

static volatile bool g_scan_done;
static struct skw_bss_s g_bss[SKW_SCAN_MAX_BSS];
static unsigned int g_nbss;
static unsigned int g_nreport;

static uint8_t g_mac[6];
static bool g_wifi_ready;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint16_t skw_le16(FAR const uint8_t *p)
{
  return p[0] | p[1] << 8;
}

/* skw_event_scan_report: skw_mgmt_hdr (chan, band, s16 signal, u16 len,
 * u16 resv) then a beacon / probe response; IEs start 36 bytes into it.
 */

static void wifi_scan_report(FAR const uint8_t *data, size_t len)
{
  FAR const uint8_t *mgmt = data + 8;
  FAR const uint8_t *ie;
  FAR struct skw_bss_s *bss = NULL;
  size_t mlen;
  size_t off;
  unsigned int i;

  g_nreport++;
  if (len < 8 + 36)
    {
      return;
    }

  mlen = skw_le16(data + 4);
  if (mlen > len - 8)
    {
      mlen = len - 8;
    }

  if (mlen < 36)
    {
      return;
    }

  for (i = 0; i < g_nbss; i++)
    {
      if (memcmp(g_bss[i].bssid, mgmt + 16, 6) == 0)
        {
          bss = &g_bss[i];
          break;
        }
    }

  if (bss == NULL)
    {
      if (g_nbss >= SKW_SCAN_MAX_BSS)
        {
          return;
        }

      bss = &g_bss[g_nbss++];
      memset(bss, 0, sizeof(*bss));
      memcpy(bss->bssid, mgmt + 16, 6);
    }

  bss->chan = data[0];
  bss->signal = (int16_t)skw_le16(data + 2);

  ie = mgmt + 36;
  for (off = 0; off + 2 <= mlen - 36; off += 2 + ie[off + 1])
    {
      if (ie[off] == 0 && off + 2 + ie[off + 1] <= mlen - 36)
        {
          size_t n = ie[off + 1] < 32 ? ie[off + 1] : 32;

          memcpy(bss->ssid, ie + off + 2, n);
          bss->ssid[n] = '\0';
          bss->ssid_len = n;
          break;
        }
    }
}

static void wifi_rx_cb(FAR void *arg, uint8_t channel,
                       FAR const uint8_t *data, size_t len)
{
  uint8_t type;
  uint8_t id;
  uint16_t seq;
  size_t total;

  if (channel != SKW_WIFI_CMD_CH || len < SKW_MSG_HDR_LEN)
    {
      return;
    }

  type = data[0] >> 4;
  id = data[1];
  seq = skw_le16(data + 2);
  total = skw_le16(data + 4);
  if (total < SKW_MSG_HDR_LEN || total > len)
    {
      total = len;
    }

  if (type == SKW_MSG_CMD_ACK)
    {
      if (id != g_cmd_id || seq != g_cmd_seq || total < SKW_ACK_HDR_LEN)
        {
          syslog(LOG_WARNING, "SKW WiFi: stray ack id %u seq %u\n", id, seq);
          return;
        }

      g_cmd_status = skw_le16(data + SKW_MSG_HDR_LEN);
      g_cmd_rsp_len = total - SKW_ACK_HDR_LEN;
      if (g_cmd_rsp_len > sizeof(g_cmd_rsp))
        {
          g_cmd_rsp_len = sizeof(g_cmd_rsp);
        }

      memcpy(g_cmd_rsp, data + SKW_ACK_HDR_LEN, g_cmd_rsp_len);
      g_cmd_acked = true;
    }
  else if (type == SKW_MSG_EVENT)
    {
      FAR const uint8_t *ev = data + SKW_MSG_HDR_LEN;
      size_t evlen = total - SKW_MSG_HDR_LEN;

      switch (id)
        {
          case SKW_EVENT_SCAN_REPORT:
            wifi_scan_report(ev, evlen);
            break;

          case SKW_EVENT_NORMAL_SCAN_CMPL:
            g_scan_done = true;
            break;

          case SKW_EVENT_CREDIT_UPDATE:
            break;

          default:
            syslog(LOG_INFO, "SKW WiFi: event %u len %zu\n", id, evlen);
            break;
        }
    }
  else
    {
      syslog(LOG_INFO, "SKW WiFi: rx type %u id %u len %zu\n",
             type, id, len);
    }
}

/* skw_set_cmd + skw_msg_xmit: one command, bounded wait for its ack.
 * Returns the CP status (>= 0) or a negative errno.
 */

static int wifi_cmd(uint8_t inst, uint8_t id, FAR const void *param,
                    size_t plen, FAR void *rsp, size_t rspsize,
                    unsigned int timeout_ms)
{
  static uint8_t msg[SKW_MSG_HDR_LEN + 1024];
  size_t total = SKW_MSG_HDR_LEN + plen;
  clock_t start;
  int ret;

  if (total > sizeof(msg))
    {
      return -E2BIG;
    }

  g_cmd_id = id;
  g_cmd_seq++;
  g_cmd_acked = false;

  msg[0] = (inst & 0x0f) | SKW_MSG_CMD << 4;
  msg[1] = id;
  msg[2] = g_cmd_seq & 0xff;
  msg[3] = g_cmd_seq >> 8;
  msg[4] = total & 0xff;
  msg[5] = total >> 8;
  msg[6] = 0;
  msg[7] = 0;
  if (plen > 0)
    {
      memcpy(msg + SKW_MSG_HDR_LEN, param, plen);
    }

  ret = skw6621s_wifi_xmit(SKW_WIFI_CMD_CH, msg, total);
  if (ret < 0)
    {
      syslog(LOG_ERR, "SKW WiFi: cmd %u xmit %d\n", id, ret);
      return ret;
    }

  start = clock_systime_ticks();
  while (TICK2MSEC(clock_systime_ticks() - start) < timeout_ms)
    {
      ret = skw6621s_poll();
      if (ret < 0)
        {
          return ret;
        }

      if (g_cmd_acked)
        {
          if (rsp != NULL)
            {
              memcpy(rsp, g_cmd_rsp,
                     g_cmd_rsp_len < rspsize ? g_cmd_rsp_len : rspsize);
              if (g_cmd_rsp_len != rspsize)
                {
                  syslog(LOG_INFO, "SKW WiFi: cmd %u rsp %zu, want %zu\n",
                         id, g_cmd_rsp_len, rspsize);
                }
            }

          return g_cmd_status;
        }

      if (ret == 0)
        {
          up_mdelay(2);
        }
    }

  syslog(LOG_ERR, "SKW WiFi: cmd %u seq %u timeout\n", id, g_cmd_seq);
  return -ETIMEDOUT;
}

static int wifi_load(FAR const char *path, FAR uint8_t **data,
                     FAR size_t *size)
{
  struct file filep;
  struct stat st;
  ssize_t nread;
  int ret;

  ret = file_open(&filep, path, O_RDONLY);
  if (ret < 0)
    {
      return ret;
    }

  ret = file_fstat(&filep, &st);
  if (ret == OK && (st.st_size <= 0 || st.st_size > SKW_CALIB_MAX))
    {
      ret = -EFBIG;
    }

  if (ret == OK)
    {
      *data = kmm_malloc(st.st_size);
      ret = *data == NULL ? -ENOMEM : OK;
    }

  if (ret == OK)
    {
      nread = file_read(&filep, *data, st.st_size);
      if (nread != st.st_size)
        {
          kmm_free(*data);
          *data = NULL;
          ret = nread < 0 ? (int)nread : -EIO;
        }
      else
        {
          *size = st.st_size;
        }
    }

  file_close(&filep);
  return ret;
}

/* skw_calib_bin_download: 512-byte chunks, seq from 0, end on the last */

static int wifi_calib(FAR const char *path)
{
  FAR struct skw_calib_param_s *calib;
  FAR uint8_t *data = NULL;
  size_t size = 0;
  size_t off = 0;
  int seq = 0;
  int ret;

  ret = wifi_load(path, &data, &size);
  if (ret < 0)
    {
      syslog(LOG_ERR, "SKW WiFi: load %s: %d\n", path, ret);
      return ret;
    }

  calib = kmm_zalloc(sizeof(*calib));
  if (calib == NULL)
    {
      kmm_free(data);
      return -ENOMEM;
    }

  while (off < size)
    {
      size_t n = size - off < SKW_CALIB_CHUNK ? size - off : SKW_CALIB_CHUNK;

      calib->seq = seq;
      calib->len = n;
      calib->end = off + n >= size;
      memcpy(calib->data, data + off, n);

      ret = wifi_cmd(0, SKW_CMD_PHY_BB_CFG, calib, sizeof(*calib),
                     NULL, 0, SKW_CALIB_TIMEOUT_MS);
      if (ret != 0)
        {
          syslog(LOG_ERR, "SKW WiFi: calib seq %d: %d\n", seq, ret);
          ret = ret > 0 ? -EIO : ret;
          break;
        }

      off += n;
      seq++;
    }

  syslog(LOG_INFO, "SKW WiFi: calib %s, %zu bytes, %d chunks: %d\n",
         path, size, seq, ret);
  kmm_free(calib);
  kmm_free(data);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int skw6621s_wifi_init(FAR const char *dir)
{
  FAR struct skw_chip_info_s *chip;
  FAR uint8_t *ver;
  struct skw_open_dev_s open;
  char path[96];
  uint64_t ts;
  int vendor = 0;
  int ret;
  int i;

  if (g_wifi_ready)
    {
      return OK;
    }

  ret = skw6621s_wifi_start();
  if (ret < 0)
    {
      return ret;
    }

  skw6621s_set_wifi_cb(wifi_rx_cb, NULL);

  chip = kmm_zalloc(sizeof(*chip));
  ver = kmm_zalloc(512);
  if (chip == NULL || ver == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  /* skw_sync_cmd_event_version: cmd[256] + event[256] versions */

  ret = wifi_cmd(0, SKW_CMD_SYN_VERSION, NULL, 0, ver, 512,
                 SKW_CMD_TIMEOUT_MS);
  syslog(LOG_INFO, "SKW WiFi: SYN_VERSION %d, GET_INFO v%u OPEN_DEV v%u "
         "START_SCAN v%u\n", ret, ver[SKW_CMD_GET_INFO],
         ver[SKW_CMD_OPEN_DEV], ver[SKW_CMD_START_SCAN]);
  if (ret != 0)
    {
      goto fail;
    }

  /* skw_sync_chip_info: parameter is the host time in ms */

  ts = TICK2MSEC(clock_systime_ticks());
  ret = wifi_cmd(0, SKW_CMD_GET_INFO, &ts, sizeof(ts), chip, sizeof(*chip),
                 SKW_CMD_TIMEOUT_MS);
  if (ret != 0)
    {
      goto fail;
    }

  chip->fw_build_time[sizeof(chip->fw_build_time) - 1] = '\0';
  chip->fw_wifi_ver[sizeof(chip->fw_wifi_ver) - 1] = '\0';
  chip->fw_plat_ver[sizeof(chip->fw_plat_ver) - 1] = '\0';
  memcpy(g_mac, chip->mac, 6);
  syslog(LOG_INFO, "SKW WiFi: chip type 0x%" PRIx32 " module 0x%04x, "
         "wifi %s plat %s built %s\n", chip->fw_chip_type,
         chip->calib_module_id, (FAR char *)chip->fw_wifi_ver,
         (FAR char *)chip->fw_plat_ver, (FAR char *)chip->fw_build_time);
  syslog(LOG_INFO, "SKW WiFi: MAC %02x:%02x:%02x:%02x:%02x:%02x, "
         "bw 0x%" PRIx32 ", %u lmac\n", g_mac[0], g_mac[1], g_mac[2],
         g_mac[3], g_mac[4], g_mac[5], chip->fw_bw_capa,
         (unsigned int)(chip->priv >> 24));

  if (chip->fw_chip_type != 0x300)
    {
      syslog(LOG_ERR, "SKW WiFi: unexpected chip type\n");
      ret = -ENODEV;
      goto out;
    }

  /* CALIB_APPEND_MODULE_ID: nibble 0xf in module id -> vendor bit */

  for (i = 0; i < 4; i++)
    {
      if (((chip->calib_module_id >> (4 * i)) & 0xf) == 0xf)
        {
          vendor |= 1 << (4 + i);
        }
    }

  snprintf(path, sizeof(path), "%s/SWT6621S_SEEKWAVE_R%02X%03X.bin", dir,
           vendor & 0xff, skw6621s_revision());
  ret = wifi_calib(path);
  if (ret < 0)
    {
      goto out;
    }

  /* skw_cmd_open_dev: station on inst 0 with the efuse MAC */

  if ((g_mac[0] & 1) || (g_mac[0] | g_mac[1] | g_mac[2] |
                         g_mac[3] | g_mac[4] | g_mac[5]) == 0)
    {
      static const uint8_t fallback[6] =
      {
        0xfe, 0xfd, 0xfc, 0x00, 0x66, 0x21
      };

      memcpy(g_mac, fallback, 6);
      syslog(LOG_WARNING, "SKW WiFi: efuse MAC invalid, using fallback\n");
    }

  open.mode = SKW_STA_MODE;
  open.flags = 0;
  memcpy(open.mac, g_mac, 6);
  ret = wifi_cmd(0, SKW_CMD_OPEN_DEV, &open, sizeof(open), NULL, 0,
                 SKW_CMD_TIMEOUT_MS);
  syslog(LOG_INFO, "SKW WiFi: OPEN_DEV sta: %d\n", ret);
  if (ret != 0)
    {
      goto fail;
    }

  g_wifi_ready = true;
  goto out;

fail:
  ret = ret > 0 ? -EIO : ret;

out:
  kmm_free(chip);
  kmm_free(ver);
  return ret;
}

int skw6621s_wifi_scan(void)
{
  uint8_t buf[sizeof(struct skw_scan_param_s) + 13 * 3 + 33];
  FAR struct skw_scan_param_s *param = (FAR struct skw_scan_param_s *)buf;
  FAR uint8_t *chan = buf + sizeof(*param);
  clock_t start;
  unsigned int i;
  int ret;

  if (!g_wifi_ready)
    {
      return -ENODEV;
    }

  /* skw_scan: 2.4 GHz channels 1-13, one wildcard SSID (active) */

  memset(buf, 0, sizeof(buf));
  param->nr_chan = 13;
  param->chan_offset = sizeof(*param);
  for (i = 0; i < 13; i++)
    {
      chan[i * 3] = i + 1;
      chan[i * 3 + 1] = 0;             /* SKW_BAND_2GHZ */
      chan[i * 3 + 2] = 0;
    }

  param->n_ssid = 1;
  param->ssid_offset = sizeof(*param) + 13 * 3;

  g_nbss = 0;
  g_nreport = 0;
  g_scan_done = false;

  ret = wifi_cmd(0, SKW_CMD_START_SCAN, buf, sizeof(buf), NULL, 0,
                 SKW_CMD_TIMEOUT_MS);
  if (ret != 0)
    {
      syslog(LOG_ERR, "SKW WiFi: START_SCAN %d\n", ret);
      return ret > 0 ? -EIO : ret;
    }

  start = clock_systime_ticks();
  while (!g_scan_done &&
         TICK2MSEC(clock_systime_ticks() - start) < SKW_SCAN_TIMEOUT_MS)
    {
      ret = skw6621s_poll();
      if (ret < 0)
        {
          return ret;
        }

      if (ret == 0)
        {
          up_mdelay(5);
        }
    }

  printf("scan %s in %u ms: %u reports, %u BSS\n",
         g_scan_done ? "done" : "TIMEOUT",
         (unsigned int)TICK2MSEC(clock_systime_ticks() - start),
         g_nreport, g_nbss);

  for (i = 0; i < g_nbss; i++)
    {
      FAR struct skw_bss_s *b = &g_bss[i];

      printf("  %02x:%02x:%02x:%02x:%02x:%02x  ch %2u  %4d dBm  %s\n",
             b->bssid[0], b->bssid[1], b->bssid[2], b->bssid[3],
             b->bssid[4], b->bssid[5], b->chan, b->signal,
             b->ssid_len ? b->ssid : "<hidden>");
    }

  return g_scan_done ? OK : -ETIMEDOUT;
}
