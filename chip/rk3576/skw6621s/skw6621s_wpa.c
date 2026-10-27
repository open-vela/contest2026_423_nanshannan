/* SPDX-License-Identifier: Apache-2.0 */

/****************************************************************************
 * Minimal WPA2-PSK supplicant for the SKW6621S station.
 *
 * The CP firmware runs in STA_SME_EXT mode: the host owns the RSN
 * handshake and installs the keys with ADD_KEY.  This file only does the
 * IEEE 802.11i-2004 key hierarchy and EAPOL-Key frames (descriptor
 * version 2: HMAC-SHA1-128 MIC, AES key wrap), on top of the NuttX
 * kernel crypto (sha1, AES key wrap).  HMAC is built here on sha1*()
 * because crypto/hmac.c exports its final step as "hmca_sha1_final".
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>

#include <crypto/aes.h>
#include <crypto/key_wrap.h>
#include <crypto/sha1.h>

#include "skw6621s_wpa.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* EAPOL-Key frame offsets (from the EAPOL header) */

#define EK_TYPE          1
#define EK_LEN           2
#define EK_DESC          4
#define EK_INFO          5
#define EK_KEYLEN        7
#define EK_REPLAY        9
#define EK_NONCE         17
#define EK_MIC           81
#define EK_DATALEN       97
#define EK_DATA          99

#define EAPOL_KEY        3
#define EAPOL_DESC_RSN   2

/* Key information */

#define KI_VER_MASK      0x0007
#define KI_VER_AES       2
#define KI_PAIRWISE      0x0008
#define KI_INSTALL       0x0040
#define KI_ACK           0x0080
#define KI_MIC           0x0100
#define KI_SECURE        0x0200
#define KI_ENCDATA       0x1000

#define RSN_CIPHER_TKIP  2
#define RSN_CIPHER_CCMP  4
#define RSN_AKM_PSK      2
#define RSN_CAP_MFPR     0x0040

