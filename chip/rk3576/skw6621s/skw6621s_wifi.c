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
 *
 * Connecting follows the STA_SME_EXT path (skw_join / skw_auth /
 * skw_assoc): JOIN with the scanned IEs, AUTH (open system) and ASSOC,
 * each answered by an RX_MGMT event carrying the AP's frame.  The RSN
 * 4-way handshake is ours (skw6621s_wpa.c); EAPOL frames are sent as
 * TX_DATA_FRAME commands, as the vendor driver does for every
 * "filtered" protocol, and arrive on data port 7.
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
#include <nuttx/kthread.h>
#include <nuttx/mutex.h>
#include <nuttx/signal.h>
#include <nuttx/spinlock.h>

#ifdef CONFIG_MM_IOB
#  include <nuttx/net/netdev_lowerhalf.h>
#endif

#ifdef CONFIG_NETDEV_WIRELESS_HANDLER
#  include <net/if_arp.h>
#  include <nuttx/wireless/wireless.h>
#endif

#include "skw6621s_sdio.h"
#include "skw6621s_wpa.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SKW_WIFI_CMD_CH             6
#define SKW_WIFI_DATA_CH            7

/* enum SKW_MSG_TYPE */

#define SKW_MSG_CMD                 0
#define SKW_MSG_CMD_ACK             1
#define SKW_MSG_EVENT               2

/* enum SKW_CMD_ID */

#define SKW_CMD_GET_INFO            1
#define SKW_CMD_SYN_VERSION         2
#define SKW_CMD_OPEN_DEV            3
#define SKW_CMD_START_SCAN          5
#define SKW_CMD_JOIN                9
#define SKW_CMD_AUTH                10
#define SKW_CMD_ASSOC               11
#define SKW_CMD_ADD_KEY             12
#define SKW_CMD_TX_DATA_FRAME       15
#define SKW_CMD_DISCONNECT          17
#define SKW_CMD_PHY_BB_CFG          50

/* enum SKW_EVENT_ID */

#define SKW_EVENT_NORMAL_SCAN_CMPL  0
#define SKW_EVENT_DISCONNECT        2
#define SKW_EVENT_RX_MGMT           4
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
#define SKW_SCAN_PASSIVE            0x80
#define SKW_BAND_2GHZ               0
#define SKW_BAND_5GHZ               1
#define SKW_BSS_IE_MAX              768

#define SKW_MGMT_TIMEOUT_MS         1000
#define SKW_EAPOL_TIMEOUT_MS        5000

/* 802.11 management subtypes (frame control byte 0) */

#define WLAN_FC_ASSOC_RESP          0x10
#define WLAN_FC_REASSOC_RESP        0x30
#define WLAN_FC_DISASSOC            0xa0
#define WLAN_FC_AUTH                0xb0
#define WLAN_FC_DEAUTH              0xc0

/* enum SKW_KEY_TYPE / SKW_CIPHER_TYPE */

#define SKW_KEY_TYPE_PTK            0
#define SKW_KEY_TYPE_GTK            1
#define SKW_CIPHER_TYPE_TKIP        3
#define SKW_CIPHER_TYPE_CCMP        8

/* SDIO RX descriptor (skw_rx_desc, words 13-17): msdu_offset counts from
 * 52 bytes before the descriptor; the MSDU is an Ethernet frame that is
 * msdu_len + 6 bytes long.
 */

#define SKW_RX_DESC_LEN             20
#define SKW_RX_MSDU_BASE            52
#define SKW_MSDU_HDR_LEN            6

#define ETH_HLEN                    14
#define ETH_P_PAE                   0x888e
#define SKW_FRAME_MAX               1536
#define SKW_EAPOL_SLOTS             2

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
  uint8_t band;                         /* SKW_BAND_2GHZ / SKW_BAND_5GHZ */
  int16_t signal;
  uint16_t capability;                  /* bit 4: privacy (WEP / WPA) */
  uint8_t ssid_len;
  char ssid[33];
};

/* The BSS being joined: what JOIN needs from the beacon / probe response */

struct skw_target_s
{
  struct skw_bss_s bss;
  uint16_t beacon_int;
  uint16_t capability;
  uint16_t ie_len;
  uint8_t ies[SKW_BSS_IE_MAX];
};

begin_packed_struct struct skw_join_param_s
{
  uint8_t chan_num;
  uint8_t center_chn1;
  uint8_t center_chn2;
  uint8_t bandwidth;
  uint8_t band;
  uint16_t beacon_interval;
  uint16_t capability;
  uint8_t bssid_index;
  uint8_t max_bssid_indicator;
  uint8_t bssid[6];
  uint16_t roaming;
  uint16_t bss_ie_offset;
  uint32_t bss_ie_len;
} end_packed_struct;

begin_packed_struct struct skw_join_resp_s
{
  uint8_t peer_idx;
  uint8_t lmac_id;
  uint8_t inst;
  uint8_t multicast_idx;
} end_packed_struct;

begin_packed_struct struct skw_auth_param_s
{
  uint16_t auth_algorithm;
  uint16_t key_data_offset;
  uint16_t key_data_len;
  uint16_t auth_data_offset;
  uint16_t auth_data_len;
  uint16_t auth_ie_offset;
  uint16_t auth_ie_len;
} end_packed_struct;

/* struct skw_assoc_req_param: ieee80211_ht_cap (26) + ieee80211_vht_cap
 * (12), zero as from a supplicant without capability masks.
 */

begin_packed_struct struct skw_assoc_param_s
{
  uint8_t ht_capa[26];
  uint8_t vht_capa[12];
  uint8_t bssid[6];
  uint8_t pre_bssid[6];
  uint16_t req_ie_offset;
  uint16_t req_ie_len;
} end_packed_struct;

begin_packed_struct struct skw_key_param_s
{
  uint8_t mac_addr[6];
  uint8_t key_type;
  uint8_t cipher_type;
  uint8_t pn[6];
  uint8_t key_id;
  uint8_t key_len;
  uint8_t key[32];
} end_packed_struct;

begin_packed_struct struct skw_disconnect_param_s
{
  uint8_t type;
  uint8_t local_state_change;
  uint16_t reason_code;
  uint16_t ie_offset;
  uint16_t ie_len;
} end_packed_struct;

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

/* China: 2.4 GHz 1-13, 5 GHz 36-64 (52-64 DFS: passive) and 149-165 */

static const uint8_t g_chan5g[] =
{
  36, 40, 44, 48, 52, 56, 60, 64, 149, 153, 157, 161, 165
};

#define SKW_SCAN_NCHAN              (13 + sizeof(g_chan5g))

static volatile bool g_scan_done;
static struct skw_bss_s g_bss[SKW_SCAN_MAX_BSS];
static unsigned int g_nbss;
static unsigned int g_nreport;

static uint8_t g_mac[6];
static bool g_wifi_ready;
static mutex_t g_cmd_lock = NXMUTEX_INITIALIZER;

