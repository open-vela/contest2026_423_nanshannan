/****************************************************************************
 * app/kickpi_ui/k7_wireless.c
 * SPDX-License-Identifier: Apache-2.0
 *
 * 设置页 WLAN / 蓝牙的后台线程，接口说明见 k7_wireless.h。
 *
 * WiFi 走板级函数（kickpi_k7_wifi_*，平坦地址空间直接调）；地址用
 * netlib 的 DHCP 客户端，和 nsh 的 `renew wlan0` 是同一条路。
 * 蓝牙扫描走 NuttX 蓝牙协议栈的 ioctl（SIOCBTSCANSTART/GET/STOP，
 * 与 btsak 相同），接口名 bnep0。
 *
 * 保存的网络在 /data/k7wifi.conf，一行一个："SSID<TAB>密码"。板上本地
 * 文件，不进仓库。
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <arch/board/board.h>
#include <netutils/netlib.h>

#ifdef CONFIG_NET_BLUETOOTH
#  include <netpacket/bluetooth.h>
#  include <nuttx/wireless/bluetooth/bt_ioctl.h>
#endif

#include "k7_wireless.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WIFI_IFNAME     "wlan0"
#define BT_IFNAME       "bnep0"
#define WIFI_CONF       "/data/k7wifi.conf"
#define WIFI_SAVED_MAX  8

#define BT_SCAN_MS      15000   /* 一次扫描的时长，到点自动停 */
#define BT_POLL_MS      200     /* 内核只缓存 8 条，扫描时要勤取 */
#define LINK_CHECK_MS   2000    /* 连上后多久看一次有没有掉线 */

/* 请求位 */

#define REQ_WIFI_ON     (1 << 0)
#define REQ_WIFI_OFF    (1 << 1)
#define REQ_WIFI_SCAN   (1 << 2)
#define REQ_WIFI_CONN   (1 << 3)
#define REQ_BT_ON       (1 << 4)
#define REQ_BT_OFF      (1 << 5)
#define REQ_BT_SCAN     (1 << 6)
#define REQ_WIFI_DISC   (1 << 7)

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static sem_t      g_wake;
static pthread_t  g_thread;
static bool       g_started;
static unsigned   g_req;
static char       g_req_ssid[33];
static char       g_req_psk[65];

static struct k7_wifi_status_s g_wifi;
static struct k7_bt_status_s   g_bt;

static bool       g_wifi_ready;     /* 模组 WiFi 服务已起来（不会再落回去） */
static bool       g_bt_ready;
static uint64_t   g_bt_scan_end;    /* 0 = 没在扫 */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint64_t now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 状态改动都经过这两个：持锁改、gen 加一 */

static void wifi_set(uint8_t state, uint8_t fail, int err)
{
  pthread_mutex_lock(&g_lock);
  g_wifi.state = state;
  if (fail != K7_FAIL_NONE)
    {
      g_wifi.fail = fail;
      g_wifi.err = err < 0 ? -err : err;
    }

  if (state != K7_WL_CONNECTED)
    {
      g_wifi.ip[0] = '\0';
    }

  g_wifi.gen++;
  pthread_mutex_unlock(&g_lock);
}

static void bt_set(uint8_t state, int err)
{
  pthread_mutex_lock(&g_lock);
  g_bt.state = state;
  g_bt.err = err < 0 ? -err : err;
  g_bt.gen++;
  pthread_mutex_unlock(&g_lock);
}

/* ---- 保存的网络 ---- */

struct saved_s
{
  char ssid[33];
  char psk[65];
};

static int saved_load(struct saved_s *s, int max)
{
  char line[112];
  FILE *fp = fopen(WIFI_CONF, "r");
  int n = 0;

  if (fp == NULL)
    {
      return 0;
    }

  while (n < max && fgets(line, sizeof(line), fp) != NULL)
    {
      char *tab = strchr(line, '\t');

      line[strcspn(line, "\r\n")] = '\0';
      if (tab == NULL || tab == line)
        {
          continue;
        }

      *tab = '\0';
      strlcpy(s[n].ssid, line, sizeof(s[n].ssid));
      strlcpy(s[n].psk, tab + 1, sizeof(s[n].psk));
      n++;
    }

  fclose(fp);
  return n;
}

