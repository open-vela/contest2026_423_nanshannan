/****************************************************************************
 * app/kickpi_ui/k7_wireless.h
 * SPDX-License-Identifier: Apache-2.0
 *
 * 设置页的 WLAN / 蓝牙：后台一个线程按顺序做模组启动、扫描、连接。
 *
 * 两样都在 SKW6621S 一颗模组上（同一份 CP 固件），驱动的启动函数没有
 * 互斥，所以只用一个线程串行处理。启动要 40 s 左右，扫描 1~8 s，连接
 * 加 DHCP 最多十几秒 —— 都不能在界面线程里做。界面只调这里的请求函数
 * （立即返回）和取状态函数（拷一份快照），按 gen 判断要不要重画。
 *
 * 只在用户点了开关之后才启动模组，不在开机路径上：模组卡住时开机不受
 * 影响（见 HANDOFF"未验证的硬件流程不放进启动路径"）。
 *
 * 模组不能单独断电（WiFi 与蓝牙共用），所以"关"= 断开连接 / 停止扫描。
 ****************************************************************************/

#ifndef __APP_KICKPI_UI_K7_WIRELESS_H
#define __APP_KICKPI_UI_K7_WIRELESS_H

#include <stdbool.h>
#include <stdint.h>

#define K7_WIFI_MAX_AP  24
#define K7_BT_MAX_DEV   24

enum k7_wl_state_e
{
  K7_WL_OFF = 0,        /* 没开（或已关） */
  K7_WL_STARTING,       /* 模组启动中（WiFi 约 40 s） */
  K7_WL_SCANNING,
  K7_WL_IDLE,           /* 已开，没连 / 扫描完了 */
  K7_WL_CONNECTING,     /* WiFi：认证 + 四次握手 */
  K7_WL_DHCP,           /* WiFi：取地址 */
  K7_WL_CONNECTED,      /* WiFi：已连上并拿到地址 */
  K7_WL_FAILED          /* 启动失败，err 里是原因 */
};

/* 最近一次失败发生在哪一步，界面据此给提示 */

enum k7_wl_fail_e
{
  K7_FAIL_NONE = 0,
  K7_FAIL_START,        /* 模组启动 */
  K7_FAIL_SCAN,
  K7_FAIL_NOTFOUND,     /* 没扫到这个网络 */
  K7_FAIL_AUTH,         /* 认证 / 握手失败，多半是密码错 */
  K7_FAIL_DHCP,
  K7_FAIL_LOST          /* 连上后掉线 */
};

struct k7_wifi_ap_s
{
  char    ssid[33];
  int16_t signal;       /* dBm，同名多个 BSS 取最强 */
  uint8_t band;         /* 0 = 2.4 GHz，1 = 5 GHz */
  bool    secure;
  bool    saved;        /* /data 里存了这个网络的密码 */
};

struct k7_wifi_status_s
{
  uint8_t  state;       /* enum k7_wl_state_e */
  uint8_t  fail;        /* enum k7_wl_fail_e，最近一次失败 */
  int      err;         /* 失败时驱动返回的 errno（正数） */
  uint32_t gen;         /* 任何字段变了就加一 */
  char     ssid[33];    /* 正在连 / 已连上的网络 */
  char     ip[16];
  int      nap;
  struct k7_wifi_ap_s ap[K7_WIFI_MAX_AP];   /* 按信号从强到弱 */
};

struct k7_bt_dev_s
{
  uint8_t addr[6];      /* addr[5] 是最高字节，显示时倒着打 */
  int8_t  rssi;
  char    name[32];     /* 广播里的设备名，没有为 "" */
};

struct k7_bt_status_s
{
  uint8_t  state;       /* OFF / STARTING / SCANNING / IDLE / FAILED */
  int      err;
  uint32_t gen;
  int      ndev;
  struct k7_bt_dev_s dev[K7_BT_MAX_DEV];    /* 按信号从强到弱 */
};

/* 请求都是异步的，立即返回；同类请求没处理完时再发会被合并。 */

void k7_wifi_enable(bool on);     /* 开：启动模组并扫描；关：断开 */
void k7_wifi_scan(void);
void k7_wifi_connect(const char *ssid, const char *psk);  /* psk 可 NULL */
void k7_wifi_disconnect(void);
void k7_wifi_forget(const char *ssid);
void k7_wifi_status(struct k7_wifi_status_s *st);

void k7_bt_enable(bool on);       /* 开：启动并扫描 15 s；关：停扫 */
void k7_bt_scan(void);
void k7_bt_status(struct k7_bt_status_s *st);

/* 存过密码返回 true（psk 拷进去），给界面判断要不要弹键盘 */

bool k7_wifi_saved_psk(const char *ssid, char *psk, int len);

#endif /* __APP_KICKPI_UI_K7_WIRELESS_H */
