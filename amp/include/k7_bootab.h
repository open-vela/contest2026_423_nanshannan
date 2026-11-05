/****************************************************************************
 * amp/include/k7_bootab.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * openvela 镜像（AMP FIT）的 A/B 槽与启动控制块。写入方是 openvela 的
 * bootctl / ota 命令（app/k7ota），读取方是 U-Boot 的 bootamp（选槽、校验、
 * 失败回退）。两边按这一份定义。
 *
 * ★ 三个位置
 *
 *   救援 FIT  trust 分区  LBA 24576，上限 8184 扇区（≈4MB）
 *             flash.sh 用 rkdeveloptool 写的就是这一份，永远保留。
 *   控制块    trust 分区末尾 LBA 32760 / 32761（两份），32762..32767 不用
 *             放在 rkdeveloptool 写得到的范围（< 65536）里：flash.sh 写完
 *             救援 FIT 顺手把控制块清零，开发时的烧写流程不受 A/B 影响。
 *   A/B 槽    出厂 GPT 的 recovery 分区（LBA 182272，96MB），每槽 32MB：
 *               A  分区内扇区 2048  .. 67583
 *               B  分区内扇区 67584 .. 133119
 *             槽里直接放 FIT（mkimage -E 外置数据格式），没有额外头部。
 *             rkdeveloptool 写不到这里（LBA ≥ 65536），只能由 openvela 经
 *             网络写（ota 命令）——这本来就是 OTA 的路径。
 *
 * ★ 状态语义与 openvela frameworks/system/ota 的 bootctl 一致
 *
 *   每槽 active / bootable / successful 三个标志，外加全局 try：
 *     bootctl update  目标槽清成不可启动（开始写之前）
 *     bootctl done    目标槽 active + bootable，当前槽 active 清掉
 *     （重启）        U-Boot 见 active+bootable 但未 successful：置 try，试启动
 *     bootctl         启动成功，当前槽 successful，清 try
 *     （试启动失败）  再次进 U-Boot 时 try 仍在：该槽作废，回到上一个
 *                     successful 的槽；两个都不行就启动救援 FIT
 *   上游 bootctl 把这些标志存在 KVDB 里，U-Boot 读不了；这里换成一个扇区
 *   的二进制结构，选槽规则照搬上游 bootctl_boot()。
 *
 * ★ 写法：两份轮流写，seq 递增，crc32 覆盖之前所有字节
 *
 *   读取方取 crc 正确且 seq 较大的那份。写到一半断电，坏的只是正在写的
 *   那份，另一份还是上一个完整状态。两份都无效 = 没有 A/B，启动救援 FIT
 *   （和没有这套机制之前完全一样）。
 ****************************************************************************/

#ifndef __AMP_INCLUDE_K7_BOOTAB_H
#define __AMP_INCLUDE_K7_BOOTAB_H

#include <stdint.h>

#define K7AB_MAGIC          "K7ABCTRL"     /* 8 字节，不含结尾 0 */
#define K7AB_VERSION        1
#define K7AB_SECTOR         512

/* 控制块：整盘 LBA（trust 分区末尾） */

#define K7AB_CTRL_LBA       32760
#define K7AB_CTRL_COPIES    2
#define K7AB_CTRL_SECTORS   8              /* 32760..32767 整段归控制块 */

/* A/B 槽：recovery 分区，扇区号从分区起点算 */

#define K7AB_PART_LBA       182272
#define K7AB_PART_SECTORS   196608         /* 96MB */
#define K7AB_SLOT_SECTORS   65536          /* 32MB */
#define K7AB_SLOT_A         2048
#define K7AB_SLOT_B         (K7AB_SLOT_A + K7AB_SLOT_SECTORS)

#define K7AB_NSLOTS         2
#define K7AB_BOOTED_NONE    0xff           /* 启动的是救援 FIT */

struct k7ab_slot_s
{
  uint8_t  active;          /* 当前选中的槽 */
  uint8_t  bootable;        /* 写完并校验过，可以试启动 */
  uint8_t  successful;      /* 启动成功被确认过 */
  uint8_t  reserved;
  uint32_t length;          /* 槽里 FIT 的字节数 */
  uint8_t  sha256[32];      /* 槽里 FIT 的 SHA-256（openvela 写入时校验用） */
};

struct k7ab_ctrl_s
{
  char     magic[8];
  uint32_t version;
  uint32_t seq;             /* 每写一次加一 */
  uint8_t  try_boot;        /* 上游 bootctl 的 try：正在试启动未确认的槽 */
  uint8_t  booted;          /* U-Boot 实际启动的槽：0=A 1=B，0xff=救援 */
  uint8_t  boot_count;      /* 试启动次数（只增，诊断用） */
  uint8_t  reserved;
  struct k7ab_slot_s slot[K7AB_NSLOTS];
  uint32_t crc32;           /* 覆盖本字段之前的所有字节，zlib crc32 */
};

#endif /* __AMP_INCLUDE_K7_BOOTAB_H */
