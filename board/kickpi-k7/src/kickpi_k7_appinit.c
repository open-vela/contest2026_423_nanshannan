/****************************************************************************
 * vendor/rockchip/boards/rk3576/kickpi-k7/src/kickpi_k7_appinit.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <debug.h>
#include <errno.h>
#include <syslog.h>
#include <inttypes.h>
#include <nuttx/kmalloc.h>
#include <nuttx/video/fb.h>
#include <nuttx/timers/oneshot.h>
#include "arm64_arch_timer.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sched.h>
#include <nuttx/board.h>
#include <nuttx/kthread.h>
#include <nuttx/sdio.h>
#include <nuttx/mmcsd.h>
#include <nuttx/fs/fs.h>
#include <nuttx/fs/partition.h>
#include <nuttx/drivers/drivers.h>
#include <arch/board/board.h>

#include "arm64_internal.h"
#include "rk3576_pl330.h"
#include "rk3576_power.h"
#include "rk3576_sai.h"
#include "rk3576_dcphy.h"
#include "rk3576_dsi2.h"
#include "rk3576_dwmmc.h"
#include "rk3576_wdt.h"
#include "rk3576_timer.h"
#include "rk3576_rng.h"
#include "rk3576_gmac.h"
#include "rk3576_eth.h"
#include "rk3576_vop2.h"
#include "rk3576_sdhci.h"
#include "rk3576_rptun.h"
#include "kickpi_k7.h"

#if defined(CONFIG_NETUTILS_NETINIT) && !defined(CONFIG_NSH_NETINIT)
#  include "netutils/netinit.h"
#endif


/* 板级 late init 里直接把界面拉起来，见下面 fb_register 之后那一段。
 * Application.mk 会把应用的 main 重命名成 <PROGNAME>_main，所以这里
 * 声明的就是 apps 侧那个入口。
 */

#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_423_KICKPI_UI
int kickpi_ui_main(int argc, FAR char *argv[]);
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#ifdef CONFIG_RK3576_GMAC
/****************************************************************************
 * Name: gmac_bringup_thread
 *
 * Description:
 *   GMAC 的整条上电序列，放到后台线程里跑。
 *
 * ★ 为什么要挪出启动路径
 *
 *   这一段里有一个 up_mdelay(2500)：给 1000Base-T 自协商留时间。实测它
 *   **占了整个启动的 2.8 秒**，是单项最大开销。
 *
 *   而它等的东西并不会来 —— 同一份日志里写着
 *     "GMAC0/1: 25M 焊盘采样 200 次 高=0 跳变=0 —— ★无跳变，时钟没到引脚"
 *   参考时钟根本没出到 PHY，链路不可能建立，2.5 秒等的是一个永远不发生的
 *   事件。等待时长和它所等的事件差着数量级，这在本项目已经是第三次了
 *   （前两次是 I2C 超时和 GMAC 链路状态轮询）。
 *
 *   但"删掉这个等待"和"让网口能用"是两件事：时钟没出来是独立的硬件/配置
 *   问题，还没修。所以这里不删逻辑，只是把它从**串行的启动路径**挪到后台
 *   ——  网口该多久起来还是多久，nsh 不再陪着等。
 *
 ****************************************************************************/

static int gmac_bringup_thread(int argc, FAR char *argv[])
{
  int ret;

  UNUSED(argc);
  UNUSED(argv);

  rk3576_gmac_refclk25m(0);
  rk3576_gmac_refclk25m(1);

  rk3576_gmac_phy_reset(BOARD_GMAC1_RST_BANK, BOARD_GMAC1_RST_PIN, true);
  rk3576_gmac_phy_reset(BOARD_GMAC0_RST_BANK, BOARD_GMAC0_RST_PIN, true);

  /* PHY 自协商要时间（1000Base-T 通常 1~3 秒）。软复位等 RX 时钟，
   * 而 RX 时钟要等链路 —— 这里给协商留出时间，否则必然超时。
   */

  up_mdelay(2500);

  /* ★ 两个口都要配。
   *
   *   板上把两个网口用网线对接了，所以对端就是本板的另一个 MAC/PHY。
   *   只配 GMAC0 的话，对端那半条链路没人管 —— 而 RGMII 的 RX 时钟由
   *   对端 PHY 在链路建立后才输出，链路建不起来就一直没有时钟。
   */

  ret = rk3576_gmac_probe(0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: GMAC0 探测失败: %d\n", ret);
    }

  ret = rk3576_gmac_probe(1);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: GMAC1 探测失败: %d\n", ret);
    }