static void saved_store(const struct saved_s *s, int n)
{
  FILE *fp = fopen(WIFI_CONF, "w");
  int i;

  if (fp == NULL)
    {
      syslog(LOG_WARNING, "WLAN: 写不了 %s: %d\n", WIFI_CONF, errno);
      return;
    }

  for (i = 0; i < n; i++)
    {
      fprintf(fp, "%s\t%s\n", s[i].ssid, s[i].psk);
    }

  fclose(fp);
}

/* 连上之后记下来，放在第一个（最近用过的在前） */

static void saved_put(const char *ssid, const char *psk)
{
  struct saved_s s[WIFI_SAVED_MAX];
  struct saved_s tmp;
  int n = saved_load(s, WIFI_SAVED_MAX);
  int i;

  for (i = 0; i < n && strcmp(s[i].ssid, ssid) != 0; i++)
    {
    }

  if (i == n)
    {
      if (n == WIFI_SAVED_MAX)
        {
          i = --n;          /* 满了，挤掉最旧的 */
        }

      n++;
    }

  strlcpy(tmp.ssid, ssid, sizeof(tmp.ssid));
  strlcpy(tmp.psk, psk != NULL ? psk : "", sizeof(tmp.psk));
  memmove(&s[1], &s[0], sizeof(s[0]) * i);
  s[0] = tmp;
  saved_store(s, n);
}

static void saved_del(const char *ssid)
{
  struct saved_s s[WIFI_SAVED_MAX];
  int n = saved_load(s, WIFI_SAVED_MAX);
  int i;

  for (i = 0; i < n; i++)
    {
      if (strcmp(s[i].ssid, ssid) == 0)
        {
          memmove(&s[i], &s[i + 1], sizeof(s[0]) * (n - i - 1));
          saved_store(s, n - 1);
          return;
        }
    }
}

/* ---- WiFi ---- */

static int ap_cmp(const void *a, const void *b)
{
  return ((const struct k7_wifi_ap_s *)b)->signal -
         ((const struct k7_wifi_ap_s *)a)->signal;
}

/* 扫描结果合并同名 BSS（取最强），去掉隐藏网络，按信号排序 */

static int wifi_do_scan(void)
{
  static struct kickpi_k7_wifi_ap_s raw[48];
  struct k7_wifi_ap_s ap[K7_WIFI_MAX_AP];
  struct saved_s s[WIFI_SAVED_MAX];
  int nsaved;
  int nraw;
  int nap = 0;
  int ret;
  int i;
  int j;

  wifi_set(g_wifi.state == K7_WL_CONNECTED ? K7_WL_CONNECTED :
           K7_WL_SCANNING, K7_FAIL_NONE, 0);

  ret = kickpi_k7_wifi_scan();
  nraw = kickpi_k7_wifi_results(raw, 48);
  if (ret < 0 && nraw <= 0)
    {
      return ret;
    }

  nsaved = saved_load(s, WIFI_SAVED_MAX);

  for (i = 0; i < nraw; i++)
    {
      if (raw[i].ssid[0] == '\0')
        {
          continue;
        }

      for (j = 0; j < nap && strcmp(ap[j].ssid, raw[i].ssid) != 0; j++)
        {
        }

      if (j < nap)
        {
          if (raw[i].signal > ap[j].signal)
            {
              ap[j].signal = raw[i].signal;
              ap[j].band = raw[i].band;
            }

          continue;
        }

      if (nap == K7_WIFI_MAX_AP)
        {
          continue;
        }

      strlcpy(ap[nap].ssid, raw[i].ssid, sizeof(ap[nap].ssid));
      ap[nap].signal = raw[i].signal;
      ap[nap].band = raw[i].band;
      ap[nap].secure = raw[i].secure;
      ap[nap].saved = false;
      for (j = 0; j < nsaved; j++)
        {
          if (strcmp(s[j].ssid, ap[nap].ssid) == 0)
            {
              ap[nap].saved = true;
            }
        }

      nap++;
    }

  qsort(ap, nap, sizeof(ap[0]), ap_cmp);

  pthread_mutex_lock(&g_lock);
  memcpy(g_wifi.ap, ap, sizeof(ap[0]) * nap);
  g_wifi.nap = nap;
  g_wifi.gen++;
  pthread_mutex_unlock(&g_lock);
  return OK;
}