/* One scan / connect / disconnect at a time: the settings UI thread and
 * wapi (ioctl, in the caller's thread) both drive the chip, and a scan
 * rewrites g_bss and g_target under whoever is reading them.
 */

static mutex_t g_op_lock = NXMUTEX_INITIALIZER;
static bool g_scan_valid;               /* g_bss holds a finished scan */

/* Connection state; the mgmt / eapol fields are filled by the callback */

static FAR const char *g_want;          /* SSID substring while scanning */
static bool g_want_exact;               /* ... or the whole SSID (wapi) */
static uint8_t g_skip_bssid[6];         /* BSS that rejected our auth */
static bool g_skip_valid;
static bool g_pn_reuse;                 /* chip priv bit 2: msdu_len in pn[4] */
static struct skw_target_s g_target;
static bool g_have_target;

static volatile uint8_t g_mgmt_fc;      /* last auth / assoc reply */
static volatile uint16_t g_mgmt_status;
static volatile bool g_mgmt_seen;
static volatile bool g_deauth;
static volatile uint16_t g_deauth_reason;

static uint8_t g_eapol[SKW_EAPOL_SLOTS][SKW_WPA_EAPOL_MAX];
static volatile size_t g_eapol_len[SKW_EAPOL_SLOTS];

static struct skw_join_resp_s g_join;
static bool g_joined;
static bool g_connected;
static unsigned int g_data_dumps;
static FAR struct skw_wpa_s *g_wpa;

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void wifi_free_wpa(void);
static int wifi_scan_run(void);
static int wifi_connect(FAR const char *ssid, FAR const char *passphrase);
static int wifi_disconnect(void);
static void wifi_netdev_rx(FAR const uint8_t *frame, size_t len);
static void wifi_netdev_link(bool up);

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
  bss->band = data[1];
  bss->signal = (int16_t)skw_le16(data + 2);
  bss->capability = skw_le16(mgmt + 34);

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

  /* Keep the strongest match's IEs for JOIN (beacon interval and
   * capability sit just before them).
   */

  if (g_want != NULL && bss->ssid_len > 0 &&
      (g_want_exact ? strcmp(bss->ssid, g_want) == 0 :
                      strstr(bss->ssid, g_want) != NULL) &&
      !(g_skip_valid && memcmp(g_skip_bssid, bss->bssid, 6) == 0) &&
      (!g_have_target || memcmp(g_target.bss.bssid, bss->bssid, 6) == 0 ||
       bss->signal > g_target.bss.signal))
    {
      g_target.bss = *bss;
      g_target.beacon_int = skw_le16(mgmt + 32);
      g_target.capability = skw_le16(mgmt + 34);
      g_target.ie_len = mlen - 36 < SKW_BSS_IE_MAX ? mlen - 36 :
                        SKW_BSS_IE_MAX;
      memcpy(g_target.ies, ie, g_target.ie_len);
      g_have_target = true;
    }
}

/* skw_event_rx_mgmt: skw_mgmt_hdr then the frame (auth, assoc response,
 * deauth ...) addressed to us.
 */

static void wifi_rx_mgmt(FAR const uint8_t *data, size_t len)
{
  FAR const uint8_t *f = data + 8;
  size_t flen;
  uint8_t fc;

  if (len < 8 + 24 + 2)
    {
      return;
    }

  flen = len - 8;
  fc = f[0] & 0xfc;
  syslog(LOG_INFO, "SKW WiFi: rx mgmt fc 0x%02x len %zu from "
         "%02x:%02x:%02x:..\n", fc, flen, f[10], f[11], f[12]);

  switch (fc)
    {
      case WLAN_FC_AUTH:
        if (flen >= 24 + 6)
          {
            syslog(LOG_INFO, "SKW WiFi: auth da %02x:..:%02x alg %u "
                   "seq %u status %u\n", f[4], f[9], skw_le16(f + 24),
                   skw_le16(f + 26), skw_le16(f + 28));
            g_mgmt_status = skw_le16(f + 28);
            g_mgmt_fc = fc;
            g_mgmt_seen = true;
          }
        break;

      case WLAN_FC_ASSOC_RESP:
      case WLAN_FC_REASSOC_RESP:
        if (flen >= 24 + 6)
          {
            g_mgmt_status = skw_le16(f + 26);
            g_mgmt_fc = WLAN_FC_ASSOC_RESP;
            g_mgmt_seen = true;
          }
        break;

      case WLAN_FC_DEAUTH:
      case WLAN_FC_DISASSOC:
        g_deauth_reason = skw_le16(f + 24);
        g_deauth = true;
        break;

      default:
        break;
    }
}

/* Data port: RX descriptor, then an Ethernet frame */

static void wifi_rx_data(FAR const uint8_t *d, size_t len)
{
  FAR const uint8_t *eth;
  size_t shift;
  size_t off;
  size_t flen;
  unsigned int i;

  /* With the firmware's PN reuse (chip priv bit 2) the vendor driver does
   * not drop the 4-byte SDIO header on the data port: the RX descriptor
   * starts at that header, i.e. 4 bytes before d, and msdu_len moves into
   * pn[4..5].  Seen on the board: msdu_offset at d+14, length at d+12,
   * Ethernet frame at d+18.  Only descriptor bytes 4.. are read.
   */

  shift = g_pn_reuse ? 4 : 0;
  if (len + shift < SKW_RX_DESC_LEN)
    {
      return;
    }

  off = d[18 - shift];
  flen = skw_le16(g_pn_reuse ? d + 16 - shift : d + 2) + SKW_MSDU_HDR_LEN;
  if (off < SKW_RX_MSDU_BASE + shift ||
      off - SKW_RX_MSDU_BASE < SKW_RX_DESC_LEN ||
      off - SKW_RX_MSDU_BASE - shift + flen > len || flen < ETH_HLEN)
    {
      static int dumps;

      syslog(LOG_WARNING, "SKW WiFi: rx data bad desc off %zu len %zu/%zu\n",
             off, flen, len);
      if (dumps++ < 2)
        {
          for (i = 0; i + 16 <= len && i < 64; i += 16)
            {
              syslog(LOG_WARNING, "  %02x: %02x %02x %02x %02x %02x %02x "
                     "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                     i,
                     d[i], d[i + 1], d[i + 2], d[i + 3], d[i + 4], d[i + 5],
                     d[i + 6], d[i + 7], d[i + 8], d[i + 9], d[i + 10],
                     d[i + 11], d[i + 12], d[i + 13], d[i + 14], d[i + 15]);
            }
        }

      return;
    }

  eth = d + off - SKW_RX_MSDU_BASE - shift;

  if (g_data_dumps < 4)
    {
      g_data_dumps++;
      syslog(LOG_INFO, "SKW WiFi: rx data len %zu off %zu frame %zu "
             "filter %u type %02x%02x\n", len, off, flen, d[7 - shift],
             eth[12], eth[13]);
    }

  if ((eth[12] << 8 | eth[13]) == ETH_P_PAE)
    {
      for (i = 0; i < SKW_EAPOL_SLOTS; i++)
        {
          if (g_eapol_len[i] == 0 && flen - ETH_HLEN <= SKW_WPA_EAPOL_MAX)
            {
              memcpy(g_eapol[i], eth + ETH_HLEN, flen - ETH_HLEN);
              g_eapol_len[i] = flen - ETH_HLEN;
              return;
            }
        }

      syslog(LOG_WARNING, "SKW WiFi: EAPOL dropped\n");
      return;
    }

  wifi_netdev_rx(eth, flen);
}