#ifdef CONFIG_RK3576_ETH
  /* ★ 网络设备要在 rk3576_gmac_probe() **之后**注册。
   *
   *   probe 里做的是时钟、GRF、引脚复用、RGMII 延时线，以及 PHY 的
   *   厂商初始化 —— 少了最后一项，PHY 的模拟前端不工作，DMA 软复位
   *   不会完成，网络设备注册上去也是死的。
   *
   *   arm64 通用层默认在 OS 早期就调 arm64_netinitialize()，那时
   *   板级初始化还没跑。所以开 CONFIG_NETDEV_LATEINIT 把它挪到这里。
   */

  ret = rk3576_eth_netinitialize(0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 网络设备注册失败: %d\n", ret);
    }
  else
    {
      syslog(LOG_INFO, "网络: eth0 已注册（GMAC%d）\n",
             CONFIG_RK3576_ETH_PORT);

#if defined(CONFIG_NETUTILS_NETINIT) && !defined(CONFIG_NSH_NETINIT)
      /* ★ 地址和 ifup 在这里做，不交给 nsh 的 netinit。
       *
       *   nsh 在 board_app_initialize 返回后立刻起 netinit，而 eth0 要
       *   等本线程忙等完 2.5 秒才注册 —— netinit 先跑的话，设地址和
       *   ifup 都会因为找不到设备静默失败，eth0 停在 DOWN / 0.0.0.0。
       *
       *   以前一直没出事，是因为 CPU1 被绕开，剩下三个核被本线程和
       *   devinit 这些优先级 100 的忙等占满，优先级 80 的 netinit 恰好
       *   被饿到 eth0 注册之后。修好 CPU1 后（rk3576_smp.c），netinit
       *   立刻有核可用，顺序就反了（2026-10-24 板上复现）。
       *
       *   在这里同步调用，顺序由代码保证，不再靠调度碰巧排对。
       */

      ret = netinit_bringup();
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: 网络配置失败: %d\n", ret);
        }
#endif
    }
#endif

  UNUSED(ret);
  return 0;
}

#endif /* CONFIG_RK3576_GMAC */

/****************************************************************************
 * Name: devinit_thread
 *
 * Description:
 *   触摸、TF 卡、摄像头、V4L2 的初始化，做完再拉起界面。后台线程里跑。
 *
 * ★ 为什么挪出启动路径（2026-09-19，xTS 2.1.4 启动时间）
 *
 *   board_app_initialize() 跑在 nsh 任务里，它不返回 nsh 就不出提示符。
 *   串口时间戳实测这几项串行要 0.85 秒：触摸 0.22（FT8756 上电等待）、
 *   TF 卡 0.35（400kHz 识别）、摄像头 + V4L2 0.28（三路 MCLK、I2C 探测）。
 *   nsh 本身一样都不用。
 *
 *   顺序照旧：界面要 /dev/input0，所以仍然最后启动（见下面界面那段的
 *   说明）。界面出现的时刻不变，只是 nsh 不再陪着等。
 *
 *   与主线程、GMAC 线程并发是安全的：各用各的 I2C 总线和控制器；CRU/GRF
 *   是高 16 位掩码写，不做读改写。栈 8192 的理由见 gmac_bringup_thread。
 *
 ****************************************************************************/

static int devinit_thread(int argc, FAR char *argv[])
{
  int ret;

  UNUSED(argc);
  UNUSED(argv);

#ifdef CONFIG_INPUT_FT5X06
  ret = kickpi_k7_touch_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 触摸初始化失败: %d\n", ret);
    }
#endif

#ifdef CONFIG_RK3576_DWMMC
  /* SD 卡（TF）控制器。与 eMMC 是两种不同的 IP，各走各的驱动。 */

  ret = rk3576_dwmmc_probe(RK3576_DWMMC_SD_BASE);
  if (ret < 0)
    {
      /* -ENODEV 是"卡不在位"，不是故障 —— SD 卡本来就是可插拔的，
       * 这里分开说，免得把"没插卡"记成"驱动坏了"。
       */

      syslog(ret == -ENODEV ? LOG_INFO : LOG_ERR,
             "SD 卡: %s (%d)\n",
             ret == -ENODEV ? "未插卡，跳过" : "控制器探测失败", ret);
    }
  else
    {
      struct sdio_dev_s *sd = rk3576_dwmmc_initialize(RK3576_DWMMC_SD_BASE);

      if (sd == NULL)
        {
          syslog(LOG_ERR, "ERROR: SD 卡 sdio_dev 初始化失败\n");
        }
      else
        {
          ret = mmcsd_slotinitialize(1, sd);
          if (ret < 0)
            {
              syslog(LOG_ERR,
                     "ERROR: SD 卡 mmcsd_slotinitialize 失败: %d\n", ret);
            }
          else
            {
              /* 同 eMMC：返回值不足为凭，实际 stat 节点才算数。 */

              struct stat sdst;

              if (stat("/dev/mmcsd1", &sdst) == 0)
                {
                  syslog(LOG_INFO, "SD 卡: /dev/mmcsd1 就绪\n");
                }
              else
                {
                  syslog(LOG_ERR,
                         "ERROR: SD 卡未被识别，/dev/mmcsd1 不存在\n");
                }
            }
        }
    }
#endif

#ifdef CONFIG_RK3576_I2C
  /* 摄像头传感器探测（不建立取图通路，见 kickpi_k7_camera.c 说明） */

  ret = kickpi_k7_camera_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 摄像头探测失败: %d\n", ret);
    }
#endif

#ifdef CONFIG_RK3576_VIDEO
  /* ★ V4L2 设备要在摄像头初始化之后注册。
   *
   *   探测、MCLK、传感器寄存器表都在上一步做完；本步只把
   *   imgsensor + imgdata 交给视频框架。顺序反了的话，
   *   capture_register() 会在传感器还没上电时去问 is_available()。
   */

  ret = kickpi_k7_video_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: V4L2 设备注册失败: %d\n", ret);
    }
