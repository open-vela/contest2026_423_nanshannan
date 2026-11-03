/****************************************************************************
 * app/kickpi_ui/k7_memo.c
 * SPDX-License-Identifier: Apache-2.0
 *
 * ★ 存储
 *
 *   /data 是 eMMC 上的 vfat 分区（见 board 的 appinit），重启不丢。没挂上
 *   （/data 只是 tmpfs 或不存在）时退到 /tmp/memo，界面明说"不会保存"。
 *   memo.jsonl 一行一条，每次改动整份重写：先写 .tmp 再改名，写到一半掉电
 *   最多丢这一次改动，不会把整份写坏。一百条也才几十 KB。
 *
 * ★ 打标签
 *
 *   原话先存，立即在界面上出现；分类、标题、标签由后台线程请求模型补上。
 *   失败（断网、超时）不丢条目，10 分钟后重试，最多 3 次；之后只留原话。
 *   模型被要求只回一行 JSON，这里仍然只取第一个 { 到最后一个 } 之间解析，
 *   类型不在约定集合里一律归"其他"。
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <cJSON.h>

#include "k7_llm.h"
#include "k7_memo.h"

#define MEMO_DATA_DIR    "/data/memo"
#define MEMO_TMP_DIR     "/tmp/memo"
#define MEMO_FILE        "memo.jsonl"
#define MEMO_TRIES       3
#define MEMO_RETRY_S     (10 * 60)
#define MEMO_MAXCOPY     4
#define MEMO_LINE_CAP    2048

static const char *const g_types[] =
{
  "选题", "待办", "客户", "灵感", "其他"
};

struct memo_copy_s
{
  uint32_t id;
  char     src[32];
};

static struct
{
  pthread_mutex_t lock;
  pthread_cond_t  cond;
  bool     started;
  bool     persistent;
  bool     dirty;                  /* 有改动没写盘 */
  const char *dir;

  struct k7_memo_s item[K7_MEMO_MAX];   /* 旧的在前 */
  uint8_t  tries[K7_MEMO_MAX];
  int      n;
  uint32_t last_id;
  time_t   next_try;               /* 上次打标签失败后，这之前不再试 */
  uint32_t gen;

  struct memo_copy_s copy[MEMO_MAXCOPY];
  int      ncopy;
} g_m =
{
  .lock = PTHREAD_MUTEX_INITIALIZER,
  .cond = PTHREAD_COND_INITIALIZER,
};

/* 按字符截断地拷 UTF-8：不会把一个汉字切成两半 */

static void utf8_copy(char *dst, size_t cap, const char *src)
{
  size_t n = 0;
  size_t i = 0;

  while (src[i] != '\0')
    {
      size_t len = 1;

      if (((unsigned char)src[i] & 0xe0) == 0xc0)
        {
          len = 2;
        }
      else if (((unsigned char)src[i] & 0xf0) == 0xe0)
        {
          len = 3;
        }
      else if (((unsigned char)src[i] & 0xf8) == 0xf0)
        {
          len = 4;
        }

      if (n + len >= cap)
        {
          break;
        }

      memcpy(dst + n, src + i, len);
      n += len;
      i += len;
    }

  dst[n] = '\0';
}

static int memo_find(uint32_t id)
{
  int i;

  for (i = 0; i < g_m.n; i++)
    {
      if (g_m.item[i].id == id)
        {
          return i;
        }
    }

  return -1;
}

static void memo_wav_path(char *buf, size_t cap, uint32_t id)
{
  snprintf(buf, cap, "%s/%08" PRIx32 ".wav", g_m.dir, id);
}

/* ---- 读写 memo.jsonl ---- */

static const char *json_str(cJSON *o, const char *key)
{
  cJSON *v = cJSON_GetObjectItem(o, key);

  return cJSON_IsString(v) ? v->valuestring : "";
}

