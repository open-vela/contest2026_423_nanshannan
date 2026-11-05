/****************************************************************************
 * chip/rk3576/rk3576_ir.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_IR_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_IR_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* 诊断快照。寄存器字段是读取那一刻的值，计数从初始化起累计。 */

struct rk3576_ir_stats_s
{
  uint32_t irqs;            /* 进中断次数（每个电平沿一次）   */
  uint32_t frames;          /* 完整解出的 NEC 帧              */
  uint32_t repeats;         /* 有效重复码                     */
  uint32_t errors;          /* 帧中途出现非法间隔             */
  uint32_t last_scancode;
  uint16_t last_proto;      /* enum rc_proto                  */
  uint32_t intsts;
  uint32_t enable;
  uint32_t ctrl;
  uint32_t clkctl;
  uint32_t level_hpc;
  uint32_t level_lpc;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#undef EXTERN
#if defined(__cplusplus)
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Name: rk3576_ir_initialize
 *
 * Description:
 *   开 PWM0 时钟、配引脚与捕获模式、挂中断，注册 /dev/lirc<devno>。
 *   只能成功一次，再调返回 -EEXIST。
 *
 *   ★ AMP 下 PWM0 及其中断（SPI 100）必须先从 Linux 的 DTB 里让出来，
 *     否则 Linux 的 remotectl 驱动和本驱动抢同一个通道。
 *
 ****************************************************************************/

int rk3576_ir_initialize(int devno);

/* 是否已初始化 */

bool rk3576_ir_running(void);

/****************************************************************************
 * Name: rk3576_ir_getstats
 *
 * Description:
 *   取诊断快照，以及最近至多 nraw 条原始电平事件（按时间顺序；
 *   bit31 = 1 表示脉冲/低电平，低 31 位为微秒）。
 *
 * Returned Value:
 *   实际给出的原始事件条数；未初始化返回 -ENODEV。
 *
 ****************************************************************************/

int rk3576_ir_getstats(FAR struct rk3576_ir_stats_s *stats,
                       FAR uint32_t *raw, unsigned int nraw);

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_IR_H */