static void wifi_rx_cb(FAR void *arg, uint8_t channel,
                       FAR const uint8_t *data, size_t len)
{
  uint8_t type;
  uint8_t id;
  uint16_t seq;
  size_t total;

  if (channel == SKW_WIFI_DATA_CH)
    {
      wifi_rx_data(data, len);
      return;
    }

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

          case SKW_EVENT_RX_MGMT:
            wifi_rx_mgmt(ev, evlen);
            break;

          case SKW_EVENT_DISCONNECT:
            if (evlen >= 2)
              {
                g_deauth_reason = skw_le16(ev);
              }

            g_deauth = true;
            syslog(LOG_INFO, "SKW WiFi: DISCONNECT event, reason %u\n",
                   g_deauth_reason);
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
  static uint8_t msg[SKW_MSG_HDR_LEN + 8 + SKW_FRAME_MAX];
  size_t total = SKW_MSG_HDR_LEN + plen;
  clock_t start;
  int ret;

  if (total > sizeof(msg))
    {
      return -E2BIG;
    }

  nxmutex_lock(&g_cmd_lock);
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
      nxmutex_unlock(&g_cmd_lock);
      return ret;
    }

  start = clock_systime_ticks();
  while (TICK2MSEC(clock_systime_ticks() - start) < timeout_ms)
    {
      ret = skw6621s_poll();
      if (ret < 0)
        {
          nxmutex_unlock(&g_cmd_lock);
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

          nxmutex_unlock(&g_cmd_lock);
          return g_cmd_status;
        }

      if (ret == 0)
        {
          up_mdelay(2);
        }
    }

  syslog(LOG_ERR, "SKW WiFi: cmd %u seq %u timeout\n", id, g_cmd_seq);
  nxmutex_unlock(&g_cmd_lock);
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

static FAR const uint8_t *wifi_find_ie(uint8_t id)
{
  FAR const uint8_t *ie = g_target.ies;
  size_t len = g_target.ie_len;
  size_t off;

  for (off = 0; off + 2 <= len && off + 2 + ie[off + 1] <= len;
       off += 2 + ie[off + 1])
    {
      if (ie[off] == id)
        {
          return ie + off;
        }
    }

  return NULL;
}

/* Poll until the auth / assoc reply (fc) shows up, bounded */

static int wifi_wait_mgmt(uint8_t fc)
{
  clock_t start = clock_systime_ticks();
  int ret;

  while (TICK2MSEC(clock_systime_ticks() - start) < SKW_MGMT_TIMEOUT_MS)
    {
      if (g_mgmt_seen && g_mgmt_fc == fc)
        {
          return g_mgmt_status;
        }

      if (g_deauth)
        {
          return -ECONNREFUSED;
        }

      ret = skw6621s_poll();
      if (ret < 0)
        {
          return ret;
        }

      if (ret == 0)
        {
          up_mdelay(2);
        }
    }

  return -ETIMEDOUT;
}

/* skw_ndo_xmit for a "filtered" frame: skw_tx_desc_hdr (6 bytes),
 * skw_tx_desc_conf (2 bytes, no checksum offload), Ethernet frame; sent
 * as SKW_CMD_TX_DATA_FRAME.
 */

static uint8_t g_txbuf[8 + SKW_FRAME_MAX];
static int g_tx_dumps;
static mutex_t g_tx_lock = NXMUTEX_INITIALIZER;

static int wifi_tx_eth(size_t mlen)
{
  FAR uint8_t *buf = g_txbuf;
  uint8_t peer = (buf[8] & 1) ? g_join.multicast_idx : g_join.peer_idx;
  uint16_t w0;
  uint16_t w1;
  int ret;

  if (!g_joined || mlen < ETH_HLEN || mlen > SKW_FRAME_MAX)
    {
      return -EINVAL;
    }

  w0 = (g_join.inst & 0x3) << 2 | (peer & 0x1f) << 8;  /* tid 0, ether */
  w1 = (mlen & 0xfff) | (g_join.lmac_id & 0x3) << 12;

  buf[0] = w0 & 0xff;
  buf[1] = w0 >> 8;
  buf[2] = w1 & 0xff;
  buf[3] = w1 >> 8;
  buf[4] = buf[8 + 12];                 /* eth_type, network order */
  buf[5] = buf[8 + 13];
  buf[6] = 0;
  buf[7] = 0;

  ret = wifi_cmd(g_join.inst, SKW_CMD_TX_DATA_FRAME, buf, 8 + mlen, NULL, 0,
                 SKW_CMD_TIMEOUT_MS);
  if (g_tx_dumps < 4)
    {
      g_tx_dumps++;
      syslog(LOG_INFO, "SKW WiFi: tx len %zu type %02x%02x peer %u: %d\n",
             mlen, buf[8 + 12], buf[8 + 13], peer, ret);
    }

  return ret > 0 ? -EIO : ret;
}

static int wifi_tx_frame(FAR const uint8_t *dst, uint16_t proto,
                         FAR const uint8_t *payload, size_t plen)
{
  FAR uint8_t *eth = g_txbuf + 8;
  int ret;

  if (ETH_HLEN + plen > SKW_FRAME_MAX)
    {
      return -E2BIG;
    }

  nxmutex_lock(&g_tx_lock);
  memcpy(eth, dst, 6);
  memcpy(eth + 6, g_mac, 6);
  eth[12] = proto >> 8;
  eth[13] = proto & 0xff;
  memcpy(eth + ETH_HLEN, payload, plen);
  ret = wifi_tx_eth(ETH_HLEN + plen);
  nxmutex_unlock(&g_tx_lock);
  return ret;
}

static int wifi_add_key(uint8_t type, uint8_t cipher, uint8_t idx,
                        FAR const uint8_t *key, uint8_t len)
{
  struct skw_key_param_s k;
  int ret;

  memset(&k, 0, sizeof(k));
  memcpy(k.mac_addr, g_target.bss.bssid, 6);
  k.key_type = type;
  k.cipher_type = cipher;
  k.pn[0] = 1;
  k.key_id = idx;
  k.key_len = len;
  memcpy(k.key, key, len);

  ret = wifi_cmd(g_join.inst, SKW_CMD_ADD_KEY, &k, sizeof(k), NULL, 0,
                 SKW_CMD_TIMEOUT_MS);
  explicit_bzero(&k, sizeof(k));
  syslog(LOG_INFO, "SKW WiFi: ADD_KEY type %u cipher %u idx %u: %d\n",
         type, cipher, idx, ret);
  return ret > 0 ? -EIO : ret;
}

