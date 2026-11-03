/****************************************************************************
 * app/mqttd/k7mqttd_main.c
 * SPDX-License-Identifier: Apache-2.0
 *
 * k7mqttd：板上自带的精简 MQTT 3.1.1 broker。
 *
 *   k7mqttd [-p 端口]     前台运行（nsh 里 `k7mqttd &` 放后台）
 *   cat /tmp/k7mqttd.stat 当前连接、订阅、retain 条数
 *
 * 只实现家居用得到的部分：
 *
 *   CONNECT / CONNACK   可选用户名密码；同 client id 再连就踢掉旧的
 *   SUBSCRIBE           + 和 # 通配；订阅时把匹配的 retain 消息补发过去
 *   UNSUBSCRIBE
 *   PUBLISH             转发给所有匹配的订阅者；retain 的存下来（空载荷
 *                       表示删除）。QoS 1 回 PUBACK、QoS 2 回 PUBREC /
 *                       PUBCOMP，但一律按 QoS 0 转发 —— 家居状态是"最新
 *                       值覆盖旧值"，丢一条下一条就补上，不值得为它做
 *                       重传队列和会话保存
 *   PINGREQ / PINGRESP
 *   遗嘱                心跳超时、连接异常断开、被同 id 顶掉时发出；
 *                       收到 DISCONNECT 正常退出时不发
 *
 * ★ 一个线程、一次 poll
 *
 *   连接上限 16 个，每个连接收发各有一块固定缓冲（静态分配，不吃堆）。
 *   发送一律 MSG_DONTWAIT：写不进 socket 的部分留在发送缓冲里，等 POLLOUT
 *   再写；发送缓冲满了说明对端不收，直接断开它，不让一个卡住的设备拖住
 *   整个 broker。
 *
 * ★ 所有等待都有上界
 *
 *   poll 每轮最多 1 秒；连上来 10 秒内不发 CONNECT 就断；心跳超过
 *   keepalive 的 1.5 倍没有任何报文就断（并发遗嘱）。单个报文超过
 *   收缓冲（2KB）就断 —— 家居消息都是几十到几百字节。
 ****************************************************************************/

#include <nuttx/config.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#ifdef CONFIG_LVX_DEMO_CONTEST2026_423_MQTTD_PORT
#  define MQTTD_PORT CONFIG_LVX_DEMO_CONTEST2026_423_MQTTD_PORT
#else
#  define MQTTD_PORT 1883
#endif

#define MAX_CLIENTS     16
#define MAX_SUBS        16      /* 每个连接 */
#define MAX_RETAIN      64
#define MAX_USERS       4
#define RX_CAP          2048    /* 单个报文上限 */
#define TX_CAP          8192
#define ID_MAX          64
#define TOPIC_MAX       128
#define PAYLOAD_MAX     512     /* retain / 遗嘱能存的载荷上限 */
#define CRED_MAX        64
#define CONNECT_MS      10000
#ifndef CONF_PATH                       /* 主机上联调时可在命令行改 */
#  define CONF_PATH     "/data/k7mqttd.conf"
#endif
#ifndef STAT_PATH
#  define STAT_PATH     "/tmp/k7mqttd.stat"
#endif

/* 报文类型（固定头高 4 位） */

#define T_CONNECT       1
#define T_CONNACK       2
#define T_PUBLISH       3
#define T_PUBACK        4
#define T_PUBREC        5
#define T_PUBREL        6
#define T_PUBCOMP       7
#define T_SUBSCRIBE     8
#define T_SUBACK        9
#define T_UNSUBSCRIBE   10
#define T_UNSUBACK      11
#define T_PINGREQ       12
#define T_PINGRESP      13
#define T_DISCONNECT    14

/* CONNACK 返回码 */

#define RC_OK           0
#define RC_PROTOCOL     1
#define RC_BAD_ID       2
#define RC_BAD_AUTH     4
#define RC_NOT_AUTH     5

