/* SPDX-License-Identifier: Apache-2.0 */

/****************************************************************************
 * SKW6621S SDIO probe interface.
 ****************************************************************************/

#ifndef __CONTEST_SKW6621S_SDIO_H
#define __CONTEST_SKW6621S_SDIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/sdio.h>

#include <stddef.h>
#include <stdint.h>

enum skw6621s_protocol_e
{
  SKW6621S_SDIO_V1 = 1,
  SKW6621S_SDIO_V2 = 2
};

struct skw6621s_probe_s
{
  uint8_t identity[16];
  uint8_t cccr_revision;
  uint16_t vendor;
  uint16_t device;
  uint16_t blocksize;
  enum skw6621s_protocol_e protocol;
};

int skw6621s_sdio_probe(FAR struct sdio_dev_s *sdio,
                        FAR struct skw6621s_probe_s *result);

/* First firmware boot after a fresh probe.  Loads SWT6621S_IRAM_SDIO.bin,
 * SWT6621S_DRAM_SDIO.bin and the given NV file from dir, downloads them and
 * waits (bounded) for the LOOPCHECK BSPREADY / trunk_W handshake.
 */

int skw6621s_boot(FAR struct sdio_dev_s *sdio,
                  FAR const struct skw6621s_probe_s *probe,
                  FAR const char *dir, FAR const char *nvname);

/* Bluetooth transport over the booted CP (SDIO v2 ports 2 / 5, H4 framing).
 * The HCI callback runs in the caller of skw6621s_poll() with the bus lock
 * held: it must not call back into this driver.
 */

typedef void (*skw6621s_hci_cb_t)(FAR void *arg, uint8_t type,
                                  FAR const uint8_t *data, size_t len);

int skw6621s_bt_start(void);
void skw6621s_set_hci_cb(skw6621s_hci_cb_t cb, FAR void *arg);
int skw6621s_poll(void);
int skw6621s_hci_send(uint8_t type, FAR const uint8_t *data, size_t len);

/* Vendor HCI init after skw6621s_bt_start: read version, NVDS, reset */

int skw6621s_bt_hci_init(FAR const char *nvpath);

#endif /* __CONTEST_SKW6621S_SDIO_H */
