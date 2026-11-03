############################################################################
# Contest 2026 team 423 lxroot - Makefile
############################################################################

include $(APPDIR)/Make.defs

# 镜像格式（amp/include/k7_lxroot.h），U-Boot 侧也按它读
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../contest2026_423_nanshannan/amp/include

PROGNAME  = lxroot
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = 16384
MODULE    = $(CONFIG_LVX_USE_DEMO_CONTEST2026_423_LXROOT)

MAINSRC = lxroot_main.c

include $(APPDIR)/Application.mk
