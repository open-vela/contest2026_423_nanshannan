/****************************************************************************
 * app/k7ota/k7ab.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * openvela 镜像 A/B 控制块的读写（格式见 amp/include/k7_bootab.h）。
 * bootctl / ota 两个命令和界面（启动成功后自动确认）共用。
 ****************************************************************************/

#ifndef __APP_K7OTA_K7AB_H
#define __APP_K7OTA_K7AB_H

#include <stdbool.h>

#include "k7_bootab.h"

#define K7AB_CTRL_DEV  "/dev/abctl"
#define K7AB_SLOT_DEV  "/dev/abslot"

/* 读控制块。返回 0 = 有效；-ENOENT = 两份都无效（从没做过 OTA，或
 * flash.sh 刚清过），此时 *c 被填成一个干净的初始状态（booted = 救援）；
 * 其他负值 = 读设备出错。
 */

int k7ab_load(FAR struct k7ab_ctrl_s *c);

/* seq 加一、算 crc，写 seq 奇偶对应的那一份并读回比对 */

int k7ab_store(FAR struct k7ab_ctrl_s *c);

/* 两份都清零：下次启动用救援 FIT */

int k7ab_clear(void);

/* 要升级的槽：不是本次启动的那个。从救援 FIT 启动时，选不是
 * "active 且 successful" 的那个（都不是就选 A）。
 */

int k7ab_target(FAR const struct k7ab_ctrl_s *c);

/* 上游 bootctl 的三个动作。update / done 作用在 k7ab_target() 上。
 * k7ab_success() 返回 1 = 这次才确认，0 = 早已确认或启动的是救援 FIT。
 */

int k7ab_update(void);
int k7ab_done(uint32_t length, FAR const uint8_t *sha256);
int k7ab_success(void);

/* 槽在 /dev/abslot 里的起始扇区；名字 'A' / 'B' */

uint32_t k7ab_slot_sector(int slot);
char k7ab_slot_name(int slot);

void k7ab_print(FAR const struct k7ab_ctrl_s *c);

#endif /* __APP_K7OTA_K7AB_H */
