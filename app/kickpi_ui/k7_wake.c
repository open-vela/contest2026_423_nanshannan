/****************************************************************************
 * app/kickpi_ui/k7_wake.c
 * SPDX-License-Identifier: Apache-2.0
 *
 * 语音唤醒。数据流：
 *
 *   ES8388 48k 立体声 ─ 采集线程 ─ 16k 单声道 ─┬─ 环形缓冲（12 秒）
 *                                              └─ rpmsg 'P' 帧 → Linux k7d
 *   k7d（A72）：端点检测 → 'S' 开始说话 / 'V' 起点 终点 / 'L' 电平
 *   识别线程：按 [起点, 终点) 从环形缓冲取段 → WAV → mimo-v2.5-asr → 文字
 *            → 匹配"你好 openvela" → 事件交给界面
 *
 * ★ 为什么端点检测放在 Linux
 *
 *   这正是 AMP 里"计算域"的用法：实时、持续、与界面无关的信号处理放到
 *   另一簇核上，openvela 只负责外设和网络。语音本身不回传 —— rpmsg 上
 *   只有 PCM 往 Linux 一个方向，回来的只是几十字节的区间。
 *
 * ★ 样本序号怎么对齐
 *
 *   发 'A' 的同时把本地计数清零；之后每发出一个样本计数 +1，只有真的
 *   发出去的样本才进环形缓冲。k7d 从 'A' 之后第一个样本记 0，两边数的是
 *   同一串样本（rpmsg 可靠、有序）。
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/rpmsg/rpmsg.h>
#include <cJSON.h>

#include "k7_audio.h"
#include "k7_net.h"
#include "k7_wake.h"

#ifdef __has_include
#  if __has_include("k7_agent_key.h")
#    include "k7_agent_key.h"
#  endif
#endif

#define WAKE_RATE        16000
#define WAKE_FRAME       160              /* 10ms，一帧 rpmsg */
#define WAKE_RING        (WAKE_RATE * 12) /* 12 秒 */
#define WAKE_MAXSEG      4
#define WAKE_EPT         "rpmsg-raw"
#define WAKE_ASR_MODEL   "mimo-v2.5-asr"
#define WAKE_ASR_PATH    "/v1/chat/completions"
#define WAKE_RESP_CAP    (16 * 1024)
#define WAKE_WOKEN_S     10               /* 只说唤醒词后等问题的时长 */
#define WAKE_KEEP        3                /* 保留最近几句送识别的音频 */

/* 48k → 16k 抽取前的低通：31 抽头 Hamming 窗 sinc，截止 7kHz，Q15。
 * 3.4kHz 内平坦，8kHz -14dB，9kHz -30dB，12kHz -60dB。原来的三点平均
 * 在 8kHz 附近只衰减几 dB，8~24kHz 的成分折叠进语音频段，识别变差。
 */

#define WAKE_TAPS        31

static const int16_t g_lpf[WAKE_TAPS] =
{
  51, 17, -58, -146, -134, 84, 426, 555, 114, -838, -1592, -1105, 1213,
  4834, 8186, 9551, 8186, 4834, 1213, -1105, -1592, -838, 114, 555, 426,
  84, -134, -146, -58, 17, 51
};

struct wake_seg_s
{
  uint32_t start;
  uint32_t end;
};

struct wake_s
{
  pthread_mutex_t lock;
  pthread_cond_t  cond;

  /* 开关与线程 */

  volatile bool   running;
  volatile bool   pause_req;
  volatile bool   capturing;
  bool            cap_started;
  bool            asr_started;

  /* rpmsg */

  struct rpmsg_endpoint ept;
  volatile bool   bound;
  volatile bool   hello;
  volatile bool   streaming;       /* 已发 'A' */
  bool            registered;

  /* 环形缓冲 */

  int16_t        *ring;
  volatile uint32_t wr;            /* 已发出的样本总数 */

  /* 降采样的半成品 */

  int16_t         out[WAKE_FRAME];
  int             out_n;
  int16_t         hist[WAKE_TAPS];   /* 最近的 48k 输入，环形 */
  int             hist_pos;
  int             phase;             /* 每 3 个输入出 1 个 */

  /* 待识别的段 */

