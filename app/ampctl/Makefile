############################################################################
# Contest 2026 team 423 ampctl - Makefile
############################################################################

include $(APPDIR)/Make.defs

# 与 Linux 共用的帧共享区布局（amp/include/k7_frame.h）
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../contest2026_423_nanshannan/amp/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/external/libjpeg-turbo/libjpeg-turbo

PROGNAME  = ampctl
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = 8192
MODULE    = $(CONFIG_LVX_USE_DEMO_CONTEST2026_423_AMPCTL)

MAINSRC = ampctl_main.c

include $(APPDIR)/Application.mk
