/****************************************************************************
 * app/kickpi_ui/k7_studio.c
 * SPDX-License-Identifier: Apache-2.0
 *
 * ★ 布景与恢复由面板自己做，不交给 home/scene/set
 *
 *   broker 是板上的 k7mqttd，只转发、不编排；而且"结束后恢复原样"需要
 *   开始前的快照，只有面板手里有。所以开始时逐台记下状态、逐台下发；
 *   结束时只恢复**被我们改过、而且录制中没人再动过**的设备 —— 用户在录制
 *   中手动开了空调，结束时不该替他关掉。开始前状态未知（设备没报过）的
 *   不恢复。
 *
 * ★ 录制期间语音不出板子
 *
 *   语音唤醒对每一句话都做云端识别。录视频时你说的每句话都会被切出来
 *   上传，既花钱也不合适。录制期间让唤醒只做端点检测（k7_wake_set_offline），
 *   代价是不能用语音结束录制，只能点屏幕。
 *
 * ★ 噪声打点：只在"没在说话"时打
 *
 *   人说话肯定比环境声大，只看音量会全是误报。A72 上的端点检测本来就报
 *   "开始说话 / 说完"，规则是：不在说话、电平超过底噪的 STUDIO_K 倍且不低于
 *   STUDIO_MIN，就记一个点；2 秒内的连成一个。底噪按不说话时的峰值慢慢
 *   跟踪。电平来自唤醒那条采集，带 ALC，数值只能相对比较，不是分贝。
 *   这是给后期剪辑的提示，不保证每一处都准。
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>

#include "k7_home.h"
#include "k7_studio.h"
#include "k7_wake.h"

#define STUDIO_ONAIR_TOPIC "home/studio/onair"
#define STUDIO_REPLY_TOPIC "home/doorbell/reply"
#define STUDIO_REPLY       "主人正在录制，请把东西放在门口，谢谢。"
#define STUDIO_DIR         "/data/studio"
#define STUDIO_LOG         STUDIO_DIR "/marks.txt"
#define STUDIO_TZ_S        (8 * 3600)

#define STUDIO_K           6       /* 超过底噪几倍算突发 */
#define STUDIO_MIN         1500    /* 绝对下限，免得安静时一点声音就算 */
#define STUDIO_MERGE_MS    2000
#define STUDIO_FLOOR_MIN   30

/* 录制预设：补光打开，会出声的关掉 */

static const struct
{
  const char *id;
  bool        on;
  const char *name;
}
g_preset[] =
{
  { "desk_lamp", true,  "台灯" },
  { "ac",        false, "空调" },
  { "fresh_air", false, "新风" },
  { "tv",        false, "电视" },
  { "kettle",    false, "饮水机" },
};

#define STUDIO_NPRESET (sizeof(g_preset) / sizeof(g_preset[0]))

static struct
{
  bool     onair;
  bool     wake_ours;              /* 唤醒是我们为录制临时开的，结束时关掉 */
  uint64_t t0_ms;
  time_t   t0_wall;
  int8_t   snap[STUDIO_NPRESET];   /* 开始前的状态，-1 未知 */
  bool     changed[STUDIO_NPRESET];

  /* 打点 */

  struct k7_studio_mark_s mark[K7_STUDIO_MAXMARK];
  int      nmarks;
  int      rings;
  int      floor;
  uint64_t ev_start;               /* 正在进行的突发，0 表示没有 */
  uint16_t ev_peak;

  uint32_t gen;
  char     note[160];
} g_s;

static uint64_t studio_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void studio_note(const char *text)
{
  snprintf(g_s.note, sizeof(g_s.note), "%s", text);
  g_s.gen++;
}

/* 记一个点；离上一个同类的点不到 STUDIO_MERGE_MS 就并进去 */

static void studio_mark(uint8_t kind, uint64_t start, uint64_t end,
                        uint16_t peak)
{
  struct k7_studio_mark_s *m;
  uint32_t at = (uint32_t)(start - g_s.t0_ms);

  if (g_s.nmarks > 0)
    {
      m = &g_s.mark[g_s.nmarks - 1];
      if (m->kind == kind && at <= m->at_ms + m->len_ms + STUDIO_MERGE_MS)
        {
          m->len_ms = (uint32_t)(end - g_s.t0_ms) - m->at_ms;
          if (peak > m->peak)
            {
              m->peak = peak;
            }

          g_s.gen++;
          return;
        }
    }

  if (g_s.nmarks >= K7_STUDIO_MAXMARK)
    {
      return;
    }

  m = &g_s.mark[g_s.nmarks++];
  m->at_ms = at;
  m->len_ms = (uint32_t)(end - start);
  m->peak = peak;
  m->kind = kind;
  g_s.gen++;
}