  struct wake_seg_s seg[WAKE_MAXSEG];
  int             seg_n;

  /* 状态 */

  bool            speaking;
  bool            recognizing;
  int             last_peak;       /* 最近一段的峰值与增益（Q4），诊断用 */
  int             last_gain;
  int             keep_idx;
  int             level;
  int             peak;
  bool            offline;
  time_t          woken_until;
  char            cur_wav[32];     /* 正在处理的那句的录音 */

  /* 事件（一次只留一个，界面每 250ms 取） */

  int             ev;
  char            ev_text[512];
  char            ev_question[512];
  char            ev_wav[32];
  char            got_wav[32];     /* 界面最近取走的那个事件的 */
};

static struct wake_s g_wake =
{
  .lock = PTHREAD_MUTEX_INITIALIZER,
  .cond = PTHREAD_COND_INITIALIZER,
};

/****************************************************************************
 * rpmsg
 ****************************************************************************/

static int wake_ept_cb(struct rpmsg_endpoint *ept, void *data, size_t len,
                       uint32_t src, void *priv)
{
  struct wake_s *w = &g_wake;
  char buf[64];
  const char *p = data;
  unsigned long a;
  unsigned long b;

  (void)ept;
  (void)src;
  (void)priv;

  if (len < 1)
    {
      return 0;
    }

  len = len - 1 < sizeof(buf) - 1 ? len - 1 : sizeof(buf) - 1;
  memcpy(buf, p + 1, len);
  buf[len] = '\0';

  pthread_mutex_lock(&w->lock);
  switch (p[0])
    {
      case 'H':
        w->hello = true;
        break;

      case 'L':
        a = strtoul(buf, NULL, 10);
        w->peak = (int)(a > 32767 ? 32767 : a);
        w->level = (int)(a * 100 / 32768);
        break;

      case 'S':
        w->speaking = true;
        break;

      case 'V':
        w->speaking = false;
        if (sscanf(buf, "%lu %lu", &a, &b) == 2 && b > a &&
            w->seg_n < WAKE_MAXSEG)
          {
            w->seg[w->seg_n].start = (uint32_t)a;
            w->seg[w->seg_n].end   = (uint32_t)b;
            w->seg_n++;
            pthread_cond_broadcast(&w->cond);
          }
        break;

      default:
        break;
    }

  pthread_mutex_unlock(&w->lock);
  return 0;
}

static void wake_dev_created(struct rpmsg_device *rdev, void *priv)
{
  struct wake_s *w = priv;

  if (strcmp(rpmsg_get_cpuname(rdev), CONFIG_RK3576_RPTUN_CPUNAME) != 0 ||
      w->bound)
    {
      return;
    }

  if (rpmsg_create_ept(&w->ept, rdev, WAKE_EPT, RPMSG_ADDR_ANY,
                       RPMSG_ADDR_ANY, wake_ept_cb, NULL) == 0)
    {
      w->bound = true;
    }
}

static void wake_dev_destroy(struct rpmsg_device *rdev, void *priv)
{
  struct wake_s *w = priv;

  if (strcmp(rpmsg_get_cpuname(rdev), CONFIG_RK3576_RPTUN_CPUNAME) == 0 &&
      w->bound)
    {
      rpmsg_destroy_ept(&w->ept);
      w->bound = false;
      w->hello = false;
      w->streaming = false;
    }
}

/****************************************************************************
 * 采集：48k 立体声 → 16k 单声道 → 环形缓冲 + rpmsg
 ****************************************************************************/

static void wake_flush_frame(struct wake_s *w)
{
  char frame[1 + WAKE_FRAME * 2];
  uint32_t i;

  frame[0] = 'P';
  memcpy(frame + 1, w->out, WAKE_FRAME * 2);
  if (rpmsg_send(&w->ept, frame, sizeof(frame)) < 0)
    {
      return;              /* 没发出去的样本不进环，保持两边序号一致 */
    }

  for (i = 0; i < WAKE_FRAME; i++)
    {
      w->ring[(w->wr + i) % WAKE_RING] = w->out[i];
    }

  w->wr += WAKE_FRAME;
}

