/* SPDX-License-Identifier: Apache-2.0 */

/****************************************************************************
 * Minimal WPA2-PSK supplicant (IEEE 802.11i 4-way and group handshakes,
 * CCMP pairwise, CCMP or TKIP group, no PMF).
 ****************************************************************************/

#ifndef __CONTEST_SKW6621S_WPA_H
#define __CONTEST_SKW6621S_WPA_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SKW_WPA_IE_MAX        64
#define SKW_WPA_EAPOL_MAX     256

/* skw_wpa_rx() results */

#define SKW_WPA_IGNORE        0   /* nothing to do */
#define SKW_WPA_REPLY         1   /* send the reply frame */
#define SKW_WPA_PTK_DONE      2   /* send the reply, then install PTK + GTK */
#define SKW_WPA_GTK_DONE      3   /* send the reply, then install the GTK */

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct skw_wpa_s
{
  uint8_t pmk[32];
  uint8_t aa[6];                  /* authenticator (BSSID) */
  uint8_t spa[6];                 /* supplicant (our MAC) */
  uint8_t anonce[32];
  uint8_t snonce[32];
  uint8_t ptk[48];                /* KCK | KEK | TK */
  uint8_t replay[8];
  bool replay_valid;
  bool ptk_valid;

  uint8_t ie[SKW_WPA_IE_MAX];     /* our RSN IE, sent in assoc and msg 2 */
  size_t ie_len;
  uint8_t group_cipher;           /* RSN suite type: 2 TKIP, 4 CCMP */

  uint8_t gtk[32];
  uint8_t gtk_len;
  uint8_t gtk_idx;

  uint8_t out[SKW_WPA_EAPOL_MAX]; /* reply EAPOL frame (no Ethernet header) */
  size_t out_len;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* PMK = PBKDF2-HMAC-SHA1(passphrase, ssid, 4096, 32) */

int skw_wpa_init(FAR struct skw_wpa_s *wpa, FAR const char *passphrase,
                 FAR const uint8_t *ssid, size_t ssid_len,
                 FAR const uint8_t *aa, FAR const uint8_t *spa);

/* Check the AP's RSN IE (element 48 incl. header) and build ours.
 * Needs PSK AKM and CCMP pairwise; returns -ENOTSUP otherwise.
 */

int skw_wpa_select(FAR struct skw_wpa_s *wpa, FAR const uint8_t *rsn);

/* Handle one received EAPOL frame (after the Ethernet header) */

int skw_wpa_rx(FAR struct skw_wpa_s *wpa, FAR const uint8_t *frame,
               size_t len);

#endif /* __CONTEST_SKW6621S_WPA_H */