static void wifi_unjoin(void)
{
  struct skw_disconnect_param_s d;
  int ret;

  if (!g_joined)
    {
      return;
    }

  wifi_netdev_link(false);
  memset(&d, 0, sizeof(d));
  d.type = g_connected ? 2 : 0;         /* SEND_DEAUTH : DISCONNECT_ONLY */
  d.local_state_change = !g_connected;
  d.reason_code = 3;                    /* deauth leaving */
  ret = wifi_cmd(g_join.inst, SKW_CMD_DISCONNECT, &d, sizeof(d), NULL, 0,
                 SKW_CMD_TIMEOUT_MS);
  syslog(LOG_INFO, "SKW WiFi: DISCONNECT: %d\n", ret);
  g_joined = false;
  g_connected = false;
}

static void wifi_free_wpa(void)
{
  if (g_wpa != NULL)
    {
      explicit_bzero(g_wpa, sizeof(*g_wpa));
      kmm_free(g_wpa);
      g_wpa = NULL;
    }
}

/* Run the 4-way handshake on EAPOL frames from the data port, bounded */

static int wifi_handshake(FAR struct skw_wpa_s *wpa)
{
  clock_t start = clock_systime_ticks();
  unsigned int i;
  int step;
  int ret;

  while (TICK2MSEC(clock_systime_ticks() - start) < SKW_EAPOL_TIMEOUT_MS)
    {
      if (g_deauth)
        {
          return -ECONNRESET;
        }

      for (i = 0; i < SKW_EAPOL_SLOTS; i++)
        {
          if (g_eapol_len[i] == 0)
            {
              continue;
            }

          step = skw_wpa_rx(wpa, g_eapol[i], g_eapol_len[i]);
          g_eapol_len[i] = 0;
          if (step == SKW_WPA_IGNORE)
            {
              continue;
            }

          ret = wifi_tx_frame(wpa->aa, ETH_P_PAE, wpa->out, wpa->out_len);
          if (ret < 0)
            {
              syslog(LOG_ERR, "SKW WiFi: EAPOL tx %d\n", ret);
              return ret;
            }

          /* 4/4 goes out in the clear; then install PTK and GTK */

          if (step == SKW_WPA_PTK_DONE)
            {
              uint8_t gc = wpa->group_cipher == 2 ? SKW_CIPHER_TYPE_TKIP :
                           SKW_CIPHER_TYPE_CCMP;

              ret = wifi_add_key(SKW_KEY_TYPE_PTK, SKW_CIPHER_TYPE_CCMP, 0,
                                 wpa->ptk + 32, 16);
              if (ret == 0)
                {
                  ret = wifi_add_key(SKW_KEY_TYPE_GTK, gc, wpa->gtk_idx,
                                     wpa->gtk, wpa->gtk_len);
                }

              return ret;
            }
        }

      ret = skw6621s_poll();
      if (ret < 0)
        {
          return ret;
        }

      if (ret == 0)
        {
          up_mdelay(2);
        }
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Network device (wlan0)
 *
 * netdev_lowerhalf on top of the command channel: TX frames go out as
 * TX_DATA_FRAME like EAPOL (one ack each, so this is a bring-up data path,
 * not a fast one); RX frames come from data port 7 through the callback.
 * A kernel thread polls the bus while associated and answers group-key
 * rekeys.
 ****************************************************************************/

#define SKW_POLL_IDLE_US            5000

#ifdef CONFIG_MM_IOB
#ifdef CONFIG_NETDEV_WIRELESS_HANDLER
static const struct wireless_ops_s g_wlan_iw_ops;
#endif

static struct netdev_lowerhalf_s g_wlan;
static netpkt_queue_t g_rxq;
static spinlock_t g_rxq_lock = SP_UNLOCKED;
static bool g_wlan_registered;
static pid_t g_poll_pid = -1;

static int wlan_ifup(FAR struct netdev_lowerhalf_s *dev)
{
  return OK;
}

static int wlan_ifdown(FAR struct netdev_lowerhalf_s *dev)
{
  return OK;
}

static int wlan_transmit(FAR struct netdev_lowerhalf_s *dev,
                         FAR netpkt_t *pkt)
{
  unsigned int len = netpkt_getdatalen(dev, pkt);
  int ret;

  if (!g_connected || len < ETH_HLEN || len > SKW_FRAME_MAX)
    {
      return -ENETDOWN;
    }

  nxmutex_lock(&g_tx_lock);
  netpkt_copyout(dev, g_txbuf + 8, pkt, len, 0);
  ret = wifi_tx_eth(len);
  nxmutex_unlock(&g_tx_lock);
  if (ret < 0)
    {
      NETDEV_TXERRORS(&dev->netdev);
    }

  netpkt_free(dev, pkt, NETPKT_TX);
  netdev_lower_txdone(dev);
  return OK;
}

static FAR netpkt_t *wlan_receive(FAR struct netdev_lowerhalf_s *dev)
{
  FAR netpkt_t *pkt;
  irqstate_t flags;

  flags = spin_lock_irqsave(&g_rxq_lock);
  pkt = netpkt_remove_queue(&g_rxq);
  spin_unlock_irqrestore(&g_rxq_lock, flags);
  return pkt;
}

static const struct netdev_ops_s g_wlan_ops =
{
  .ifup     = wlan_ifup,
  .ifdown   = wlan_ifdown,
  .transmit = wlan_transmit,
  .receive  = wlan_receive,
};

/* Runs in the poller with the bus lock held: copy only, never transmit */

static void wifi_netdev_rx(FAR const uint8_t *frame, size_t len)
{
  FAR struct netdev_lowerhalf_s *dev = &g_wlan;
  FAR netpkt_t *pkt;
  irqstate_t flags;
  int ret;

  if (!g_wlan_registered || !g_connected)
    {
      return;
    }

  pkt = netpkt_alloc(dev, NETPKT_RX);
  if (pkt == NULL)
    {
      NETDEV_RXDROPPED(&dev->netdev);
      return;
    }

  if (netpkt_copyin(dev, pkt, frame, len, 0) < 0)
    {
      netpkt_free(dev, pkt, NETPKT_RX);
      NETDEV_RXDROPPED(&dev->netdev);
      return;
    }

  flags = spin_lock_irqsave(&g_rxq_lock);
  ret = netpkt_tryadd_queue(pkt, &g_rxq);
  spin_unlock_irqrestore(&g_rxq_lock, flags);
  if (ret < 0)
    {
      netpkt_free(dev, pkt, NETPKT_RX);
      NETDEV_RXDROPPED(&dev->netdev);
      return;
    }

  netdev_lower_rxready(dev);
}

static void wifi_netdev_link(bool up)
{
  if (!g_wlan_registered)
    {
      return;
    }

  if (up)
    {
      netdev_lower_carrier_on(&g_wlan);
    }
  else
    {
      netdev_lower_carrier_off(&g_wlan);
    }
}

static int wifi_netdev_register(void)
{
  int ret;

  if (g_wlan_registered)
    {
      return OK;
    }

  memcpy(g_wlan.netdev.d_mac.ether.ether_addr_octet, g_mac, 6);
  strlcpy(g_wlan.netdev.d_ifname, "wlan0", IFNAMSIZ);
  g_wlan.quota[NETPKT_RX] = 2;
  g_wlan.quota[NETPKT_TX] = 1;
  g_wlan.ops = &g_wlan_ops;
#ifdef CONFIG_NETDEV_WIRELESS_HANDLER
  g_wlan.iw_ops = &g_wlan_iw_ops;
#endif

  /* NET_LL_ETHERNET, not IEEE80211: that type needs
   * CONFIG_DRIVERS_IEEE80211, which also makes netinit configure wlan0
   * instead of eth0 at boot.  The wireless ioctls do not care; only
   * SIOCGIWNAME (unused by wapi) answers -ENODEV.
   */

  ret = netdev_lower_register(&g_wlan, NET_LL_ETHERNET);
  syslog(LOG_INFO, "SKW WiFi: wlan0 register %d\n", ret);
  if (ret == OK)
    {
      g_wlan_registered = true;
    }

  return ret;
}
#else
static void wifi_netdev_rx(FAR const uint8_t *frame, size_t len)
{
}

static void wifi_netdev_link(bool up)
{
}
#endif

/* Poll while joined: deliver RX, answer group rekeys, notice deauth */

static int wifi_poll_thread(int argc, FAR char *argv[])
{
  unsigned int i;
  int step;
  int ret;

  for (; ; )
    {
      if (!g_connected)
        {
          nxsig_usleep(50 * 1000);
          continue;
        }

      ret = skw6621s_poll();

      if (g_deauth)
        {
          syslog(LOG_WARNING, "SKW WiFi: link lost, reason %u\n",
                 g_deauth_reason);
          g_connected = false;
          wifi_netdev_link(false);
          continue;
        }

      for (i = 0; g_wpa != NULL && i < SKW_EAPOL_SLOTS; i++)
        {
          if (g_eapol_len[i] == 0)
            {
              continue;
            }

          step = skw_wpa_rx(g_wpa, g_eapol[i], g_eapol_len[i]);
          g_eapol_len[i] = 0;
          if (step == SKW_WPA_IGNORE)
            {
              continue;
            }

          if (wifi_tx_frame(g_wpa->aa, ETH_P_PAE, g_wpa->out,
                            g_wpa->out_len) == 0 &&
              step == SKW_WPA_GTK_DONE)
            {
              wifi_add_key(SKW_KEY_TYPE_GTK, g_wpa->group_cipher == 2 ?
                           SKW_CIPHER_TYPE_TKIP : SKW_CIPHER_TYPE_CCMP,
                           g_wpa->gtk_idx, g_wpa->gtk, g_wpa->gtk_len);
            }
        }

      if (ret <= 0)
        {
          nxsig_usleep(SKW_POLL_IDLE_US);
        }
    }

  return 0;
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
  g_pn_reuse = (chip->priv & (1u << 2)) != 0;
  syslog(LOG_INFO, "SKW WiFi: chip type 0x%" PRIx32 " module 0x%04x, "
         "wifi %s plat %s built %s\n", chip->fw_chip_type,
         chip->calib_module_id, (FAR char *)chip->fw_wifi_ver,
         (FAR char *)chip->fw_plat_ver, (FAR char *)chip->fw_build_time);
  syslog(LOG_INFO, "SKW WiFi: MAC %02x:%02x:%02x:%02x:%02x:%02x, "
         "bw 0x%" PRIx32 ", %u lmac, priv 0x%08" PRIx32 "\n", g_mac[0],
         g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5], chip->fw_bw_capa,
         (unsigned int)(chip->priv >> 24), chip->priv);

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

#ifdef CONFIG_MM_IOB
  /* wlan0 exists from here on (carrier off until joined), so wapi can
   * scan and connect through the standard wireless ioctls.
   */

  wifi_netdev_register();
#endif
  goto out;

fail:
  ret = ret > 0 ? -EIO : ret;

out:
  kmm_free(chip);
  kmm_free(ver);
  return ret;
}

static int wifi_scan_run(void)
{
  uint8_t buf[sizeof(struct skw_scan_param_s) + SKW_SCAN_NCHAN * 3 + 33];
  FAR struct skw_scan_param_s *param = (FAR struct skw_scan_param_s *)buf;
  FAR uint8_t *chan = buf + sizeof(*param);
  clock_t start;
  unsigned int i;
  int ret;

  if (!g_wifi_ready)
    {
      return -ENODEV;
    }

  /* skw_scan: one wildcard SSID, active except on DFS channels */

  memset(buf, 0, sizeof(buf));
  param->nr_chan = SKW_SCAN_NCHAN;
  param->chan_offset = sizeof(*param);
  for (i = 0; i < SKW_SCAN_NCHAN; i++)
    {
      if (i < 13)
        {
          chan[i * 3] = i + 1;
          chan[i * 3 + 1] = SKW_BAND_2GHZ;
        }
      else
        {
          chan[i * 3] = g_chan5g[i - 13];
          chan[i * 3 + 1] = SKW_BAND_5GHZ;
          if (chan[i * 3] >= 52 && chan[i * 3] <= 64)
            {
              chan[i * 3 + 2] = SKW_SCAN_PASSIVE;
            }
        }
    }

  param->n_ssid = 1;
  param->ssid_offset = sizeof(*param) + SKW_SCAN_NCHAN * 3;

  g_nbss = 0;
  g_nreport = 0;
  g_scan_done = false;
  g_scan_valid = false;

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

      printf("  %02x:%02x:%02x:%02x:%02x:%02x  ch %3u  %4d dBm  %s\n",
             b->bssid[0], b->bssid[1], b->bssid[2], b->bssid[3],
             b->bssid[4], b->bssid[5], b->chan, b->signal,
             b->ssid_len ? b->ssid : "<hidden>");
    }

  g_scan_valid = g_scan_done;
  return g_scan_done ? OK : -ETIMEDOUT;
}

