/****************************************************************************
 * app/kickpi_ui/k7_home.c
 * SPDX-License-Identifier: Apache-2.0
 *
 * 家居 MQTT 客户端。只做家居控制用得到的那一小块 MQTT 3.1.1：
 * CONNECT（带遗嘱）、SUBSCRIBE、QoS 0 的 PUBLISH 收发、PINGREQ。
 *
 * ★ 为什么不用 apps/netutils/mqttc
 *
 *   它在编译时现从 GitHub 拉源码包再打补丁，换一台机器、断网就编不过；
 *   而这里要的东西几百行就写完了，还能把每一处等待都控制在自己手里。
 *
 * ★ 所有等待都有上界
 *
 *   连接 3 秒、CONNACK 3 秒；之后的收包用 poll 1 秒一轮。45 秒没收到
 *   任何东西（PINGRESP 也算）就当断线，关掉重连。重连间隔 1 → 15 秒
 *   翻倍。broker 不在、网线没插、eth0 没起来，线程都只是安静地重试，
 *   界面从这里读到"离线"。
 *
 * ★ 线程
 *
 *   收包和重连在本线程；界面线程下发 set 时持锁直接写 socket
 *   （MSG_DONTWAIT，几十字节，写不进去就报错，不在界面线程里等）。
 ****************************************************************************/

#include <nuttx/config.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "k7_home.h"

#ifdef CONFIG_LVX_DEMO_CONTEST2026_423_KICKPI_UI_MQTT_HOST
#  define HOME_HOST CONFIG_LVX_DEMO_CONTEST2026_423_KICKPI_UI_MQTT_HOST
#else
#  define HOME_HOST ""
#endif

#ifdef CONFIG_LVX_DEMO_CONTEST2026_423_KICKPI_UI_MQTT_PORT
#  define HOME_PORT CONFIG_LVX_DEMO_CONTEST2026_423_KICKPI_UI_MQTT_PORT
#else
#  define HOME_PORT 1883
#endif

#define HOME_CLIENT_ID    "k7-panel"
#define HOME_PREFIX       "home/"
#define HOME_SELF_TOPIC   "home/k7panel/status"
#define HOME_BELL_TOPIC   "home/doorbell/event"
#define HOME_KEEPALIVE_S  30
#define HOME_PING_S       15             /* 这么久没发过东西就发 PINGREQ */
#define HOME_DEAD_S       45             /* 这么久没收到东西就当断线 */
#define HOME_CONNECT_MS   3000
#define HOME_BACKOFF_MAX  15
#define HOME_RX_CAP       1024           /* 单个报文上限，超了直接断开 */

/* MQTT 报文类型（固定头高 4 位） */

#define MQTT_CONNECT      0x10
#define MQTT_CONNACK      0x20
#define MQTT_PUBLISH      0x30
#define MQTT_SUBSCRIBE    0x82           /* 低 4 位规定是 0010 */
#define MQTT_SUBACK       0x90
#define MQTT_PINGREQ      0xc0
#define MQTT_PINGRESP     0xd0
#define MQTT_DISCONNECT   0xe0

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *const *g_ids;
static int g_n;
static int g_fd = -1;
static bool g_started;
static int8_t g_state[K7_HOME_MAX];
static struct k7_home_status_s g_st;
static time_t g_last_tx;

static time_t home_now(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec;
}

/* ---- 报文编码 ---- */

static size_t put_len(uint8_t *p, size_t len)
{
  size_t n = 0;

  do
    {
      uint8_t b = len & 0x7f;

      len >>= 7;
      p[n++] = len ? (b | 0x80) : b;
    }
  while (len);

  return n;
}

static size_t put_str(uint8_t *p, const char *s, size_t len)
{
  p[0] = len >> 8;
  p[1] = len & 0xff;
  memcpy(p + 2, s, len);
  return len + 2;
}

/* 固定头 + 已经排好的 body 拷进 out，返回总长；放不下返回 0 */

static size_t frame(uint8_t *out, size_t cap, uint8_t type,
                    const uint8_t *body, size_t len)
{
  uint8_t hdr[5];
  size_t h;

  hdr[0] = type;
  h = 1 + put_len(hdr + 1, len);
  if (h + len > cap)
    {
      return 0;
    }

  memcpy(out, hdr, h);
  memcpy(out + h, body, len);
  return h + len;
}

/* 持锁调用。整包要么全发出去，要么算失败（调用者断开重连）。 */

static int send_locked(const uint8_t *buf, size_t len)
{
  ssize_t ret;

  if (g_fd < 0)
    {
      return -ENOTCONN;
    }

  ret = send(g_fd, buf, len, MSG_DONTWAIT);
  if (ret < 0)
    {
      return -errno;
    }

  if ((size_t)ret != len)
    {
      return -EAGAIN;
    }

  g_last_tx = home_now();
  return 0;
}