static void memo_load(void)
{
  char path[48];
  char *line;
  FILE *fp;

  snprintf(path, sizeof(path), "%s/%s", g_m.dir, MEMO_FILE);
  fp = fopen(path, "r");
  if (fp == NULL)
    {
      return;
    }

  line = malloc(MEMO_LINE_CAP);
  while (line != NULL && fgets(line, MEMO_LINE_CAP, fp) != NULL &&
         g_m.n < K7_MEMO_MAX)
    {
      struct k7_memo_s *m = &g_m.item[g_m.n];
      cJSON *o = cJSON_Parse(line);
      cJSON *v;

      if (o == NULL)
        {
          continue;
        }

      memset(m, 0, sizeof(*m));
      v = cJSON_GetObjectItem(o, "id");
      m->id = cJSON_IsNumber(v) ? (uint32_t)v->valuedouble : 0;
      v = cJSON_GetObjectItem(o, "ts");
      m->ts = cJSON_IsNumber(v) ? (time_t)v->valuedouble : 0;
      v = cJSON_GetObjectItem(o, "state");
      m->state = cJSON_IsNumber(v) ? (uint8_t)v->valueint : K7_MEMO_PENDING;
      m->has_wav = cJSON_IsTrue(cJSON_GetObjectItem(o, "wav"));
      utf8_copy(m->text, sizeof(m->text), json_str(o, "text"));
      utf8_copy(m->type, sizeof(m->type), json_str(o, "type"));
      utf8_copy(m->title, sizeof(m->title), json_str(o, "title"));
      utf8_copy(m->tags, sizeof(m->tags), json_str(o, "tags"));
      cJSON_Delete(o);

      if (m->id != 0 && m->text[0] != '\0')
        {
          if (m->id > g_m.last_id)
            {
              g_m.last_id = m->id;
            }

          g_m.tries[g_m.n] = 0;
          g_m.n++;
        }
    }

  free(line);
  fclose(fp);
  syslog(LOG_INFO, "速记: 读入 %d 条（%s）\n", g_m.n, g_m.dir);
}

/* 后台线程里调。先在锁里拷一份，写盘不持锁。 */

static void memo_save(void)
{
  static struct k7_memo_s snap[K7_MEMO_MAX];
  char path[48];
  char tmp[56];
  FILE *fp;
  int n;
  int i;

  pthread_mutex_lock(&g_m.lock);
  n = g_m.n;
  memcpy(snap, g_m.item, n * sizeof(snap[0]));
  g_m.dirty = false;
  pthread_mutex_unlock(&g_m.lock);

  snprintf(path, sizeof(path), "%s/%s", g_m.dir, MEMO_FILE);
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  fp = fopen(tmp, "w");
  if (fp == NULL)
    {
      syslog(LOG_WARNING, "速记: 写不了 %s（%d）\n", tmp, errno);
      return;
    }

  for (i = 0; i < n; i++)
    {
      cJSON *o = cJSON_CreateObject();
      char *s;

      cJSON_AddNumberToObject(o, "id", snap[i].id);
      cJSON_AddNumberToObject(o, "ts", (double)snap[i].ts);
      cJSON_AddNumberToObject(o, "state", snap[i].state);
      cJSON_AddBoolToObject(o, "wav", snap[i].has_wav);
      cJSON_AddStringToObject(o, "text", snap[i].text);
      cJSON_AddStringToObject(o, "type", snap[i].type);
      cJSON_AddStringToObject(o, "title", snap[i].title);
      cJSON_AddStringToObject(o, "tags", snap[i].tags);
      s = cJSON_PrintUnformatted(o);
      cJSON_Delete(o);
      if (s != NULL)
        {
          fprintf(fp, "%s\n", s);
          cJSON_free(s);
        }
    }

  fclose(fp);

  /* vfat 上 rename 不覆盖已有文件，先删旧的 */

  unlink(path);
  if (rename(tmp, path) < 0)
    {
      syslog(LOG_WARNING, "速记: 改名 %s 失败（%d）\n", tmp, errno);
    }
}