struct client_s
{
  int      fd;
  bool     connected;           /* CONNECT 已接受 */
  bool     kill;                /* 本轮处理完后断开 */
  bool     kill_will;           /*   断开时发不发遗嘱 */
  bool     local;               /* 127.0.0.1，免认证 */
  struct in_addr addr;
  uint64_t since_ms;            /* accept 的时刻 */
  uint64_t last_rx_ms;
  uint16_t keepalive;
  char     id[ID_MAX];

  bool     will;
  bool     will_retain;
  char     will_topic[TOPIC_MAX];
  uint8_t  will_msg[PAYLOAD_MAX];
  size_t   will_len;

  int      nsubs;
  char     subs[MAX_SUBS][TOPIC_MAX];

  size_t   rxlen;
  size_t   txlen;
  uint8_t  rx[RX_CAP];
  uint8_t  tx[TX_CAP];
};

struct retain_s
{
  bool     used;
  char     topic[TOPIC_MAX];
  size_t   len;
  uint8_t  payload[PAYLOAD_MAX];
};

struct user_s
{
  char name[CRED_MAX];
  char pass[CRED_MAX];
};

static struct client_s g_cli[MAX_CLIENTS];
static struct retain_s g_ret[MAX_RETAIN];
static struct user_s g_users[MAX_USERS];
static int g_nusers;
static int g_auto_id;
static bool g_stat_dirty = true;

static uint64_t now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- 发送 ---- */

static void flush_tx(struct client_s *c)
{
  ssize_t n;

  while (c->txlen > 0)
    {
      n = send(c->fd, c->tx, c->txlen, MSG_DONTWAIT);
      if (n <= 0)
        {
          if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                        errno == EINTR))
            {
              return;
            }

          c->kill = true;
          c->kill_will = true;
          return;
        }

      c->txlen -= n;
      memmove(c->tx, c->tx + n, c->txlen);
    }
}

/* 按"固定头 + 两段数据"排进发送缓冲。放不下说明对端长时间不收，断开。 */

static void queue(struct client_s *c, uint8_t hdr,
                  const void *a, size_t alen,
                  const void *b, size_t blen)
{
  size_t len = alen + blen;
  uint8_t fh[5];
  size_t h = 1;
  size_t v = len;

  if (c->kill)
    {
      return;
    }

  fh[0] = hdr;
  do
    {
      fh[h] = v & 0x7f;
      v >>= 7;
      if (v)
        {
          fh[h] |= 0x80;
        }

      h++;
    }
  while (v);

  if (c->txlen + h + len > TX_CAP)
    {
      syslog(LOG_WARNING, "k7mqttd: %s 发送缓冲满，断开\n", c->id);
      c->kill = true;
      c->kill_will = true;
      return;
    }

  memcpy(c->tx + c->txlen, fh, h);
  if (alen)
    {
      memcpy(c->tx + c->txlen + h, a, alen);
    }

  if (blen)
    {
      memcpy(c->tx + c->txlen + h + alen, b, blen);
    }

  c->txlen += h + len;
}

static void send_ack(struct client_s *c, uint8_t type, uint16_t pid)
{
  uint8_t v[2];

  v[0] = pid >> 8;
  v[1] = pid & 0xff;
  queue(c, (type << 4) | (type == T_PUBREL ? 2 : 0), v, 2, NULL, 0);
}

static void send_publish(struct client_s *c, const char *topic,
                         const uint8_t *payload, size_t len, bool retain)
{
  uint8_t th[2 + TOPIC_MAX];
  size_t tl = strlen(topic);

  th[0] = tl >> 8;
  th[1] = tl & 0xff;
  memcpy(th + 2, topic, tl);
  queue(c, (T_PUBLISH << 4) | (retain ? 1 : 0), th, tl + 2, payload, len);
}

/* ---- 主题 ---- */

/* 过滤器 f 是否匹配主题 t。'$' 开头的主题不被首层通配匹配。 */

