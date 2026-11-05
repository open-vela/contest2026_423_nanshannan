/****************************************************************************
 * chip/rk3576/rk3576_sdhci.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_SDHCI_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_SDHCI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/sdio.h>

/****************************************************************************
 * Name: rk3576_sdhci_probe
 *
 * Description:
 *   走 eMMC 上电识别序列（CMD0/CMD1/CMD2/CMD3）验证命令通路，
 *   把 OCR、CID、RCA 打到 syslog。完整的 sdio_dev_s 实现在此之后补。
 *
 ****************************************************************************/

int rk3576_sdhci_probe(void);

/****************************************************************************
 * Name: rk3576_sdhci_initialize
 *
 * Description:
 *   返回 sdio_dev_s 句柄，供 mmcsd_slotinitialize() 注册 /dev/mmcsdN。
 *
 * Input Parameters:
 *   slotno - 槽位号，本 SoC 只有一个 eMMC，取 0。
 *
 ****************************************************************************/

struct sdio_dev_s *rk3576_sdhci_initialize(int slotno);

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_SDHCI_H */