static bool wake_capture_cb(const int16_t *st, size_t frames, void *priv)
{
  struct wake_s *w = &g_wake;
  size_t i;

  (void)priv;

  if (!w->running || w->pause_req)
    {
      return false;
    }

  if (!w->streaming)
    {
      /* Linux 还没接上：采集照跑（省得反复开关设备），数据丢掉 */

      return true;
    }

  /* 48k → 16k：低通 FIR 后每 3 个取 1 个。左右声道一样，取左 */

  for (i = 0; i < frames; i++)
    {
      w->hist[w->hist_pos] = st[2 * i];
      w->hist_pos = (w->hist_pos + 1) % WAKE_TAPS;

      if (++w->phase == 3)
        {
          int32_t y = 0;
          int k;

          w->phase = 0;
          for (k = 0; k < WAKE_TAPS; k++)
            {
              y += (int32_t)g_lpf[k] *
                   w->hist[(w->hist_pos + k) % WAKE_TAPS];
            }

          y >>= 15;
          w->out[w->out_n++] = (int16_t)(y > 32767 ? 32767 :
                                         (y < -32768 ? -32768 : y));
          if (w->out_n == WAKE_FRAME)
            {
              wake_flush_frame(w);
              w->out_n = 0;
            }
        }
    }

  return true;
}

static void *wake_capture_thread(void *arg)
{
  struct wake_s *w = &g_wake;
  int waited = 0;

  (void)arg;

  while (w->running)
    {
      /* 等 Linux 握手：k7d 50ms 轮询一次 /dev/rpmsgN，一般 1 秒内到 */

      if (w->bound && w->hello && !w->streaming)
        {
          char a = 'A';

          w->wr = 0;
          w->out_n = 0;
          w->phase = 0;
          w->hist_pos = 0;
          memset(w->hist, 0, sizeof(w->hist));
          if (rpmsg_send(&w->ept, &a, 1) >= 0)
            {
              w->streaming = true;
              syslog(LOG_INFO, "唤醒: Linux 已接上，开始送音频\n");
            }
        }

      if (w->pause_req || !w->streaming)
        {
          w->capturing = false;
          usleep(50 * 1000);
          if (!w->streaming && (waited += 50) == 5000)
            {
              syslog(LOG_WARNING, "唤醒: 5 秒没等到 Linux 的 k7d\n");
            }

          continue;
        }

      w->capturing = true;
      if (k7a_capture(wake_capture_cb, NULL, 3600 * 1000) < 0)
        {
          usleep(500 * 1000);        /* 设备被占着，稍后再试 */
        }

      w->capturing = false;
    }

  return NULL;
}

/****************************************************************************
 * 识别
 ****************************************************************************/

static const char g_b64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64_encode(const uint8_t *in, size_t len, const char *prefix)
{
  size_t plen = strlen(prefix);
  char *out = malloc(plen + (len + 2) / 3 * 4 + 1);
  char *o;
  size_t i;

  if (out == NULL)
    {
      return NULL;
    }

  memcpy(out, prefix, plen);
  o = out + plen;
  for (i = 0; i + 2 < len; i += 3)
    {
      uint32_t v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];

      *o++ = g_b64[(v >> 18) & 63];
      *o++ = g_b64[(v >> 12) & 63];
      *o++ = g_b64[(v >> 6) & 63];
      *o++ = g_b64[v & 63];
    }

  if (i < len)
    {
      uint32_t v = in[i] << 16;

      if (i + 1 < len)
        {
          v |= in[i + 1] << 8;
        }

      *o++ = g_b64[(v >> 18) & 63];
      *o++ = g_b64[(v >> 12) & 63];
      *o++ = i + 1 < len ? g_b64[(v >> 6) & 63] : '=';
      *o++ = '=';
    }

  *o = '\0';
  return out;
}

static void put_le(uint8_t *p, uint32_t v, int n)
{
  int i;

  for (i = 0; i < n; i++)
    {
      p[i] = (uint8_t)(v >> (8 * i));
    }
}

/* 取段并编成 16k 单声道 WAV。段已被环覆盖就返回 NULL。 */