static bool topic_match(const char *f, const char *t)
{
  if (t[0] == '$' && (f[0] == '+' || f[0] == '#'))
    {
      return false;
    }

  for (; ; )
    {
      if (f[0] == '#')
        {
          return true;
        }

      if (f[0] == '+')
        {
          f++;
          while (*t != '\0' && *t != '/')
            {
              t++;
            }
        }
      else
        {
          while (*f != '\0' && *f != '/')
            {
              if (*f++ != *t++)
                {
                  return false;
                }
            }

          if (*t != '\0' && *t != '/')
            {
              return false;
            }
        }

      /* 这里两边都停在 '/' 或结尾 */

      if (*f == '\0')
        {
          return *t == '\0';
        }

      if (*t == '\0')
        {
          return strcmp(f, "/#") == 0;   /* "a/#" 也匹配 "a" */
        }

      f++;
      t++;
    }
}

/* 过滤器：'+' 和 '#' 必须独占一层，'#' 只能在最后 */

static bool filter_valid(const char *f)
{
  const char *p;

  if (*f == '\0')
    {
      return false;
    }

  for (p = f; *p != '\0'; p++)
    {
      if (*p == '+' || *p == '#')
        {
          if ((p != f && p[-1] != '/') ||
              (p[1] != '\0' && p[1] != '/'))
            {
              return false;
            }

          if (*p == '#' && p[1] != '\0')
            {
              return false;
            }
        }
    }

  return true;
}

static bool topic_valid(const char *t)
{
  return *t != '\0' && strpbrk(t, "+#") == NULL;
}

/* ---- retain ---- */

static void retain_store(const char *topic, const uint8_t *payload,
                         size_t len)
{
  struct retain_s *slot = NULL;
  int i;

  for (i = 0; i < MAX_RETAIN; i++)
    {
      if (g_ret[i].used && strcmp(g_ret[i].topic, topic) == 0)
        {
          slot = &g_ret[i];
          break;
        }
    }

  if (len == 0)
    {
      if (slot != NULL)
        {
          slot->used = false;            /* 空载荷 = 删除 */
          g_stat_dirty = true;
        }

      return;
    }

  if (len > PAYLOAD_MAX)
    {
      syslog(LOG_WARNING, "k7mqttd: %s 的 retain 载荷 %zu 字节超上限，"
             "不保存\n", topic, len);
      return;
    }

  for (i = 0; slot == NULL && i < MAX_RETAIN; i++)
    {
      if (!g_ret[i].used)
        {
          slot = &g_ret[i];
          strlcpy(slot->topic, topic, sizeof(slot->topic));
          slot->used = true;
          g_stat_dirty = true;
        }
    }

  if (slot == NULL)
    {
      syslog(LOG_WARNING, "k7mqttd: retain 已满 %d 条，%s 不保存\n",
             MAX_RETAIN, topic);
      return;
    }

  memcpy(slot->payload, payload, len);
  slot->len = len;
}

/* ---- 转发 ---- */

static void route(const char *topic, const uint8_t *payload, size_t len,
                  bool retain)
{
  int i;
  int j;

  if (retain)
    {
      retain_store(topic, payload, len);
    }

  /* 给已在线的订阅者转发时 retain 位清零（3.1.1 §3.3.1.3）；
   * 一个连接有多个过滤器都匹配时只发一次。
   */

  for (i = 0; i < MAX_CLIENTS; i++)
    {
      struct client_s *c = &g_cli[i];

      if (c->fd < 0 || !c->connected || c->kill)
        {
          continue;
        }

      for (j = 0; j < c->nsubs; j++)
        {
          if (topic_match(c->subs[j], topic))
            {
              send_publish(c, topic, payload, len, false);
              break;
            }
        }
    }
}

/* ---- 连接管理 ---- */

