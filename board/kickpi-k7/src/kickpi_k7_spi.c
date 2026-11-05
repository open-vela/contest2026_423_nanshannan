/****************************************************************************
 * board/kickpi-k7/src/kickpi_k7_spi.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/* 注册 SPI4 为 /dev/spi4，可用 nsh 的 spi 命令收发。
 *
 * 板上只有 SPI4 引到 40 针扩展口（P1 的 19/21/23/24 脚，见 KICKPI-K7
 * 规格书的排针定义），其余三个通用 SPI 控制器没有对外引脚。
 *
 * ★ 排针上默认什么都没接
 *
 *   所以这里只做"总线能不能跑"的验证，不探测具体从机。自检办法是把
 *   MOSI 与 MISO 短接做回环：发什么就该收到什么。没短接时 MISO 悬空，
 *   读回值取决于内部上拉，通常是全 0xff —— 这个值不能当成"通过"，
 *   要用一组变化的图案才能区分"真的回环"和"恰好读到 0xff"。
 *   回环自检由 kickpi_spi_loopback() 提供，默认不在启动时跑。
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <debug.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/spi/spi.h>
#include <nuttx/spi/spi_transfer.h>

#include <arch/board/board.h>

#include "rk3576_spi.h"
#include "kickpi_k7.h"

#ifdef CONFIG_RK3576_SPI

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct spi_dev_s *g_spi4;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_spi_initialize
 ****************************************************************************/

int kickpi_k7_spi_initialize(void)
{
  int ret;

  g_spi4 = rk3576_spibus_initialize(4);
  if (g_spi4 == NULL)
    {
      syslog(LOG_ERR, "ERROR: SPI4 初始化失败\n");
      return -ENODEV;
    }

#ifdef CONFIG_SPI_DRIVER
  ret = spi_register(g_spi4, 4);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: /dev/spi4 注册失败: %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO, "SPI4 -> /dev/spi4（40 针扩展口）\n");
#else
  UNUSED(ret);
#endif

  return OK;
}

/****************************************************************************
 * Name: kickpi_spi_loopback
 *
 * Description:
 *   MOSI/MISO 短接回环自检。用 40 针口的 19 脚(MOSI)与 21 脚(MISO)
 *   短接后调用。
 *
 *   发一组互不相同、且不等于 0xff/0x00 的图案 —— 悬空的 MISO 会稳定
 *   读出 0xff（内部上拉）或 0x00，用固定图案分不出"回环通了"和
 *   "线根本没接"。
 *
 * Returned Value:
 *   OK 表示每一字节都原样回来了；-EIO 表示有不符，并打出首个不符处。
 *
 ****************************************************************************/

int kickpi_spi_loopback(void)
{
  static const uint8_t pattern[] =
  {
    0x5a, 0xa5, 0x01, 0x80, 0x3c, 0xc3, 0x0f, 0xf0,
    0x55, 0xaa, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc
  };

  uint8_t rx[sizeof(pattern)];
  int i;

  if (g_spi4 == NULL)
    {
      syslog(LOG_ERR, "SPI4 还没初始化\n");
      return -ENODEV;
    }

  memset(rx, 0, sizeof(rx));

  SPI_LOCK(g_spi4, true);
  SPI_SETMODE(g_spi4, SPIDEV_MODE0);
  SPI_SETBITS(g_spi4, 8);
  SPI_SETFREQUENCY(g_spi4, 1000000);
  SPI_SELECT(g_spi4, SPIDEV_USER(0), true);
  SPI_EXCHANGE(g_spi4, pattern, rx, sizeof(pattern));
  SPI_SELECT(g_spi4, SPIDEV_USER(0), false);
  SPI_LOCK(g_spi4, false);

  for (i = 0; i < sizeof(pattern); i++)
    {
      if (rx[i] != pattern[i])
        {
          syslog(LOG_ERR,
                 "SPI4 回环失败：第 %d 字节 发 0x%02x 收 0x%02x\n",
                 i, pattern[i], rx[i]);
          return -EIO;
        }
    }

  syslog(LOG_INFO, "SPI4 回环通过：%d 字节全部原样返回\n",
         (int)sizeof(pattern));
  return OK;
}

#endif /* CONFIG_RK3576_SPI */