#endif

#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_423_KICKPI_UI
  /* ★ 仪表盘放在**所有设备都注册完之后**再拉起来。
   *
   *   一、它要用 /dev/fb0 和 /dev/input0 两个节点，而触摸是在显示之后
   *      才注册的。一开始图省事放在 fb_register() 后面，结果每次都是
   *      「警告：打不开 /dev/input0 —— 触摸不可用，仅显示」 ——
   *      不是触摸坏了，是**界面比触摸先起来**。
   *
   *   二、开机自启本身有两个理由：这是块带屏的板子，上电就该有画面；
   *      而且它每 250ms 刷一次，是一个**不依赖串口的旁路心跳**。
   *      AMP 调试期观察到过 openvela 打印到 NSH 横幅后串口彻底安静，
   *      「死了」和「活着但收不到 UART 中断」在串口上长得一模一样，
   *      靠屏幕动不动才分得开（那次是 Linux 的 gic_dist_config() 把
   *      openvela 的中断使能位清掉了，见 amp/linux/0001）。
   */

  {
    int pid = task_create("kickpi_ui",
                          CONFIG_LVX_DEMO_CONTEST2026_423_KICKPI_UI_PRIORITY,
                          CONFIG_LVX_DEMO_CONTEST2026_423_KICKPI_UI_STACKSIZE,
                          kickpi_ui_main, NULL);

    if (pid < 0)
      {
        syslog(LOG_ERR, "ERROR: 启动 kickpi_ui 失败: %d\n", pid);
      }
    else
      {
        syslog(LOG_INFO, "界面: kickpi_ui 已启动（pid %d）\n", pid);
      }
  }
#endif

  UNUSED(ret);
  return 0;
}

#if defined(CONFIG_RK3576_SDHCI) && defined(CONFIG_GPT_PARTITION) && \
    defined(CONFIG_FS_FAT)
/* ★ /data 落在 eMMC 出厂 GPT 的 cache 分区（384MB）。
 *
 *   AMP 下 Linux 不碰 eMMC（内核没编 MMC），cache 分区没人用；它离
 *   U-Boot / FIT / 内核（全在 LBA 65536 以内）很远。
 *
 *   只认名字**和**范围都对得上的那一项：GPT 被人改过（比如重刷了
 *   厂商固件、分区挪了位置）就不注册，宁可退回 tmpfs，也不往一块
 *   不知道是什么的地方写。预期值取自出厂 GPT 备份
 *   ~/rk3576-amp/backup/gpt-orig.bin，和 10-25 板上读出的一致。
 *
 *   ★ U-Boot SPL 每次启动刷一串 "Invalid GPT"，那是在扫 MMC2（TF 卡，
 *     本来就没有 GPT），不是 eMMC。
 *
 *   不在启动路径里格式化。第一次用要手动：mkfatfs /dev/data
 */

#define KICKPI_DATA_PART    "cache"
#define KICKPI_DATA_FIRST   1165312
#define KICKPI_DATA_NBLOCKS 786432
#define KICKPI_DATA_DEV     "/dev/data"

static void kickpi_data_part_handler(FAR struct partition_s *part,
                                     FAR void *arg)
{
  FAR bool *found = arg;
  int ret;

  if (strcmp(part->name, KICKPI_DATA_PART) != 0)
    {
      return;
    }

  if (part->firstblock != KICKPI_DATA_FIRST ||
      part->nblocks != KICKPI_DATA_NBLOCKS || part->blocksize != 512)
    {
      syslog(LOG_ERR, "ERROR: eMMC GPT 的 %s 分区是 %zu+%zu（块 %zu），"
             "不是预期的 %d+%d，不使用\n", KICKPI_DATA_PART,
             part->firstblock, part->nblocks, part->blocksize,
             KICKPI_DATA_FIRST, KICKPI_DATA_NBLOCKS);
      return;
    }

  ret = register_blockpartition(KICKPI_DATA_DEV, 0666, "/dev/mmcsd0",
                                part->firstblock, part->nblocks);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 注册 %s 失败: %d\n", KICKPI_DATA_DEV, ret);
      return;
    }

  *found = true;
}

/* 返回 true 表示 /data 已挂上 eMMC */

static bool kickpi_mount_data_emmc(void)
{
  struct stat st;
  bool found = false;
  int ret;

  if (stat("/dev/mmcsd0", &st) < 0)
    {
      return false;
    }

  ret = parse_block_partition("/dev/mmcsd0", kickpi_data_part_handler,
                              &found);
  if (ret < 0 || !found)
    {
      syslog(LOG_ERR, "ERROR: eMMC 上没找到可用的 %s 分区（%d）\n",
             KICKPI_DATA_PART, ret);
      return false;
    }

  ret = mount(KICKPI_DATA_DEV, "/data", "vfat", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "eMMC: %s 挂载失败（%d）。第一次用要先 "
             "mkfatfs %s，然后 mount -t vfat %s /data\n",
             KICKPI_DATA_DEV, -errno, KICKPI_DATA_DEV, KICKPI_DATA_DEV);
      return false;
    }

  syslog(LOG_INFO, "eMMC: %s（GPT %s）已挂到 /data\n",
         KICKPI_DATA_DEV, KICKPI_DATA_PART);
  return true;
}
#endif