static void stat_write(void)
{
  FILE *fp;
  int nret = 0;
  int i;
  int j;

  g_stat_dirty = false;
  fp = fopen(STAT_PATH, "w");
  if (fp == NULL)
    {
      return;
    }

  for (i = 0; i < MAX_RETAIN; i++)
    {
      nret += g_ret[i].used;
    }

  fprintf(fp, "k7mqttd 端口 %d  retain %d/%d  账号 %d（%s）\n",
          MQTTD_PORT, nret, MAX_RETAIN, g_nusers,
          g_nusers ? "外部连接需认证" : "允许匿名");
  for (i = 0; i < MAX_CLIENTS; i++)
    {
      struct client_s *c = &g_cli[i];

      if (c->fd < 0)
        {
          continue;
        }

      fprintf(fp, "  %-20s %-15s keepalive %us  订阅",
              c->connected ? c->id : "(未 CONNECT)", inet_ntoa(c->addr),
              c->keepalive);
      for (j = 0; j < c->nsubs; j++)
        {
          fprintf(fp, " %s", c->subs[j]);
        }

      fputc('\n', fp);
    }

  fclose(fp);
}

static void drop(struct client_s *c, bool will)
{
  if (c->fd < 0)
    {
      return;
    }

  /* 先关掉自己再发遗嘱，免得遗嘱转发回给自己 */

  close(c->fd);
  c->fd = -1;
  if (c->connected)
    {
      syslog(LOG_INFO, "k7mqttd: %s 断开%s\n", c->id,
             will && c->will ? "，发出遗嘱" : "");
      if (will && c->will)
        {
          route(c->will_topic, c->will_msg, c->will_len, c->will_retain);
        }
    }

  c->connected = false;
  g_stat_dirty = true;
}

static void kill_client(struct client_s *c, bool will, const char *why)
{
  /* 已经判了断开的不再改判：先收到 DISCONNECT（不发遗嘱）、紧接着
   * 又读到连接关闭时，不能把它改成异常断开。
   */

  if (c->kill)
    {
      return;
    }

  if (why != NULL)
    {
      syslog(LOG_WARNING, "k7mqttd: %s (%s) %s\n",
             c->connected ? c->id : "?", inet_ntoa(c->addr), why);
    }

  c->kill = true;
  c->kill_will = will;
}

/* ---- 报文解析 ---- */

/* 从 p[*off] 读一个长度前缀字符串，拷进 out（cap 含结尾 0）。
 * 越界或太长返回 -1。out 为 NULL 时只跳过。
 */

static int get_str(const uint8_t *p, size_t len, size_t *off,
                   char *out, size_t cap)
{
  size_t n;

  if (*off + 2 > len)
    {
      return -1;
    }

  n = (p[*off] << 8) | p[*off + 1];
  if (*off + 2 + n > len)
    {
      return -1;
    }

  if (out != NULL)
    {
      if (n >= cap || memchr(p + *off + 2, 0, n) != NULL)
        {
          return -1;
        }

      memcpy(out, p + *off + 2, n);
      out[n] = '\0';
    }

  *off += 2 + n;
  return n;
}

static bool auth_ok(const char *user, const char *pass)
{
  int i;

  for (i = 0; i < g_nusers; i++)
    {
      if (strcmp(g_users[i].name, user) == 0 &&
          strcmp(g_users[i].pass, pass) == 0)
        {
          return true;
        }
    }

  return false;
}

static void connack(struct client_s *c, uint8_t rc)
{
  uint8_t v[2] =
  {
    0, rc
  };

  queue(c, T_CONNACK << 4, v, 2, NULL, 0);
}