static uint8_t *wake_make_wav(struct wake_s *w, uint32_t start,
                              uint32_t end, size_t *len)
{
  uint32_t n = end - start;
  uint32_t data = n * 2;
  uint8_t *wav;
  uint32_t i;

  if (w->wr - start > WAKE_RING || end > w->wr || n == 0)
    {
      return NULL;
    }

  wav = malloc(44 + data);
  if (wav == NULL)
    {
      return NULL;
    }

  memcpy(wav, "RIFF", 4);
  put_le(wav + 4, 36 + data, 4);
  memcpy(wav + 8, "WAVEfmt ", 8);
  put_le(wav + 16, 16, 4);
  put_le(wav + 20, 1, 2);                  /* PCM */
  put_le(wav + 22, 1, 2);                  /* 单声道 */
  put_le(wav + 24, WAKE_RATE, 4);
  put_le(wav + 28, WAKE_RATE * 2, 4);
  put_le(wav + 32, 2, 2);
  put_le(wav + 34, 16, 2);
  memcpy(wav + 36, "data", 4);
  put_le(wav + 40, data, 4);

  /* ★ 送识别前按峰值归一化到约 -3dB（最多放大 8 倍）：ALC 下一句话的
   * 电平仍会随距离、音量差好几倍，太小的段识别明显变差。
   */

  {
    int peak = 1;
    int gain;

    for (i = 0; i < n; i++)
      {
        int v = w->ring[(start + i) % WAKE_RING];

        v = v < 0 ? -v : v;
        if (v > peak)
          {
            peak = v;
          }
      }

    gain = 23000 * 16 / peak;             /* Q4 */
    if (gain > 8 * 16)
      {
        gain = 8 * 16;
      }
    else if (gain < 16)
      {
        gain = 16;
      }

    for (i = 0; i < n; i++)
      {
        int32_t v = (int32_t)w->ring[(start + i) % WAKE_RING] * gain / 16;

        v = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
        put_le(wav + 44 + 2 * i, (uint16_t)(int16_t)v, 2);
      }

    w->last_peak = peak;
    w->last_gain = gain;
  }

  *len = 44 + data;
  return wav;
}

static int wake_asr(const uint8_t *wav, size_t len, char *text, size_t cap)
{
#ifndef K7_AGENT_LLM_KEY
  (void)wav;
  (void)len;
  (void)text;
  (void)cap;
  return -ENOTSUP;
#else
  static const char auth[] = "Bearer " K7_AGENT_LLM_KEY;
  const vela_header_t hdrs[] =
  {
    { "Authorization", auth },
    { NULL, NULL }
  };

  cJSON *root;
  cJSON *m;
  cJSON *content;
  cJSON *part;
  char *uri;
  char *body;
  char *resp;
  int status;
  int ret = -EPROTO;

  uri = b64_encode(wav, len, "data:audio/wav;base64,");
  if (uri == NULL)
    {
      return -ENOMEM;
    }

  root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "model", WAKE_ASR_MODEL);
  m = cJSON_CreateObject();
  cJSON_AddStringToObject(m, "role", "user");
  content = cJSON_AddArrayToObject(m, "content");
  part = cJSON_CreateObject();
  cJSON_AddStringToObject(part, "type", "input_audio");
  cJSON_AddStringToObject(cJSON_AddObjectToObject(part, "input_audio"),
                          "data", uri);
  cJSON_AddItemToArray(content, part);
  cJSON_AddItemToArray(cJSON_AddArrayToObject(root, "messages"), m);
  cJSON_AddStringToObject(cJSON_AddObjectToObject(root, "asr_options"),
                          "language", "zh");
  cJSON_AddBoolToObject(root, "stream", false);
  free(uri);

  body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  resp = malloc(WAKE_RESP_CAP);
  if (body == NULL || resp == NULL)
    {
      cJSON_free(body);
      free(resp);
      return -ENOMEM;
    }

  status = vela_https_post_json(K7_AGENT_LLM_HOST, "443", WAKE_ASR_PATH,
                                hdrs, body, resp, WAKE_RESP_CAP);
  cJSON_free(body);
  if (status != 200)
    {
      resp[status > 0 ? 160 : 0] = '\0';
      syslog(LOG_ERR, "唤醒: 识别 HTTP %d %s\n", status, resp);
      free(resp);
      return status < 0 ? status : -EIO;
    }

  root = cJSON_Parse(resp);
  free(resp);
  m = cJSON_GetObjectItem(cJSON_GetObjectItem(
        cJSON_GetArrayItem(cJSON_GetObjectItem(root, "choices"), 0),
        "message"), "content");
  if (cJSON_IsString(m))
    {
      snprintf(text, cap, "%s", m->valuestring);
      ret = 0;
    }

  cJSON_Delete(root);
  return ret;