static int publish_locked(const char *topic, const char *payload,
                          bool retain)
{
  uint8_t body[192];
  uint8_t pkt[200];
  size_t tl = strlen(topic);
  size_t pl = strlen(payload);
  size_t n;

  if (tl + pl + 2 > sizeof(body))
    {
      return -E2BIG;
    }

  n = put_str(body, topic, tl);
  memcpy(body + n, payload, pl);
  n = frame(pkt, sizeof(pkt), MQTT_PUBLISH | (retain ? 1 : 0), body,
            n + pl);
  return n ? send_locked(pkt, n) : -E2BIG;
}

/* ---- 连接 ---- */

static int tcp_connect(void)
{
  struct sockaddr_in sa;
  struct pollfd pfd;
  socklen_t sl = sizeof(int);
  int err = 0;
  int fd;

  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(HOME_PORT);
  if (inet_pton(AF_INET, HOME_HOST, &sa.sin_addr) != 1)
    {
      return -EINVAL;
    }

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    {
      return -errno;
    }

  /* 非阻塞 connect + poll：broker 不在时不会卡在内核的 SYN 重传里 */

  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    {
      if (errno != EINPROGRESS)
        {
          err = -errno;
          goto fail;
        }

      pfd.fd = fd;
      pfd.events = POLLOUT;
      if (poll(&pfd, 1, HOME_CONNECT_MS) <= 0)
        {
          err = -ETIMEDOUT;
          goto fail;
        }

      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &sl) < 0)
        {
          err = errno;
        }

      if (err)
        {
          err = -err;
          goto fail;
        }
    }

  return fd;

fail:
  close(fd);
  return err;
}

/* 收满 len 字节或超时 */

static int recv_exact(int fd, uint8_t *buf, size_t len, int timeout_ms)
{
  struct pollfd pfd;
  size_t got = 0;
  ssize_t ret;

  pfd.fd = fd;
  pfd.events = POLLIN;
  while (got < len)
    {
      if (poll(&pfd, 1, timeout_ms) <= 0)
        {
          return -ETIMEDOUT;
        }

      ret = recv(fd, buf + got, len - got, 0);
      if (ret <= 0)
        {
          return ret < 0 ? -errno : -ECONNRESET;
        }

      got += ret;
    }

  return 0;
}

static int mqtt_handshake(int fd)
{
  static const char will[] = "offline";
  uint8_t body[128];
  uint8_t pkt[136];
  uint8_t ack[4];
  size_t n = 0;
  size_t len;
  int ret;

  n += put_str(body + n, "MQTT", 4);
  body[n++] = 4;                         /* 3.1.1 */
  body[n++] = 0x02 | 0x04 | 0x20;        /* clean session, 遗嘱, 遗嘱 retain */
  body[n++] = HOME_KEEPALIVE_S >> 8;
  body[n++] = HOME_KEEPALIVE_S & 0xff;
  n += put_str(body + n, HOME_CLIENT_ID, strlen(HOME_CLIENT_ID));
  n += put_str(body + n, HOME_SELF_TOPIC, strlen(HOME_SELF_TOPIC));
  n += put_str(body + n, will, strlen(will));

  len = frame(pkt, sizeof(pkt), MQTT_CONNECT, body, n);
  if (send(fd, pkt, len, 0) != (ssize_t)len)
    {
      return -EIO;
    }

  ret = recv_exact(fd, ack, 4, HOME_CONNECT_MS);
  if (ret < 0)
    {
      return ret;
    }

  if (ack[0] != MQTT_CONNACK || ack[1] != 2)
    {
      return -EPROTO;
    }

  if (ack[3] != 0)
    {
      syslog(LOG_WARNING, "家居: broker 拒绝连接，返回码 %u\n", ack[3]);
      return -ECONNREFUSED;
    }

  /* 订阅 home/+/state 和门铃。SUBACK 交给收包循环，不在这里等。 */

  n = 0;
  body[n++] = 0;
  body[n++] = 1;                         /* packet id */
  n += put_str(body + n, HOME_PREFIX "+/state",
               strlen(HOME_PREFIX "+/state"));
  body[n++] = 0;                         /* QoS 0 */
  n += put_str(body + n, HOME_BELL_TOPIC, strlen(HOME_BELL_TOPIC));
  body[n++] = 0;
  len = frame(pkt, sizeof(pkt), MQTT_SUBSCRIBE, body, n);
  if (send(fd, pkt, len, 0) != (ssize_t)len)
    {
      return -EIO;
    }

  return 0;
}