static void on_connect(struct client_s *c, const uint8_t *p, size_t len)
{
  char proto[8];
  char user[CRED_MAX] = "";
  char pass[CRED_MAX] = "";
  size_t off = 0;
  uint8_t level;
  uint8_t flags;
  int i;

  if (c->connected)
    {
      kill_client(c, true, "重复 CONNECT");
      return;
    }

  if (get_str(p, len, &off, proto, sizeof(proto)) < 0 || off + 4 > len)
    {
      kill_client(c, false, "CONNECT 格式错");
      return;
    }

  level = p[off];
  flags = p[off + 1];
  c->keepalive = (p[off + 2] << 8) | p[off + 3];
  off += 4;

  if (!((strcmp(proto, "MQTT") == 0 && level == 4) ||
        (strcmp(proto, "MQIsdp") == 0 && level == 3)))
    {
      connack(c, RC_PROTOCOL);
      kill_client(c, false, "协议版本不支持（只认 3.1 / 3.1.1）");
      return;
    }

  if ((flags & 0x01) || get_str(p, len, &off, c->id, sizeof(c->id)) < 0)
    {
      kill_client(c, false, "CONNECT 格式错");
      return;
    }

  if (c->id[0] == '\0')
    {
      if (!(flags & 0x02))
        {
          connack(c, RC_BAD_ID);
          kill_client(c, false, "空 client id 但没要 clean session");
          return;
        }

      snprintf(c->id, sizeof(c->id), "auto-%d", ++g_auto_id);
    }

  c->will = (flags & 0x04) != 0;
  if (c->will)
    {
      int n;

      c->will_retain = (flags & 0x20) != 0;
      if (get_str(p, len, &off, c->will_topic, sizeof(c->will_topic)) < 0 ||
          !topic_valid(c->will_topic) ||
          (n = get_str(p, len, &off, NULL, 0)) < 0 || n > PAYLOAD_MAX)
        {
          kill_client(c, false, "遗嘱格式错或太长");
          return;
        }

      memcpy(c->will_msg, p + off - n, n);
      c->will_len = n;
    }

  if ((flags & 0x80) &&
      get_str(p, len, &off, user, sizeof(user)) < 0)
    {
      kill_client(c, false, "用户名格式错");
      return;
    }

  if ((flags & 0x40) &&
      get_str(p, len, &off, pass, sizeof(pass)) < 0)
    {
      kill_client(c, false, "密码格式错");
      return;
    }

  if (g_nusers > 0 && !c->local)
    {
      if (!(flags & 0x80))
        {
          connack(c, RC_NOT_AUTH);
          kill_client(c, false, "没带用户名，拒绝");
          return;
        }

      if (!auth_ok(user, pass))
        {
          connack(c, RC_BAD_AUTH);
          kill_client(c, false, "用户名或密码不对，拒绝");
          return;
        }
    }

  /* 同 id 已在线：踢掉旧连接（3.1.1 §3.1.4），旧连接算异常断开 */

  for (i = 0; i < MAX_CLIENTS; i++)
    {
      struct client_s *o = &g_cli[i];

      if (o != c && o->fd >= 0 && o->connected &&
          strcmp(o->id, c->id) == 0)
        {
          syslog(LOG_INFO, "k7mqttd: %s 重新连上，踢掉旧连接\n", c->id);
          drop(o, true);
        }
    }

  c->connected = true;
  connack(c, RC_OK);
  g_stat_dirty = true;
  syslog(LOG_INFO, "k7mqttd: %s 已连接（%s，keepalive %us%s）\n",
         c->id, inet_ntoa(c->addr), c->keepalive,
         c->will ? "，带遗嘱" : "");
}

static void on_publish(struct client_s *c, uint8_t flags,
                       const uint8_t *p, size_t len)
{
  char topic[TOPIC_MAX];
  uint8_t qos = (flags >> 1) & 3;
  uint16_t pid = 0;
  size_t off = 0;

  if (qos == 3 || get_str(p, len, &off, topic, sizeof(topic)) < 0 ||
      !topic_valid(topic))
    {
      kill_client(c, true, "PUBLISH 格式错");
      return;
    }

  if (qos > 0)
    {
      if (off + 2 > len)
        {
          kill_client(c, true, "PUBLISH 缺 packet id");
          return;
        }

      pid = (p[off] << 8) | p[off + 1];
      off += 2;
    }

  route(topic, p + off, len - off, flags & 1);

  if (qos == 1)
    {
      send_ack(c, T_PUBACK, pid);
    }
  else if (qos == 2)
    {
      send_ack(c, T_PUBREC, pid);
    }
}