#endif
}

/****************************************************************************
 * 唤醒词匹配
 ****************************************************************************/

/* 去掉空白和标点、ASCII 转小写。ASR 会写成"你好，OpenVela，"或
 * "你好 Open Vela"，比较之前先归一。
 */

static void wake_normalize(const char *in, char *out, size_t cap)
{
  static const char *const punct[] =
  {
    "，", "。", "！", "？", "、", "；", "：", "“", "”", "‘", "’",
    "（", "）", "…", "—", "·", "　", "《", "》", NULL
  };

  size_t o = 0;

  while (*in != '\0' && o + 4 < cap)
    {
      unsigned char c = (unsigned char)*in;
      int k;
      bool skip = false;

      if (c < 0x80)
        {
          if (c > ' ' && !(c >= '!' && c <= '/') &&
              !(c >= ':' && c <= '@') && !(c >= '[' && c <= '`') &&
              !(c >= '{' && c <= '~'))
            {
              out[o++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
            }

          in++;
          continue;
        }

      for (k = 0; punct[k] != NULL; k++)
        {
          size_t n = strlen(punct[k]);

          if (strncmp(in, punct[k], n) == 0)
            {
              in += n;
              skip = true;
              break;
            }
        }

      if (skip)
        {
          continue;
        }

      /* 其余多字节字符整字拷贝 */

      do
        {
          out[o++] = *in++;
        }
      while (((unsigned char)*in & 0xc0) == 0x80 && o + 1 < cap);
    }

  out[o] = '\0';
}

/* 在归一化后的文字里找唤醒词，返回唤醒词之后的位置；没有返回 NULL。 */

static const char *wake_match(const char *s)
{
  static const char *const full[] =
  {
    "openvela", "openvella", "openvila", "openvera", "欧喷维拉", "欧朋维拉",
    NULL
  };

  static const char *const tail[] =
  {
    "open", "vela", "维拉", "薇拉", "威拉", "维啦", NULL
  };

  const char *p;
  int k;

  for (k = 0; full[k] != NULL; k++)
    {
      p = strstr(s, full[k]);
      if (p != NULL)
        {
          p += strlen(full[k]);

          /* "open vela" 可能被识别成 "openvela" 后面又跟一个 "la" 之类，
           * 这里不再细抠。
           */

          return p;
        }
    }

  /* "你好" 后面紧跟（12 字节内）一个近似的尾巴 */

  p = strstr(s, "你好");
  if (p != NULL)
    {
      const char *after = p + strlen("你好");

      for (k = 0; tail[k] != NULL; k++)
        {
          const char *t = strstr(after, tail[k]);

          if (t != NULL && t - after <= 12)
            {
              t += strlen(tail[k]);

              /* 把紧跟的 vela / 拉 之类一并吃掉 */

              if (strncmp(t, "vela", 4) == 0)
                {
                  t += 4;
                }

              return t;
            }
        }
    }

  return NULL;
}

static void wake_publish(struct wake_s *w, int ev, const char *text,
                         const char *question)
{
  pthread_mutex_lock(&w->lock);
  w->ev = ev;
  snprintf(w->ev_text, sizeof(w->ev_text), "%s", text);
  snprintf(w->ev_question, sizeof(w->ev_question), "%s",
           question != NULL ? question : "");
  snprintf(w->ev_wav, sizeof(w->ev_wav), "%s", w->cur_wav);
  pthread_mutex_unlock(&w->lock);
}

static void wake_handle_text(struct wake_s *w, const char *text)
{
  char norm[512];
  const char *rest;
  time_t now = time(NULL);

  wake_normalize(text, norm, sizeof(norm));
  syslog(LOG_INFO, "唤醒: 识别「%s」\n", text);

  if (norm[0] == '\0')
    {
      return;
    }

  rest = wake_match(norm);
  if (rest != NULL)
    {
      /* 唤醒词后面至少两个汉字（6 字节）才当成问题 */

      if (strlen(rest) >= 6)
        {
          w->woken_until = 0;
          wake_publish(w, K7_WAKE_ASK, text, rest);
        }
      else
        {
          w->woken_until = now + WAKE_WOKEN_S;
          wake_publish(w, K7_WAKE_ONLY, text, NULL);
        }

      return;
    }

  if (w->woken_until != 0 && now <= w->woken_until)
    {
      w->woken_until = 0;
      wake_publish(w, K7_WAKE_ASK, text, text);
      return;
    }

  wake_publish(w, K7_WAKE_HEARD, text, NULL);
}

static void *wake_asr_thread(void *arg)
{
  struct wake_s *w = &g_wake;
  char text[512];

  (void)arg;

  while (w->running)
    {
      struct wake_seg_s seg;
      uint8_t *wav;
      size_t len;
      struct timespec ts;

      pthread_mutex_lock(&w->lock);
      while (w->running && w->seg_n == 0)
        {
          clock_gettime(CLOCK_REALTIME, &ts);
          ts.tv_sec += 1;
          pthread_cond_timedwait(&w->cond, &w->lock, &ts);
        }

      if (!w->running)
        {
          pthread_mutex_unlock(&w->lock);
          break;
        }

      seg = w->seg[0];
      memmove(&w->seg[0], &w->seg[1], (w->seg_n - 1) * sizeof(seg));
      w->seg_n--;

      /* 录制中：这句话不出板子 */

      if (w->offline)
        {
          pthread_mutex_unlock(&w->lock);
          continue;
        }

      w->recognizing = true;
      pthread_mutex_unlock(&w->lock);

      wav = wake_make_wav(w, seg.start, seg.end, &len);
      if (wav == NULL)
        {
          syslog(LOG_WARNING, "唤醒: 段 %" PRIu32 "~%" PRIu32
                 " 已不在缓冲里（写到 %" PRIu32 "）\n",
                 seg.start, seg.end, w->wr);
        }
      else
        {
          char path[32];
          FILE *fp;
          int ret;

          /* 留下最近几句的原始音频，识别效果差时能拿出来听、做对比 */

          snprintf(path, sizeof(path), "/tmp/k7-wake-%d.wav", w->keep_idx);
          w->keep_idx = (w->keep_idx + 1) % WAKE_KEEP;
          fp = fopen(path, "wb");
          if (fp != NULL)
            {
              fwrite(wav, 1, len, fp);
              fclose(fp);
            }

          text[0] = '\0';
          ret = wake_asr(wav, len, text, sizeof(text));
          syslog(LOG_INFO, "唤醒: 段 %" PRIu32 " ms 峰值 %d 增益 %d.%d "
                 "ret=%d 已存 %s\n", (seg.end - seg.start) / 16,
                 w->last_peak, w->last_gain / 16, w->last_gain % 16 * 10 / 16,
                 ret, path);
          /* 同样一行追加进文件：测试时不一定有人盯着串口 */

          fp = fopen("/tmp/k7-wake.log", "a");
          if (fp != NULL)
            {
              fprintf(fp, "%ld %s %" PRIu32 "ms peak=%d gain=%d.%d ret=%d 「%s」\n",
                      (long)time(NULL), path, (seg.end - seg.start) / 16,
                      w->last_peak, w->last_gain / 16,
                      w->last_gain % 16 * 10 / 16, ret, text);
              fclose(fp);
            }

          if (ret == 0)
            {
              snprintf(w->cur_wav, sizeof(w->cur_wav), "%s", path);
              wake_handle_text(w, text);
            }

          free(wav);
        }

      w->recognizing = false;
    }

  return NULL;
}

/****************************************************************************
 * 对外接口
 ****************************************************************************/

static int wake_spawn(void *(*fn)(void *))
{
  pthread_attr_t attr;
  pthread_t tid;
  int ret;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 32768);   /* TLS + cJSON */
  ret = pthread_create(&tid, &attr, fn, NULL);
  pthread_attr_destroy(&attr);
  if (ret == 0)
    {
      pthread_detach(tid);
    }

  return -ret;
}