/****************************************************************************
 * Name: board_app_initialize
 *
 * Description:
 *   Perform application specific initialization.  This function is never
 *   called directly from application code, but only indirectly via the
 *   (non-standard) boardctl() interface using the command BOARDIOC_INIT.
 *
 ****************************************************************************/

int board_app_initialize(uintptr_t arg)
{
#if defined(CONFIG_FS_PROCFS) || defined(CONFIG_DEV_GPIO) || \
    defined(CONFIG_RK3576_I2C)
  int ret;
#endif

#ifdef CONFIG_CXX_EXCEPTION
  /* C++ 异常：注册栈展开表（.eh_frame）。
   *
   *   arm64 NuttX 上游的普遍空缺：链接脚本曾把 .eh_frame 丢进
   *   /DISCARD/（已改回保留在 .rodata），但 _Unwind_Find_FDE 找表
   *   要靠 __register_frame_info 主动注册 —— 全树无人调用，导致
   *   cxxtest 的 Test Exception 一 throw 就崩（xTS 1.1.13 ◐）。
   *
   *   这里在首个 C++ 代码运行前注册（nsh/cxxtest 之前）。
   *   g_fde_object 对应 libgcc 未公开的 struct object，给足 64 字节。
   */

  extern void __register_frame_info(const void *, void *);
  extern const uint8_t __EH_FRAME_BEGIN__[];
  static uint8_t g_fde_object[64] __attribute__((aligned(8)));

  __register_frame_info(__EH_FRAME_BEGIN__, g_fde_object);
  syslog(LOG_INFO, "CXX: .eh_frame 已注册 @%p\n", __EH_FRAME_BEGIN__);
#endif

#ifdef CONFIG_FS_TMPFS
  /* xTS 的 cmocka 用例（syscall / fs / kv）都在
   * CONFIG_TESTS_TESTSUITES_MOUNT_DIR 下建文件，缺这个目录会在第一个
   * 用例就报 "Failed to switch the mount dir"。用 tmpfs 提供，不占用
   * eMMC，也不依赖存储先就绪。
   *
   * ★ 这一段原来嵌在 CONFIG_FS_PROCFS 的 #ifdef 里面。
   *
   *   两者毫无关系，一旦有人关掉 procfs，tmpfs 会跟着一起消失，而现象
   *   出现在很远的地方 —— cmocka 用例报 "Failed to switch the mount dir"。
   *   条件编译的嵌套错误不会有任何编译期提示，只能靠看出来。
   */

  ret = mount(NULL, CONFIG_TESTS_TESTSUITES_MOUNT_DIR, "tmpfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 挂载 tmpfs 到 %s 失败: %d\n",
             CONFIG_TESTS_TESTSUITES_MOUNT_DIR, ret);
    }

  /* /tmp 也给一块 tmpfs。
   *
   * xTS 1.3.2 的命令是 `fstest -n 10 -m /tmp` —— 用例名字叫"RAM 读写"，
   * 实际测的是在**内存文件系统**上反复建删文件、校验内容。挂载点是写死
   * 在用例命令里的，板子上没有 /tmp 就直接失败，且报的是文件系统错误，
   * 看不出缺的只是一个挂载点。
   */

  ret = mount(NULL, "/tmp", "tmpfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 挂载 tmpfs 到 /tmp 失败: %d\n", ret);
    }

  /* ★ /data 给 ai_agent 用（/data/ai_agent，技能在 /data/ai_agent/skills/）。
   *
   *   它要等 eMMC 就绪后才挂，所以不在这里，见下面 SDHCI 那段之后：
   *   优先 eMMC cache 分区（kickpi_mount_data_emmc()），不行退回 tmpfs。
   */
#endif

#ifdef CONFIG_FS_PROCFS
  /* 挂载 procfs。
   *
   * CONFIG_NSH_ARCHINIT=y 时 NSH 不会自己挂载 /proc，改由本函数负责
   * （NSH 通过 boardctl(BOARDIOC_INIT) 调到这里）。没有它，free、ps、
   * uptime 等命令都会报 "Could not open /proc/... (is procfs mounted?)"。
   *
   * 挂载失败不作为致命错误：procfs 只是观察窗口，缺了它系统其余部分
   * 照常工作，因此仅记录日志、继续初始化后面的外设。
   */

  ret = mount(NULL, CONFIG_NSH_PROC_MOUNTPOINT, "procfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 挂载 procfs 到 %s 失败: %d\n",
             CONFIG_NSH_PROC_MOUNTPOINT, ret);
    }
#endif

#ifdef CONFIG_DEV_GPIO
  /* 注册板上 GPIO 为 /dev/gpoutN。同样不作为致命错误 —— GPIO 不可用时
   * 串口控制台仍应能进入，便于继续排查。
   */

  ret = kickpi_k7_gpio_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: GPIO 初始化失败: %d\n", ret);
    }
#endif

#ifdef CONFIG_RK3576_I2C
  ret = kickpi_k7_i2c_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: I2C 初始化失败: %d\n", ret);
    }