static void studio_save_log(uint32_t secs)
{
  struct tm tm;
  time_t t = g_s.t0_wall + STUDIO_TZ_S;
  FILE *fp;
  int i;

  mkdir(STUDIO_DIR, 0777);
  fp = fopen(STUDIO_LOG, "a");
  if (fp == NULL)
    {
      syslog(LOG_WARNING, "录制: 打点清单没存（%s：%d）\n", STUDIO_LOG,
             errno);
      return;
    }

  gmtime_r(&t, &tm);
  fprintf(fp, "%04d-%02d-%02d %02d:%02d 起录 %u 分 %02u 秒，打点 %d 处\n",
          tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
          tm.tm_min, (unsigned)(secs / 60), (unsigned)(secs % 60),
          g_s.nmarks);
  for (i = 0; i < g_s.nmarks; i++)
    {
      const struct k7_studio_mark_s *m = &g_s.mark[i];

      if (m->kind == K7_STUDIO_BELL)
        {
          fprintf(fp, "  %02u:%02u  门铃\n", (unsigned)(m->at_ms / 60000),
                  (unsigned)(m->at_ms / 1000 % 60));
          continue;
        }

      fprintf(fp, "  %02u:%02u  噪声  %u.%u 秒  峰值 %u\n",
              (unsigned)(m->at_ms / 60000),
              (unsigned)(m->at_ms / 1000 % 60),
              (unsigned)(m->len_ms / 1000),
              (unsigned)(m->len_ms / 100 % 10), m->peak);
    }

  fclose(fp);
}

void k7_studio_start(void)
{
  struct k7_wake_status_s ws;
  struct k7_home_status_s hs;
  char done[96] = "";
  size_t i;

  if (g_s.onair)
    {
      return;
    }

  memset(&g_s.mark, 0, sizeof(g_s.mark));
  g_s.nmarks = 0;
  g_s.rings = 0;
  g_s.ev_start = 0;
  g_s.floor = STUDIO_FLOOR_MIN;
  g_s.t0_ms = studio_ms();
  g_s.t0_wall = time(NULL);
  g_s.onair = true;

  k7_home_status(&hs);
  for (i = 0; i < STUDIO_NPRESET; i++)
    {
      int idx = k7_home_find(g_preset[i].id);

      g_s.snap[i] = idx < 0 ? -1 : k7_home_state(idx);
      g_s.changed[i] = false;
      if (!hs.connected || g_s.snap[i] < 0 ||
          g_s.snap[i] == g_preset[i].on)
        {
          continue;
        }

      if (k7_home_set(idx, g_preset[i].on) == 0)
        {
          g_s.changed[i] = true;
          snprintf(done + strlen(done), sizeof(done) - strlen(done),
                   "%s%s%s", done[0] ? "、" : "",
                   g_preset[i].on ? "开" : "关", g_preset[i].name);
        }
    }

  k7_home_publish(STUDIO_ONAIR_TOPIC, "ON", true);

  /* 噪声打点要用唤醒那条采集的电平和端点检测。用户没开唤醒时临时开，
   * 先切成不上传再开，开起来的第一句也不会出板子。
   */

  k7_wake_set_offline(true);
  k7_wake_get_status(&ws);
  g_s.wake_ours = false;
  if (!ws.running && !ws.linking)
    {
      g_s.wake_ours = k7_wake_start() == 0;
    }

  if (!hs.connected)
    {
      studio_note("录制中。家居离线，没有布景；门口提示也没发出去。");
    }
  else if (done[0] == '\0')
    {
      studio_note("录制中。设备已经是录制状态，门口已显示\"录制中\"。");
    }
  else
    {
      snprintf(g_s.note, sizeof(g_s.note),
               "录制中。已%s，门口已显示\"录制中\"。", done);
      g_s.gen++;
    }

  syslog(LOG_INFO, "录制: 开始（%s）\n", g_s.note);
}

