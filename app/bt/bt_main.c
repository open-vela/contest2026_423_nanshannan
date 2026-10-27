/****************************************************************************
 * app/bt/bt_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 蓝牙命令。
 *
 *   bt probe   Bounded transport and chip-ID probe
 *   bt init    Firmware and HCI initialization after probe validation
 *   bt wifi    WiFi service and vendor init (boots the module if needed)
 *   bt wifi scan  2.4 GHz scan after bt wifi
 *
 * ★ 分成两条命令是有代价换来的：init 在模组不应答时会长时间重试，
 *   而它是前台任务，NSH 一直等着、控制台没有提示符、loader 发不进去，
 *   板子只能 MASKROM 恢复。probe 让"模组在不在线"这个问题先有答案。
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <string.h>

#include <arch/board/board.h>

int main(int argc, char *argv[])
{
  int ret;

  if (argc >= 2 && strcmp(argv[1], "init") == 0)
    {
      ret = kickpi_k7_bt_initialize();
      printf("ret=%d\n", ret);
      return ret < 0 ? 1 : 0;
    }

  if (argc >= 2 && strcmp(argv[1], "wifi") == 0)
    {
      if (argc >= 3 && strcmp(argv[2], "scan") == 0)
        {
          ret = kickpi_k7_wifi_scan();
        }
      else
        {
          ret = kickpi_k7_wifi_initialize();
        }

      printf("ret=%d\n", ret);
      return ret < 0 ? 1 : 0;
    }

  if (argc < 2 || strcmp(argv[1], "probe") == 0)
    {
      ret = kickpi_k7_bt_probe();
      printf("ret=%d\n", ret);
      return ret < 0 ? 1 : 0;
    }

  printf("用法: bt probe|init|wifi [scan]\n");
  return 1;
}