int skw6621s_wifi_scan(void)
{
  int ret;

  nxmutex_lock(&g_op_lock);
  ret = wifi_scan_run();
  nxmutex_unlock(&g_op_lock);
  return ret;
}

/* Copy out the last scan, for the settings UI */

int skw6621s_wifi_results(FAR struct skw6621s_ap_s *ap, int max)
{
  int n;

  nxmutex_lock(&g_op_lock);
  for (n = 0; n < max && n < (int)g_nbss; n++)
    {
      FAR const struct skw_bss_s *b = &g_bss[n];

      memcpy(ap[n].bssid, b->bssid, 6);
      strlcpy(ap[n].ssid, b->ssid, sizeof(ap[n].ssid));
      ap[n].signal = b->signal;
      ap[n].chan = b->chan;
      ap[n].band = b->band;
      ap[n].secure = (b->capability & 0x0010) != 0;
    }

  nxmutex_unlock(&g_op_lock);
  return n;
}

/* Connected (keys installed, no deauth since); ssid gets the joined BSS */

bool skw6621s_wifi_status(FAR char *ssid, size_t len)
{
  if (ssid != NULL && len > 0)
    {
      strlcpy(ssid, g_connected ? g_target.bss.ssid : "", len);
    }

  return g_connected;
}

static int wifi_connect_once(FAR const char *ssid,
                             FAR const char *passphrase)
{
  FAR struct skw_join_param_s *join = NULL;
  FAR struct skw_assoc_param_s *assoc = NULL;
  struct skw_auth_param_s auth;
  FAR const uint8_t *rsn;
  FAR struct skw_target_s *t = &g_target;
  FAR uint8_t *b = &t->bss.bssid[0];
  clock_t start;
  int ret;

  if (!g_wifi_ready)
    {
      return -ENODEV;
    }

  wifi_unjoin();
  wifi_free_wpa();

  /* Scan, keeping the strongest BSS whose SSID contains ssid */

  g_have_target = false;
  g_want = ssid;
  ret = wifi_scan_run();
  g_want = NULL;
  if (!g_have_target)
    {
      printf("no BSS matching \"%s\" (scan %d)\n", ssid, ret);
      return -ENOENT;
    }

  printf("target %02x:%02x:%02x:%02x:%02x:%02x ch %u %d dBm \"%s\" "
         "cap 0x%04x bi %u, %u IE bytes\n", b[0], b[1], b[2], b[3], b[4],
         b[5], t->bss.chan, t->bss.signal, t->bss.ssid, t->capability,
         t->beacon_int, t->ie_len);

  rsn = wifi_find_ie(48);
  if (rsn != NULL)
    {
      if (passphrase == NULL)
        {
          return -EINVAL;
        }

      g_wpa = kmm_zalloc(sizeof(*g_wpa));
      if (g_wpa == NULL)
        {
          return -ENOMEM;
        }

      start = clock_systime_ticks();
      ret = skw_wpa_init(g_wpa, passphrase, (FAR const uint8_t *)
                         t->bss.ssid, t->bss.ssid_len, b, g_mac);
      if (ret == 0)
        {
          ret = skw_wpa_select(g_wpa, rsn);
        }

      printf("WPA2-PSK: PMK in %u ms, RSN select %d\n",
             (unsigned int)TICK2MSEC(clock_systime_ticks() - start), ret);
      if (ret < 0)
        {
          goto out;
        }
    }
  else if (t->capability & 0x0010)
    {
      printf("privacy without RSN (WEP / WPA1) not supported\n");
      return -ENOTSUP;
    }

  /* skw_cmd_join: 20 MHz on the primary channel, the scanned IEs */

  join = kmm_zalloc(sizeof(*join) + t->ie_len);
  assoc = kmm_zalloc(sizeof(*assoc) + SKW_WPA_IE_MAX);
  if (join == NULL || assoc == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  join->chan_num = t->bss.chan;
  join->center_chn1 = t->bss.chan;
  join->bandwidth = 0;                  /* SKW_CHAN_WIDTH_20 */
  join->band = t->bss.band;
  join->beacon_interval = t->beacon_int;
  join->capability = t->capability;
  memcpy(join->bssid, b, 6);
  join->bss_ie_offset = sizeof(*join);
  join->bss_ie_len = t->ie_len;
  memcpy(join + 1, t->ies, t->ie_len);

  g_deauth = false;
  g_mgmt_seen = false;
  memset((FAR void *)g_eapol_len, 0, sizeof(g_eapol_len));
  g_data_dumps = 0;
  g_tx_dumps = 0;

  ret = wifi_cmd(0, SKW_CMD_JOIN, join, sizeof(*join) + t->ie_len,
                 &g_join, sizeof(g_join), SKW_CMD_TIMEOUT_MS);
  printf("JOIN %d: peer %u lmac %u inst %u mc %u\n", ret, g_join.peer_idx,
         g_join.lmac_id, g_join.inst, g_join.multicast_idx);
  if (ret != 0)
    {
      goto out;
    }

  g_joined = true;

  /* skw_cmd_auth: open system, answered by the AP's auth frame */

  memset(&auth, 0, sizeof(auth));
  ret = wifi_cmd(g_join.inst, SKW_CMD_AUTH, &auth, sizeof(auth), NULL, 0,
                 SKW_CMD_TIMEOUT_MS);
  if (ret == 0)
    {
      ret = wifi_wait_mgmt(WLAN_FC_AUTH);
    }

  printf("AUTH %d\n", ret);
  if (ret != 0)
    {
      /* A positive status is the AP refusing us (e.g. band steering a
       * dual-band client off 2.4 GHz); the caller may try another BSS.
       */

      ret = ret > 0 ? -EACCES : ret;
      goto out;
    }

  /* skw_cmd_assoc: our RSN IE as the request IEs */

  g_mgmt_seen = false;
  memcpy(assoc->bssid, b, 6);
  assoc->req_ie_offset = sizeof(*assoc);
  if (g_wpa != NULL)
    {
      assoc->req_ie_len = g_wpa->ie_len;
      memcpy(assoc + 1, g_wpa->ie, g_wpa->ie_len);
    }

  ret = wifi_cmd(g_join.inst, SKW_CMD_ASSOC, assoc,
                 sizeof(*assoc) + assoc->req_ie_len, NULL, 0,
                 SKW_CMD_TIMEOUT_MS);
  if (ret == 0)
    {
      ret = wifi_wait_mgmt(WLAN_FC_ASSOC_RESP);
    }

  printf("ASSOC %d\n", ret);
  if (ret != 0)
    {
      goto out;
    }

  if (g_wpa != NULL)
    {
      start = clock_systime_ticks();
      ret = wifi_handshake(g_wpa);
      printf("4-way handshake %d in %u ms\n", ret,
             (unsigned int)TICK2MSEC(clock_systime_ticks() - start));
      if (ret != 0)
        {
          goto out;
        }
    }

  g_connected = true;
  printf("connected to \"%s\"\n", t->bss.ssid);

#ifdef CONFIG_MM_IOB
  if (wifi_netdev_register() == OK)
    {
      wifi_netdev_link(true);
    }

  if (g_poll_pid < 0)
    {
      g_poll_pid = kthread_create("skwifi_rx", 110, 4096, wifi_poll_thread,
                                  NULL);
    }
#endif

out:
  if (ret != 0)
    {
      if (g_deauth)
        {
          printf("deauth / disconnect, reason %u\n", g_deauth_reason);
        }

      wifi_unjoin();
      wifi_free_wpa();
      ret = ret > 0 ? -EIO : ret;
    }

  kmm_free(join);
  kmm_free(assoc);
  return ret;
}

static int wifi_connect(FAR const char *ssid, FAR const char *passphrase)
{
  int ret;

  g_skip_valid = false;
  ret = wifi_connect_once(ssid, passphrase);
  if (ret == -EACCES)
    {
      memcpy(g_skip_bssid, g_target.bss.bssid, 6);
      g_skip_valid = true;
      printf("auth refused, trying another BSS\n");
      ret = wifi_connect_once(ssid, passphrase);
      g_skip_valid = false;
    }

  return ret;
}

int skw6621s_wifi_connect(FAR const char *ssid, FAR const char *passphrase)
{
  int ret;

  nxmutex_lock(&g_op_lock);
  ret = wifi_connect(ssid, passphrase);
  nxmutex_unlock(&g_op_lock);
  return ret;
}

static int wifi_disconnect(void)
{
  if (!g_joined)
    {
      return -ENOTCONN;
    }

  wifi_unjoin();
  wifi_free_wpa();
  return OK;
}

int skw6621s_wifi_disconnect(void)
{
  int ret;

  nxmutex_lock(&g_op_lock);
  ret = wifi_disconnect();
  nxmutex_unlock(&g_op_lock);
  return ret;
}

/****************************************************************************
 * Standard wireless ioctls (wapi)
 *
 * netdev_upperhalf turns SIOCSIW* / SIOCGIW* on wlan0 into these handlers,
 * so `wapi scan wlan0`, `wapi psk wlan0 <pass> 3 2`, `wapi essid wlan0
 * <ssid> 1` work as on any NuttX WiFi driver.  They run in the caller's
 * thread and block for the whole scan / connect (a few seconds, bounded
 * by SKW_SCAN_TIMEOUT_MS and the join timeouts), like the NSH path.
 *
 * Only station mode with open or WPA2-PSK (CCMP): what the join and the
 * in-driver 4-way handshake above support.  The chip itself is started by
 * the board (kickpi_k7_wifi_initialize, ~40 s of firmware load); wlan0
 * does not exist before that, so none of this can run on a cold chip.
 ****************************************************************************/

#if defined(CONFIG_MM_IOB) && defined(CONFIG_NETDEV_WIRELESS_HANDLER)

static char g_iw_essid[33];             /* set by SIOCSIWESSID */
static char g_iw_psk[64];               /* set by SIOCSIWENCODEEXT, "" = open */
static uint32_t g_iw_wpa_version = IW_AUTH_WPA_VERSION_WPA2;
static uint32_t g_iw_cipher = IW_AUTH_CIPHER_CCMP;

static unsigned int wifi_chan_mhz(uint8_t chan, uint8_t band)
{
  if (band != SKW_BAND_2GHZ)
    {
      return 5000 + 5 * chan;
    }

  return chan == 14 ? 2484 : 2407 + 5 * chan;
}

static int wlan_iw_connect(FAR struct netdev_lowerhalf_s *dev)
{
  int ret;

  if (g_iw_essid[0] == '\0')
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_op_lock);
  g_want_exact = true;
  ret = wifi_connect(g_iw_essid, g_iw_psk[0] != '\0' ? g_iw_psk : NULL);
  g_want_exact = false;
  nxmutex_unlock(&g_op_lock);
  return ret;
}