#endif

  /* ★ RTC 必须在这里初始化，不能留到后面。
   *
   *   它不只是 /dev/rtc0 —— HYM8563 的 CLKOUT 给 WiFi/蓝牙模组提供
   *   32.768kHz（原理图 K7_V2.1：32KOUT_RTC -> WIFIBT_32KIN_2T2R）。
   *   原先它排在 SDIO 探测**之后** 70 多行，等于模组是在没有 32k 的
   *   状态下被探测的，自然起不来。
   *
   *   顺序依赖不写下来就会在重排代码时悄悄失效，所以记在这里。
   */

#ifdef CONFIG_KICKPI_K7_MPU6050
  /* 40 针口上的 MPU-6050（xTS 1.3.7 用它作 I2C 外设） */

  ret = kickpi_k7_mpu6050_initialize();
  if (ret < 0)
    {
      syslog(ret == -ENODEV ? LOG_INFO : LOG_ERR,
             "MPU6050: %s (%d)\n",
             ret == -ENODEV ? "未接器件，跳过" : "初始化失败", ret);
    }
#endif

#ifdef CONFIG_RTC_HYM8563
  ret = kickpi_k7_rtc_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: RTC 初始化失败: %d\n", ret);
    }
  else
    {
      syslog(LOG_INFO, "RTC: /dev/rtc0 就绪（HYM8563@I2C2:0x51）\n");
    }
#endif

#ifdef CONFIG_RK3576_SPI
  ret = kickpi_k7_spi_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: SPI 初始化失败: %d\n", ret);
    }
#endif

#ifdef CONFIG_RK3576_SDHCI
  /* eMMC：拿到 sdio_dev_s 句柄，交给 mmcsd 上层注册 /dev/mmcsd0。
   * 失败不阻断其余初始化 —— 没有存储时控制台仍应可用，便于继续排查。
   */

    {
      struct sdio_dev_s *sdio = rk3576_sdhci_initialize(0);

      if (sdio == NULL)
        {
          syslog(LOG_ERR, "ERROR: eMMC 控制器初始化失败\n");
        }
      else
        {
          ret = mmcsd_slotinitialize(0, sdio);
          if (ret < 0)
            {
              syslog(LOG_ERR, "ERROR: mmcsd_slotinitialize 失败: %d\n", ret);
            }
          else
            {
              /* ★ 不能只看返回值。mmcsd_slotinitialize() 在卡未识别时
               * 也返回 OK —— 它只把 -ENODEV 之外的错误当作失败，
               * 而块设备注册发生在更里层的 mmcsd_probe()，条件是
               * 分区块数非零。所以这里实际 stat 一下设备节点，
               * 存在才算就绪。
               */

              struct stat st;

              if (stat("/dev/mmcsd0", &st) == 0)
                {
                  syslog(LOG_INFO, "eMMC: /dev/mmcsd0 就绪\n");

#ifdef CONFIG_BCH
                  /* NuttX 的块设备不能直接被 open() 当文件读 ——
                   * dd / hexdump 这类工具走的是字符设备接口。
                   * BCH 把块设备包成字符设备，便于裸读验证与取分区表。
                   * 只读注册，避免误写坏 eMMC 上原有的分区。
                   */

                  ret = bchdev_register("/dev/mmcsd0", "/dev/mmcsd0c", true);
                  if (ret < 0)
                    {
                      syslog(LOG_ERR, "ERROR: 注册 /dev/mmcsd0c 失败: %d\n",
                             ret);
                    }
                  else
                    {
                      syslog(LOG_INFO,
                             "eMMC: /dev/mmcsd0c 就绪（只读字符视图）\n");
                    }
#endif
                }
              else
                {
                  syslog(LOG_ERR,
                         "ERROR: mmcsd_slotinitialize 返回 OK 但 "
                         "/dev/mmcsd0 不存在 —— 卡未被识别\n");
                }
            }
        }
    }
#endif

  /* /data：优先 eMMC 的 cache 分区，不行就退回 tmpfs（掉电即失） */

    {
      bool data_ok = false;

#if defined(CONFIG_RK3576_SDHCI) && defined(CONFIG_GPT_PARTITION) && \
    defined(CONFIG_FS_FAT)
      data_ok = kickpi_mount_data_emmc();
#endif

#ifdef CONFIG_FS_TMPFS
      if (!data_ok)
        {
          ret = mount(NULL, "/data", "tmpfs", 0, NULL);
          if (ret < 0)
            {
              syslog(LOG_ERR, "ERROR: 挂载 tmpfs 到 /data 失败: %d\n", ret);
            }
          else
            {
              syslog(LOG_WARNING, "/data 是 tmpfs，掉电即失\n");
            }
        }
#endif

      UNUSED(data_ok);
    }

#ifdef CONFIG_RK3576_VOP2
  /* 显示链路第一步：VOP2 自检 + 内置彩条。
   *
   * ★ 彩条不经过帧缓冲与图层，只要 VOP 的时序发生器、像素时钟、
   *   以及到 MIPI 接口的通路对了就会出现。把"VOP 配置"与
   *   "图层/内存通路"两类问题分开验证。
   *
   *   注意此时 DSI 主机与 D-PHY 尚未实现，屏上不会有任何显示 ——
   *   本步只验证 VOP 侧寄存器可写、时序算得对。屏幕点亮要等
   *   DSI + D-PHY + 面板初始化都做完。
   */

