/* SPDX-License-Identifier: Apache-2.0 */

/****************************************************************************
 * Manual KICKPI-K7 SKW6621S SDIO probe hooks.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <syslog.h>

#include <nuttx/sdio.h>

#include "rk3576_dwmmc.h"
#include "skw6621s/skw6621s_sdio.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int kickpi_k7_bt_probe(void)
{
  struct skw6621s_probe_s result;
  FAR struct sdio_dev_s *sdio;
  int ret;

  ret = rk3576_dwmmc_probe(RK3576_DWMMC_SDIO_BASE);
  if (ret < 0)
    {
      syslog(LOG_ERR, "SKW6621S: RK3576 SDIO host probe failed: %d\n", ret);
      return ret;
    }

  sdio = rk3576_dwmmc_initialize(RK3576_DWMMC_SDIO_BASE);
  if (sdio == NULL)
    {
      return -ENODEV;
    }

  return skw6621s_sdio_probe(sdio, &result);
}

/* Probe resets the module (WL_REG_ON), so the CP starts from ROM; the
 * firmware then carries both services.  Antenna NV (ALONE / SHARE) is not
 * confirmed for K7; it only affects RF, not the boot handshake.
 */

static int kickpi_k7_skw_boot(void)
{
  struct skw6621s_probe_s result;
  FAR struct sdio_dev_s *sdio;
  int ret;

  if (skw6621s_booted())
    {
      return OK;
    }

  ret = rk3576_dwmmc_probe(RK3576_DWMMC_SDIO_BASE);
  if (ret < 0)
    {
      return ret;
    }

  sdio = rk3576_dwmmc_initialize(RK3576_DWMMC_SDIO_BASE);
  if (sdio == NULL)
    {
      return -ENODEV;
    }

  ret = skw6621s_sdio_probe(sdio, &result);
  if (ret < 0)
    {
      return ret;
    }

  return skw6621s_boot(sdio, &result, "/data/skw",
                       "SWT6621S_NV_SDIO_SHARE.bin");
}

int kickpi_k7_bt_initialize(void)
{
  int ret;

  ret = kickpi_k7_skw_boot();
  if (ret < 0)
    {
      return ret;
    }

#ifdef CONFIG_NET_BLUETOOTH
  return skw6621s_bt_register("/data/skw/sv6160lite.nvbin");
#else
  return skw6621s_bt_hci_init("/data/skw/sv6160lite.nvbin");
#endif
}

int kickpi_k7_wifi_initialize(void)
{
  int ret;

  ret = kickpi_k7_skw_boot();
  if (ret < 0)
    {
      return ret;
    }

  return skw6621s_wifi_init("/data/skw");
}

int kickpi_k7_wifi_scan(void)
{
  return skw6621s_wifi_scan();
}

int kickpi_k7_wifi_connect(FAR const char *ssid, FAR const char *psk)
{
  return skw6621s_wifi_connect(ssid, psk);
}

int kickpi_k7_wifi_disconnect(void)
{
  return skw6621s_wifi_disconnect();
}
