############################################################################
# Contest 2026 team 423 k7ota - Makefile
############################################################################

include $(APPDIR)/Make.defs

# 控制块格式（amp/include/k7_bootab.h），U-Boot 侧也按它读
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../contest2026_423_nanshannan/amp/include

PROGNAME  = ota bootctl
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = 16384 4096
MODULE    = $(CONFIG_LVX_USE_DEMO_CONTEST2026_423_K7OTA)

MAINSRC = ota_main.c bootctl_main.c
CSRCS   = k7ab.c

include $(APPDIR)/Application.mk
