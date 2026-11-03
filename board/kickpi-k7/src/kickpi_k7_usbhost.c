/* SPDX-License-Identifier: Apache-2.0 */

/****************************************************************************
 * Manual KICKPI-K7 USB host bring-up (k7diag usb).
 *
 * 三个 Type-A 口都在板载 GL3510 hub 下面，hub 接 DRD1（USB3OTG1）。
 * 未经上板验证，不放进启动路径。
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <syslog.h>

#include <nuttx/usb/usbhost.h>

#include "rk3576_usbhost.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int kickpi_k7_usbhost_initialize(void)
{
  static bool registered;
  int ret;

  if (!registered)
    {
#ifdef CONFIG_USBHOST_HUB
      ret = usbhost_hub_initialize();
      if (ret < 0)
        {
          syslog(LOG_ERR, "USB: hub class 注册失败 %d\n", ret);
          return ret;
        }
#endif

#ifdef CONFIG_USBHOST_MSC
      ret = usbhost_msc_initialize();
      if (ret < 0)
        {
          syslog(LOG_ERR, "USB: MSC class 注册失败 %d\n", ret);
          return ret;
        }
#endif

#ifdef CONFIG_USBHOST_HIDKBD
      ret = usbhost_kbdinit();
      if (ret < 0)
        {
          syslog(LOG_ERR, "USB: HID 键盘 class 注册失败 %d\n", ret);
          return ret;
        }
#endif

      registered = true;
    }

  return rk3576_usbhost_initialize();
}

bool kickpi_k7_usbhost_running(void)
{
  return rk3576_usbhost_running();
}
