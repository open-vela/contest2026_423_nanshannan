/* SPDX-License-Identifier: Apache-2.0 */

/****************************************************************************
 * KICKPI-K7 红外接收的板级入口（ir init / ir st / ir raw）。
 *
 * 未经上板验证，不放进启动路径。
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <inttypes.h>
#include <stdio.h>

#include <arch/board/board.h>

#include "rk3576_ir.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int kickpi_k7_ir_initialize(void)
{
  return rk3576_ir_initialize(0);
}

int kickpi_k7_ir_dump(bool raw)
{
  struct rk3576_ir_stats_s st;
  uint32_t ev[64];
  int n;
  int i;

  n = rk3576_ir_getstats(&st, ev, raw ? 64 : 0);
  if (n < 0)
    {
      return n;
    }

  printf("irqs=%" PRIu32 " frames=%" PRIu32 " repeats=%" PRIu32
         " errors=%" PRIu32 "\n",
         st.irqs, st.frames, st.repeats, st.errors);
  printf("last scancode=0x%06" PRIx32 " proto=%u\n",
         st.last_scancode, st.last_proto);
  printf("ENABLE=%08" PRIx32 " CTRL=%08" PRIx32 " CLK=%08" PRIx32
         " INTSTS=%08" PRIx32 " HPC=%" PRIu32 " LPC=%" PRIu32 "\n",
         st.enable, st.ctrl, st.clkctl, st.intsts,
         st.level_hpc, st.level_lpc);

  /* P = 低电平（载波），S = 高电平（间隔），单位 us */

  for (i = 0; i < n; i++)
    {
      printf("%c%" PRIu32 "%s", (ev[i] & 0x80000000u) ? 'P' : 'S',
             ev[i] & 0x7fffffffu, (i % 8 == 7) ? "\n" : " ");
    }

  if (n % 8)
    {
      printf("\n");
    }

  return OK;
}
