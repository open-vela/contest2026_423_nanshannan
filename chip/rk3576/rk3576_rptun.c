/****************************************************************************
 * chip/rk3576/rk3576_rptun.c
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
 * RK3576 AMP：openvela ←→ Linux 的 rpmsg 传输层（rptun 后端）。
 *
 * ★ 这一层要解决的问题
 *
 *   AMP 里两个 OS 各占一个簇（openvela 四个 A53、Linux 四个 A72），共享
 *   同一片 DDR 和同一个 GIC。它们之间要能说话，就得有一条约定好的通道。
 *   Rockchip 给的方案是 **vring 放共享内存 + mailbox 当门铃**：
 *
 *     drivers/rpmsg/rockchip_rpmsg_mbox.c  ← Linux 侧（master）
 *     drivers/mailbox/rockchip-mailbox.c   ← 门铃
 *
 *   本文件是这条线的 openvela 侧。
 *
 * ★ 它不是标准的 remoteproc 协议 —— 这是全部难点所在
 *
 *   OpenAMP 的常规流程是：master 把 resource table 放进共享内存，remote
 *   启动后去读，从里面得到 vring 地址、buffer 大小、feature 位。Rockchip
 *   这套**线上根本没有 resource table**：
 *
 *     - vring 地址写死在双方的 DTS / 头文件里；
 *     - "kick" 就是往 mailbox 写 {cmd = link_id, data = 0x524D5347}；
 *     - Linux 只宣告一个 feature：VIRTIO_RPMSG_F_NS。
 *
 *   所以我们**自己在本地造一份 resource table**，填上和 Linux 一样的几何
 *   参数。rptun 拿它当真表用，get_resource() 立刻返回，不等任何人。
 *
 * ★ 握手：为什么要在中断里改 status
 *
 *   rptun_dev_start() 里有这么一段（drivers/rptun/rptun.c）：
 *
 *     role = IS_MASTER ^ (reserved[0] == VIRTIO_DEV_DRIVER);
 *     if (role == VIRTIO_DEV_DEVICE && !(status & DRIVER_OK))
 *         return -EAGAIN;
 *
 *   device 一侧要等 driver 一侧把 DRIVER_OK 写进表里才肯往下走。可 Linux
 *   压根不知道有这张表，这个位永远不会被置上，rptun 就一直 -EAGAIN。
 *
 *   真正等价的信号是 **Linux 的第一次 kick**：rockchip_rpmsg_mbox.c 里
 *   first_notify 那段，Linux 建好两个 virtqueue、填好接收缓冲之后才发。
 *   收到它，就说明对端已经 DRIVER_OK 了。于是在 mailbox 回调里把本地表的
 *   status 补上，再通知 rptun 重试 —— 这不是绕过检查，是把检查的输入从
 *   "对端写的内存"换成"对端发的门铃"，语义一致。
 *
 * ★ 两个 mailbox group，不是一个
 *
 *   Linux 的 DTS 写 mboxes = <&mailbox0 0>, <&mailbox3 0>，名字
 *   "rpmsg-rx"/"rpmsg-tx"。也就是说**收发各用一个 group**。原因在硬件：
 *   一个 group 只有一格信箱，收发共用会互相覆盖。
 *
 *     vring0（我们发数据给 Linux） → group 0 的 B2A → Linux 的 rpmsg-rx
 *     vring1（还 Linux 的发送缓冲） → group 3 的 B2A
 *     Linux 发给我们                ← group 3 的 A2B ← 我们的 IRQ 174
 *
 * 出处：Linux 6.1 drivers/rpmsg/rockchip_rpmsg_mbox.c、
 *       include/linux/rpmsg/rockchip_rpmsg.h（几何参数与 magic），
 *       nuttx/drivers/rptun/rptun.c（role 与 DRIVER_OK 的判定）。
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/kmalloc.h>
#include <nuttx/rptun/rptun.h>

#include "rk3576_mailbox.h"
#include "rk3576_rptun.h"

#ifdef CONFIG_RK3576_RPTUN

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 线上常量，必须和 include/linux/rpmsg/rockchip_rpmsg.h 一致 */

#define RK3576_RPMSG_MBOX_MAGIC 0x524d5347u   /* "RMSG" */