int k7_wake_start(void)
{
  struct wake_s *w = &g_wake;
  int ret;

  if (w->running)
    {
      return 0;
    }

  if (w->ring == NULL)
    {
      w->ring = malloc(WAKE_RING * sizeof(int16_t));
      if (w->ring == NULL)
        {
          return -ENOMEM;
        }
    }

  w->seg_n = 0;
  w->ev = K7_WAKE_NONE;
  w->woken_until = 0;
  w->pause_req = false;
  w->running = true;

  if (!w->registered)
    {
      rpmsg_register_callback(w, wake_dev_created, wake_dev_destroy,
                              NULL, NULL);
      w->registered = true;
    }

  ret = wake_spawn(wake_capture_thread);
  if (ret == 0)
    {
      ret = wake_spawn(wake_asr_thread);
    }

  if (ret < 0)
    {
      w->running = false;
    }

  syslog(LOG_INFO, "唤醒: 开启 ret=%d\n", ret);
  return ret;
}

void k7_wake_stop(void)
{
  struct wake_s *w = &g_wake;
  int i;

  if (!w->running)
    {
      return;
    }

  w->running = false;
  pthread_cond_broadcast(&w->cond);

  /* 等采集停下，再撤端点（k7d 那边 read 返回 EPIPE，子进程退出） */

  for (i = 0; i < 60 && w->capturing; i++)
    {
      usleep(50 * 1000);
    }

  if (w->registered)
    {
      rpmsg_unregister_callback(w, wake_dev_created, wake_dev_destroy,
                                NULL, NULL);
      w->registered = false;
    }

  if (w->bound)
    {
      rpmsg_destroy_ept(&w->ept);
      w->bound = false;
    }

  w->hello = false;
  w->streaming = false;
  w->speaking = false;
  syslog(LOG_INFO, "唤醒: 关闭\n");
}