static bool wifi_start(void)
{
  int ret;

  if (g_wifi_ready)
    {
      return true;
    }

  wifi_set(K7_WL_STARTING, K7_FAIL_NONE, 0);
  ret = kickpi_k7_wifi_initialize();
  syslog(LOG_INFO, "WLAN: 模组启动 %d\n", ret);
  if (ret < 0)
    {
      wifi_set(K7_WL_FAILED, K7_FAIL_START, ret);
      return false;
    }

  g_wifi_ready = true;
  return true;
}

static void wifi_connect(const char *ssid, const char *psk)
{
  struct in_addr addr;
  int ret;

  pthread_mutex_lock(&g_lock);
  strlcpy(g_wifi.ssid, ssid, sizeof(g_wifi.ssid));
  pthread_mutex_unlock(&g_lock);
  wifi_set(K7_WL_CONNECTING, K7_FAIL_NONE, 0);

  ret = kickpi_k7_wifi_connect(ssid, psk != NULL && psk[0] ? psk : NULL);
  syslog(LOG_INFO, "WLAN: 连接 %d\n", ret);
  if (ret < 0)
    {
      wifi_set(K7_WL_IDLE, ret == -ENOENT ? K7_FAIL_NOTFOUND :
               K7_FAIL_AUTH, ret);
      return;
    }

  wifi_set(K7_WL_DHCP, K7_FAIL_NONE, 0);
  netlib_ifup(WIFI_IFNAME);
  ret = netlib_obtain_ipv4addr(WIFI_IFNAME);
  if (ret < 0 || netlib_get_ipv4addr(WIFI_IFNAME, &addr) < 0 ||
      addr.s_addr == 0)
    {
      syslog(LOG_WARNING, "WLAN: DHCP 失败 %d\n", ret);
      kickpi_k7_wifi_disconnect();
      wifi_set(K7_WL_IDLE, K7_FAIL_DHCP, ret < 0 ? errno : ETIMEDOUT);
      return;
    }

  saved_put(ssid, psk);

  pthread_mutex_lock(&g_lock);
  inet_ntop(AF_INET, &addr, g_wifi.ip, sizeof(g_wifi.ip));
  g_wifi.state = K7_WL_CONNECTED;
  g_wifi.fail = K7_FAIL_NONE;
  g_wifi.gen++;
  pthread_mutex_unlock(&g_lock);
  syslog(LOG_INFO, "WLAN: 已连上，地址 %s\n", g_wifi.ip);
}

static void wifi_off(void)
{
  if (g_wifi.state == K7_WL_CONNECTED || g_wifi.state == K7_WL_DHCP)
    {
      kickpi_k7_wifi_disconnect();
      netlib_ifdown(WIFI_IFNAME);
    }

  pthread_mutex_lock(&g_lock);
  g_wifi.nap = 0;
  g_wifi.ssid[0] = '\0';
  pthread_mutex_unlock(&g_lock);
  wifi_set(K7_WL_OFF, K7_FAIL_NONE, 0);
}

/* 连上之后掉线（AP 关了、走远了）：驱动置 g_connected = false */