static int wlan_iw_disconnect(FAR struct netdev_lowerhalf_s *dev)
{
  int ret;

  nxmutex_lock(&g_op_lock);
  ret = wifi_disconnect();
  nxmutex_unlock(&g_op_lock);
  return ret == -ENOTCONN ? OK : ret;
}

static int wlan_iw_essid(FAR struct netdev_lowerhalf_s *dev,
                         FAR struct iwreq *iwr, bool set)
{
  FAR const char *ssid;
  size_t len;

  if (set)
    {
      len = iwr->u.essid.length;
      if (iwr->u.essid.pointer == NULL || len == 0 ||
          len >= sizeof(g_iw_essid))
        {
          return -EINVAL;
        }

      memcpy(g_iw_essid, iwr->u.essid.pointer, len);
      g_iw_essid[len] = '\0';
      return OK;
    }

  ssid = g_connected ? g_target.bss.ssid : g_iw_essid;
  len = strlen(ssid);
  if (iwr->u.essid.pointer == NULL || iwr->u.essid.length < len)
    {
      return -E2BIG;
    }

  memcpy(iwr->u.essid.pointer, ssid, len);
  if (iwr->u.essid.length > len)
    {
      ((FAR char *)iwr->u.essid.pointer)[len] = '\0';
    }

  iwr->u.essid.length = len;
  iwr->u.essid.flags = g_connected ? IW_ESSID_ON : IW_ESSID_OFF;
  return OK;
}