void k7_wake_pause(bool pause)
{
  struct wake_s *w = &g_wake;
  int i;

  w->pause_req = pause;
  if (!pause || !w->running)
    {
      return;
    }

  /* 采集回调在下一个缓冲（约 43ms）返回 false；k7a 收尾还要一点时间 */

  for (i = 0; i < 40 && (w->capturing || k7a_busy()); i++)
    {
      usleep(50 * 1000);
    }
}

void k7_wake_get_status(struct k7_wake_status_s *st)
{
  struct wake_s *w = &g_wake;

  pthread_mutex_lock(&w->lock);
  st->running     = w->running && w->streaming;
  st->linking     = w->running && !w->streaming;
  st->speaking    = w->speaking;
  st->recognizing = w->recognizing;
  st->woken       = w->woken_until != 0 && time(NULL) <= w->woken_until;
  st->level       = w->level;
  st->peak        = w->peak;
  st->offline     = w->offline;
  pthread_mutex_unlock(&w->lock);
}

void k7_wake_set_offline(bool offline)
{
  struct wake_s *w = &g_wake;

  pthread_mutex_lock(&w->lock);
  w->offline = offline;
  w->woken_until = 0;
  pthread_mutex_unlock(&w->lock);
  syslog(LOG_INFO, "唤醒: %s\n", offline ? "录制中，暂停上传识别"
                                         : "恢复识别");
}

void k7_wake_event_wav(char *path, size_t cap)
{
  struct wake_s *w = &g_wake;

  pthread_mutex_lock(&w->lock);
  snprintf(path, cap, "%s", w->got_wav);
  pthread_mutex_unlock(&w->lock);
}

int k7_wake_get_event(char *text, size_t tcap, char *question, size_t qcap)
{
  struct wake_s *w = &g_wake;
  int ev;

  pthread_mutex_lock(&w->lock);
  ev = w->ev;
  if (ev != K7_WAKE_NONE)
    {
      snprintf(text, tcap, "%s", w->ev_text);
      snprintf(question, qcap, "%s", w->ev_question);
      memcpy(w->got_wav, w->ev_wav, sizeof(w->got_wav));
      w->ev = K7_WAKE_NONE;
    }

  pthread_mutex_unlock(&w->lock);
  return ev;
}