static void wifi_check_link(void)
{
  if (g_wifi.state == K7_WL_CONNECTED && !kickpi_k7_wifi_status(NULL, 0))
    {
      syslog(LOG_WARNING, "WLAN: 掉线\n");
      netlib_ifdown(WIFI_IFNAME);
      wifi_set(K7_WL_IDLE, K7_FAIL_LOST, ENOTCONN);
    }
}

/* ---- 蓝牙 ---- */

#ifdef CONFIG_NET_BLUETOOTH
static int bt_ioctl(int cmd, struct btreq_s *req)
{
  int fd = socket(PF_BLUETOOTH, SOCK_RAW, BTPROTO_L2CAP);
  int ret;

  if (fd < 0)
    {
      return -errno;
    }

  strlcpy(req->btr_name, BT_IFNAME, IFNAMSIZ);
  ret = ioctl(fd, cmd, (unsigned long)(uintptr_t)req);
  ret = ret < 0 ? -errno : ret;
  close(fd);
  return ret;
}

/* 广播数据里的设备名：AD 类型 0x09（完整）优先，0x08（缩写）其次 */

static void bt_adv_name(const uint8_t *d, int len, char *name, int cap)
{
  int off = 0;

  while (off + 2 <= len && d[off] != 0 && off + 1 + d[off] <= len)
    {
      int n = d[off] - 1;
      uint8_t type = d[off + 1];

      if ((type == 0x09 || (type == 0x08 && name[0] == '\0')) && n > 0)
        {
          if (n > cap - 1)
            {
              n = cap - 1;
            }

          memcpy(name, d + off + 2, n);
          name[n] = '\0';
          if (type == 0x09)
            {
              return;
            }
        }

      off += 1 + d[off];
    }
}

static int dev_cmp(const void *a, const void *b)
{
  return ((const struct k7_bt_dev_s *)b)->rssi -
         ((const struct k7_bt_dev_s *)a)->rssi;
}

/* 把内核缓存的扫描结果取空，按地址合并进设备表 */

static void bt_collect(void)
{
  struct bt_scanresponse_s rsp[CONFIG_BLUETOOTH_MAXSCANRESULT];
  struct btreq_s req;
  bool changed = false;
  int n;
  int i;
  int j;

  do
    {
      memset(&req, 0, sizeof(req));
      req.btr_nrsp = CONFIG_BLUETOOTH_MAXSCANRESULT;
      req.btr_rsp = rsp;
      if (bt_ioctl(SIOCBTSCANGET, &req) < 0)
        {
          return;
        }

      n = req.btr_nrsp;

      pthread_mutex_lock(&g_lock);
      for (i = 0; i < n; i++)
        {
          struct k7_bt_dev_s *d = NULL;

          for (j = 0; j < g_bt.ndev; j++)
            {
              if (memcmp(g_bt.dev[j].addr, rsp[i].sr_addr.val, 6) == 0)
                {
                  d = &g_bt.dev[j];
                  break;
                }
            }

          if (d == NULL)
            {
              if (g_bt.ndev == K7_BT_MAX_DEV)
                {
                  continue;
                }

              d = &g_bt.dev[g_bt.ndev++];
              memset(d, 0, sizeof(*d));
              memcpy(d->addr, rsp[i].sr_addr.val, 6);
            }

          d->rssi = rsp[i].sr_rssi;
          bt_adv_name(rsp[i].sr_data, rsp[i].sr_len, d->name,
                      sizeof(d->name));
          changed = true;
        }

      if (changed)
        {
          qsort(g_bt.dev, g_bt.ndev, sizeof(g_bt.dev[0]), dev_cmp);
          g_bt.gen++;
        }

      pthread_mutex_unlock(&g_lock);
    }
  while (n == CONFIG_BLUETOOTH_MAXSCANRESULT);
}

static void bt_scan_stop(void)
{
  struct btreq_s req;

  if (g_bt_scan_end != 0)
    {
      memset(&req, 0, sizeof(req));
      bt_ioctl(SIOCBTSCANSTOP, &req);
      g_bt_scan_end = 0;
    }
}