static void on_subscribe(struct client_s *c, const uint8_t *p, size_t len,
                         bool sub)
{
  char filter[TOPIC_MAX];
  char added[MAX_SUBS][TOPIC_MAX];
  uint8_t codes[2 + MAX_SUBS];
  int nadded = 0;
  int ncodes = 0;
  size_t off = 2;
  int i;
  int j;

  if (len < 2)
    {
      kill_client(c, true, "SUBSCRIBE 格式错");
      return;
    }

  codes[0] = p[0];                       /* packet id 原样回 */
  codes[1] = p[1];

  while (off < len)
    {
      uint8_t code = 0;

      if (get_str(p, len, &off, filter, sizeof(filter)) < 0 ||
          (sub && off >= len))
        {
          kill_client(c, true, "订阅主题格式错或太长");
          return;
        }

      if (sub)
        {
          off++;                         /* 请求的 QoS：一律授予 0 */
        }

      for (j = 0; j < c->nsubs; j++)
        {
          if (strcmp(c->subs[j], filter) == 0)
            {
              break;
            }
        }

      if (!sub)
        {
          if (j < c->nsubs)
            {
              memmove(c->subs[j], c->subs[j + 1],
                      (c->nsubs - j - 1) * TOPIC_MAX);
              c->nsubs--;
            }

          continue;
        }

      if (!filter_valid(filter))
        {
          code = 0x80;
        }
      else if (j == c->nsubs)
        {
          if (c->nsubs < MAX_SUBS)
            {
              strlcpy(c->subs[c->nsubs++], filter, TOPIC_MAX);
            }
          else
            {
              code = 0x80;
            }
        }

      if (code == 0 && nadded < MAX_SUBS)
        {
          strlcpy(added[nadded++], filter, TOPIC_MAX);
        }

      if (ncodes < MAX_SUBS)
        {
          codes[2 + ncodes++] = code;
        }
    }

  g_stat_dirty = true;
  if (!sub)
    {
      queue(c, (T_UNSUBACK << 4), codes, 2, NULL, 0);
      return;
    }

  queue(c, (T_SUBACK << 4), codes, 2 + ncodes, NULL, 0);

  /* 补发 retain：面板一连上就靠这个拿到所有设备的当前状态 */

  for (i = 0; i < MAX_RETAIN; i++)
    {
      if (!g_ret[i].used)
        {
          continue;
        }

      for (j = 0; j < nadded; j++)
        {
          if (topic_match(added[j], g_ret[i].topic))
            {
              send_publish(c, g_ret[i].topic, g_ret[i].payload,
                           g_ret[i].len, true);
              break;
            }
        }
    }
}

static void handle(struct client_s *c, uint8_t hdr, const uint8_t *p,
                   size_t len)
{
  uint8_t type = hdr >> 4;

  if (!c->connected && type != T_CONNECT)
    {
      kill_client(c, false, "第一个报文不是 CONNECT");
      return;
    }

  switch (type)
    {
      case T_CONNECT:
        on_connect(c, p, len);
        break;

      case T_PUBLISH:
        on_publish(c, hdr & 0x0f, p, len);
        break;

      case T_PUBREL:
        if (len >= 2)
          {
            send_ack(c, T_PUBCOMP, (p[0] << 8) | p[1]);
          }
        break;

      case T_SUBSCRIBE:
      case T_UNSUBSCRIBE:
        if ((hdr & 0x0f) != 0x02)
          {
            kill_client(c, true, "(UN)SUBSCRIBE 固定头标志错");
            break;
          }

        on_subscribe(c, p, len, type == T_SUBSCRIBE);
        break;

      case T_PINGREQ:
        queue(c, T_PINGRESP << 4, NULL, 0, NULL, 0);
        break;

      case T_DISCONNECT:
        kill_client(c, false, NULL);     /* 正常退出，不发遗嘱 */
        break;

      case T_PUBACK:
      case T_PUBREC:
      case T_PUBCOMP:
        break;                           /* 只发 QoS 0，不会有；忽略 */

      default:
        kill_client(c, true, "不认识的报文类型");
        break;
    }
}