#define WPA_PBKDF2_ITER  4096

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const uint8_t g_rsn_oui[3] =
{
  0x00, 0x0f, 0xac
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint16_t be16(FAR const uint8_t *p)
{
  return p[0] << 8 | p[1];
}

static void put_be16(FAR uint8_t *p, uint16_t v)
{
  p[0] = v >> 8;
  p[1] = v & 0xff;
}

/* HMAC-SHA1 with the key pads hashed once (PBKDF2 reuses them) */

struct hmac_s
{
  SHA1_CTX inner;
  SHA1_CTX outer;
};

static void hmac_key(FAR struct hmac_s *h, FAR const uint8_t *key,
                     size_t klen)
{
  uint8_t pad[SHA1_BLOCK_LENGTH];
  uint8_t kd[SHA1_DIGEST_LENGTH];
  int i;

  if (klen > SHA1_BLOCK_LENGTH)
    {
      SHA1_CTX c;

      sha1init(&c);
      sha1update(&c, key, klen);
      sha1final(kd, &c);
      key = kd;
      klen = sizeof(kd);
    }

  memset(pad, 0x36, sizeof(pad));
  for (i = 0; i < klen; i++)
    {
      pad[i] ^= key[i];
    }

  sha1init(&h->inner);
  sha1update(&h->inner, pad, sizeof(pad));

  memset(pad, 0x5c, sizeof(pad));
  for (i = 0; i < klen; i++)
    {
      pad[i] ^= key[i];
    }

  sha1init(&h->outer);
  sha1update(&h->outer, pad, sizeof(pad));
  explicit_bzero(pad, sizeof(pad));
  explicit_bzero(kd, sizeof(kd));
}

static void hmac_run(FAR const struct hmac_s *h, FAR const uint8_t *d1,
                     size_t l1, FAR const uint8_t *d2, size_t l2,
                     FAR uint8_t *out)
{
  SHA1_CTX c = h->inner;
  uint8_t ih[SHA1_DIGEST_LENGTH];

  sha1update(&c, d1, l1);
  if (l2 > 0)
    {
      sha1update(&c, d2, l2);
    }

  sha1final(ih, &c);

  c = h->outer;
  sha1update(&c, ih, sizeof(ih));
  sha1final(out, &c);
}

static void pbkdf2_sha1(FAR const char *pass, FAR const uint8_t *salt,
                        size_t slen, FAR uint8_t *out, size_t olen)
{
  struct hmac_s h;
  uint8_t u[SHA1_DIGEST_LENGTH];
  uint8_t t[SHA1_DIGEST_LENGTH];
  uint8_t cnt[4];
  uint32_t blk;
  size_t n;
  int i;
  int j;

  hmac_key(&h, (FAR const uint8_t *)pass, strlen(pass));

  for (blk = 1; olen > 0; blk++)
    {
      cnt[0] = blk >> 24;
      cnt[1] = blk >> 16;
      cnt[2] = blk >> 8;
      cnt[3] = blk;
      hmac_run(&h, salt, slen, cnt, 4, u);
      memcpy(t, u, sizeof(t));

      for (i = 1; i < WPA_PBKDF2_ITER; i++)
        {
          hmac_run(&h, u, sizeof(u), NULL, 0, u);
          for (j = 0; j < sizeof(t); j++)
            {
              t[j] ^= u[j];
            }
        }

      n = olen < sizeof(t) ? olen : sizeof(t);
      memcpy(out, t, n);
      out += n;
      olen -= n;
    }

  explicit_bzero(&h, sizeof(h));
  explicit_bzero(u, sizeof(u));
  explicit_bzero(t, sizeof(t));
}

/* IEEE 802.11i PRF-n: HMAC-SHA1(K, A || 0 || B || i) */

static void prf_sha1(FAR const uint8_t *key, size_t klen,
                     FAR const char *label, FAR const uint8_t *data,
                     size_t dlen, FAR uint8_t *out, size_t olen)
{
  struct hmac_s h;
  uint8_t a[64];
  uint8_t b[128];
  uint8_t d[SHA1_DIGEST_LENGTH];
  size_t alen = strlen(label) + 1;
  size_t n;
  uint8_t i;

  memcpy(a, label, alen);       /* includes the 0 separator */
  memcpy(b, data, dlen);
  hmac_key(&h, key, klen);

  for (i = 0; olen > 0; i++)
    {
      b[dlen] = i;
      hmac_run(&h, a, alen, b, dlen + 1, d);
      n = olen < sizeof(d) ? olen : sizeof(d);
      memcpy(out, d, n);
      out += n;
      olen -= n;
    }

  explicit_bzero(&h, sizeof(h));
  explicit_bzero(d, sizeof(d));
}

static void wpa_derive_ptk(FAR struct skw_wpa_s *wpa)
{
  uint8_t data[6 + 6 + 32 + 32];
  bool alo = memcmp(wpa->aa, wpa->spa, 6) < 0;
  bool nlo = memcmp(wpa->anonce, wpa->snonce, 32) < 0;

  memcpy(data, alo ? wpa->aa : wpa->spa, 6);
  memcpy(data + 6, alo ? wpa->spa : wpa->aa, 6);
  memcpy(data + 12, nlo ? wpa->anonce : wpa->snonce, 32);
  memcpy(data + 44, nlo ? wpa->snonce : wpa->anonce, 32);

  prf_sha1(wpa->pmk, sizeof(wpa->pmk), "Pairwise key expansion",
           data, sizeof(data), wpa->ptk, sizeof(wpa->ptk));
}

static void wpa_mic(FAR const struct skw_wpa_s *wpa, FAR const uint8_t *f,
                    size_t len, FAR uint8_t *mic)
{
  struct hmac_s h;
  uint8_t d[SHA1_DIGEST_LENGTH];

  hmac_key(&h, wpa->ptk, 16);   /* KCK */
  hmac_run(&h, f, len, NULL, 0, d);
  memcpy(mic, d, 16);
  explicit_bzero(&h, sizeof(h));
}

static bool wpa_mic_ok(FAR const struct skw_wpa_s *wpa,
                       FAR const uint8_t *f, size_t len)
{
  uint8_t copy[SKW_WPA_EAPOL_MAX * 2];
  uint8_t mic[16];

  if (len > sizeof(copy))
    {
      return false;
    }

  memcpy(copy, f, len);
  memset(copy + EK_MIC, 0, 16);
  wpa_mic(wpa, copy, len, mic);
  return memcmp(mic, f + EK_MIC, 16) == 0;
}

/* Build an EAPOL-Key reply into wpa->out and sign it */

static void wpa_reply(FAR struct skw_wpa_s *wpa, uint8_t version,
                      uint16_t info, FAR const uint8_t *replay,
                      FAR const uint8_t *nonce, FAR const uint8_t *data,
                      size_t dlen)
{
  FAR uint8_t *o = wpa->out;
  size_t len = EK_DATA + dlen;

  memset(o, 0, len);
  o[0] = version;
  o[EK_TYPE] = EAPOL_KEY;
  put_be16(o + EK_LEN, len - 4);
  o[EK_DESC] = EAPOL_DESC_RSN;
  put_be16(o + EK_INFO, info);
  memcpy(o + EK_REPLAY, replay, 8);
  if (nonce != NULL)
    {
      memcpy(o + EK_NONCE, nonce, 32);
    }

  put_be16(o + EK_DATALEN, dlen);
  if (dlen > 0)
    {
      memcpy(o + EK_DATA, data, dlen);
    }

  wpa_mic(wpa, o, len, o + EK_MIC);
  wpa->out_len = len;
}

/* Decrypt the key data and pick the GTK KDE (00-0f-ac:1) */

static int wpa_gtk(FAR struct skw_wpa_s *wpa, FAR const uint8_t *f,
                   uint16_t info)
{
  aes_key_wrap_ctx kw;
  uint8_t plain[SKW_WPA_EAPOL_MAX];
  size_t dlen = be16(f + EK_DATALEN);
  FAR const uint8_t *p;
  size_t len;
  size_t off;
  int ret;

  if ((info & KI_ENCDATA) == 0 || dlen < 24 || (dlen % 8) != 0 ||
      dlen - 8 > sizeof(plain))
    {
      return -EINVAL;
    }

  aes_key_wrap_set_key(&kw, wpa->ptk + 16, 16);    /* KEK */
  ret = aes_key_unwrap(&kw, f + EK_DATA, plain, dlen / 8 - 1);
  explicit_bzero(&kw, sizeof(kw));
  if (ret != 0)
    {
      syslog(LOG_ERR, "SKW WPA: key data unwrap failed\n");
      return -EBADMSG;
    }

  p = plain;
  len = dlen - 8;
  for (off = 0; off + 2 <= len; off += 2 + p[off + 1])
    {
      uint8_t elen = p[off + 1];

      if (p[off] == 0xdd && elen == 0)
        {
          break;                /* padding */
        }

      if (off + 2 + elen > len)
        {
          break;
        }

      if (p[off] == 0xdd && elen >= 6 + 5 &&
          memcmp(p + off + 2, g_rsn_oui, 3) == 0 && p[off + 5] == 1)
        {
          size_t glen = elen - 6;

          if (glen > sizeof(wpa->gtk))
            {
              break;
            }

          wpa->gtk_idx = p[off + 6] & 0x03;
          wpa->gtk_len = glen;
          memcpy(wpa->gtk, p + off + 8, glen);
          explicit_bzero(plain, sizeof(plain));
          return OK;
        }
    }

  explicit_bzero(plain, sizeof(plain));
  syslog(LOG_ERR, "SKW WPA: no GTK in key data\n");
  return -ENOENT;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int skw_wpa_init(FAR struct skw_wpa_s *wpa, FAR const char *passphrase,
                 FAR const uint8_t *ssid, size_t ssid_len,
                 FAR const uint8_t *aa, FAR const uint8_t *spa)
{
  size_t plen = strlen(passphrase);

  if (plen < 8 || plen > 63 || ssid_len == 0 || ssid_len > 32)
    {
      return -EINVAL;
    }

  memset(wpa, 0, sizeof(*wpa));
  memcpy(wpa->aa, aa, 6);
  memcpy(wpa->spa, spa, 6);
  pbkdf2_sha1(passphrase, ssid, ssid_len, wpa->pmk, sizeof(wpa->pmk));
  return OK;
}

int skw_wpa_select(FAR struct skw_wpa_s *wpa, FAR const uint8_t *rsn)
{
  FAR const uint8_t *p = rsn + 2;
  FAR const uint8_t *end = rsn + 2 + rsn[1];
  bool ccmp = false;
  bool psk = false;
  uint16_t n;
  uint16_t caps = 0;

  /* version, group suite */

  if (rsn[0] != 48 || rsn[1] < 2 + 4 || be16(p) != 0x0100)
    {
      return -EINVAL;
    }

  p += 2;
  if (memcmp(p, g_rsn_oui, 3) != 0 ||
      (p[3] != RSN_CIPHER_CCMP && p[3] != RSN_CIPHER_TKIP))
    {
      return -ENOTSUP;
    }

  wpa->group_cipher = p[3];
  p += 4;

  /* pairwise suites */

  if (p + 2 > end)
    {
      return -EINVAL;
    }

  n = p[0] | p[1] << 8;
  p += 2;
  for (; n > 0 && p + 4 <= end; n--, p += 4)
    {
      if (memcmp(p, g_rsn_oui, 3) == 0 && p[3] == RSN_CIPHER_CCMP)
        {
          ccmp = true;
        }
    }

  /* AKM suites */

  if (p + 2 > end)
    {
      return -EINVAL;
    }

  n = p[0] | p[1] << 8;
  p += 2;
  for (; n > 0 && p + 4 <= end; n--, p += 4)
    {
      if (memcmp(p, g_rsn_oui, 3) == 0 && p[3] == RSN_AKM_PSK)
        {
          psk = true;
        }
    }

  if (p + 2 <= end)
    {
      caps = p[0] | p[1] << 8;
    }

  syslog(LOG_INFO, "SKW WPA: AP group %u, ccmp %d, psk %d, caps 0x%04x\n",
         wpa->group_cipher, ccmp, psk, caps);

  if (!ccmp || !psk || (caps & RSN_CAP_MFPR) != 0)
    {
      return -ENOTSUP;
    }

  /* Ours: v1, AP's group, CCMP pairwise, PSK, no capabilities */

  p = (FAR const uint8_t *)"\x30\x14\x01\x00"
      "\x00\x0f\xac\x04"
      "\x01\x00\x00\x0f\xac\x04"
      "\x01\x00\x00\x0f\xac\x02"
      "\x00\x00";
  memcpy(wpa->ie, p, 22);
  wpa->ie[7] = wpa->group_cipher;
  wpa->ie_len = 22;
  return OK;
}

int skw_wpa_rx(FAR struct skw_wpa_s *wpa, FAR const uint8_t *f, size_t len)
{
  uint16_t info;
  size_t flen;
  int ret;

  if (len < EK_DATA || f[EK_TYPE] != EAPOL_KEY || f[EK_DESC] !=
      EAPOL_DESC_RSN)
    {
      return SKW_WPA_IGNORE;
    }

  flen = 4 + be16(f + EK_LEN);
  if (flen > len || flen < EK_DATA + be16(f + EK_DATALEN))
    {
      syslog(LOG_WARNING, "SKW WPA: short EAPOL-Key %zu/%zu\n", len, flen);
      return SKW_WPA_IGNORE;
    }

  info = be16(f + EK_INFO);
  if ((info & KI_VER_MASK) != KI_VER_AES || (info & KI_ACK) == 0)
    {
      syslog(LOG_WARNING, "SKW WPA: key info 0x%04x unsupported\n", info);
      return SKW_WPA_IGNORE;
    }

  if (wpa->replay_valid && memcmp(f + EK_REPLAY, wpa->replay, 8) <= 0)
    {
      syslog(LOG_WARNING, "SKW WPA: replayed EAPOL-Key\n");
      return SKW_WPA_IGNORE;
    }

  if ((info & KI_PAIRWISE) != 0 && (info & KI_MIC) == 0)
    {
      /* Message 1/4: new ANonce, fresh SNonce, PTK, reply 2/4 */

      memcpy(wpa->anonce, f + EK_NONCE, 32);
      arc4random_buf(wpa->snonce, sizeof(wpa->snonce));
      wpa_derive_ptk(wpa);
      wpa->ptk_valid = true;

      wpa_reply(wpa, f[0], KI_VER_AES | KI_PAIRWISE | KI_MIC,
                f + EK_REPLAY, wpa->snonce, wpa->ie, wpa->ie_len);
      syslog(LOG_INFO, "SKW WPA: 1/4 -> 2/4\n");
      return SKW_WPA_REPLY;
    }

  if (!wpa->ptk_valid || !wpa_mic_ok(wpa, f, flen))
    {
      syslog(LOG_ERR, "SKW WPA: MIC check failed (info 0x%04x)\n", info);
      return SKW_WPA_IGNORE;
    }

  if ((info & KI_PAIRWISE) != 0)
    {
      /* Message 3/4: same ANonce, GTK in the encrypted key data */

      if ((info & KI_INSTALL) == 0 ||
          memcmp(wpa->anonce, f + EK_NONCE, 32) != 0)
        {
          syslog(LOG_ERR, "SKW WPA: bad 3/4 (info 0x%04x)\n", info);
          return SKW_WPA_IGNORE;
        }

      ret = wpa_gtk(wpa, f, info);
      if (ret < 0)
        {
          return SKW_WPA_IGNORE;
        }

      memcpy(wpa->replay, f + EK_REPLAY, 8);
      wpa->replay_valid = true;
      wpa_reply(wpa, f[0], KI_VER_AES | KI_PAIRWISE | KI_MIC | KI_SECURE,
                f + EK_REPLAY, NULL, NULL, 0);
      syslog(LOG_INFO, "SKW WPA: 3/4 -> 4/4, GTK idx %u len %u\n",
             wpa->gtk_idx, wpa->gtk_len);
      return SKW_WPA_PTK_DONE;
    }

  /* Group key message 1/2 (rekey) */

  ret = wpa_gtk(wpa, f, info);
  if (ret < 0)
    {
      return SKW_WPA_IGNORE;
    }

  memcpy(wpa->replay, f + EK_REPLAY, 8);
  wpa->replay_valid = true;
  wpa_reply(wpa, f[0], KI_VER_AES | KI_MIC | KI_SECURE, f + EK_REPLAY,
            NULL, NULL, 0);
  syslog(LOG_INFO, "SKW WPA: group 1/2 -> 2/2, GTK idx %u\n", wpa->gtk_idx);
  return SKW_WPA_GTK_DONE;
}