#if defined(CONFIG_INPUT_FT5X06) || defined(CONFIG_RK3576_VOP2)
  /* ★ 必须排在显示链路之前。屏与触摸共用 VCC3V3_LCD_S0，面板要先上电、
   * 复位释放，之后配 D-PHY / DSI 才有意义；顺序反了的话 DSI 是对着一块
   * 没电的屏在配置。触摸同理 —— 没电时扫任何总线都不会应答。
   */

  ret = kickpi_k7_lcd_power(true);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: LCD 电源轨打开失败: %d\n", ret);
    }
#endif

    {
      /* 时序取自 docs/refs/panel/rk3308b-mipi-display-v11.dtsi 的
       * 720x1280 5 寸屏（ST7703）。规格与本板 F050008M01 一致
       * （4 lane RGB888、68x121mm），但那是另一块屏的数据 ——
       * 见 docs/refs/panel/README.md 中标注的适用边界。
       */

      static const struct rk3576_vop2_timing_s timing =
      {
        .pixclk_hz    = 65000000,
        .hactive      = 720,
        .hfront_porch = 48,
        .hsync_len    = 8,
        .hback_porch  = 52,
        .vactive      = 1280,
        .vfront_porch = 16,
        .vsync_len    = 6,
        .vback_porch  = 15,
      };

      /* 每通道码率 = 像素时钟 * 每像素位数 / 通道数，单位 kbps。
       * D-PHY 与 DSI 的时序换算都要用它，只算一次，避免两处各算一遍
       * 后悄悄不一致。
       */

      const uint32_t lane_kbps = timing.pixclk_hz / 1000 * 24 / 4;

      UNUSED(lane_kbps);

      /* ★ 先读后写。
       *
       * 这块板的出厂固件能在这块屏上显示 Rockchip logo，说明 U-Boot
       * 已经完整跑通过面板初始化 + VOP2 + DSI + D-PHY。它留在寄存器里
       * 的是一份针对这块屏的已知可用配置 —— 包括我们拿不到的面板初始化
       * 序列所产生的效果。先把它原样读出来。
       *
       * KEEP_UBOOT_DISPLAY 为 1 时完全不碰显示，用来判断：不去动它的话
       * logo 是不是还在。这一个观测能区分「U-Boot 根本没点屏」和
       * 「U-Boot 点了、被我们改配置改灭了」，而这两种情况此前无法区分。
       */

#define KEEP_UBOOT_DISPLAY 1

      rk3576_vop2_dump_uboot_state();
#ifdef CONFIG_RK3576_DSI2
      rk3576_dsi2_dump_uboot_state();
#endif

#if KEEP_UBOOT_DISPLAY
      /* 接管而不是重建。U-Boot 已经把这块屏点亮（面板初始化序列在拿不到
       * 的厂商 dtsi 里，复位就回不来），所以只把图层扩到全屏、指向我们
       * 自己的帧缓冲，其余一律不碰。
       */

#ifdef CONFIG_RK3576_FB
      ret = fb_register(0, 0);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: 注册 /dev/fb0 失败: %d\n", ret);
        }
      else
        {
          syslog(LOG_INFO, "显示: /dev/fb0 就绪\n");

        }
#endif
#else
      ret = rk3576_vop2_probe();
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: VOP2 探测失败: %d\n", ret);
        }
      else
        {
          rk3576_vop2_colorbar(&timing, true);
        }

#ifdef CONFIG_RK3576_DCPHY
      /* D-PHY：PLL 锁定是整条显示链路第一个真正的硬件反馈。
       * 码率 = 像素时钟 * 每像素位数 / 通道数 = 65MHz * 24 / 4。
       */

      if (rk3576_dcphy_probe() == OK)
        {
          rk3576_dcphy_enable(lane_kbps, 4);
        }
#endif

#ifdef CONFIG_RK3576_DSI2
      if (rk3576_dsi2_probe() == OK)
        {
          rk3576_dsi2_configure(&timing, 4, lane_kbps / 1000);   /* 换成 Mbps */

          /* 面板唤醒。厂商的完整初始化序列拿不到，但这两条是
           * MIPI DCS 标准命令、与具体面板无关，且几乎所有 MIPI 屏
           * 上电后都停在睡眠态 —— 不发这两条就是背光亮、无图像。
           *
           *   0x11 exit_sleep_mode   之后须等 >120ms
           *   0x29 set_display_on
           *
           * dtype 0x05 = DCS short write, no parameter。
           */

          {
            static const uint8_t dcs_sleep_out = 0x11;
            static const uint8_t dcs_display_on = 0x29;
            int r1;
            int r2;

            r1 = rk3576_dsi2_send_cmd(0x05, &dcs_sleep_out, 1);
            up_mdelay(150);
            r2 = rk3576_dsi2_send_cmd(0x05, &dcs_display_on, 1);
            up_mdelay(50);

            /* 返回值必须打出来。命令接口忙超时的话这两条根本没发出去，
             * 而"背光亮无图像"的现象与没发是一模一样的 —— 不打就分不清。
             */

            syslog(LOG_INFO,
                   "DSI2: 面板唤醒 sleep_out=%d display_on=%d\n", r1, r2);
          }

          /* ★ 配置完停在命令模式是没有图像的直接原因 —— DSI 不接收
           * VOP 送来的像素流。必须显式切到视频模式。
           *
           * 本板不能用命令模式：原理图第 27 页 FPC Pin18 (LCD_TE)
           * 打叉未连线，没有 TE 就无法与面板刷新同步。
           */

          rk3576_dsi2_set_video_mode();
        }