/* link_id 高 4 位是 master 的 cpu_id、低 4 位是 remote 的（同一头文件里的
 * RPMSG_GET_M_CPU_ID / RPMSG_GET_R_CPU_ID）。它只是个标签，两边 DTS /
 * Kconfig 填一样就行，和 MPIDR 没关系。
 */

#define RK3576_RPMSG_LINK_ID    CONFIG_RK3576_RPTUN_LINK_ID

/* 收发用的 mailbox group */

#define RK3576_RPMSG_TX_GROUP   CONFIG_RK3576_RPTUN_TX_GROUP
#define RK3576_RPMSG_RX_GROUP   CONFIG_RK3576_RPTUN_RX_GROUP

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_rptun_dev_s
{
  struct rptun_dev_s rptun;      /* 必须是第一个成员 */
  rptun_callback_t   callback;
  void              *arg;
  char               cpuname[RPMSG_NAME_SIZE + 1];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rptun_rsc_s          g_rsc aligned_data(8);
static struct rk3576_rptun_stat_s  g_stat;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static const char *rk3576_rptun_get_cpuname(struct rptun_dev_s *dev)
{
  struct rk3576_rptun_dev_s *priv = (struct rk3576_rptun_dev_s *)dev;
  return priv->cpuname;
}

static struct resource_table *
rk3576_rptun_get_resource(struct rptun_dev_s *dev)
{
  UNUSED(dev);

  /* 表是我们自己造的（见文件头），这里永远不会阻塞。 */

  return &g_rsc.rsc_tbl_hdr;
}

static bool rk3576_rptun_is_autostart(struct rptun_dev_s *dev)
{
  UNUSED(dev);
  return true;
}

static bool rk3576_rptun_is_master(struct rptun_dev_s *dev)
{
  UNUSED(dev);

  /* Linux 是 master，我们是 remote。 */

  return false;
}

static int rk3576_rptun_start(struct rptun_dev_s *dev)
{
  UNUSED(dev);

  /* 对端是另一个 OS，不由我们上下电。 */

  return 0;
}

static int rk3576_rptun_stop(struct rptun_dev_s *dev)
{
  UNUSED(dev);
  return 0;
}

/****************************************************************************
 * Name: rk3576_rptun_notify
 *
 * Description:
 *   按门铃。notifyid 是 vring 的编号：0 号是"我发给你的数据好了"，
 *   1 号是"你发给我的缓冲我用完了，还给你"。两者走不同的 mailbox group,
 *   否则后一条会把前一条覆盖掉。
 *
 ****************************************************************************/

static int rk3576_rptun_notify(struct rptun_dev_s *dev, uint32_t notifyid)
{
  unsigned int group;
  int ret;

  UNUSED(dev);

  if (notifyid == 0 || notifyid == RPTUN_NOTIFY_ALL)
    {
      group = RK3576_RPMSG_TX_GROUP;
    }
  else if (notifyid == 1)
    {
      group = RK3576_RPMSG_RX_GROUP;
    }
  else
    {
      return -EINVAL;
    }

  ret = rk3576_mailbox_send(group, RK3576_RPMSG_LINK_ID & 0xffu,
                            RK3576_RPMSG_MBOX_MAGIC);
  if (ret == -EBUSY)
    {
      /* 信箱还没被对端清空。丢掉这一次门铃是安全的：vring 里的数据已经
       * 可见，对端处理上一条门铃时自然会扫到；门铃是边沿提示，不是队列。
       */

      g_stat.tx_busy++;
      return OK;
    }

  if (ret >= 0)
    {
      g_stat.kicks_tx++;
    }

  return ret;
}

/****************************************************************************
 * Name: rk3576_rptun_mbox_callback
 *
 * Description:
 *   对端按门铃了（mailbox 中断上下文）。
 *
 ****************************************************************************/

static void rk3576_rptun_mbox_callback(void *arg, uint32_t cmd,
                                       uint32_t data)
{
  struct rk3576_rptun_dev_s *priv = arg;

  g_stat.last_cmd  = cmd;
  g_stat.last_data = data;

  if ((cmd & 0xffu) != (RK3576_RPMSG_LINK_ID & 0xffu) ||
      data != RK3576_RPMSG_MBOX_MAGIC)
    {
      /* 不是这条链路的消息。不报错 —— 别的 group 复用同一个 pclk，
       * 调试期收到别人的门铃是可能的。
       */

      return;
    }

  g_stat.kicks_rx++;

  /* 第一次 kick 等价于对端 DRIVER_OK，见文件头「握手」一节。 */

  if ((__atomic_load_n(&g_rsc.rpmsg_vdev.status, __ATOMIC_ACQUIRE) &
       VIRTIO_CONFIG_STATUS_DRIVER_OK) == 0)
    {
      g_rsc.rpmsg_vdev.gfeatures = g_rsc.rpmsg_vdev.dfeatures;
      __atomic_store_n(&g_rsc.rpmsg_vdev.status,
                       VIRTIO_CONFIG_STATUS_DRIVER_OK, __ATOMIC_RELEASE);
      g_stat.driver_ok = true;
    }

  if (priv->callback != NULL)
    {
      /* 门铃不带队列号，只能让上层把两个 vring 都扫一遍。 */

      priv->callback(priv->arg, RPTUN_NOTIFY_ALL);
    }
}

static int rk3576_rptun_register_callback(struct rptun_dev_s *dev,
                                          rptun_callback_t callback,
                                          void *arg)
{
  struct rk3576_rptun_dev_s *priv = (struct rk3576_rptun_dev_s *)dev;

  priv->callback = callback;
  priv->arg      = arg;

  if (callback != NULL)
    {
      rk3576_mailbox_register_callback(rk3576_rptun_mbox_callback, priv);
    }
  else
    {
      rk3576_mailbox_register_callback(NULL, NULL);
    }

  return 0;
}

static const struct rptun_ops_s g_rk3576_rptun_ops =
{
  .get_cpuname       = rk3576_rptun_get_cpuname,
  .get_resource      = rk3576_rptun_get_resource,
  .is_autostart      = rk3576_rptun_is_autostart,
  .is_master         = rk3576_rptun_is_master,
  .start             = rk3576_rptun_start,
  .stop              = rk3576_rptun_stop,
  .notify            = rk3576_rptun_notify,
  .register_callback = rk3576_rptun_register_callback,
};

/****************************************************************************
 * Name: rk3576_rptun_setup_rsc
 *
 * Description:
 *   造本地 resource table。几何参数（num / align / buf size）三处必须一致：
 *   这里、Linux 的 rockchip_rpmsg.h、以及双方 DTS 里划的那片保留内存。
 *   对不上的表现不是报错，是 vring 索引算出来指到别处，收到乱码。
 *
 ****************************************************************************/

static void rk3576_rptun_setup_rsc(const char *cpuname)
{
  struct rptun_rsc_s *rsc = &g_rsc;

  memset(rsc, 0, sizeof(*rsc));

  rsc->rsc_tbl_hdr.ver = 1;
  rsc->rsc_tbl_hdr.num = 2;
  rsc->offset[0]       = offsetof(struct rptun_rsc_s, rpmsg_vdev);
  rsc->offset[1]       = offsetof(struct rptun_rsc_s, carveout);

  rsc->rpmsg_vdev.type       = RSC_VDEV;
  rsc->rpmsg_vdev.id         = VIRTIO_ID_RPMSG;

  /* ★ 这里要区分两类 feature，我一开始搞混了。
   *
   *   Linux 的 rk_rpmsg_get_features() 只返回 VIRTIO_RPMSG_F_NS。所以
   *   **会改变线上行为**的扩展一位都不能宣告 —— ACK（要求对端回确认）、
   *   BUFSZ（从 config 协商缓冲大小）都属于这一类，宣告了就会发出
   *   Linux 解析不了的报文。
   *
   *   但 **CPUNAME 不属于这一类**。它唯一的作用是让 rpmsg_virtio 去读
   *   本地 resource table 的 config 段里那两个名字（host_cpuname /
   *   remote_cpuname），**不产生任何线上报文**。而这张表从头到尾只有
   *   我们自己读 —— Linux 那套 rpmsg-over-mailbox 线上根本没有
   *   resource table。
   *
   *   我最初只宣告了 NS，理由是"多宣告一位会让对端解析不了"。那条规则
   *   本身对，但套用到 CPUNAME 上是错的。代价是上板直接断言：
   *
   *     Assertion failed : at file: rpmsg/rpmsg_virtio.c:835
   *
   *   那一行是 DEBUGASSERT(virtio_has_feature(vdev, VIRTIO_RPMSG_F_CPUNAME))
   *   —— openvela 的 rpmsg_virtio **要求**这一位存在，否则它拿不到
   *   cpu 名字。
   *
   *   同一个断言在同届 contest2026_062_PharosTech 的板测记录里也出现过
   *   （"openvela 本地资源表缺 CPUNAME 配置，触发 rpmsg_virtio.c:835
   *   断言"）。我读过那句，但当时没理解它和"只宣告 NS"是同一件事的
   *   两面。
   */

  rsc->rpmsg_vdev.dfeatures  = (1u << VIRTIO_RPMSG_F_NS) |
                               (1u << VIRTIO_RPMSG_F_CPUNAME);
  rsc->rpmsg_vdev.config_len = sizeof(rsc->config);
  rsc->rpmsg_vdev.num_of_vrings = 2;
  rsc->rpmsg_vdev.notifyid   = RSC_NOTIFY_ID_ANY;

  /* 表里记的是"相对 master"的角色；rptun 会按 is_master 取反，
   * 于是本端成为 virtio device。
   */

  rsc->rpmsg_vdev.reserved[0] = VIRTIO_DEV_DRIVER;

  strlcpy((char *)rsc->config.host_cpuname, cpuname,
          sizeof(rsc->config.host_cpuname));
  strlcpy((char *)rsc->config.remote_cpuname, CONFIG_RPMSG_LOCAL_CPUNAME,
          sizeof(rsc->config.remote_cpuname));

  rsc->rpmsg_vring0.da       = RK3576_RPTUN_VRING0_DA;
  rsc->rpmsg_vring0.align    = RK3576_RPTUN_VRING_ALIGN;
  rsc->rpmsg_vring0.num      = RK3576_RPTUN_VRING_NUM;
  rsc->rpmsg_vring0.notifyid = 0;

  rsc->rpmsg_vring1.da       = RK3576_RPTUN_VRING1_DA;
  rsc->rpmsg_vring1.align    = RK3576_RPTUN_VRING_ALIGN;
  rsc->rpmsg_vring1.num      = RK3576_RPTUN_VRING_NUM;
  rsc->rpmsg_vring1.notifyid = 1;

  rsc->carveout.type = RSC_CARVEOUT;
  rsc->carveout.da   = RK3576_RPTUN_POOL_DA;
  rsc->carveout.pa   = RK3576_RPTUN_POOL_DA;
  rsc->carveout.len  = RK3576_RPTUN_POOL_LEN;
  strlcpy((char *)rsc->carveout.name, "vdev0buffer",
          sizeof(rsc->carveout.name));
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int rk3576_rptun_init(const char *cpuname)
{
  struct rk3576_rptun_dev_s *priv;
  int ret;

  priv = kmm_zalloc(sizeof(*priv));
  if (priv == NULL)
    {
      return -ENOMEM;
    }

  priv->rptun.ops = &g_rk3576_rptun_ops;
  strlcpy(priv->cpuname, cpuname, sizeof(priv->cpuname));

  rk3576_rptun_setup_rsc(cpuname);

  ret = rk3576_mailbox_initialize(RK3576_RPMSG_RX_GROUP);
  if (ret < 0)
    {
      syslog(LOG_ERR, "AMP: mailbox 初始化失败: %d\n", ret);
      goto err;
    }

  ret = rptun_initialize(&priv->rptun);
  if (ret < 0)
    {
      syslog(LOG_ERR, "AMP: rptun_initialize 失败: %d\n", ret);
      goto err;
    }

  g_stat.registered = true;

  syslog(LOG_INFO,
         "AMP: rptun 就绪 对端=%s vring0=%08x vring1=%08x 池=%08x+%dK "
         "门铃 发=group%d 收=group%d\n",
         cpuname, RK3576_RPTUN_VRING0_DA, RK3576_RPTUN_VRING1_DA,
         RK3576_RPTUN_POOL_DA, RK3576_RPTUN_POOL_LEN / 1024,
         RK3576_RPMSG_TX_GROUP, RK3576_RPMSG_RX_GROUP);
  return OK;

err:
  kmm_free(priv);
  return ret;
}

int rk3576_rptun_getstat(struct rk3576_rptun_stat_s *stat)
{
  if (stat == NULL)
    {
      return -EINVAL;
    }

  *stat = g_stat;
  stat->driver_ok = (g_rsc.rpmsg_vdev.status &
                     VIRTIO_CONFIG_STATUS_DRIVER_OK) != 0;
  return OK;
}

#endif /* CONFIG_RK3576_RPTUN */
