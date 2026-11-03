############################################################################
# vendor/rockchip/boards/rk3576/kickpi-k7/src/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.  The
# ASF licenses this file to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance with the
# License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations
# under the License.
#
############################################################################

include $(TOPDIR)/Make.defs

CSRCS = kickpi_k7_boardinit.c
CSRCS += kickpi_k7_appinit.c

ifeq ($(CONFIG_RK3576_SKW6621S),y)
CSRCS += kickpi_k7_skw6621s.c
endif

ifeq ($(CONFIG_USBHOST_XHCI_PLATFORM),y)
CSRCS += kickpi_k7_usbhost.c
endif

ifeq ($(CONFIG_RK3576_IR),y)
CSRCS += kickpi_k7_ir.c
endif

ifeq ($(CONFIG_DEV_GPIO),y)
CSRCS += kickpi_k7_gpio.c

ifeq ($(CONFIG_BLUETOOTH_BCM4343X),y)
CSRCS += kickpi_k7_bt.c kickpi_k7_bt_firmware.c
endif
endif

ifeq ($(CONFIG_RK3576_I2C),y)
CSRCS += kickpi_k7_i2c.c
endif

ifeq ($(CONFIG_KICKPI_K7_MPU6050),y)
CSRCS += kickpi_k7_mpu6050.c
endif

ifeq ($(CONFIG_RK3576_SPI),y)
CSRCS += kickpi_k7_spi.c
endif

# 屏与触摸共用同一条电源轨，任一方启用就需要这个文件
ifneq ($(CONFIG_INPUT_FT5X06)$(CONFIG_RK3576_VOP2),)
CSRCS += kickpi_k7_lcd.c
endif

ifeq ($(CONFIG_INPUT_FT5X06),y)
CSRCS += kickpi_k7_touch.c
endif

ifeq ($(CONFIG_AUDIO_ES8388),y)
CSRCS += kickpi_k7_audio.c
endif

# 网络地址配置由 gmac 后台线程调 netinit_bringup()（见 kickpi_k7_appinit.c），
# 头文件在 apps 侧。
ifeq ($(CONFIG_NETUTILS_NETINIT),y)
CFLAGS += ${INCDIR_PREFIX}$(TOPDIR)/../apps/include
endif

# 摄像头只依赖 I2C（传感器时钟来自板上晶振，供电是普通 GPIO）
ifeq ($(CONFIG_RK3576_I2C),y)
CSRCS += kickpi_k7_camera.c

ifeq ($(CONFIG_KICKPI_K7_IMGPROC),y)
CSRCS += kickpi_k7_imgproc.c
# libjpeg-turbo 的头文件没有导出到公共头路径（树里没有别的代码用它），
# 这里显式加上。jconfig.h 是它构建时生成的，也在同一目录。
# ★ 这里是 nuttx 侧的 Makefile，APPDIR 不一定有定义，用 TOPDIR 走过去。
CFLAGS += ${INCDIR_PREFIX}$(TOPDIR)/../apps/external/libjpeg-turbo/libjpeg-turbo
endif

ifeq ($(CONFIG_RK3576_VIDEO),y)
CSRCS += kickpi_k7_video.c
endif
endif

include $(TOPDIR)/boards/Board.mk
