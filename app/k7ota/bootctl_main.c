/****************************************************************************
 * app/k7ota/bootctl_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * openvela 镜像的 A/B 启动控制。子命令与 frameworks/system/ota 的 bootctl
 * 相同（无参数 = 标记启动成功，update / done / slot），状态存在 eMMC 上的
 * 控制块里，由 U-Boot 的 bootamp 读（amp/include/k7_bootab.h）。
 *
 *   bootctl          本次启动成功，确认当前槽（界面起来后也会自动做）
 *   bootctl slot     本次从哪里启动
 *   bootctl info     控制块全部内容
 *   bootctl update   目标槽清成不可启动（ota install 会自己调）
 *   bootctl done     目标槽设为下次启动（ota install 会自己调）
 *   bootctl clear    清控制块：下次启动救援 FIT
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "k7ab.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct k7ab_ctrl_s c;
  int ret;

  if (argc == 1 || (argc == 2 && strcmp(argv[1], "success") == 0))
    {
      ret = k7ab_success();
      if (ret == 1)
        {
          printf("已确认：本次启动的槽标记为 successful\n");
        }
      else if (ret == 0)
        {
          printf("无需确认（已确认过，或本次启动的是救援 FIT）\n");
        }
    }
  else if (argc == 2 && strcmp(argv[1], "slot") == 0)
    {
      ret = k7ab_load(&c);
      printf("run %c\n", ret == 0 ? k7ab_slot_name(c.booted) : 'R');
      ret = ret == -ENOENT ? 0 : ret;
    }
  else if (argc == 2 && strcmp(argv[1], "info") == 0)
    {
      ret = k7ab_load(&c);
      if (ret == -ENOENT)
        {
          printf("没有控制块：启动的是救援 FIT，还没装过 OTA\n");
          ret = 0;
        }
      else if (ret == 0)
        {
          k7ab_print(&c);
          printf("下一次升级写槽 %c\n", k7ab_slot_name(k7ab_target(&c)));
        }
    }
  else if (argc == 2 && strcmp(argv[1], "update") == 0)
    {
      ret = k7ab_update();
    }
  else if (argc == 2 && strcmp(argv[1], "done") == 0)
    {
      /* 手动 done 只在槽里已有 ota 写好的镜像时有意义：长度和 sha256
       * 沿用控制块里的记录，记录为空就拒绝。
       */

      ret = k7ab_load(&c);
      if (ret == 0 && c.slot[k7ab_target(&c)].length > 0)
        {
          FAR struct k7ab_slot_s *s = &c.slot[k7ab_target(&c)];

          ret = k7ab_done(s->length, s->sha256);
        }
      else
        {
          printf("目标槽没有记录过镜像，用 ota install 写\n");
          ret = -ENOENT;
        }
    }
  else if (argc == 2 && strcmp(argv[1], "clear") == 0)
    {
      ret = k7ab_clear();
      if (ret == 0)
        {
          printf("控制块已清：下次启动救援 FIT（trust 分区）\n");
        }
    }
  else
    {
      printf("用法: bootctl [success|slot|info|update|done|clear]\n");
      return EXIT_FAILURE;
    }

  if (ret < 0)
    {
      printf("bootctl 失败: %d\n", ret);
    }

  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