/* 从收缓冲里拆出完整报文逐个处理 */

static void parse(struct client_s *c)
{
  size_t off = 0;

  while (!c->kill && c->rxlen - off >= 2)
    {
      size_t rl = 0;
      size_t h = 1;
      int shift = 0;

      for (; ; )
        {
          if (off + h >= c->rxlen)
            {
              goto out;                  /* 长度字段还没收全 */
            }

          rl |= (size_t)(c->rx[off + h] & 0x7f) << shift;
          if (!(c->rx[off + h++] & 0x80))
            {
              break;
            }

          shift += 7;
          if (h > 4)
            {
              kill_client(c, true, "长度字段错");
              goto out;
            }
        }

      if (h + rl > RX_CAP)
        {
          kill_client(c, true, "报文超过 2KB");
          goto out;
        }

      if (off + h + rl > c->rxlen)
        {
          break;
        }

      handle(c, c->rx[off], c->rx + off + h, rl);
      off += h + rl;
    }

out:
  c->rxlen -= off;
  memmove(c->rx, c->rx + off, c->rxlen);
}

/* ---- 主循环 ---- */

static void load_users(void)
{
  FILE *fp = fopen(CONF_PATH, "r");
  char line[160];

  if (fp == NULL)
    {
      return;
    }

  while (g_nusers < MAX_USERS && fgets(line, sizeof(line), fp) != NULL)
    {
      struct user_s *u = &g_users[g_nusers];

      if (line[0] == '#' ||
          sscanf(line, "%63s %63s", u->name, u->pass) != 2)
        {
          continue;
        }

      g_nusers++;
    }

  fclose(fp);
}

static int listen_on(int port)
{
  struct sockaddr_in sa;
  int one = 1;
  int fd;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    {
      return -errno;
    }

  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  sa.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
      listen(fd, 8) < 0)
    {
      int err = -errno;

      close(fd);
      return err;
    }

  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  return fd;
}

static void accept_one(int lfd)
{
  struct sockaddr_in sa;
  socklen_t sl = sizeof(sa);
  struct client_s *c = NULL;
  int fd;
  int i;

  fd = accept(lfd, (struct sockaddr *)&sa, &sl);
  if (fd < 0)
    {
      return;
    }

  for (i = 0; i < MAX_CLIENTS; i++)
    {
      if (g_cli[i].fd < 0)
        {
          c = &g_cli[i];
          break;
        }
    }

  if (c == NULL)
    {
      syslog(LOG_WARNING, "k7mqttd: 已满 %d 个连接，拒绝 %s\n",
             MAX_CLIENTS, inet_ntoa(sa.sin_addr));
      close(fd);
      return;
    }

  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  memset(c, 0, offsetof(struct client_s, rx));
  c->fd = fd;
  c->addr = sa.sin_addr;
  c->local = sa.sin_addr.s_addr == htonl(INADDR_LOOPBACK);
  c->since_ms = c->last_rx_ms = now_ms();
  g_stat_dirty = true;
}

static void read_client(struct client_s *c)
{
  ssize_t n = recv(c->fd, c->rx + c->rxlen, RX_CAP - c->rxlen,
                   MSG_DONTWAIT);

  if (n == 0)
    {
      kill_client(c, true, NULL);        /* 对端关了连接，没发 DISCONNECT */
      return;
    }

  if (n < 0)
    {
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        {
          kill_client(c, true, NULL);
        }

      return;
    }

  c->rxlen += n;
  c->last_rx_ms = now_ms();
  parse(c);
}