/* ---- 收包 ---- */

static void on_publish(const uint8_t *p, size_t len, uint8_t flags)
{
  const char *topic;
  const char *payload;
  size_t tl;
  size_t pl;
  size_t pre = strlen(HOME_PREFIX);
  int v;
  int i;

  if (len < 2)
    {
      return;
    }

  tl = (p[0] << 8) | p[1];
  if (2 + tl > len)
    {
      return;
    }

  topic = (const char *)p + 2;
  payload = topic + tl;
  pl = len - 2 - tl;
  if ((flags >> 1) & 3)
    {
      /* QoS 1/2 带两字节 packet id。订阅时要的是 QoS 0，按理不会来 */

      if (pl < 2)
        {
          return;
        }

      payload += 2;
      pl -= 2;
    }

  /* 门铃。retain 的旧消息是连上时补发的，不算这次按了门铃 */

  if (tl == strlen(HOME_BELL_TOPIC) &&
      memcmp(topic, HOME_BELL_TOPIC, tl) == 0)
    {
      if (!(flags & 1))
        {
          pthread_mutex_lock(&g_lock);
          g_st.rings++;
          g_st.gen++;
          pthread_mutex_unlock(&g_lock);
        }

      return;
    }

  /* home/<id>/state */

  if (tl <= pre + 6 || memcmp(topic, HOME_PREFIX, pre) != 0 ||
      memcmp(topic + tl - 6, "/state", 6) != 0)
    {
      return;
    }

  if ((pl == 2 && strncasecmp(payload, "ON", 2) == 0) ||
      (pl == 1 && payload[0] == '1'))
    {
      v = 1;
    }
  else if ((pl == 3 && strncasecmp(payload, "OFF", 3) == 0) ||
           (pl == 1 && payload[0] == '0'))
    {
      v = 0;
    }
  else
    {
      return;
    }

  for (i = 0; i < g_n; i++)
    {
      size_t il = strlen(g_ids[i]);

      if (il == tl - pre - 6 && memcmp(topic + pre, g_ids[i], il) == 0)
        {
          pthread_mutex_lock(&g_lock);
          if (g_state[i] != v)
            {
              g_state[i] = v;
              g_st.gen++;
            }

          pthread_mutex_unlock(&g_lock);
          return;
        }
    }
}

/* 从 buf 里拆出完整报文逐个处理，返回消耗掉的字节数；格式错 / 太大返回 -1 */

static ssize_t parse(const uint8_t *buf, size_t len)
{
  size_t off = 0;

  while (len - off >= 2)
    {
      size_t rl = 0;
      size_t h = 1;
      int shift = 0;

      for (; ; )
        {
          if (off + h >= len)
            {
              return off;                /* 长度字段还没收全 */
            }

          rl |= (size_t)(buf[off + h] & 0x7f) << shift;
          if (!(buf[off + h++] & 0x80))
            {
              break;
            }

          shift += 7;
          if (h > 4)
            {
              return -1;
            }
        }

      if (h + rl > HOME_RX_CAP)
        {
          return -1;
        }

      if (off + h + rl > len)
        {
          return off;
        }

      if ((buf[off] & 0xf0) == MQTT_PUBLISH)
        {
          on_publish(buf + off + h, rl, buf[off] & 0x0f);
        }

      /* SUBACK / PINGRESP 只用来证明连接活着，不用解析 */

      off += h + rl;
    }

  return off;
}

