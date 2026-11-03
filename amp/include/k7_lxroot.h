/****************************************************************************
 * amp/include/k7_lxroot.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A72 上 Linux 根文件系统镜像在 eMMC 上的存放格式。写入方是 openvela 的
 * lxroot 命令（app/lxroot），读取方是 U-Boot 的 bootamp（读进内存交给
 * Linux 当 initramfs）。两边按这一份定义。
 *
 * 分区：出厂 GPT 的 backup（LBA 378880 + 786432，384MB），openvela 侧注册
 * 为 /dev/lxroot。下文的"扇区"都从分区起点算。
 *
 *   扇区 0        头部 struct k7lx_hdr_s（其余字节为 0）
 *   扇区 1..7     不用
 *   扇区 8 起     镜像本体，length 字节
 *
 * ★ 头部最后写
 *
 *   lxroot write 先把头部清零，再写本体，回读算完 sha256 一致之后才写
 *   头部。中途断电、下载中断、校验不过，留下的都是无效头部 —— 读取方
 *   见到无效头部就当没有外部镜像，不会拿半截数据去启动。
 ****************************************************************************/

#ifndef __AMP_INCLUDE_K7_LXROOT_H
#define __AMP_INCLUDE_K7_LXROOT_H

#include <stdint.h>

#define K7LX_MAGIC          "K7LXROOT"     /* 8 字节，不含结尾 0 */
#define K7LX_VERSION        1
#define K7LX_SECTOR         512
#define K7LX_DATA_SECTOR    8              /* 本体从 4KB 处开始 */

struct k7lx_hdr_s
{
  char     magic[8];
  uint32_t version;
  uint32_t data_sector;    /* 本体起点，分区内扇区号 */
  uint32_t length;         /* 本体字节数 */
  uint32_t reserved;
  uint8_t  sha256[32];     /* 本体的 SHA-256 */
};

#endif /* __AMP_INCLUDE_K7_LXROOT_H */