/* ---- 录音拷贝 ---- */

static bool memo_copy_file(const char *src, const char *dst)
{
  static char buf[4096];
  bool ok = false;
  ssize_t n;
  int in;
  int out;

  in = open(src, O_RDONLY);
  if (in < 0)
    {
      return false;
    }

  out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (out >= 0)
    {
      ok = true;
      while ((n = read(in, buf, sizeof(buf))) > 0)
        {
          if (write(out, buf, n) != n)
            {
              ok = false;
              break;
            }
        }

      close(out);
      if (!ok)
        {
          unlink(dst);
        }
    }

  close(in);
  return ok;
}

/* ---- 打标签 ---- */

static int memo_tag(const char *text, struct k7_memo_s *out)
{
  static char prompt[1024];
  static char answer[1024];
  const char *type;
  char *l;
  char *r;
  cJSON *o;
  cJSON *v;
  size_t k;
  int ret;

  snprintf(prompt, sizeof(prompt),
           "下面是用户随口记下的一条想法（语音识别的结果，可能有错别字）。"
           "请给它分类、起标题、打标签，只输出一行 JSON，不要任何别的文字："
           "{\"type\":\"选题|待办|客户|灵感|其他 选一个\","
           "\"title\":\"不超过 12 个字\","
           "\"tags\":[\"最多 3 个，每个不超过 4 个字\"]}。"
           "选题指做内容的题目，待办指要去做的事，客户指和客户或合作方有关。"
           "内容：%s", text);

  ret = k7_llm_chat(NULL, prompt, 200, answer, sizeof(answer));
  if (ret < 0)
    {
      syslog(LOG_WARNING, "速记: 打标签失败 %d（%s）\n", ret, answer);
      return ret;
    }

  l = strchr(answer, '{');
  r = strrchr(answer, '}');
  if (l == NULL || r == NULL || r < l)
    {
      syslog(LOG_WARNING, "速记: 模型没回 JSON：%.120s\n", answer);
      return -EPROTO;
    }

  r[1] = '\0';
  o = cJSON_Parse(l);
  if (o == NULL)
    {
      syslog(LOG_WARNING, "速记: JSON 解析失败：%.120s\n", l);
      return -EPROTO;
    }

  type = "其他";
  for (k = 0; k < sizeof(g_types) / sizeof(g_types[0]); k++)
    {
      if (strcmp(json_str(o, "type"), g_types[k]) == 0)
        {
          type = g_types[k];
        }
    }

  utf8_copy(out->type, sizeof(out->type), type);
  utf8_copy(out->title, sizeof(out->title), json_str(o, "title"));
  out->tags[0] = '\0';
  v = cJSON_GetObjectItem(o, "tags");
  for (k = 0; cJSON_IsArray(v) && k < 3; k++)
    {
      cJSON *t = cJSON_GetArrayItem(v, k);
      size_t len = strlen(out->tags);

      if (!cJSON_IsString(t))
        {
          break;
        }

      if (len > 0 && len + 2 < sizeof(out->tags))
        {
          strcat(out->tags, ",");
          len++;
        }

      utf8_copy(out->tags + len, sizeof(out->tags) - len, t->valuestring);
    }

  cJSON_Delete(o);
  return 0;
}

/* ---- 后台线程 ---- */