static int session(int fd)
{
  static uint8_t rx[HOME_RX_CAP];
  static const uint8_t ping[2] =
  {
    MQTT_PINGREQ, 0
  };

  struct pollfd pfd;
  size_t len = 0;
  time_t last_rx = home_now();
  ssize_t ret;

  pfd.fd = fd;
  pfd.events = POLLIN;

  for (; ; )
    {
      time_t now = home_now();

      if (now - last_rx > HOME_DEAD_S)
        {
          return -ETIMEDOUT;
        }

      pthread_mutex_lock(&g_lock);
      ret = 0;
      if (now - g_last_tx >= HOME_PING_S)
        {
          ret = send_locked(ping, sizeof(ping));
        }

      pthread_mutex_unlock(&g_lock);
      if (ret < 0)
        {
          return ret;
        }

      if (poll(&pfd, 1, 1000) <= 0)
        {
          continue;
        }

      ret = recv(fd, rx + len, sizeof(rx) - len, MSG_DONTWAIT);
      if (ret == 0)
        {
          return -ECONNRESET;
        }

      if (ret < 0)
        {
          if (errno == EAGAIN || errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      last_rx = home_now();
      len += ret;
      ret = parse(rx, len);
      if (ret < 0)
        {
          return -EPROTO;
        }

      len -= ret;
      memmove(rx, rx + ret, len);
    }
}

static void set_connected(int fd, int err)
{
  int i;

  pthread_mutex_lock(&g_lock);
  g_fd = fd;
  g_st.connected = fd >= 0;
  if (err)
    {
      g_st.err = err;
    }

  /* 断开后状态就不可信了：设备可能在这期间被别处开关过 */

  if (fd < 0)
    {
      for (i = 0; i < g_n; i++)
        {
          g_state[i] = -1;
        }
    }
  else
    {
      g_last_tx = home_now();
      publish_locked(HOME_SELF_TOPIC, "online", true);
    }

  g_st.gen++;
  pthread_mutex_unlock(&g_lock);
}

static void *home_thread(void *arg)
{
  int backoff = 1;
  int logged = 0;

  (void)arg;

  for (; ; )
    {
      int fd = tcp_connect();
      int ret;

      if (fd >= 0)
        {
          ret = mqtt_handshake(fd);
          if (ret == 0)
            {
              syslog(LOG_INFO, "家居: 已连上 broker %s\n", g_st.broker);
              set_connected(fd, 0);
              backoff = 1;
              logged = 0;
              ret = session(fd);
              set_connected(-1, -ret);
              syslog(LOG_WARNING, "家居: 与 broker 断开 (%d)\n", ret);
            }

          close(fd);
        }
      else
        {
          ret = fd;
        }

      if (ret < 0)
        {
          pthread_mutex_lock(&g_lock);
          g_st.err = -ret;
          g_st.gen++;
          pthread_mutex_unlock(&g_lock);

          /* 连不上时只记前几次，免得 broker 不在时日志刷屏 */

          if (logged++ < 3)
            {
              syslog(LOG_INFO, "家居: 连不上 %s (%d)，%d 秒后重试\n",
                     g_st.broker, ret, backoff);
            }
        }

      sleep(backoff);
      backoff = backoff * 2 > HOME_BACKOFF_MAX ? HOME_BACKOFF_MAX
                                               : backoff * 2;
    }

  return NULL;
}

/* ---- 对界面的接口 ---- */

int k7_home_start(const char *const *ids, int n)
{
  pthread_attr_t attr;
  pthread_t tid;
  int ret;
  int i;

  if (g_started)
    {
      return 0;
    }

  g_ids = ids;
  g_n = n > K7_HOME_MAX ? K7_HOME_MAX : n;
  for (i = 0; i < K7_HOME_MAX; i++)
    {
      g_state[i] = -1;
    }

  snprintf(g_st.broker, sizeof(g_st.broker), "%s:%d", HOME_HOST,
           HOME_PORT);
  if (HOME_HOST[0] == '\0')
    {
      g_st.err = ENOSYS;                 /* 没配置 broker */
      return -ENOSYS;
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 8192);
  ret = pthread_create(&tid, &attr, home_thread, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      g_st.err = ret;
      return -ret;
    }

  pthread_setname_np(tid, "k7home");
  pthread_detach(tid);
  g_started = true;
  return 0;
}

void k7_home_status(struct k7_home_status_s *st)
{
  pthread_mutex_lock(&g_lock);
  *st = g_st;
  pthread_mutex_unlock(&g_lock);
}

int k7_home_find(const char *id)
{
  int i;

  for (i = 0; i < g_n; i++)
    {
      if (strcmp(g_ids[i], id) == 0)
        {
          return i;
        }
    }

  return -1;
}

int k7_home_state(int i)
{
  int v;

  if (i < 0 || i >= g_n)
    {
      return -1;
    }

  pthread_mutex_lock(&g_lock);
  v = g_state[i];
  pthread_mutex_unlock(&g_lock);
  return v;
}

int k7_home_set(int i, bool on)
{
  char topic[64];
  int ret;

  if (i < 0 || i >= g_n)
    {
      return -EINVAL;
    }

  snprintf(topic, sizeof(topic), HOME_PREFIX "%s/set", g_ids[i]);
  pthread_mutex_lock(&g_lock);
  ret = publish_locked(topic, on ? "ON" : "OFF", false);
  pthread_mutex_unlock(&g_lock);
  return ret;
}

int k7_home_scene(const char *name)
{
  int ret;

  pthread_mutex_lock(&g_lock);
  ret = publish_locked(HOME_PREFIX "scene/set", name, false);
  pthread_mutex_unlock(&g_lock);
  return ret;
}

int k7_home_publish(const char *topic, const char *payload, bool retain)
{
  int ret;

  pthread_mutex_lock(&g_lock);
  ret = publish_locked(topic, payload, retain);
  pthread_mutex_unlock(&g_lock);
  return ret;
}
