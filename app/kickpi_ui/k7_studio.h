/****************************************************************************
 * app/kickpi_ui/k7_studio.h
 * SPDX-License-Identifier: Apache-2.0
 *
 * 工作室录制模式：开始时记下设备状态、按预设布景（开补光、关有噪声的设备），
 * 门口显示"录制中"，门铃代答，录制期间给疑似噪声打时间点；结束时把设备
 * 恢复原样，打点清单存进 /data/studio/。
 *
 * 主题：
 *   home/studio/onair    ON / OFF，带 retain（门口的屏或灯订阅）
 *   home/doorbell/reply  录制中门铃响时，请门口设备代说的话
 *
 * 全部接口只在界面线程里调，内部不加锁。
 ****************************************************************************/

#ifndef __APP_KICKPI_UI_K7_STUDIO_H
#define __APP_KICKPI_UI_K7_STUDIO_H

#include <stdbool.h>
#include <stdint.h>

#define K7_STUDIO_MAXMARK 32

enum k7_studio_kind_e
{
  K7_STUDIO_NOISE = 0,    /* 没在说话时的突发声响 */
  K7_STUDIO_BELL,         /* 门铃 */
};

struct k7_studio_mark_s
{
  uint32_t at_ms;         /* 距开始录制 */
  uint32_t len_ms;
  uint16_t peak;          /* 0~32767，ALC 之后的值，只能相对比较 */
  uint8_t  kind;
};

struct k7_studio_status_s
{
  bool     onair;
  bool     listening;     /* 语音唤醒在跑或正在连 Linux：打点要靠它的电平和端点检测 */
  uint32_t elapsed_s;
  int      nmarks;
  int      rings;         /* 本次录制里的门铃次数 */
  int      floor;         /* 当前底噪估计（峰值，0~32767） */
  uint32_t gen;           /* 打点、说明变了就加一 */
  char     note[160];     /* 给用户看的最近一条说明 */
};

/* 开始 / 结束。家居离线时照样进入录制（只是不布景、不恢复），note 里说明。 */

void k7_studio_start(void);
void k7_studio_stop(void);

/* 界面定时器里每轮调一次（250ms），做噪声打点 */

void k7_studio_poll(void);

/* 门铃响了。录制中会请门口设备代答并打一个点，返回 true；不在录制返回
 * false，由界面自己提示。
 */

bool k7_studio_ring(void);

void k7_studio_status(struct k7_studio_status_s *st);
int  k7_studio_marks(struct k7_studio_mark_s *marks, int max);

#endif /* __APP_KICKPI_UI_K7_STUDIO_H */