static int wlan_iw_bssid(FAR struct netdev_lowerhalf_s *dev,
                         FAR struct iwreq *iwr, bool set)
{
  if (set)
    {
      return -ENOTSUP;                  /* joins by SSID only */
    }

  iwr->u.ap_addr.sa_family = ARPHRD_ETHER;
  memset(iwr->u.ap_addr.sa_data, 0, sizeof(iwr->u.ap_addr.sa_data));
  if (g_connected)
    {
      memcpy(iwr->u.ap_addr.sa_data, g_target.bss.bssid, 6);
    }

  return OK;
}

/* The key is the passphrase (8..63 characters); it never goes back out */

static int wlan_iw_passwd(FAR struct netdev_lowerhalf_s *dev,
                          FAR struct iwreq *iwr, bool set)
{
  FAR struct iw_encode_ext *ext = iwr->u.encoding.pointer;

  if (ext == NULL || iwr->u.encoding.length < sizeof(*ext))
    {
      return -EINVAL;
    }

  if (!set)
    {
      ext->alg = g_iw_psk[0] != '\0' ? IW_ENCODE_ALG_CCMP :
                                       IW_ENCODE_ALG_NONE;
      ext->key_len = 0;
      return OK;
    }

  if (ext->alg == IW_ENCODE_ALG_NONE || ext->key_len == 0)
    {
      memset(g_iw_psk, 0, sizeof(g_iw_psk));
      return OK;
    }

  if (ext->alg != IW_ENCODE_ALG_CCMP)
    {
      return -ENOTSUP;                  /* no WEP / TKIP-only */
    }

  if (ext->key_len < 8 || ext->key_len >= sizeof(g_iw_psk) ||
      iwr->u.encoding.length < sizeof(*ext) + ext->key_len)
    {
      return -EINVAL;                   /* 64 hex digits (raw PMK) too */
    }

  memcpy(g_iw_psk, ext->key, ext->key_len);
  g_iw_psk[ext->key_len] = '\0';
  return OK;
}