static void check_timeouts(void)
{
  uint64_t now = now_ms();
  int i;

  for (i = 0; i < MAX_CLIENTS; i++)
    {
      struct client_s *c = &g_cli[i];

      if (c->fd < 0 || c->kill)
        {
          continue;
        }

      if (!c->connected && now - c->since_ms > CONNECT_MS)
        {
          kill_client(c, false, "10 秒内没发 CONNECT");
        }
      else if (c->connected && c->keepalive > 0 &&
               now - c->last_rx_ms > (uint64_t)c->keepalive * 1500)
        {
          kill_client(c, true, "心跳超时");
        }
    }
}

int main(int argc, FAR char *argv[])
{
  struct pollfd pfd[1 + MAX_CLIENTS];
  int map[1 + MAX_CLIENTS];
  int port = MQTTD_PORT;
  int lfd;
  int opt;
  int i;

  while ((opt = getopt(argc, argv, "p:h")) != -1)
    {
      switch (opt)
        {
          case 'p':
            port = atoi(optarg);
            break;

          default:
            printf("用法: k7mqttd [-p 端口]   （nsh 里 k7mqttd & 放后台）\n"
                   "  状态: cat " STAT_PATH "\n"
                   "  账号: " CONF_PATH "，每行 \"用户名 密码\"；"
                   "没有则允许匿名，本机连接免认证\n");
            return opt == 'h' ? 0 : 2;
        }
    }

  for (i = 0; i < MAX_CLIENTS; i++)
    {
      g_cli[i].fd = -1;
    }

  lfd = listen_on(port);
  if (lfd < 0)
    {
      fprintf(stderr, "k7mqttd: 监听 %d 失败 (%d)%s\n", port, lfd,
              lfd == -EADDRINUSE ? "，已经有一个在跑？" : "");
      return 1;
    }

  load_users();
  syslog(LOG_INFO, "k7mqttd: 监听 %d，%s\n", port,
         g_nusers ? "外部连接需用户名密码" : "允许匿名");

  for (; ; )
    {
      int n = 0;

      pfd[n].fd = lfd;
      pfd[n].events = POLLIN;
      map[n++] = -1;
      for (i = 0; i < MAX_CLIENTS; i++)
        {
          if (g_cli[i].fd >= 0)
            {
              pfd[n].fd = g_cli[i].fd;
              pfd[n].events = POLLIN | (g_cli[i].txlen ? POLLOUT : 0);
              pfd[n].revents = 0;
              map[n++] = i;
            }
        }

      if (poll(pfd, n, 1000) > 0)
        {
          for (i = 1; i < n; i++)
            {
              struct client_s *c = &g_cli[map[i]];

              if (c->fd < 0 || c->kill)
                {
                  continue;              /* 本轮里已被顶掉 */
                }

              if (pfd[i].revents & (POLLERR | POLLHUP | POLLNVAL))
                {
                  /* 先把还在缓冲里的数据收完，再按断开处理 */

                  read_client(c);
                  kill_client(c, true, NULL);
                  continue;
                }

              if (pfd[i].revents & POLLIN)
                {
                  read_client(c);
                }
            }

          if (pfd[0].revents & POLLIN)
            {
              accept_one(lfd);
            }
        }

      check_timeouts();

      /* 统一在这里断开和发送：处理一个连接的报文时可能往别的连接排了
       * 数据，也可能把别的连接标成了要断开。断开时的遗嘱会再往别的连接
       * 排数据，所以先断开、后发送。
       */

      for (i = 0; i < MAX_CLIENTS; i++)
        {
          if (g_cli[i].fd >= 0 && g_cli[i].kill)
            {
              flush_tx(&g_cli[i]);       /* CONNACK 拒绝码尽量送到 */
              drop(&g_cli[i], g_cli[i].kill_will);
            }
        }

      for (i = 0; i < MAX_CLIENTS; i++)
        {
          if (g_cli[i].fd >= 0 && g_cli[i].txlen)
            {
              flush_tx(&g_cli[i]);
            }
        }

      if (g_stat_dirty)
        {
          stat_write();
        }
    }

  return 0;
}
