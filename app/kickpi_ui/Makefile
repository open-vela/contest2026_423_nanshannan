############################################################################
# packages/demos/contest2026_423_kickpi_ui/Makefile
############################################################################

include $(APPDIR)/Make.defs

CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/external/libjpeg-turbo/libjpeg-turbo
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../packages/ai_agent/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/netutils/cjson/cJSON
# 与 Linux 共用的帧共享区布局
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/../contest2026_423_nanshannan/amp/include

PROGNAME  = kickpi_ui
PRIORITY  = $(CONFIG_LVX_DEMO_CONTEST2026_423_KICKPI_UI_PRIORITY)
STACKSIZE = $(CONFIG_LVX_DEMO_CONTEST2026_423_KICKPI_UI_STACKSIZE)
MODULE    = $(CONFIG_LVX_USE_DEMO_CONTEST2026_423_KICKPI_UI)

MAINSRC = kickpi_ui_main.c

# 中文字库（scripts/gen-cjk-font.sh 生成，约 430KB）
CSRCS += lv_font_k7_cjk_20.c
CSRCS += lv_font_k7_digits_72.c
CSRCS += lv_font_k7_digits_48.c
CSRCS += lv_font_k7_ui_28.c
CSRCS += lv_font_k7_icons_32.c
CSRCS += lv_font_k7_icons_64.c
CSRCS += k7_audio.c
CSRCS += k7_tts.c
CSRCS += k7_llm.c
CSRCS += k7_vision.c
CSRCS += k7_wake.c
CSRCS += k7_home.c
CSRCS += k7_studio.c
CSRCS += k7_memo.c

include $(APPDIR)/Application.mk