static int wlan_iw_mode(FAR struct netdev_lowerhalf_s *dev,
                        FAR struct iwreq *iwr, bool set)
{
  if (set)
    {
      return iwr->u.mode == IW_MODE_INFRA ? OK : -ENOTSUP;
    }

  iwr->u.mode = IW_MODE_INFRA;
  return OK;
}

static int wlan_iw_auth(FAR struct netdev_lowerhalf_s *dev,
                        FAR struct iwreq *iwr, bool set)
{
  int idx = iwr->u.param.flags & IW_AUTH_INDEX;

  switch (idx)
    {
      case IW_AUTH_WPA_VERSION:
        if (set)
          {
            if (iwr->u.param.value & (IW_AUTH_WPA_VERSION_WPA |
                                      IW_AUTH_WPA_VERSION_WPA3))
              {
                return -ENOTSUP;        /* WPA2-PSK only */
              }

            g_iw_wpa_version = iwr->u.param.value;
          }
        else
          {
            iwr->u.param.value = g_iw_wpa_version;
          }

        return OK;

      case IW_AUTH_CIPHER_PAIRWISE:
        if (set)
          {
            if (iwr->u.param.value != IW_AUTH_CIPHER_CCMP &&
                iwr->u.param.value != IW_AUTH_CIPHER_NONE)
              {
                return -ENOTSUP;
              }

            g_iw_cipher = iwr->u.param.value;
          }
        else
          {
            iwr->u.param.value = g_iw_cipher;
          }

        return OK;

      default:
        return set ? OK : -ENOTSUP;
    }
}

static int wlan_iw_freq(FAR struct netdev_lowerhalf_s *dev,
                        FAR struct iwreq *iwr, bool set)
{
  if (set)
    {
      return -ENOTSUP;
    }

  if (!g_connected)
    {
      return -ENOTCONN;
    }

  iwr->u.freq.flags = IW_FREQ_FIXED;
  iwr->u.freq.e = -1;
  iwr->u.freq.m = wifi_chan_mhz(g_target.bss.chan, g_target.bss.band) * 10;
  return OK;
}

/* One BSS as wireless-extension events, laid out like wifi_sim.c */

static int wlan_iw_put_bss(FAR char *buf, size_t size, size_t *off,
                           FAR const struct skw_bss_s *b)
{
  FAR struct iw_event *iwe;
  FAR char *p = buf + *off;
  size_t need;
  size_t slen = b->ssid_len < 32 ? b->ssid_len : 32;

  need = IW_EV_LEN(ap_addr) + IW_EV_LEN(essid) + ((slen + 3) & ~3) +
         IW_EV_LEN(qual) + IW_EV_LEN(freq) + IW_EV_LEN(data);
  *off += need;
  if (*off > size)
    {
      return -E2BIG;
    }

  iwe = (FAR struct iw_event *)p;
  iwe->cmd = SIOCGIWAP;
  iwe->u.ap_addr.sa_family = ARPHRD_ETHER;
  memcpy(iwe->u.ap_addr.sa_data, b->bssid, 6);
  iwe->len = IW_EV_LEN(ap_addr);
  p += iwe->len;

  iwe = (FAR struct iw_event *)p;
  iwe->cmd = SIOCGIWESSID;
  iwe->u.essid.flags = 0;
  iwe->u.essid.length = slen;
  iwe->u.essid.pointer = (FAR void *)sizeof(iwe->u.essid);
  memcpy(&iwe->u.essid + 1, b->ssid, slen);
  iwe->len = IW_EV_LEN(essid) + ((slen + 3) & ~3);
  p += iwe->len;

  iwe = (FAR struct iw_event *)p;
  iwe->cmd = IWEVQUAL;
  iwe->u.qual.qual = 0;
  iwe->u.qual.level = (uint8_t)b->signal;
  iwe->u.qual.noise = 0;
  iwe->u.qual.updated = IW_QUAL_DBM | IW_QUAL_ALL_UPDATED;
  iwe->len = IW_EV_LEN(qual);
  p += iwe->len;

  iwe = (FAR struct iw_event *)p;
  iwe->cmd = SIOCGIWFREQ;
  iwe->u.freq.e = -1;
  iwe->u.freq.m = wifi_chan_mhz(b->chan, b->band) * 10;
  iwe->len = IW_EV_LEN(freq);
  p += iwe->len;

  iwe = (FAR struct iw_event *)p;
  iwe->cmd = SIOCGIWENCODE;
  iwe->u.data.flags = (b->capability & 0x0010) ?
                      IW_ENCODE_ENABLED | IW_ENCODE_NOKEY :
                      IW_ENCODE_DISABLED;
  iwe->u.data.length = 0;
  iwe->u.data.pointer = NULL;
  iwe->len = IW_EV_LEN(data);
  return OK;
}

/* set: scan now (blocks, up to SKW_SCAN_TIMEOUT_MS).  get: the last
 * finished scan, -EAGAIN while one is running or none has finished,
 * -E2BIG with the needed length in u.data.length if the buffer is short.
 */

static int wlan_iw_scan(FAR struct netdev_lowerhalf_s *dev,
                        FAR struct iwreq *iwr, bool set)
{
  size_t off = 0;
  unsigned int i;
  int ret = OK;

  if (set)
    {
      if (g_connected)
        {
          /* Scanning off-channel while joined is not something this
           * driver has been tested with; the joined BSS is enough.
           */

          return -EBUSY;
        }

      nxmutex_lock(&g_op_lock);
      ret = wifi_scan_run();
      nxmutex_unlock(&g_op_lock);
      return ret;
    }

  if (nxmutex_trylock(&g_op_lock) < 0)
    {
      return -EAGAIN;
    }

  if (!g_scan_valid)
    {
      nxmutex_unlock(&g_op_lock);
      return -EAGAIN;
    }

  for (i = 0; i < g_nbss; i++)
    {
      if (wlan_iw_put_bss(iwr->u.data.pointer, iwr->u.data.length, &off,
                          &g_bss[i]) < 0)
        {
          ret = -E2BIG;
        }
    }

  nxmutex_unlock(&g_op_lock);
  if (ret == -E2BIG)
    {
      iwr->u.data.length = off;
      return ret;
    }

  iwr->u.data.length = off;
  return OK;
}

static const struct wireless_ops_s g_wlan_iw_ops =
{
  .connect    = wlan_iw_connect,
  .disconnect = wlan_iw_disconnect,
  .essid      = wlan_iw_essid,
  .bssid      = wlan_iw_bssid,
  .passwd     = wlan_iw_passwd,
  .mode       = wlan_iw_mode,
  .auth       = wlan_iw_auth,
  .freq       = wlan_iw_freq,
  .scan       = wlan_iw_scan,
};

#endif /* CONFIG_MM_IOB && CONFIG_NETDEV_WIRELESS_HANDLER */
