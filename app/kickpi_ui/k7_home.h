/****************************************************************************
 * app/kickpi_ui/k7_home.h
 * SPDX-License-Identifier: Apache-2.0
 *
 * 家居：经 MQTT 对接局域网里的 broker（mosquitto / Home Assistant）。
 *
 * 主题约定（设备 id 由界面给）：
 *   home/<id>/state   设备上报，ON / OFF，带 retain：一连上就拿到当前状态
 *   home/<id>/set     面板下发，ON / OFF
 *   home/scene/set    面板下发场景名（home / away / focus / rest）
 *   home/k7panel/status  面板自己的在线状态（遗嘱 offline，连上后 online）
 *   home/doorbell/event  门铃按下（内容不看），面板只数次数
 *   其余主题（录制状态、门口回话等）由调用者用 k7_home_publish 发。
 *
 * 只做 MQTT 3.1.1 的 QoS 0。设备状态以 state 为准：面板发了 set 不算
 * 成功，要等设备回报。
 ****************************************************************************/

#ifndef __APP_KICKPI_UI_K7_HOME_H
#define __APP_KICKPI_UI_K7_HOME_H

#include <stdbool.h>
#include <stdint.h>

#define K7_HOME_MAX 8

struct k7_home_status_s
{
  bool     connected;
  int      err;        /* 最近一次断开 / 连不上的原因（errno，正数） */
  uint32_t gen;        /* 连接状态或任一设备状态变了就加一 */
  char     broker[48]; /* "地址:端口"，界面显示用 */
  uint32_t rings;      /* 收到的门铃次数，界面比对变化 */
};

/* ids 指向的字符串要一直有效。重复调用无效果。 */

int  k7_home_start(const char *const *ids, int n);
void k7_home_status(struct k7_home_status_s *st);

/* 按设备 id 找下标，没有返回 -1 */

int  k7_home_find(const char *id);

/* -1 未知（设备还没报过），0 关，1 开 */

int  k7_home_state(int i);

/* 下发。离线返回 -ENOTCONN，发送失败返回负 errno。 */

int  k7_home_set(int i, bool on);
int  k7_home_scene(const char *name);
int  k7_home_publish(const char *topic, const char *payload, bool retain);

#endif /* __APP_KICKPI_UI_K7_HOME_H */