static void *memo_thread(void *arg)
{
  (void)arg;

  for (; ; )
    {
      struct memo_copy_s job;
      struct k7_memo_s tag;
      struct timespec ts;
      char text[sizeof(((struct k7_memo_s *)0)->text)];
      uint32_t id = 0;
      bool have_job = false;
      bool save;
      int i;

      pthread_mutex_lock(&g_m.lock);
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += 60;
      while (g_m.ncopy == 0 && !g_m.dirty)
        {
          /* 有到期该补打的标签就不睡 */

          bool due = false;

          for (i = 0; i < g_m.n; i++)
            {
              if (g_m.item[i].state == K7_MEMO_PENDING &&
                  time(NULL) >= g_m.next_try)
                {
                  due = true;
                  break;
                }
            }

          if (due ||
              pthread_cond_timedwait(&g_m.cond, &g_m.lock, &ts) == ETIMEDOUT)
            {
              break;
            }
        }

      /* 1) 拷录音：/tmp 下的轮换文件几句话后就被覆盖，先做 */

      if (g_m.ncopy > 0)
        {
          job = g_m.copy[0];
          memmove(&g_m.copy[0], &g_m.copy[1],
                  (g_m.ncopy - 1) * sizeof(job));
          g_m.ncopy--;
          have_job = true;
        }

      pthread_mutex_unlock(&g_m.lock);

      if (have_job)
        {
          char dst[48];
          bool ok;

          memo_wav_path(dst, sizeof(dst), job.id);
          ok = memo_copy_file(job.src, dst);
          pthread_mutex_lock(&g_m.lock);
          i = memo_find(job.id);
          if (i >= 0 && ok)
            {
              g_m.item[i].has_wav = true;
              g_m.dirty = true;
              g_m.gen++;
            }
          else if (i < 0 && ok)
            {
              unlink(dst);               /* 拷的过程中被删了 */
            }

          pthread_mutex_unlock(&g_m.lock);
          if (!ok)
            {
              syslog(LOG_WARNING, "速记: 录音 %s 没拷成\n", job.src);
            }
        }

      /* 2) 补一条标签（一次只打一条，打完回头再看有没有新录音要拷） */

      pthread_mutex_lock(&g_m.lock);
      if (time(NULL) >= g_m.next_try)
        {
          for (i = g_m.n - 1; i >= 0; i--)
            {
              if (g_m.item[i].state == K7_MEMO_PENDING)
                {
                  id = g_m.item[i].id;
                  memcpy(text, g_m.item[i].text, sizeof(text));
                  break;
                }
            }
        }

      pthread_mutex_unlock(&g_m.lock);

      if (id != 0)
        {
          int ret;

          memset(&tag, 0, sizeof(tag));
          ret = memo_tag(text, &tag);
          pthread_mutex_lock(&g_m.lock);
          i = memo_find(id);
          if (i >= 0)
            {
              if (ret == 0)
                {
                  struct k7_memo_s *m = &g_m.item[i];

                  memcpy(m->type, tag.type, sizeof(m->type));
                  memcpy(m->title, tag.title, sizeof(m->title));
                  memcpy(m->tags, tag.tags, sizeof(m->tags));
                  m->state = K7_MEMO_TAGGED;
                  g_m.next_try = 0;
                }
              else
                {
                  g_m.next_try = time(NULL) + MEMO_RETRY_S;
                  if (++g_m.tries[i] >= MEMO_TRIES)
                    {
                      g_m.item[i].state = K7_MEMO_FAILED;
                    }
                }

              g_m.dirty = true;
              g_m.gen++;
            }

          pthread_mutex_unlock(&g_m.lock);
        }

      pthread_mutex_lock(&g_m.lock);
      save = g_m.dirty;
      pthread_mutex_unlock(&g_m.lock);
      if (save)
        {
          memo_save();
        }
    }

  return NULL;
}

/* ---- 对界面的接口 ---- */

int k7_memo_start(void)
{
  pthread_attr_t attr;
  pthread_t tid;
  struct statfs sf;
  int ret;

  if (g_m.started)
    {
      return 0;
    }

  g_m.persistent = statfs("/data", &sf) == 0 &&
                   sf.f_type == MSDOS_SUPER_MAGIC;
  g_m.dir = g_m.persistent ? MEMO_DATA_DIR : MEMO_TMP_DIR;
  mkdir(g_m.dir, 0777);
  memo_load();

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 32768);   /* TLS 握手 + cJSON */
  ret = pthread_create(&tid, &attr, memo_thread, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      return -ret;
    }

  pthread_setname_np(tid, "k7memo");
  pthread_detach(tid);
  g_m.started = true;
  return 0;
}