#endif

      /* 整条链路配完之后再问一句：VP 到底在不在扫描。
       * 放在最后是因为 DSI 起来前 VP 可能被下游反压。
       */

      rk3576_vop2_check_scanning(0);
#endif  /* KEEP_UBOOT_DISPLAY */
    }
#endif

#ifdef CONFIG_RK3576_RPTUN
  /* AMP：到另一个簇上 Linux 的 rpmsg 通道。
   *
   * 放在这里（外设之前）是有意的：对端 Linux 起得比我们快，它建好
   * virtqueue 就会按第一次门铃，而那一次门铃是握手的唯一信号。晚注册
   * 一点不会丢消息 —— mailbox 的 pending 位会一直挂着直到我们清它 ——
   * 但越早注册，握手完成得越早，后面依赖 rpmsg 的东西才不用空等。
   */

  ret = rk3576_rptun_init(CONFIG_RK3576_RPTUN_CPUNAME);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: AMP rptun 初始化失败: %d\n", ret);
    }
#endif

#ifdef CONFIG_RK3576_DMA
  /* PL330 DMA 控制器（dmac0）。必须在 SAI 之前初始化 —— 音频的
   * DMA 搬运挂在它上面（SAI1 TX/RX 请求号 2/3）。
   */

  struct dma_dev_s *dma = rk3576_pl330_initialize(0);
  if (dma == NULL)
    {
      syslog(LOG_ERR, "ERROR: PL330 初始化失败\n");
    }
#endif

#ifdef CONFIG_RK3576_SAI
  /* 音频前置链路：PD_AUDIO + 三路时钟 + 引脚复用 + 版本自检。
   * 必须在 kickpi_k7_audio_initialize() 之前 —— 后者依赖这里
   * 打开的电源域与时钟。
   */

  ret = rk3576_sai_probe();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: SAI 探测失败: %d\n", ret);
    }
#endif

#if defined(CONFIG_AUDIO_ES8388) && defined(CONFIG_RK3576_SAI)
  ret = kickpi_k7_audio_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 音频初始化失败: %d\n", ret);
    }
#endif

  /* 触摸、TF 卡、摄像头和界面放到后台（见 devinit_thread 的说明） */

  {
    int tid = kthread_create("devinit", 100, 8192, devinit_thread, NULL);

    if (tid < 0)
      {
        syslog(LOG_ERR, "ERROR: devinit 后台线程创建失败: %d\n", tid);
      }
  }


/* 蓝牙（SKW6621S）的 SDIO 探测不再放在启动路径上（2026-09-19）。
 *
 *   枚举至今没通过（notes/bt-final-2026-09-19/README.md），用户决定截止前
 *   停止。留在启动路径上，每次开机都打出 SDIO 报错、多等 200ms 以上 ——
 *   xTS 1.2.1 要求启动日志无异常。需要时用 `bt probe` 手动触发。
 */

/* ★ 蓝牙**不在**启动路径上初始化，由 `bt init` 命令手动触发。
 *
 *   固件加载会等模组的 HCI 应答，而这条路在验证通过之前随时可能卡住。
 *   board_app_initialize() 跑在 nsh 任务上下文 —— 它一卡，控制台就起不来，
 *   连 loader 命令都发不进去，板子只能靠 MASKROM 恢复。
 *
 *   我已经这样把板子挂死过一次。未经验证的硬件启动流程放进启动路径，
 *   失败代价是"整块板子进不去"，而收益只是省一条命令。
 */

#ifdef CONFIG_RK3576_RNG
  ret = rk3576_rng_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 硬件随机数初始化失败: %d\n", ret);
    }
  else
    {
      syslog(LOG_INFO, "随机数: /dev/random 就绪\n");
    }
#endif

#ifdef CONFIG_RK3576_TIMER
  /* 注册 /dev/oneshot（xTS 1.3.13/14 的 cmocka_driver_oneshot 需要它）。
   *
   * ★ 下半部用的是 RK3576 的独立 TIMER，不是 arm64 通用定时器。
   *
   *   最初这里写的是 arm64_oneshot_initialize()，理由是"通用定时器是
   *   SoC 无关的那一个，本板不需要额外硬件"。那个理由本身没错，错在
   *   **它不是空闲的**：内核的 up_timer_initialize() 已经把同一个下半部
   *   实例交给调度器了，而 struct oneshot_lowerhalf_s 只有一对
   *   callback/arg，谁后设谁赢。
   *
   *   于是注册 /dev/oneshot 就把调度器的定时回调顶掉，系统时基当场死掉：
   *   sleep 永不返回、延时 work_queue 永不触发、看门狗 automonitor 从不
   *   喂狗因而每 95 秒硬件复位一次。而串口、nsh、所有命令都正常（靠
   *   UART 中断唤醒），up_mdelay() 也正常（忙等读计数器）—— 这个故障
   *   因此可以长期潜伏，并且会把"板子自己重启"栽赃给别的模块。
   *
   *   详见 notes/DEBUG-CASES.md 案例 16。
   */

  {
    struct oneshot_lowerhalf_s *os = rk3576_oneshot_initialize();

    if (os == NULL)
      {
        syslog(LOG_ERR, "ERROR: oneshot 下半部初始化失败\n");
      }
    else
      {
        ret = oneshot_register("/dev/oneshot", os);
        if (ret < 0)
          {
            syslog(LOG_ERR, "ERROR: 注册 /dev/oneshot 失败: %d\n", ret);
          }
        else
          {
            syslog(LOG_INFO, "定时器: /dev/oneshot 就绪\n");
          }
      }
  }
