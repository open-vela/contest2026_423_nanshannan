/****************************************************************************
 * app/kickpi_ui/k7_wake.h
 * SPDX-License-Identifier: Apache-2.0
 *
 * 语音唤醒："你好 openvela" + 问题。
 *   openvela：采集、环形缓冲、云端识别（mimo-v2.5-asr）、唤醒词匹配
 *   Linux（A72）：k7d 做端点检测，告诉这边一句话从哪到哪
 ****************************************************************************/

#ifndef __APP_KICKPI_UI_K7_WAKE_H
#define __APP_KICKPI_UI_K7_WAKE_H

#include <stdbool.h>
#include <stddef.h>

enum k7_wake_event_e
{
  K7_WAKE_NONE = 0,
  K7_WAKE_HEARD,       /* 识别出一句话，但不是唤醒词（text 里是原话） */
  K7_WAKE_ONLY,        /* 只说了唤醒词，等下一句当问题               */
  K7_WAKE_ASK,         /* 唤醒词 + 问题（或唤醒后的下一句）：question  */
};

struct k7_wake_status_s
{
  bool running;        /* 开关打开且 Linux 那边已经接上       */
  bool linking;        /* 开关打开，还在等 Linux 握手         */
  bool speaking;       /* Linux 报"开始说话"，还没说完        */
  bool recognizing;    /* 有一句在识别中                       */
  bool woken;          /* 说过唤醒词，正在等问题               */
  int  level;          /* 0~100                                 */
  int  peak;           /* 最近 100ms 的峰值 0~32767（ALC 之后）    */
  bool offline;        /* 录制中：只做端点检测，不送识别          */
};

int  k7_wake_start(void);
void k7_wake_stop(void);

/* 朗读、录音机要用音频设备时先暂停采集，用完恢复。暂停会等采集真正
 * 停下（最多约 2 秒）才返回。
 */

void k7_wake_pause(bool pause);

void k7_wake_get_status(struct k7_wake_status_s *st);

/* 录制时打开：端点检测和电平照常，但每句话都不上传识别（不花钱、不外传
 * 录制内容），因此也不会有任何事件。关掉后恢复。
 */

void k7_wake_set_offline(bool offline);

/* 取一个事件（没有返回 K7_WAKE_NONE）。text 是识别原话，question 是
 * 要交给助手的问题（K7_WAKE_ASK 时有效）。
 */

int  k7_wake_get_event(char *text, size_t tcap, char *question,
                       size_t qcap);

/* 上一次 k7_wake_get_event 取到的那句话的原始录音（16kHz 单声道 WAV，
 * /tmp 下轮换的几个文件之一，之后再说三句就会被覆盖，要留就趁早拷走）。
 * 没有时返回空串。
 */

void k7_wake_event_wav(char *path, size_t cap);

#endif /* __APP_KICKPI_UI_K7_WAKE_H */