static void bt_scan_start(void)
{
  struct btreq_s req;
  int ret;

  bt_scan_stop();

  pthread_mutex_lock(&g_lock);
  g_bt.ndev = 0;
  pthread_mutex_unlock(&g_lock);

  memset(&req, 0, sizeof(req));
  req.btr_dupenable = true;     /* 控制器过滤重复广播，缓存才不会被刷满 */
  ret = bt_ioctl(SIOCBTSCANSTART, &req);
  syslog(LOG_INFO, "蓝牙: 开始扫描 %d\n", ret);
  if (ret < 0)
    {
      bt_set(K7_WL_FAILED, ret);
      return;
    }

  g_bt_scan_end = now_ms() + BT_SCAN_MS;
  bt_set(K7_WL_SCANNING, 0);
}

static void bt_on(void)
{
  int ret;

  if (!g_bt_ready)
    {
      bt_set(K7_WL_STARTING, 0);
      ret = kickpi_k7_bt_initialize();
      syslog(LOG_INFO, "蓝牙: 模组启动 %d\n", ret);
      if (ret < 0 && ret != -EALREADY)
        {
          bt_set(K7_WL_FAILED, ret);
          return;
        }

      g_bt_ready = true;
    }

  bt_scan_start();
}
#else
static void bt_collect(void)
{
}

static void bt_scan_stop(void)
{
}

static void bt_scan_start(void)
{
  bt_set(K7_WL_FAILED, ENOSYS);
}

static void bt_on(void)
{
  bt_set(K7_WL_FAILED, ENOSYS);
}
#endif

/* ---- 线程 ---- */

static void *wl_thread(void *arg)
{
  uint64_t last_link = 0;

  (void)arg;

  for (; ; )
    {
      struct timespec ts;
      char ssid[33];
      char psk[65];
      unsigned req;

      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += (g_bt_scan_end != 0 ? BT_POLL_MS : 1000) * 1000000l;
      ts.tv_sec += ts.tv_nsec / 1000000000;
      ts.tv_nsec %= 1000000000;
      sem_timedwait(&g_wake, &ts);

      pthread_mutex_lock(&g_lock);
      req = g_req;
      g_req = 0;
      strlcpy(ssid, g_req_ssid, sizeof(ssid));
      strlcpy(psk, g_req_psk, sizeof(psk));
      pthread_mutex_unlock(&g_lock);

      /* 关在前：开 + 关同时到，以后到的为准由请求函数保证（互相清位） */

      if (req & REQ_WIFI_OFF)
        {
          wifi_off();
        }
      else if ((req & REQ_WIFI_DISC) && (g_wifi.state == K7_WL_CONNECTED ||
                                         g_wifi.state == K7_WL_DHCP))
        {
          kickpi_k7_wifi_disconnect();
          netlib_ifdown(WIFI_IFNAME);
          wifi_set(K7_WL_IDLE, K7_FAIL_NONE, 0);
        }

      if ((req & (REQ_WIFI_ON | REQ_WIFI_SCAN | REQ_WIFI_CONN)) &&
          wifi_start())
        {
          if (req & (REQ_WIFI_ON | REQ_WIFI_SCAN))
            {
              int ret = wifi_do_scan();

              if (ret < 0)
                {
                  wifi_set(K7_WL_IDLE, K7_FAIL_SCAN, ret);
                }
              else if (g_wifi.state != K7_WL_CONNECTED)
                {
                  wifi_set(K7_WL_IDLE, K7_FAIL_NONE, 0);
                }
            }

          if (req & REQ_WIFI_CONN)
            {
              wifi_connect(ssid, psk);
            }
        }

      if (req & REQ_BT_OFF)
        {
          bt_scan_stop();
          bt_set(K7_WL_OFF, 0);
        }

      if (req & REQ_BT_ON)
        {
          bt_on();
        }
      else if ((req & REQ_BT_SCAN) && g_bt_ready)
        {
          bt_scan_start();
        }

      if (g_bt_scan_end != 0)
        {
          bt_collect();
          if (now_ms() >= g_bt_scan_end)
            {
              bt_scan_stop();
              bt_set(K7_WL_IDLE, 0);
            }
        }

      if (now_ms() - last_link >= LINK_CHECK_MS)
        {
          last_link = now_ms();
          wifi_check_link();
        }
    }

  return NULL;
}

