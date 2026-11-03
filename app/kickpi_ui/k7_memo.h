/****************************************************************************
 * app/kickpi_ui/k7_memo.h
 * SPDX-License-Identifier: Apache-2.0
 *
 * 灵感速记："你好 openvela，记一下……" 记下原话和原始录音，后台让模型分类、
 * 起标题、打标签。存在 /data/memo/（memo.jsonl + <id>.wav），重启还在。
 *
 * 加一条立即返回（先存原话），录音拷贝和打标签在本模块自己的线程里做；
 * 断网时标签留空，之后自动补打。界面线程只读内存里的列表，按 gen 判断
 * 要不要重画。
 ****************************************************************************/

#ifndef __APP_KICKPI_UI_K7_MEMO_H
#define __APP_KICKPI_UI_K7_MEMO_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define K7_MEMO_MAX 100           /* 超过后删最旧的 */

enum k7_memo_state_e
{
  K7_MEMO_PENDING = 0,            /* 还没打标签（排队中或断网） */
  K7_MEMO_TAGGED,
  K7_MEMO_FAILED,                 /* 试了几次都不行，只留原话 */
};

struct k7_memo_s
{
  uint32_t id;                    /* 也是录音文件名 */
  time_t   ts;
  uint8_t  state;
  bool     has_wav;
  char     text[480];             /* 识别出的原话（去掉"记一下"之前的部分） */
  char     type[16];              /* 选题 / 待办 / 客户 / 灵感 / 其他 */
  char     title[64];
  char     tags[64];              /* 逗号分隔 */
};

struct k7_memo_info_s
{
  int      count;
  bool     persistent;            /* /data 可写：重启不丢 */
  int      pending;
  uint32_t gen;                   /* 列表或任一条变了就加一 */
};

/* 起后台线程并读入已存的速记。重复调用无效果。 */

int  k7_memo_start(void);

/* 加一条。wav 是这句话的原始录音路径（可为 NULL 或空串），会被拷走。
 * 返回新条目的 id，失败返回负 errno。
 */

int  k7_memo_add(const char *text, const char *wav);

void k7_memo_info(struct k7_memo_info_s *info);

/* 第 i 条，0 是最新的。没有返回 false。 */

bool k7_memo_get(int i, struct k7_memo_s *m);

int  k7_memo_delete(uint32_t id);

#endif /* __APP_KICKPI_UI_K7_MEMO_H */