int k7_memo_add(const char *text, const char *wav)
{
  struct k7_memo_s *m;
  time_t now = time(NULL);

  if (!g_m.started || text == NULL || text[0] == '\0')
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_m.lock);

  /* 满了删最旧的（连同录音） */

  if (g_m.n >= K7_MEMO_MAX)
    {
      char path[48];

      memo_wav_path(path, sizeof(path), g_m.item[0].id);
      unlink(path);
      memmove(&g_m.item[0], &g_m.item[1],
              (g_m.n - 1) * sizeof(g_m.item[0]));
      memmove(&g_m.tries[0], &g_m.tries[1], g_m.n - 1);
      g_m.n--;
    }

  m = &g_m.item[g_m.n];
  memset(m, 0, sizeof(*m));
  m->id = (uint32_t)now > g_m.last_id ? (uint32_t)now : g_m.last_id + 1;
  g_m.last_id = m->id;
  m->ts = now;
  m->state = K7_MEMO_PENDING;
  utf8_copy(m->text, sizeof(m->text), text);
  g_m.tries[g_m.n] = 0;
  g_m.n++;

  /* 新记的一条马上试，不受上一条失败的退避影响 */

  g_m.next_try = 0;
  if (wav != NULL && wav[0] != '\0' && g_m.ncopy < MEMO_MAXCOPY)
    {
      g_m.copy[g_m.ncopy].id = m->id;
      snprintf(g_m.copy[g_m.ncopy].src, sizeof(g_m.copy[0].src), "%s", wav);
      g_m.ncopy++;
    }

  g_m.dirty = true;
  g_m.gen++;
  pthread_cond_signal(&g_m.cond);
  pthread_mutex_unlock(&g_m.lock);
  syslog(LOG_INFO, "速记: 记下 %" PRIu32 "「%s」录音 %s\n", m->id, text,
         wav != NULL && wav[0] ? wav : "无");
  return (int)m->id;
}

void k7_memo_info(struct k7_memo_info_s *info)
{
  int i;

  pthread_mutex_lock(&g_m.lock);
  info->count = g_m.n;
  info->persistent = g_m.persistent;
  info->gen = g_m.gen;
  info->pending = 0;
  for (i = 0; i < g_m.n; i++)
    {
      info->pending += g_m.item[i].state == K7_MEMO_PENDING;
    }

  pthread_mutex_unlock(&g_m.lock);
}

bool k7_memo_get(int i, struct k7_memo_s *m)
{
  bool ok;

  pthread_mutex_lock(&g_m.lock);
  ok = i >= 0 && i < g_m.n;
  if (ok)
    {
      *m = g_m.item[g_m.n - 1 - i];
    }

  pthread_mutex_unlock(&g_m.lock);
  return ok;
}

int k7_memo_delete(uint32_t id)
{
  char path[48];
  int i;

  pthread_mutex_lock(&g_m.lock);
  i = memo_find(id);
  if (i < 0)
    {
      pthread_mutex_unlock(&g_m.lock);
      return -ENOENT;
    }

  memmove(&g_m.item[i], &g_m.item[i + 1],
          (g_m.n - 1 - i) * sizeof(g_m.item[0]));
  memmove(&g_m.tries[i], &g_m.tries[i + 1], g_m.n - 1 - i);
  g_m.n--;
  g_m.dirty = true;
  g_m.gen++;
  pthread_cond_signal(&g_m.cond);
  pthread_mutex_unlock(&g_m.lock);

  memo_wav_path(path, sizeof(path), id);
  unlink(path);
  return 0;
}