void k7_studio_stop(void)
{
  struct k7_home_status_s hs;
  uint32_t secs;
  int kept = 0;
  int back = 0;
  size_t i;

  if (!g_s.onair)
    {
      return;
    }

  /* 还在进行的突发也算进去 */

  if (g_s.ev_start != 0)
    {
      studio_mark(K7_STUDIO_NOISE, g_s.ev_start, studio_ms(), g_s.ev_peak);
      g_s.ev_start = 0;
    }

  secs = (uint32_t)((studio_ms() - g_s.t0_ms) / 1000);
  g_s.onair = false;

  k7_home_status(&hs);
  for (i = 0; i < STUDIO_NPRESET; i++)
    {
      int idx;

      if (!g_s.changed[i])
        {
          continue;
        }

      idx = k7_home_find(g_preset[i].id);
      if (!hs.connected || k7_home_state(idx) != g_preset[i].on)
        {
          kept++;                        /* 离线，或者录制中有人动过 */
          continue;
        }

      if (k7_home_set(idx, g_s.snap[i]) == 0)
        {
          back++;
        }
    }

  k7_home_publish(STUDIO_ONAIR_TOPIC, "OFF", true);
  if (g_s.wake_ours)
    {
      k7_wake_stop();
      g_s.wake_ours = false;
    }

  k7_wake_set_offline(false);
  studio_save_log(secs);

  snprintf(g_s.note, sizeof(g_s.note),
           "录了 %u 分 %02u 秒，打点 %d 处。%s%s",
           (unsigned)(secs / 60), (unsigned)(secs % 60), g_s.nmarks,
           back ? "设备已恢复原样。" : "",
           kept ? "有设备录制中被动过或家居离线，没去恢复。" : "");
  g_s.gen++;
  syslog(LOG_INFO, "录制: 结束（%s）\n", g_s.note);
}

void k7_studio_poll(void)
{
  struct k7_wake_status_s ws;
  uint64_t now;
  int thr;

  if (!g_s.onair)
    {
      return;
    }

  k7_wake_get_status(&ws);
  if (!ws.running)
    {
      return;
    }

  now = studio_ms();
  if (ws.speaking)
    {
      /* 说话开始前那一段突发照常收尾，说话期间不判 */

      if (g_s.ev_start != 0)
        {
          studio_mark(K7_STUDIO_NOISE, g_s.ev_start, now, g_s.ev_peak);
          g_s.ev_start = 0;
        }

      return;
    }

  thr = g_s.floor * STUDIO_K;
  if (thr < STUDIO_MIN)
    {
      thr = STUDIO_MIN;
    }

  if (ws.peak >= thr)
    {
      if (g_s.ev_start == 0)
        {
          g_s.ev_start = now;
          g_s.ev_peak = 0;
        }

      if (ws.peak > g_s.ev_peak)
        {
          g_s.ev_peak = (uint16_t)ws.peak;
        }

      return;
    }

  if (g_s.ev_start != 0)
    {
      studio_mark(K7_STUDIO_NOISE, g_s.ev_start, now, g_s.ev_peak);
      g_s.ev_start = 0;
    }

  /* 底噪：只用不说话、不在突发里的读数，跟踪得慢一点 */

  g_s.floor += (ws.peak - g_s.floor) / 8;
  if (g_s.floor < STUDIO_FLOOR_MIN)
    {
      g_s.floor = STUDIO_FLOOR_MIN;
    }
}

bool k7_studio_ring(void)
{
  uint64_t now;

  if (!g_s.onair)
    {
      return false;
    }

  now = studio_ms();
  g_s.rings++;
  studio_mark(K7_STUDIO_BELL, now, now, 0);
  if (k7_home_publish(STUDIO_REPLY_TOPIC, STUDIO_REPLY, false) == 0)
    {
      studio_note("门铃响了，已请门口代答：请把东西放门口。");
    }
  else
    {
      studio_note("门铃响了，但代答没发出去（家居离线）。");
    }

  return true;
}

void k7_studio_status(struct k7_studio_status_s *st)
{
  struct k7_wake_status_s ws;

  k7_wake_get_status(&ws);
  st->onair = g_s.onair;
  st->listening = ws.running || ws.linking;  /* 握手中几秒也算，免得误报 */
  st->elapsed_s = g_s.onair ?
                  (uint32_t)((studio_ms() - g_s.t0_ms) / 1000) : 0;
  st->nmarks = g_s.nmarks;
  st->rings = g_s.rings;
  st->floor = g_s.floor;
  st->gen = g_s.gen;
  snprintf(st->note, sizeof(st->note), "%s", g_s.note);
}

int k7_studio_marks(struct k7_studio_mark_s *marks, int max)
{
  int n = g_s.nmarks < max ? g_s.nmarks : max;

  memcpy(marks, g_s.mark, n * sizeof(*marks));
  return n;
}