static void wl_request(unsigned set, unsigned clear)
{
  pthread_mutex_lock(&g_lock);
  if (!g_started)
    {
      pthread_attr_t attr;

      sem_init(&g_wake, 0, 0);
      pthread_attr_init(&attr);
      pthread_attr_setstacksize(&attr, 8192);
      if (pthread_create(&g_thread, &attr, wl_thread, NULL) == 0)
        {
          pthread_setname_np(g_thread, "k7wireless");
          g_started = true;
        }

      pthread_attr_destroy(&attr);
    }

  g_req = (g_req & ~clear) | set;
  pthread_mutex_unlock(&g_lock);
  sem_post(&g_wake);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void k7_wifi_enable(bool on)
{
  if (on)
    {
      wl_request(REQ_WIFI_ON, REQ_WIFI_OFF);
    }
  else
    {
      wl_request(REQ_WIFI_OFF, REQ_WIFI_ON | REQ_WIFI_SCAN | REQ_WIFI_CONN);
    }
}

void k7_wifi_scan(void)
{
  wl_request(REQ_WIFI_SCAN, 0);
}

void k7_wifi_connect(const char *ssid, const char *psk)
{
  pthread_mutex_lock(&g_lock);
  strlcpy(g_req_ssid, ssid, sizeof(g_req_ssid));
  strlcpy(g_req_psk, psk != NULL ? psk : "", sizeof(g_req_psk));
  pthread_mutex_unlock(&g_lock);
  wl_request(REQ_WIFI_CONN, REQ_WIFI_OFF);
}

void k7_wifi_disconnect(void)
{
  wl_request(REQ_WIFI_DISC, REQ_WIFI_CONN);
}

void k7_wifi_forget(const char *ssid)
{
  int i;

  /* 只改文件和列表里的标记，不在界面线程里碰模组 */

  saved_del(ssid);

  pthread_mutex_lock(&g_lock);
  for (i = 0; i < g_wifi.nap; i++)
    {
      if (strcmp(g_wifi.ap[i].ssid, ssid) == 0)
        {
          g_wifi.ap[i].saved = false;
        }
    }

  g_wifi.gen++;
  pthread_mutex_unlock(&g_lock);
}

bool k7_wifi_saved_psk(const char *ssid, char *psk, int len)
{
  struct saved_s s[WIFI_SAVED_MAX];
  int n = saved_load(s, WIFI_SAVED_MAX);
  int i;

  for (i = 0; i < n; i++)
    {
      if (strcmp(s[i].ssid, ssid) == 0)
        {
          strlcpy(psk, s[i].psk, len);
          return true;
        }
    }

  return false;
}

void k7_wifi_status(struct k7_wifi_status_s *st)
{
  pthread_mutex_lock(&g_lock);
  *st = g_wifi;
  pthread_mutex_unlock(&g_lock);
}

void k7_bt_enable(bool on)
{
  if (on)
    {
      wl_request(REQ_BT_ON, REQ_BT_OFF);
    }
  else
    {
      wl_request(REQ_BT_OFF, REQ_BT_ON | REQ_BT_SCAN);
    }
}

void k7_bt_scan(void)
{
  wl_request(REQ_BT_SCAN, 0);
}

void k7_bt_status(struct k7_bt_status_s *st)
{
  pthread_mutex_lock(&g_lock);
  *st = g_bt;
  pthread_mutex_unlock(&g_lock);
}