#endif




#ifdef CONFIG_RK3576_GMAC
  /* 以太网前置链路：PD_SDGMAC + 时钟 + DMA 复位 + MDIO 读 PHY ID。
   *
   * ★ 两层判据强弱不同：MAC_VERSION 只证明寄存器块活着；
   *   PHY ID 才证明 MDIO 时序、PHY 供电与连线都对。
   *   前者正常而后者读回 0xffff，说明问题在板级而非 SoC 侧。
   *
   * 板上有两路千兆网口，先探 GMAC0。
   */

  /* ★ 必须先释放 PHY 复位。DWMAC 的 DMA 软复位需要 PHY 提供的接收
   * 时钟才能完成；PHY 被摁着时 SWR 位永不自清，表现为软复位超时，
   * 而 SoC 侧的寄存器读写一切正常。本端口为此误查过时钟与模块复位两轮。
   */

  /* ★ 两颗 PHY 都要放出复位，即使我们只用 GMAC0。
   *
   *   RTL8211F 这类 PHY **只在链路建立之后才输出 RXCLK**。而 dw_gmac 的
   *   DMA 软复位要等所有时钟域就绪，其中 RX 时钟正是 PHY 送来的 —— 所以
   *   没有链路伙伴，软复位就永远不完成，MAC 起不来，TXCLK 也就不输出。
   *   两个"无时钟"是同一个源头的上下游，不是两个独立故障。
   *
   *   板上把两个网口对接之后，对端就是 GMAC1 的那颗 PHY。它的复位脚
   *   GPIO3_A3 在 board.h 里定义了却从没被调用 —— 一直被摁在复位里，
   *   于是"接了网线"和"没接"对 GMAC0 毫无区别。
   *
   *   ★ 这解释了为什么此前把两个口串起来毫无变化：我们以为在测链路，
   *     其实对端根本没上电工作。**做对照实验前要先确认对照组是活的。**
   */

  /* ★ 参考时钟要先于复位释放。
   *
   *   PHY 的 25MHz 由 SoC 送出（dtsi 里 PHY 节点的
   *   clocks = <&cru REFCLKO25M_GMAC0_OUT>），板上没有晶振。
   *   PHY 出复位那一刻就得有时钟，否则模拟前端不启动。
   */

  /* GMAC 上电要 2.8 秒（见 gmac_bringup_thread 的说明），扔后台去跑，
   * 不占启动路径。优先级压到 100，别和界面抢。
   */

  /* ★ 栈给 8192，别用 2048。
   *
   *   我第一版写的 2048 —— 板上直接崩：CPU1 在 gmac_bringup_thread 的
   *   最后一句（"网络: eth0 已注册"）之后取数异常
   *   ESR=0x96000006（level 2 translation fault）。这个线程里全是带一堆
   *   参数的 syslog，vsnprintf 的栈开销远不止 2KB。
   *
   *   栈溢出的坏处不止是"这个线程死了"：它踩的是相邻内存，后果会延迟
   *   到别处才发作。今天几次"串口突然全哑、USB 也不枚举、只能按 RESET"
   *   很可能都是它。**挑线程栈大小不是估一个够用的数，而是照系统默认来**
   *   —— CONFIG_DEFAULT_TASK_STACKSIZE 就是 8192。
   */

  {
    int tid = kthread_create("gmac", 100, 8192, gmac_bringup_thread, NULL);

    if (tid < 0)
      {
        syslog(LOG_ERR, "ERROR: GMAC 后台线程创建失败: %d\n", tid);
      }
  }
#endif


#ifdef CONFIG_RK3576_WDT
  /* ★ 看门狗**必须最后注册**。
   *
   *   开了 CONFIG_WATCHDOG_AUTOMONITOR 之后，watchdog_register() 当场
   *   就把看门狗跑起来并开始自动喂。注册得早，本函数后面任何一处初始化
   *   卡住都会被判成"系统失控"而复位 —— 而复位之后又会走同一条初始化
   *   路径再卡住，于是变成**每 90 秒重启一次的死循环**。
   *
   *   那比原本的"挂死不动"更难救：挂死至少还能断电重来，启动死循环连
   *   nsh 提示符都进不去，`flash.sh` 靠串口敲 loader 的免 recovery 路子
   *   就用不了了。
   *
   *   放到最后，被看着的就只有"启动完成之后"这一段 —— 那正是我们想要
   *   的范围：跑测试跑挂了，自己回来。
   */

  ret = rk3576_wdt_initialize("/dev/watchdog0");
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 看门狗初始化失败: %d\n", ret);
    }
  else
    {
      syslog(LOG_INFO, "看门狗: /dev/watchdog0 就绪\n");
    }
#endif


  return OK;
}
