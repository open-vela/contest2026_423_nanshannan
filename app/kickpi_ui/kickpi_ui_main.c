/****************************************************************************
 * packages/demos/contest2026_423_kickpi_ui/kickpi_ui_main.c
 *
 * KICKPI-K7 板载仪表盘 —— 把这块 BSP 上跑着的东西直接画到屏上。
 *
 *   VOP2 帧缓冲 /dev/fb0    ->  LVGL 显示
 *   FT8756 触摸 /dev/input0 ->  LVGL 输入
 *
 * 主界面是底部四个页签（规划见 docs/ui-product-plan.md）：
 *
 *   今日     时钟、久坐计时、喝水记录。提醒按计时触发，到点弹全屏卡片
 *            （用眼休息只出顶部横幅）。
 *   家居     家庭网关状态。没接网关时只说明情况，不摆假设备。
 *   助手     AI 问答、看桌面、语音唤醒与播报。
 *   设置     提醒开关与间隔、音量、开发者工具入口。
 *
 * 开发者工具里是原来的工程页，原样保留：
 *
 *   双核     数字全部来自 rk3576_rptun_getstat()，也就是 openvela 这一侧
 *            rptun 驱动自己的计数器，不是写死的样例数据。带门铃自检按钮。
 *   设备     设备节点在不在。能安全自检的那几个给按钮，危险的明确不给
 *            （理由写在 g_devices[] 旁边）。
 *   曲线     堆和门铃速率两条曲线，250ms 一格，**只在这一页可见时刷新**。
 *   音视频   录音机、桌面摄像头。
 *   关于     分辨率、构建信息，以及触摸圆点 —— 它一个动作同时证明显示
 *            和输入两条链路，留着。
 *
 * ★ 中文字库
 *
 *   LVGL 内置的 lv_font_simsun_16_cjk 缺很多简体常用字（实测「双核 设
 *   门铃 检 握」都不在），所以用 scripts/gen-cjk-font.sh 自己生成的
 *   lv_font_k7_cjk_20（ASCII + GB2312 一级汉字）。界面文字改完跑
 *   scripts/check-ui-glyphs.py 查缺字。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <dirent.h>
#include <inttypes.h>
#include <math.h>
#include <malloc.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <semaphore.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>
#include <nuttx/input/touchscreen.h>
#include <nuttx/video/fb.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include <lvgl/lvgl.h>
#include <lvgl/src/drivers/nuttx/lv_nuttx_entry.h>
#include <jpeglib.h>
#include <cJSON.h>
#include <velaclaw/client.h>

#ifdef CONFIG_NETUTILS_NTPCLIENT
/* jmorecfg.h（jpeglib）定义了 EXTERN(type)，ntpclient.h 又不先 undef 就
 * 重定义，-Werror 下报错。后面不再用 jpeglib 的那个宏。
 */
#  undef EXTERN
#  include <netutils/ntpclient.h>
#endif

#include "k7_audio.h"
#include "k7_tts.h"
#include "k7_vision.h"
#include "k7_wake.h"
#include "k7_home.h"
#include "k7_studio.h"
#include "k7_memo.h"
#include "k7_wireless.h"

#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_423_K7OTA
#  include "k7ab.h"
#endif

#include <arch/board/board.h>

#ifdef CONFIG_RK3576_FRAME_SHM
#  include <nuttx/cache.h>
#  include "k7_frame.h"
#endif

#ifdef CONFIG_RK3576_RPTUN
#  include <arch/chip/amp.h>
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 屏幕方向。面板本身是 720x1280 竖屏（DSI 时序、帧缓冲都按竖的走），
 * 界面横过来按 1280x720 排：LVGL 在逻辑坐标里画，kickpi_flush_cb 转到
 * 面板坐标再写帧缓冲；触摸由 LVGL 按同一方向从面板坐标转回逻辑坐标
 * （lv_indev.c indev_pointer_proc）。
 *
 * 只能取 0 / 90 / 270。90 和 270 的差别是装屏时 FPC 排线在哪一侧 ——
 * 画面倒了就换另一个，其余代码都按 ui_to_panel() 走，不用动。
 *
 * 不用 VOP2 硬件旋转：RK3576 只有 Cluster 图层能转 90/270，而且要 AFBC
 * 格式，等于重写显示驱动。软件转一次是逐像素拷贝，与原来的 memcpy 同量级。
 */

#define UI_ROTATE 90

#define UI_BG        0x0e1116
#define UI_CARD      0x181d25
#define UI_ACCENT    0x4fc3f7
#define UI_OK        0x66bb6a
#define UI_BAD       0xef5350
#define UI_DIM       0x8a96a3

/* 产品界面配色：每类提醒一个主色，全界面统一用它（图标、圆环、按钮） */

#define UI_SURFACE2       0x242b36       /* 卡片里的次级底色 */
#define UI_TEXT           0xe8edf2
#define UI_SIT            0x2ec4a6
#define UI_SIT_OVER       0xff8a65
#define UI_WATER          0x4fa8ff
#define UI_EYE            0xb39dff
#define UI_ON_COLOR       0x0e1116       /* 彩色按钮上的字 */

/* 图标（lv_font_k7_icons_*，FontAwesome 5 码位的 UTF-8）。拼字符串时
 * 图标单独成一个字面量，免得 \x 把后面的十六进制字符也吃进去。
 */

#define ICON_SUN          "\xEF\x86\x85"
#define ICON_HOME         "\xEF\x80\x95"
#define ICON_MIC          "\xEF\x84\xB0"
#define ICON_COG          "\xEF\x80\x93"
#define ICON_CHAIR        "\xEF\x9B\x80"
#define ICON_TINT         "\xEF\x81\x83"
#define ICON_EYE          "\xEF\x81\xAE"
#define ICON_RIGHT        "\xEF\x81\x94"
#define ICON_DOOR         "\xEF\x94\xAB"
#define ICON_LEAVE        "\xEF\x8B\xB5"
#define ICON_MOON         "\xEF\x86\x86"
#define ICON_BRAIN        "\xEF\x97\x9C"
#define ICON_CODE         "\xEF\x84\xA1"
#define ICON_VOLUME       "\xEF\x80\xA8"
#define ICON_BELL         "\xEF\x83\xB3"
#define ICON_UNLINK       "\xEF\x84\xA7"
#define ICON_BACK         "\xEF\x81\x93"
#define ICON_BULB         "\xEF\x83\xAB"
#define ICON_SNOW         "\xEF\x8B\x9C"
#define ICON_WIND         "\xEF\x9C\xAE"
#define ICON_PLUG         "\xEF\x87\xA6"
#define ICON_TV           "\xEF\x89\xAC"
#define ICON_WIFI         "\xEF\x87\xAB"
#define ICON_VIDEO        "\xEF\x80\xBD"
#define ICON_NOTE         "\xEF\x89\x89"
#define ICON_BLUETOOTH    "\xEF\x8A\x94"
#define ICON_LOCK         "\xEF\x80\xA3"

#define CHART_POINTS 60          /* 60 x 250ms = 15 秒窗口 */
#define TICK_MS      250

/* 门铃自检借用的 group。必须是 rptun **没占用**的那个，否则自检会打乱
 * 正在跑的握手状态机。rptun 用 group3（收）/group0（发），5 是空的。
 */

#define SELFTEST_GROUP 5

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* 中文字库（lv_font_k7_cjk_20.c，scripts/gen-cjk-font.sh 生成） */

LV_FONT_DECLARE(lv_font_k7_cjk_20);

/* 首页时钟的大号数字（lv_font_k7_digits_72.c，scripts/gen-clock-font.sh） */

LV_FONT_DECLARE(lv_font_k7_digits_72);

/* 产品界面的标题字（只含界面用到的字，scripts/gen-ui-font.sh）、卡片里的
 * 大数字（gen-clock-font.sh）、图标（FontAwesome 子集，gen-icon-font.sh）
 */

LV_FONT_DECLARE(lv_font_k7_ui_28);
LV_FONT_DECLARE(lv_font_k7_digits_48);
LV_FONT_DECLARE(lv_font_k7_icons_32);
LV_FONT_DECLARE(lv_font_k7_icons_64);

struct devrow_s
{
  const char *label;
  const char *path;
  const char *off;       /* 非 NULL：这一项是配置里有意没开的（或可选的），
                          * 不在时显示灰色的这句原因，而不是红色 ×
                          */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 设备清单。
 *
 * ★ 为什么没有"点一下就跑 xTS 自检"
 *
 *   这块板上有两组已知会把机器带走的用例，都记在 docs/ 里：
 *     - cmocka_driver_block 从 0 扇区开始写，对着 /dev/mmcsd0 跑等于毁
 *       eMMC 上的 GPT 和 bootloader；
 *     - xTS 1.3.7 的 spidev/i2cdev 用例会把板子挂死，只能断电恢复。
 *   仪表盘上一个误触就触发这种事，代价和收益完全不成比例。所以这里只
 *   展示"节点在不在"，真正的自检留在 nsh 里手工跑 —— 除了 mailbox 那个，
 *   它是纯软件回环，不碰任何存储和外部总线。
 */

static const struct devrow_s g_devices[] =
{
  { "显示",           "/dev/fb0",          NULL },
  { "触摸",           "/dev/input0",       NULL },
  { "摄像头",         "/dev/video0",       NULL },
  { "放音",           "/dev/audio/pcm0",   NULL },
  { "录音",           "/dev/audio/pcm1",   NULL },
  { "Linux 通道",     "/dev/rpmsg/linux",  NULL },
  { "网络 telnet",    "/dev/telnet",       NULL },
  { "RTC",            "/dev/rtc0",         NULL },
  { "看门狗",         "/dev/watchdog0",    NULL },
  { "随机数",         "/dev/random",       NULL },
  { "I2C3 音频",      "/dev/i2c3",         NULL },
  { "SPI4",           "/dev/spi4",         NULL },
  { "GPIO",           "/dev/gpio2",        NULL },
  { "TF 卡",          "/dev/mmcsd1",       "没插卡" },

  /* 双系统打通阶段把 eMMC 驱动关了（提交 83b8f3c），之后没加回来：
   * 它用中断，要在 Linux 的 DTB 里声明成 openvela 的；而且 eMMC 上就是
   * U-Boot、Linux 内核和本镜像，不值得在截止前冒险。单系统配置里是开的。
   */

  { "eMMC",           "/dev/mmcsd0",       "双系统下未启用" },
};

static lv_obj_t  *g_tabview;
static lv_obj_t  *g_tab_live;

/* AMP 页 */

static lv_obj_t  *g_amp_badges[3];
static lv_obj_t  *g_amp_values[5];
static lv_obj_t  *g_amp_selftest;

/* DEVICES 页 */

static lv_obj_t  *g_dev_marks[sizeof(g_devices) / sizeof(g_devices[0])];
static lv_obj_t  *g_dev_hint;

/* LIVE 页 */

static lv_obj_t        *g_chart_heap;
static lv_chart_series_t *g_ser_heap;
static lv_obj_t        *g_chart_kick;
static lv_chart_series_t *g_ser_kick;
static lv_obj_t        *g_live_stats;

/* ABOUT 页 */

static lv_obj_t  *g_dot;
static lv_obj_t  *g_touch_label;
static unsigned   g_touch_count;

static uint32_t   g_last_kicks;
static time_t     g_t0;

/* Camera snapshots are captured by the same V4L2 tool used to verify the
 * agent camera path.  Only the UI thread touches LVGL objects.
 */

#define UI_CAMERA_FILE "/tmp/k7-ui-camera.jpg"

/* 拍照程序。摄像头归 Linux 时 openvela 没有 /dev/video0，改由
 * `ampctl snap` 从帧共享区取一帧存 JPEG，参数和 v4l2cap 相同。
 */

#ifdef CONFIG_KICKPI_K7_CAMERA_LINUX
#  define UI_SNAP_ARGV(file) { "ampctl", "snap", "1280", "720", file, NULL }
#  define UI_SNAP_FAIL       "拍照失败，看串口上 ampctl snap 的输出"
#else
#  define UI_SNAP_ARGV(file) { "v4l2cap", "1280", "720", file, NULL }
#  define UI_SNAP_FAIL       "拍照失败，检查 /dev/video0"
#endif
#define UI_AGENT_TIMEOUT_S 240
static lv_obj_t     *g_camera_image;
static lv_obj_t     *g_camera_status;
static lv_obj_t     *g_camera_button;
static lv_draw_buf_t *g_camera_frame;
static pid_t         g_camera_pid = -1;
static lv_obj_t     *g_assist_list;
static lv_obj_t     *g_assist_state;
static lv_obj_t     *g_assist_spinner;
static pthread_mutex_t g_agent_lock = PTHREAD_MUTEX_INITIALIZER;
static char          g_agent_reply[2048];
static bool          g_agent_reply_ready;
static bool          g_agent_busy;
static time_t        g_agent_started;
static lv_obj_t     *g_guard_switch;
static lv_obj_t     *g_guard_state;
static bool          g_guard_enabled;
static bool          g_guard_request;
static time_t        g_guard_next;
static int           g_guard_keys = -1;
static int           g_guard_cup = -1;

struct ui_jpeg_error_s
{
  struct jpeg_error_mgr pub;
  jmp_buf jump;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool path_exists(const char *path)
{
  struct stat st;
  return stat(path, &st) == 0;
}

static lv_obj_t *card_create(lv_obj_t *parent, const char *title)
{
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_t *lbl;

  lv_obj_set_width(card, LV_PCT(100));
  lv_obj_set_height(card, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(card, lv_color_hex(UI_CARD), 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_radius(card, 8, 0);
  lv_obj_set_style_pad_all(card, 10, 0);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card, 4, 0);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  if (title != NULL)
    {
      lbl = lv_label_create(card);
      lv_label_set_text(lbl, title);
      lv_obj_set_style_text_color(lbl, lv_color_hex(UI_ACCENT), 0);
    }

  return card;
}

/* 一行 "名字 ....... 值"，返回值那一半，方便之后 set_text。 */

static lv_obj_t *kv_create(lv_obj_t *parent, const char *key)
{
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_t *k;
  lv_obj_t *v;

  lv_obj_remove_style_all(row);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  k = lv_label_create(row);
  lv_label_set_text(k, key);
  lv_obj_set_style_text_color(k, lv_color_hex(UI_DIM), 0);

  v = lv_label_create(row);
  lv_label_set_text(v, "-");
  lv_obj_set_style_text_color(v, lv_color_white(), 0);

  return v;
}

static void badge_set(lv_obj_t *badge, const char *text, bool good)
{
  lv_label_set_text(badge, text);
  lv_obj_set_style_bg_color(badge,
                            lv_color_hex(good ? UI_OK : UI_BAD), 0);
}

static lv_obj_t *badge_create(lv_obj_t *parent)
{
  lv_obj_t *b = lv_label_create(parent);

  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(UI_BAD), 0);
  lv_obj_set_style_text_color(b, lv_color_black(), 0);
  lv_obj_set_style_radius(b, 4, 0);
  lv_obj_set_style_pad_all(b, 4, 0);
  lv_label_set_text(b, "?");

  return b;
}

/****************************************************************************
 * AMP 页
 ****************************************************************************/

#ifdef CONFIG_RK3576_RPTUN
static void selftest_cb(lv_event_t *e)
{
  uint32_t cmd = 0;
  uint32_t data = 0;
  int pos;
  int neg;

  (void)e;

  lv_label_set_text(g_amp_selftest, "自检中…");
  lv_refr_now(NULL);

  /* 正例：自己给自己按门铃，必须收到原样的 cmd/data。 */

  pos = rk3576_mailbox_selftest(SELFTEST_GROUP, 0x5a5a5a5a, 0xa5a5a5a5,
                                &cmd, &data);

  /* 反例：只写 CMD 不触发，必须超时。
   *
   * 没有这一条，正例通过也说明不了什么 —— 如果读到的是别处留下的残留
   * 状态，正例一样"通过"。反例失败（也就是居然收到了）才是真问题。
   */

  neg = rk3576_mailbox_selftest_notrigger(SELFTEST_GROUP, NULL, NULL);

  if (pos == 0 && cmd == 0x5a5a5a5a && data == 0xa5a5a5a5 && neg != 0)
    {
      lv_label_set_text_fmt(g_amp_selftest,
                            LV_SYMBOL_OK " 通过  收到 %08" PRIx32
                            "/%08" PRIx32 "，不触发时如期超时",
                            cmd, data);
      lv_obj_set_style_text_color(g_amp_selftest, lv_color_hex(UI_OK), 0);
    }
  else
    {
      lv_label_set_text_fmt(g_amp_selftest,
                            LV_SYMBOL_CLOSE " 失败  正例=%d 收到 %08" PRIx32
                            "/%08" PRIx32 "  反例=%d", pos, cmd, data, neg);
      lv_obj_set_style_text_color(g_amp_selftest, lv_color_hex(UI_BAD), 0);
    }
}
#endif

static void amp_build(lv_obj_t *tab)
{
  lv_obj_t *card;
  lv_obj_t *row;
  int i;

  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(tab, 8, 0);

  card = card_create(tab, "openvela（A53 ×4） ← → Linux（A72 ×4）");

  row = lv_obj_create(card);
  lv_obj_remove_style_all(row);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_column(row, 6, 0);

  for (i = 0; i < 3; i++)
    {
      g_amp_badges[i] = badge_create(row);
    }

  card = card_create(tab, "门铃（mailbox）");
  g_amp_values[0] = kv_create(card, "收到门铃");
  g_amp_values[1] = kv_create(card, "发出门铃");
  g_amp_values[2] = kv_create(card, "发送被挡回");
  g_amp_values[3] = kv_create(card, "最后 cmd");
  g_amp_values[4] = kv_create(card, "最后 data");

  card = card_create(tab, "共享内存（须与 Linux 设备树一致）");
#ifdef CONFIG_RK3576_RPTUN
  lv_label_set_text_fmt(kv_create(card, "vring0 → Linux"),
                        "0x%08lx  %ldK",
                        (unsigned long)RK3576_RPTUN_VRING0_DA,
                        (long)RK3576_RPTUN_VRING_SIZE / 1024);
  lv_label_set_text_fmt(kv_create(card, "vring1 → openvela"),
                        "0x%08lx  %ldK",
                        (unsigned long)RK3576_RPTUN_VRING1_DA,
                        (long)RK3576_RPTUN_VRING_SIZE / 1024);
  lv_label_set_text_fmt(kv_create(card, "缓冲池"),
                        "0x%08lx  %ldK",
                        (unsigned long)RK3576_RPTUN_POOL_DA,
                        (long)RK3576_RPTUN_POOL_LEN / 1024);
#else
  lv_label_set_text(kv_create(card, "rptun"), "未编入");
#endif

  card = card_create(tab, "门铃自检（group 5，本端回环）");
  g_amp_selftest = lv_label_create(card);
  lv_label_set_text(g_amp_selftest, "未运行");
  lv_obj_set_style_text_color(g_amp_selftest, lv_color_hex(UI_DIM), 0);
  lv_label_set_long_mode(g_amp_selftest, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(g_amp_selftest, LV_PCT(100));

#ifdef CONFIG_RK3576_RPTUN
  {
    lv_obj_t *btn = lv_button_create(card);
    lv_obj_t *lbl = lv_label_create(btn);

    lv_label_set_text(lbl, LV_SYMBOL_PLAY "  运行自检");
    lv_obj_add_event_cb(btn, selftest_cb, LV_EVENT_CLICKED, NULL);
  }
#endif
}

static void amp_refresh(void)
{
#ifdef CONFIG_RK3576_RPTUN
  struct rk3576_rptun_stat_s st;

  if (rk3576_rptun_getstat(&st) < 0)
    {
      badge_set(g_amp_badges[0], "rptun 状态未知", false);
      return;
    }

  badge_set(g_amp_badges[0],
            st.registered ? "rptun 已注册" : "rptun 未注册", st.registered);
  badge_set(g_amp_badges[1],
            st.driver_ok ? "握手完成" : "未握手", st.driver_ok);
  badge_set(g_amp_badges[2],
            path_exists("/dev/rpmsg/linux") ? "rpmsg 通道就绪"
                                            : "无 rpmsg 节点",
            path_exists("/dev/rpmsg/linux"));

  lv_label_set_text_fmt(g_amp_values[0], "%" PRIu32, st.kicks_rx);
  lv_label_set_text_fmt(g_amp_values[1], "%" PRIu32, st.kicks_tx);
  lv_label_set_text_fmt(g_amp_values[2], "%" PRIu32, st.tx_busy);
  lv_label_set_text_fmt(g_amp_values[3], "%08" PRIx32, st.last_cmd);
  lv_label_set_text_fmt(g_amp_values[4], "%08" PRIx32, st.last_data);
#else
  badge_set(g_amp_badges[0], "rptun 未编入", false);
#endif
}

/****************************************************************************
 * DEVICES 页
 ****************************************************************************/

static void dev_refresh(void)
{
  int ok_n = 0;
  int off_n = 0;
  int bad_n = 0;
  size_t i;

  for (i = 0; i < sizeof(g_devices) / sizeof(g_devices[0]); i++)
    {
      bool ok = path_exists(g_devices[i].path);
      uint32_t color;

      if (ok)
        {
          /* 这一列用的是中文字库，没有 LVGL 的 FontAwesome 图标
           * （LV_SYMBOL_OK 显示成方框），用字库里有的 ✓ ✗
           */

          lv_label_set_text(g_dev_marks[i], "✓");
          color = UI_OK;
          ok_n++;
        }
      else if (g_devices[i].off != NULL)
        {
          lv_label_set_text(g_dev_marks[i], g_devices[i].off);
          color = UI_DIM;
          off_n++;
        }
      else
        {
          lv_label_set_text(g_dev_marks[i], "✗");
          color = UI_BAD;
          bad_n++;
        }

      lv_obj_set_style_text_color(g_dev_marks[i], lv_color_hex(color), 0);
    }

  if (bad_n > 0)
    {
      lv_label_set_text_fmt(g_dev_hint, "%d 项就绪 · %d 项未启用 · %d 项缺失",
                            ok_n, off_n, bad_n);
    }
  else
    {
      lv_label_set_text_fmt(g_dev_hint, "%d 项就绪 · %d 项未启用",
                            ok_n, off_n);
    }
}

static void dev_refresh_cb(lv_event_t *e)
{
  (void)e;
  dev_refresh();
}

static void dev_build(lv_obj_t *tab)
{
  lv_obj_t *card;
  lv_obj_t *btn;
  lv_obj_t *lbl;
  size_t i;

  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(tab, 8, 0);

  card = card_create(tab, NULL);
  lbl = lv_label_create(card);
  lv_obj_set_style_text_font(lbl, &lv_font_k7_cjk_20, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_ACCENT), 0);
  lv_label_set_text(lbl, "设备节点");

  for (i = 0; i < sizeof(g_devices) / sizeof(g_devices[0]); i++)
    {
      lv_obj_t *row = lv_obj_create(card);
      lv_obj_t *name;
      lv_obj_t *path;

      lv_obj_remove_style_all(row);
      lv_obj_set_width(row, LV_PCT(100));
      lv_obj_set_height(row, LV_SIZE_CONTENT);
      lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(row, 10, 0);

      name = lv_label_create(row);
      lv_obj_set_style_text_font(name, &lv_font_k7_cjk_20, 0);
      lv_obj_set_style_text_color(name, lv_color_white(), 0);
      lv_obj_set_width(name, 150);
      lv_label_set_text(name, g_devices[i].label);

      path = lv_label_create(row);
      lv_obj_set_style_text_color(path, lv_color_hex(UI_DIM), 0);
      lv_obj_set_flex_grow(path, 1);
      lv_label_set_text(path, g_devices[i].path);

      g_dev_marks[i] = lv_label_create(row);
      lv_obj_set_style_text_font(g_dev_marks[i], &lv_font_k7_cjk_20, 0);
    }

  g_dev_hint = lv_label_create(card);
  lv_obj_set_style_text_font(g_dev_hint, &lv_font_k7_cjk_20, 0);
  lv_obj_set_style_text_color(g_dev_hint, lv_color_hex(UI_DIM), 0);

  btn = lv_button_create(card);
  lv_obj_add_event_cb(btn, dev_refresh_cb, LV_EVENT_CLICKED, NULL);
  lbl = lv_label_create(btn);
  lv_obj_set_style_text_font(lbl, &lv_font_k7_cjk_20, 0);
  lv_label_set_text(lbl, "重新扫描");

  dev_refresh();
}

/****************************************************************************
 * LIVE 页
 ****************************************************************************/

static int task_count(void)
{
  DIR *d = opendir("/proc");
  struct dirent *ent;
  int n = 0;

  if (d == NULL)
    {
      return -1;
    }

  while ((ent = readdir(d)) != NULL)
    {
      if (ent->d_name[0] >= '0' && ent->d_name[0] <= '9')
        {
          n++;
        }
    }

  closedir(d);
  return n;
}

static void live_build(lv_obj_t *tab)
{
  lv_obj_t *card;

  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(tab, 8, 0);

  card = card_create(tab, "空闲堆（KiB）");
  g_chart_heap = lv_chart_create(card);
  lv_obj_set_width(g_chart_heap, LV_PCT(100));
  lv_obj_set_height(g_chart_heap, 120);
  lv_chart_set_point_count(g_chart_heap, CHART_POINTS);
  lv_chart_set_update_mode(g_chart_heap, LV_CHART_UPDATE_MODE_SHIFT);
  lv_chart_set_div_line_count(g_chart_heap, 3, 0);
  g_ser_heap = lv_chart_add_series(g_chart_heap, lv_color_hex(UI_ACCENT),
                                   LV_CHART_AXIS_PRIMARY_Y);

  card = card_create(tab, "每秒门铃数");
  g_chart_kick = lv_chart_create(card);
  lv_obj_set_width(g_chart_kick, LV_PCT(100));
  lv_obj_set_height(g_chart_kick, 120);
  lv_chart_set_point_count(g_chart_kick, CHART_POINTS);
  lv_chart_set_update_mode(g_chart_kick, LV_CHART_UPDATE_MODE_SHIFT);
  lv_chart_set_div_line_count(g_chart_kick, 3, 0);
  lv_chart_set_range(g_chart_kick, LV_CHART_AXIS_PRIMARY_Y, 0, 20);
  g_ser_kick = lv_chart_add_series(g_chart_kick, lv_color_hex(UI_OK),
                                   LV_CHART_AXIS_PRIMARY_Y);

  card = card_create(tab, "计数");
  g_live_stats = lv_label_create(card);
  lv_obj_set_style_text_color(g_live_stats, lv_color_white(), 0);
  lv_label_set_text(g_live_stats, "-");
}

static void live_refresh(void)
{
  struct mallinfo mi = mallinfo();
  uint32_t kicks = 0;
  int32_t rate = 0;
  int tasks = task_count();

#ifdef CONFIG_RK3576_RPTUN
  struct rk3576_rptun_stat_s st;

  if (rk3576_rptun_getstat(&st) == 0)
    {
      kicks = st.kicks_rx + st.kicks_tx;
    }
#endif

  rate = (int32_t)(kicks - g_last_kicks) * (1000 / TICK_MS);
  g_last_kicks = kicks;

  lv_chart_set_next_value(g_chart_heap, g_ser_heap,
                          (int32_t)(mi.fordblks / 1024));
  lv_chart_set_next_value(g_chart_kick, g_ser_kick, rate);

  lv_label_set_text_fmt(g_live_stats,
                        "堆    已用 %u / %u KiB\n"
                        "任务  %d 个\n"
                        "运行  %lu 秒\n"
                        "门铃  共 %" PRIu32 " 次，%" PRId32 " 次/秒",
                        (unsigned)(mi.uordblks / 1024),
                        (unsigned)(mi.arena / 1024),
                        tasks,
                        (unsigned long)(time(NULL) - g_t0),
                        kicks, rate);
}

/****************************************************************************
 * CAMERA / DESK pages
 ****************************************************************************/

/****************************************************************************
 * 大图缓冲从系统堆分配
 *
 * ★ lv_draw_buf_create() 走 LVGL 自己的内存池，本配置只有 256KB
 *   （CONFIG_LV_MEM_SIZE_KILOBYTES）。一张 640x360 的 ARGB8888 就要
 *   921KB，必然分配失败 —— 表现是点"打开相机"没有任何反应、拍照后
 *   "JPEG decode failed"。系统堆还有几十 MB，图像数据放那边，
 *   只把描述结构交给 LVGL。
 ****************************************************************************/

static lv_draw_buf_t *ui_draw_buf_create(uint32_t w, uint32_t h,
                                         lv_color_format_t cf)
{
  lv_draw_buf_t *buf = calloc(1, sizeof(*buf));
  uint32_t stride = lv_draw_buf_width_to_stride(w, cf);
  size_t size = (size_t)stride * h;
  uint8_t *data = memalign(LV_DRAW_BUF_ALIGN, size);

  if (buf == NULL || data == NULL ||
      lv_draw_buf_init(buf, w, h, cf, stride,
                       data, size) != LV_RESULT_OK)
    {
      free(buf);
      free(data);
      syslog(LOG_ERR, "界面: 图像缓冲 %ux%u 分配失败\n",
             (unsigned)w, (unsigned)h);
      return NULL;
    }

  memset(data, 0, size);
  return buf;
}

static void ui_draw_buf_destroy(lv_draw_buf_t *buf)
{
  if (buf != NULL)
    {
      lv_image_cache_drop(buf);
      free(buf->data);
      free(buf);
    }
}

static void ui_jpeg_error(j_common_ptr info)
{
  struct ui_jpeg_error_s *err = (struct ui_jpeg_error_s *)info->err;
  longjmp(err->jump, 1);
}

static lv_draw_buf_t *camera_decode(const char *path)
{
  struct jpeg_decompress_struct jpeg;
  struct ui_jpeg_error_s error;
  lv_draw_buf_t *frame = NULL;
  FILE *file;
  uint8_t *row = NULL;
  bool created = false;
  unsigned int x;

  file = fopen(path, "rb");
  if (file == NULL)
    {
      return NULL;
    }

  memset(&jpeg, 0, sizeof(jpeg));
  jpeg.err = jpeg_std_error(&error.pub);
  error.pub.error_exit = ui_jpeg_error;
  if (setjmp(error.jump) != 0)
    {
      goto fail;
    }

  jpeg_create_decompress(&jpeg);
  created = true;
  jpeg_stdio_src(&jpeg, file);
  jpeg_read_header(&jpeg, TRUE);
  jpeg.scale_num = 1;
  jpeg.scale_denom = 2;
  jpeg.out_color_space = JCS_RGB;
  jpeg_start_decompress(&jpeg);

  if (jpeg.output_width == 0 || jpeg.output_height == 0 ||
      jpeg.output_width > 1280 || jpeg.output_height > 720 ||
      jpeg.output_components != 3)
    {
      goto fail;
    }

  frame = ui_draw_buf_create(jpeg.output_width, jpeg.output_height,
                             LV_COLOR_FORMAT_ARGB8888);
  row = malloc(jpeg.output_width * 3);
  if (frame == NULL || row == NULL)
    {
      goto fail;
    }

  while (jpeg.output_scanline < jpeg.output_height)
    {
      JSAMPROW scanline = row;
      uint32_t *pixels = (uint32_t *)
        lv_draw_buf_goto_xy(frame, 0, jpeg.output_scanline);

      if (jpeg_read_scanlines(&jpeg, &scanline, 1) != 1)
        {
          goto fail;
        }

      for (x = 0; x < jpeg.output_width; x++)
        {
          pixels[x] = 0xff000000u |
                      ((uint32_t)row[x * 3] << 16) |
                      ((uint32_t)row[x * 3 + 1] << 8) |
                      row[x * 3 + 2];
        }
    }

  jpeg_finish_decompress(&jpeg);
  jpeg_destroy_decompress(&jpeg);
  free(row);
  fclose(file);
  return frame;

fail:
  if (created)
    {
      jpeg_destroy_decompress(&jpeg);
    }

  if (frame != NULL)
    {
      ui_draw_buf_destroy(frame);
    }

  free(row);
  fclose(file);
  return NULL;
}

static void camera_start(void)
{
  char *argv[] = UI_SNAP_ARGV(UI_CAMERA_FILE);
  int ret;

  if (g_camera_pid > 0)
    {
      return;
    }

  unlink(UI_CAMERA_FILE);
  ret = posix_spawn(&g_camera_pid, argv[0], NULL, NULL, argv, NULL);
  if (ret != 0)
    {
      g_camera_pid = -1;
      lv_label_set_text_fmt(g_camera_status,
                            "Cannot start camera capture: %d", ret);
      return;
    }

  lv_obj_add_state(g_camera_button, LV_STATE_DISABLED);
  lv_label_set_text(g_camera_status, "正在拍照…");
}

/****************************************************************************
 * 相机页：全屏实时画面
 *
 * ★ 线程分工
 *
 *   取帧 + 缩放在后台线程里做（live_cam_frame 会阻塞到下一帧，
 *   约 33ms），写进 g_live_back；写完在锁里和 g_live_ready 交换指针。
 *   LVGL 只在自己的线程里动对象：定时器发现有新帧，就把 g_live_ready
 *   拷进图片用的 draw_buf 再 invalidate。这样 LVGL 渲染时读的那块内存
 *   永远不会被后台线程改写。
 *
 * ★ 和拍照、agent 互斥
 *
 *   预览、v4l2cap、agent 的 camera_capture 用的是同一个 CIF。拍照和问
 *   agent 之前都先关掉相机页（停流），板级那边也会对后来者返回 -EBUSY。
 ****************************************************************************/

/* 帧率诊断：各段耗时累计（微秒），相机页开着时每 3 秒打一行 */

static uint64_t g_st_grab_us;
static uint32_t g_st_grab_n;
static uint64_t g_st_loop_us;
static uint32_t g_st_loop_n;
static uint64_t g_st_flush_us;
static uint32_t g_st_flush_n;
static uint32_t g_st_frames_n;
static uint64_t g_st_t0;

/* 分段（2026-11-02 查相机页帧率）：整轮刷新、flush 里的拷贝、
 * 刷 cache + 翻页、等翻页 latch、定时器里拷画面。累计微秒。
 */

static uint64_t g_st_refr_us;
static uint32_t g_st_refr_n;
static uint64_t g_st_refr_t0;
static bool     g_st_refr_drew;   /* 这一轮真的 flush 过（空刷新不算） */
static uint64_t g_st_copy_us;
static uint64_t g_st_latch_us;
static uint64_t g_st_take_us;

static void kickpi_fb_dbuf(bool on);   /* 显示那一节，相机页开关时调 */

static uint64_t now_us(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000;
}

/* 预览尺寸。摄像头归 Linux 时整屏铺满（Linux 的 k7cam 直接出 1280x720，
 * 这里原样拷）；openvela 自己取图时仍是 640x360（板级缩放的上限）。
 */

#ifdef CONFIG_KICKPI_K7_CAMERA_LINUX
#  define LIVE_W      1280
#  define LIVE_H      720
#else
#  define LIVE_W      640
#  define LIVE_H      360
#endif
#define LIVE_TIMER_MS 30

static lv_obj_t      *g_live_page;
static lv_obj_t      *g_live_img;
static lv_obj_t      *g_live_info;
static lv_obj_t      *g_live_flash;
static lv_draw_buf_t *g_live_buf;
static lv_timer_t    *g_live_timer;
static uint32_t      *g_live_back;
static uint32_t      *g_live_ready;
static bool           g_live_new;
static volatile bool  g_live_run;
static bool           g_live_thread_ok;
static pthread_t      g_live_thread;
static pthread_mutex_t g_live_lock = PTHREAD_MUTEX_INITIALIZER;
static int            g_live_err;
static unsigned       g_live_frames;
static unsigned       g_live_shown;
static time_t         g_live_t0;

/* 预览的画面来源。
 *
 * 摄像头归 Linux（CONFIG_KICKPI_K7_CAMERA_LINUX）时，`ampctl cam start`
 * 让 Linux 的 k7cam live 持续把画面（720x404 XRGB，30fps）写进帧共享区，
 * 这里看帧头的 seq 有没有变，变了就缩到 LIVE_W x LIVE_H 拷走。读法按
 * k7_frame.h 的发布协议：先 invalidate，拷完再看一次 seq，期间绕回本槽
 * 就丢掉这一帧。
 *
 * k7cam live 最长跑 10 分钟、取帧出错也会退；8 秒没有新帧就再发一次
 * start（start 会先停掉还在跑的那个），相机页开多久都不会断。8 秒是因为
 * 冷启动要先起 rkaiq（k7aiq start 最多等 5 秒）再起 ISP 的流。
 */

#ifdef CONFIG_KICKPI_K7_CAMERA_LINUX
#define LXCAM_RESTART_MS 8000

static uint32_t g_lxcam_seq;
static uint64_t g_lxcam_last_us;

static void lxcam_spawn(const char *what)
{
  char *argv[] =
    {
      "ampctl", "cam", (char *)what, NULL
    };

  posix_spawn_file_actions_t fa;
  pid_t pid;
  int ret;

  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  ret = posix_spawn(&pid, "ampctl", &fa, NULL, argv, NULL);
  posix_spawn_file_actions_destroy(&fa);
  syslog(LOG_INFO, "界面: ampctl cam %s %d\n", what, ret);
}

static uint32_t lxcam_seq(void)
{
  uintptr_t base = CONFIG_RK3576_FRAME_SHM_BASE;

  up_invalidate_dcache(base, base + K7F_HDR_SIZE);
  return ((FAR volatile struct k7f_hdr_s *)base)->seq;
}

static int live_cam_start(void)
{
  g_lxcam_seq = lxcam_seq();
  g_lxcam_last_us = now_us();
  lxcam_spawn("start");
  return OK;
}

static void live_cam_stop(void)
{
  lxcam_spawn("stop");
}

static int live_cam_frame(uint32_t *dst, int w, int h, int timeout_ms)
{
  uintptr_t base = CONFIG_RK3576_FRAME_SHM_BASE;
  FAR const struct k7f_hdr_s *vh = (FAR const void *)base;
  struct k7f_hdr_s hd;
  const uint8_t *src;
  int waited;
  int x;
  int y;

  for (waited = 0; lxcam_seq() == g_lxcam_seq; waited += 10)
    {
      if (now_us() - g_lxcam_last_us > LXCAM_RESTART_MS * 1000ull)
        {
          g_lxcam_last_us = now_us();
          lxcam_spawn("start");
        }

      if (waited >= timeout_ms || !g_live_run)
        {
          return -ETIMEDOUT;
        }

      usleep(10 * 1000);
    }

  memcpy(&hd, vh, sizeof(hd));
  g_lxcam_seq = hd.seq;
  g_lxcam_last_us = now_us();

  if (hd.magic != K7F_MAGIC || hd.slot >= K7F_SLOTS ||
      hd.format != K7F_FMT_XRGB8888 || hd.width == 0 || hd.height == 0 ||
      hd.width > 4096 || hd.height > 4096 || hd.stride < hd.width * 4 ||
      hd.bytes != hd.stride * hd.height || hd.bytes > hd.slot_size ||
      (uint64_t)K7F_HDR_SIZE + (uint64_t)hd.slot_size * K7F_SLOTS >
      CONFIG_RK3576_FRAME_SHM_SIZE)
    {
      return -EINVAL;
    }

  src = (const uint8_t *)(base + k7f_slot_offset(&hd, hd.slot));
  up_invalidate_dcache((uintptr_t)src, (uintptr_t)src + hd.bytes);

  for (y = 0; y < h; y++)
    {
      const uint32_t *line = (const uint32_t *)
        (src + (size_t)(y * hd.height / h) * hd.stride);

      for (x = 0; x < w; x++)
        {
          dst[y * w + x] = line[x * hd.width / w];
        }
    }

  return lxcam_seq() - hd.seq >= K7F_SLOTS - 1 ? -ETIMEDOUT : OK;
}
#else
#  define live_cam_start kickpi_camera_live_start
#  define live_cam_stop  kickpi_camera_live_stop
#  define live_cam_frame kickpi_camera_live_frame
#endif

static void *live_thread(void *arg)
{
  (void)arg;

  while (g_live_run)
    {
      uint64_t t0 = now_us();
      int ret = live_cam_frame(g_live_back, LIVE_W, LIVE_H, 500);

      g_st_grab_us += now_us() - t0;
      g_st_grab_n++;

      pthread_mutex_lock(&g_live_lock);
      if (ret == OK)
        {
          uint32_t *t = g_live_ready;

          g_live_ready = g_live_back;
          g_live_back = t;
          g_live_new = true;
          g_live_frames++;
        }
      else
        {
          g_live_err = ret;
        }

      pthread_mutex_unlock(&g_live_lock);

      if (ret != OK && ret != -ETIMEDOUT)
        {
          break;
        }
    }

  return NULL;
}

static void live_timer_cb(lv_timer_t *t)
{
  bool fresh = false;
  int err;

  (void)t;

  pthread_mutex_lock(&g_live_lock);
  if (g_live_new)
    {
      uint64_t tc = now_us();

      memcpy(g_live_buf->data, g_live_ready,
             (size_t)LIVE_W * LIVE_H * 4);
      g_st_take_us += now_us() - tc;
      g_live_new = false;
      fresh = true;
    }

  err = g_live_err;
  g_live_err = 0;
  pthread_mutex_unlock(&g_live_lock);

  if (g_st_t0 == 0)
    {
      g_st_t0 = now_us();
    }
  else if (now_us() - g_st_t0 >= 3000000)
    {
      syslog(LOG_INFO, "界面: 3 秒内 取帧 %u 次 均 %u ms | 循环 %u 次 均 %u ms"
             " | 刷屏 %u 次 均 %u ms | 显示新帧 %u\n",
             g_st_grab_n,
             g_st_grab_n ? (unsigned)(g_st_grab_us / g_st_grab_n / 1000) : 0,
             g_st_loop_n,
             g_st_loop_n ? (unsigned)(g_st_loop_us / g_st_loop_n / 1000) : 0,
             g_st_flush_n,
             g_st_flush_n ? (unsigned)(g_st_flush_us / g_st_flush_n / 1000) : 0,
             g_st_frames_n);

      /* 串口上的 syslog 会丢行，同一组数再写一份文件，telnet 上
       * cat /tmp/k7-camstat 读。
       */

      {
        FILE *fp = fopen("/tmp/k7-camstat", "w");

        if (fp != NULL)
          {
            unsigned n = g_st_frames_n ? g_st_frames_n : 1;
            unsigned r = g_st_refr_n ? g_st_refr_n : 1;

            fprintf(fp, "3s: grab %u avg %u ms | refr %u avg %u ms |"
                    " flush-copy %u ms/refr | update+pan %u ms/refr |"
                    " latch-wait %u ms total | take %u ms/frame |"
                    " new frames %u\n",
                    g_st_grab_n,
                    g_st_grab_n ?
                    (unsigned)(g_st_grab_us / g_st_grab_n / 1000) : 0,
                    g_st_refr_n, (unsigned)(g_st_refr_us / r / 1000),
                    (unsigned)(g_st_copy_us / r / 1000),
                    (unsigned)(g_st_flush_us / r / 1000),
                    (unsigned)(g_st_latch_us / 1000),
                    (unsigned)(g_st_take_us / n / 1000), g_st_frames_n);
            fclose(fp);
          }
      }

      g_st_grab_us = g_st_loop_us = g_st_flush_us = 0;
      g_st_grab_n = g_st_loop_n = g_st_flush_n = g_st_frames_n = 0;
      g_st_refr_us = g_st_copy_us = g_st_latch_us = g_st_take_us = 0;
      g_st_refr_n = 0;
      g_st_t0 = now_us();
    }

  if (fresh)
    {
      g_st_frames_n++;
      g_live_shown++;
      lv_image_cache_drop(g_live_buf);
      lv_obj_invalidate(g_live_img);
    }

  if (err != 0 && err != -ETIMEDOUT)
    {
      lv_label_set_text_fmt(g_live_info, "相机已停止（%d）", err);
    }
  else if (fresh && (g_live_shown % 15) == 1)
    {
      time_t dt = time(NULL) - g_live_t0;
      unsigned fps = dt > 0 ? (unsigned)(g_live_shown / dt) : 0;

      lv_label_set_text_fmt(g_live_info, "实时 · %u fps", fps);
      if ((g_live_shown % 150) == 1 && dt > 0)
        {
          syslog(LOG_INFO, "界面: 相机页 采集 %u 帧 显示 %u 帧 / %ld 秒\n",
                 g_live_frames, g_live_shown, (long)dt);
        }
    }
}

static void live_stop(void)
{
  if (g_live_timer != NULL)
    {
      lv_timer_delete(g_live_timer);
      g_live_timer = NULL;
    }

  g_live_run = false;
  if (g_live_thread_ok)
    {
      pthread_join(g_live_thread, NULL);
      g_live_thread_ok = false;
    }

  live_cam_stop();
  kickpi_fb_dbuf(false);
}

static void live_page_close(void)
{
  if (g_live_page == NULL ||
      lv_obj_has_flag(g_live_page, LV_OBJ_FLAG_HIDDEN))
    {
      return;
    }

  live_stop();
  lv_obj_add_flag(g_live_page, LV_OBJ_FLAG_HIDDEN);
}

static void live_close_cb(lv_event_t *event)
{
  (void)event;
  live_page_close();
}

static void live_flash_cb(void *obj, int32_t v)
{
  lv_obj_set_style_bg_opa(obj, v, 0);
}

static void live_flash_done_cb(lv_anim_t *a)
{
  lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN);
}

static void live_snapshot_cb(lv_event_t *event)
{
  (void)event;

#ifdef CONFIG_KICKPI_K7_CAMERA_LINUX
  /* ampctl snap 拿帧共享区里的最新一帧，不碰取图链：预览不用停。
   * 屏幕白闪一下当快门反馈，照片照旧出现在开发者工具的音视频页。
   */

  {
    lv_anim_t a;

    lv_obj_remove_flag(g_live_flash, LV_OBJ_FLAG_HIDDEN);
    lv_anim_init(&a);
    lv_anim_set_var(&a, g_live_flash);
    lv_anim_set_exec_cb(&a, live_flash_cb);
    lv_anim_set_values(&a, LV_OPA_80, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, 250);
    lv_anim_set_completed_cb(&a, live_flash_done_cb);
    lv_anim_start(&a);
  }

  lv_label_set_text(g_live_info, g_camera_pid > 0 ? "上一张还在存" : "已拍照");
  camera_start();
#else
  /* 先停流再拍：v4l2cap 走 /dev/video0，和预览抢同一个 CIF */

  live_page_close();
  camera_start();
#endif
}

static bool live_page_create(void)
{
  lv_obj_t *bar;
  lv_obj_t *btn;
  lv_obj_t *obj;

  /* XRGB：画面不透明，LVGL 直接拷贝，不做逐像素 alpha 混合 */

  g_live_buf = ui_draw_buf_create(LIVE_W, LIVE_H, LV_COLOR_FORMAT_XRGB8888);
  g_live_back = malloc((size_t)LIVE_W * LIVE_H * 4);
  g_live_ready = malloc((size_t)LIVE_W * LIVE_H * 4);
  if (g_live_buf == NULL || g_live_back == NULL || g_live_ready == NULL)
    {
      if (g_live_buf != NULL)
        {
          ui_draw_buf_destroy(g_live_buf);
          g_live_buf = NULL;
        }

      free(g_live_back);
      free(g_live_ready);
      g_live_back = NULL;
      g_live_ready = NULL;
      return false;
    }

  /* 布局：画面铺满整屏，控件浮在画面上 ——
   *
   *   左上  返回（半透明圆钮）        右上  状态胶囊（红点 + 实时 · fps）
   *                 底部正中  快门（白色圆环 + 实心圆）
   *
   * 浮层都是小块半透明底，画面每帧整屏重画时多出来的混合只在这几小块。
   * 预览不到整屏（openvela 自己取图的 640x360）时画面居中，四周是黑底。
   */

  g_live_page = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(g_live_page);
  lv_obj_set_size(g_live_page, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(g_live_page, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(g_live_page, LV_OPA_COVER, 0);
  lv_obj_add_flag(g_live_page, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(g_live_page, LV_OBJ_FLAG_SCROLLABLE);

  g_live_img = lv_image_create(g_live_page);
  lv_image_set_src(g_live_img, g_live_buf);
  lv_obj_center(g_live_img);

  /* 返回 */

  btn = lv_button_create(g_live_page);
  lv_obj_remove_style_all(btn);
  lv_obj_set_size(btn, 72, 72);
  lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 24, 24);
  lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_50, 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_STATE_PRESSED);
  lv_obj_add_event_cb(btn, live_close_cb, LV_EVENT_CLICKED, NULL);
  obj = lv_label_create(btn);
  lv_obj_set_style_text_font(obj, &lv_font_k7_icons_32, 0);
  lv_obj_set_style_text_color(obj, lv_color_white(), 0);
  lv_label_set_text(obj, ICON_BACK);
  lv_obj_center(obj);

  /* 状态胶囊 */

  bar = lv_obj_create(g_live_page);
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, LV_SIZE_CONTENT, 48);
  lv_obj_align(bar, LV_ALIGN_TOP_RIGHT, -24, 36);
  lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_50, 0);
  lv_obj_set_style_pad_hor(bar, 20, 0);
  lv_obj_set_style_pad_column(bar, 10, 0);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  obj = lv_obj_create(bar);
  lv_obj_remove_style_all(obj);
  lv_obj_set_size(obj, 12, 12);
  lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(obj, lv_color_hex(UI_BAD), 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);

  g_live_info = lv_label_create(bar);
  lv_obj_set_style_text_color(g_live_info, lv_color_white(), 0);
  lv_label_set_text(g_live_info, "正在打开相机…");

  /* 快门：外圈白环，里面实心白圆，按下时内圆缩一点 */

  btn = lv_button_create(g_live_page);
  lv_obj_remove_style_all(btn);
  lv_obj_set_size(btn, 104, 104);
  lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -32);
  lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_color(btn, lv_color_white(), 0);
  lv_obj_set_style_border_width(btn, 6, 0);
  lv_obj_set_style_border_opa(btn, LV_OPA_90, 0);
  lv_obj_add_event_cb(btn, live_snapshot_cb, LV_EVENT_CLICKED, NULL);
  obj = lv_obj_create(btn);
  lv_obj_remove_style_all(obj);
  lv_obj_set_size(obj, 80, 80);
  lv_obj_center(obj);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_transform_scale(obj, 220, LV_STATE_PRESSED);
  lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_set_style_transform_pivot_x(obj, 40, 0);
  lv_obj_set_style_transform_pivot_y(obj, 40, 0);

  /* 拍照时的白闪，平时隐藏 */

  g_live_flash = lv_obj_create(g_live_page);
  lv_obj_remove_style_all(g_live_flash);
  lv_obj_set_size(g_live_flash, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(g_live_flash, lv_color_white(), 0);
  lv_obj_remove_flag(g_live_flash, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(g_live_flash, LV_OBJ_FLAG_HIDDEN);

  return true;
}

static void live_page_open(void)
{
  int ret;

  if (g_camera_pid > 0)
    {
      lv_label_set_text(g_camera_status, "正在拍照，请稍等…");
      return;
    }

  if (g_live_page == NULL && !live_page_create())
    {
      lv_label_set_text(g_camera_status, "相机页：内存不够");
      syslog(LOG_ERR, "界面: 相机页创建失败\n");
      return;
    }

  lv_obj_remove_flag(g_live_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(g_live_page);

  ret = live_cam_start();
  syslog(LOG_INFO, "界面: 相机页打开，预览启动 %d\n", ret);
  if (ret < 0)
    {
      lv_label_set_text_fmt(g_live_info,
                            ret == -EBUSY ? "相机正忙（%d），稍后再试" :
                            "相机启动失败（%d）", ret);
      return;
    }

  g_live_new = false;
  g_live_err = 0;
  g_live_frames = 0;
  g_live_shown = 0;
  g_live_t0 = time(NULL);
  g_live_run = true;
  /* 取帧线程低于界面：画不画得出来由 LVGL 决定，取帧只管有就交 */

  {
    pthread_attr_t attr;
    struct sched_param sp;

    pthread_attr_init(&attr);
    sp.sched_priority = sched_get_priority_min(SCHED_RR) +
                        (CONFIG_LVX_DEMO_CONTEST2026_423_KICKPI_UI_PRIORITY -
                         sched_get_priority_min(SCHED_RR)) / 2;
    pthread_attr_setschedparam(&attr, &sp);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    ret = pthread_create(&g_live_thread, &attr, live_thread, NULL);
    pthread_attr_destroy(&attr);
  }

  if (ret != 0)
    {
      g_live_run = false;
      live_cam_stop();
      lv_label_set_text(g_live_info, "起不了预览线程");
      return;
    }

  g_live_thread_ok = true;
  pthread_setname_np(g_live_thread, "ui_camera");
  lv_label_set_text(g_live_info, "实时");
  g_live_timer = lv_timer_create(live_timer_cb, LIVE_TIMER_MS, NULL);
  kickpi_fb_dbuf(true);
}

static void camera_click_cb(lv_event_t *event)
{
  lv_event_code_t code = lv_event_get_code(event);
  lv_indev_t *indev = lv_indev_active();
  lv_point_t pt = { 0, 0 };

  if (indev != NULL)
    {
      lv_indev_get_point(indev, &pt);
    }

  syslog(LOG_INFO, "界面: Open camera 事件 %s (%d,%d)\n",
         code == LV_EVENT_PRESSED ? "PRESSED" :
         code == LV_EVENT_RELEASED ? "RELEASED" :
         code == LV_EVENT_PRESS_LOST ? "PRESS_LOST" :
         code == LV_EVENT_CLICKED ? "CLICKED" : "?",
         (int)pt.x, (int)pt.y);

  if (code == LV_EVENT_CLICKED)
    {
      live_page_open();
    }
}

static void camera_poll(void)
{
  lv_draw_buf_t *next;
  int status;
  pid_t done;

  if (g_camera_pid <= 0)
    {
      return;
    }

  done = waitpid(g_camera_pid, &status, WNOHANG);
  if (done == 0)
    {
      return;
    }

  g_camera_pid = -1;
  lv_obj_remove_state(g_camera_button, LV_STATE_DISABLED);

  /* ★ 退出码只在 waitpid 真拿到时才算数。
   *
   *   CONFIG_SCHED_CHILD_STATUS 没开：子进程退出后状态不保留，250ms 一次
   *   的轮询赶不上时 waitpid 返回 -1（ECHILD）。原来把这当成失败，于是
   *   照片明明拍好了，界面却报"拍照失败"。拿不到退出码就看结果文件。
   */

  if (done > 0 && (!WIFEXITED(status) || WEXITSTATUS(status) != 0))
    {
      lv_label_set_text(g_camera_status, UI_SNAP_FAIL);
      return;
    }

  if (done < 0)
    {
      struct stat st;

      if (stat(UI_CAMERA_FILE, &st) < 0 || st.st_size < 1024)
        {
          lv_label_set_text(g_camera_status, UI_SNAP_FAIL);
          return;
        }
    }

  next = camera_decode(UI_CAMERA_FILE);
  if (next == NULL)
    {
      lv_label_set_text(g_camera_status, "照片解码失败");
      return;
    }

  lv_image_set_src(g_camera_image, NULL);
  if (g_camera_frame != NULL)
    {
      ui_draw_buf_destroy(g_camera_frame);
    }

  g_camera_frame = next;
  lv_image_set_src(g_camera_image, g_camera_frame);
  lv_label_set_text(g_camera_status,
                    "拍好了。想问桌上的东西，去「助手」页。");
}

/****************************************************************************
 * AGENT 页：桌面助手
 *
 * ★ 为什么单独一页
 *
 *   原来助手是 DESK 页相机卡片下面的一个 label：回答写进去了，但
 *     1. 界面只有 Montserrat 14，模型用中文回答时整段是空白；
 *     2. 回答缓冲 512 字节，实测一次回答 882 字节，中文 UTF-8 还可能
 *        被从一个字的中间截断；
 *     3. 它在全宽相机画面的下面，拍过照之后要往下滚才看得到。
 *   用户看到的就是"点了没反应、也没有输出"。
 *
 *   这一页：问答气泡（中文字库 lv_font_k7_cjk_20）、进行中显示已等秒数、
 *   回答完整保留（2KB，截断只落在字符边界上）。
 *
 * ★ 为什么要显示秒数
 *
 *   一次看桌面是三轮：LLM 决定调相机（约 6s）→ 拍照 + 视觉模型描述 →
 *   LLM 组织回答（mimo-v2.5 推理模型，实测 85s）。串口实测总共 196s。
 *   三分钟里界面一动不动，谁都会以为卡了。
 ****************************************************************************/


#define ASSIST_MAX_BUBBLES 12
#define ASSIST_BOT_BG      0x223041
#define ASSIST_USER_BG     0x1f5f8b

/* UTF-8 安全的拷贝：截断时退到字符边界，不留半个汉字 */

static void utf8_copy(char *dst, size_t size, const char *src)
{
  size_t len = strlen(src);

  if (len >= size)
    {
      len = size - 1;
      while (len > 0 && ((unsigned char)src[len] & 0xc0) == 0x80)
        {
          len--;
        }
    }

  memcpy(dst, src, len);
  dst[len] = '\0';
}

/* 模型爱写 Markdown（**加粗**、# 标题、- 列表），label 不渲染它们，
 * 原样显示全是星号井号。就地去掉这几种标记。
 */

static void strip_markdown(char *s)
{
  char *r = s;
  char *w = s;
  bool line_start = true;

  while (*r != '\0')
    {
      if (r[0] == '*' && r[1] == '*')
        {
          r += 2;
          continue;
        }

      if (line_start && (*r == '#' || *r == '>'))
        {
          while (*r == '#' || *r == '>' || *r == ' ')
            {
              r++;
            }

          continue;
        }

      if (line_start && (*r == '-' || *r == '*') && r[1] == ' ')
        {
          *w++ = '-';                   /* 列表符统一成 '-' */
          *w++ = ' ';
          r += 2;
          line_start = false;
          continue;
        }

      line_start = *r == '\n';
      *w++ = *r++;
    }

  *w = '\0';
}

static void assist_bubble(bool user, const char *text)
{
  lv_obj_t *row;
  lv_obj_t *bubble;
  lv_obj_t *label;

  /* 只留最近几条，老的删掉，免得 LVGL 堆越用越少 */

  while (lv_obj_get_child_count(g_assist_list) >= ASSIST_MAX_BUBBLES)
    {
      lv_obj_delete(lv_obj_get_child(g_assist_list, 0));
    }

  /* flex 列里没法让单个子项靠右，所以每条套一个整行宽的 row，
   * 在 row 里按主轴 START / END 对齐。
   */

  row = lv_obj_create(g_assist_list);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, user ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  bubble = lv_obj_create(row);
  lv_obj_remove_style_all(bubble);
  lv_obj_set_width(bubble, LV_PCT(user ? 70 : 92));
  lv_obj_set_height(bubble, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(bubble,
                            lv_color_hex(user ? ASSIST_USER_BG
                                              : ASSIST_BOT_BG), 0);
  lv_obj_set_style_radius(bubble, 12, 0);
  lv_obj_set_style_pad_all(bubble, 12, 0);
  lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

  label = lv_label_create(bubble);
  lv_obj_set_width(label, LV_PCT(100));
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(label, &lv_font_k7_cjk_20, 0);
  lv_obj_set_style_text_color(label, lv_color_white(), 0);
  lv_obj_set_style_text_line_space(label, 4, 0);
  lv_label_set_text(label, text);

  lv_obj_update_layout(g_assist_list);
  lv_obj_scroll_to_view(row, LV_ANIM_ON);
}

static void assist_state(const char *text)
{
  lv_label_set_text(g_assist_state, text);
}

/* 语音播报：回答到了就念出来。合成 + 播放在工作线程里，要十几秒 */

static struct
{
  pthread_mutex_t lock;
  volatile bool   busy;
  bool            done;
  int             result;
  char            text[2048];      /* 最近一条回答，"再读一遍"用 */
} g_tts =
{
  .lock = PTHREAD_MUTEX_INITIALIZER,
};

static bool g_tts_enabled = true;

static void *tts_worker(void *arg)
{
  int ret;

  (void)arg;

  /* 放音要用音频设备：先让唤醒的采集停下（也免得把自己的声音听进去） */

  k7_wake_pause(true);
  ret = k7_tts_speak(g_tts.text);
  k7_wake_pause(false);
  syslog(LOG_INFO, "播报: 结束 ret=%d\n", ret);

  pthread_mutex_lock(&g_tts.lock);
  g_tts.result = ret;
  g_tts.done = true;
  g_tts.busy = false;
  pthread_mutex_unlock(&g_tts.lock);
  return NULL;
}

/* text 为 NULL 表示重读上一条 */

static void tts_start(const char *text)
{
  pthread_attr_t attr;
  pthread_t tid;

  if (g_tts.busy)
    {
      return;
    }

  if (text != NULL)
    {
      utf8_copy(g_tts.text, sizeof(g_tts.text), text);
    }

  if (g_tts.text[0] == '\0')
    {
      return;
    }

  g_tts.busy = true;
  pthread_attr_init(&attr);

  /* TLS 握手（mbedtls）加 cJSON，栈给足 */

  pthread_attr_setstacksize(&attr, 32768);
  if (pthread_create(&tid, &attr, tts_worker, NULL) == 0)
    {
      pthread_detach(tid);
      assist_state("正在朗读…");
    }
  else
    {
      g_tts.busy = false;
    }

  pthread_attr_destroy(&attr);
}

static void tts_poll(void)
{
  bool done;
  int result;

  pthread_mutex_lock(&g_tts.lock);
  done = g_tts.done;
  result = g_tts.result;
  g_tts.done = false;
  pthread_mutex_unlock(&g_tts.lock);

  if (done && !g_agent_busy)
    {
      if (result == 0)
        {
          assist_state("朗读完毕");
        }
      else
        {
          lv_label_set_text_fmt(g_assist_state, "朗读失败（%d）", result);
        }
    }

  /* echo 文字 > /tmp/k7-say：直接朗读这段文字（调试 / 演示用） */

  if (!g_tts.busy)
    {
      FILE *fp = fopen("/tmp/k7-say", "r");

      if (fp != NULL)
        {
          char buf[512];
          size_t n = fread(buf, 1, sizeof(buf) - 1, fp);

          fclose(fp);
          unlink("/tmp/k7-say");
          buf[n] = '\0';
          tts_start(buf);
        }
    }
}

static void tts_switch_cb(lv_event_t *event)
{
  g_tts_enabled = lv_obj_has_state(lv_event_get_target(event),
                                   LV_STATE_CHECKED);
}

static void tts_again_cb(lv_event_t *event)
{
  (void)event;
  tts_start(NULL);
}

static void agent_reply_cb(int status, const char *reply, void *cookie)
{
  (void)cookie;
  pthread_mutex_lock(&g_agent_lock);
  if (status == 0 && reply != NULL)
    {
      utf8_copy(g_agent_reply, sizeof(g_agent_reply), reply);
    }
  else
    {
      snprintf(g_agent_reply, sizeof(g_agent_reply),
               "请求失败（%d）。检查网络和 ai_agent 是否在运行。", status);
    }

  g_agent_reply_ready = true;
  pthread_mutex_unlock(&g_agent_lock);
}

/* 桌面问答的工作线程：拍照 + 一次视觉请求（见 k7_vision.c 开头为什么
 * 不走 ai_agent 的循环）。失败时 answer 里是给用户看的说明，一样当回答
 * 显示出来。
 */

static void *desk_worker(void *arg)
{
  static char answer[2048];
  char *question = arg;

  k7_vision_ask(question, answer, sizeof(answer));
  free(question);
  agent_reply_cb(0, answer, NULL);
  return NULL;
}

/* question 发给模型，shown 显示在气泡里（NULL 表示不显示，桌面守护用） */

static void agent_ask(const char *question, const char *shown)
{
  pthread_attr_t attr;
  pthread_t tid;
  char *copy;
  int ret;

  if (g_agent_busy)
    {
      assist_state("上一个问题还在处理，稍等。");
      return;
    }

  /* 拍照走 /dev/video0，和相机页的预览抢 CIF，先停预览 */

  live_page_close();

  copy = strdup(question);
  if (copy == NULL)
    {
      return;
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 32768);   /* TLS 握手 + cJSON */
  ret = pthread_create(&tid, &attr, desk_worker, copy);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      free(copy);
      lv_label_set_text_fmt(g_assist_state, "起不了工作线程（%d）。", ret);
      return;
    }

  pthread_detach(tid);

  if (shown != NULL)
    {
      assist_bubble(true, shown);
    }

  g_agent_busy = true;
  g_agent_started = time(NULL);
  lv_obj_remove_flag(g_assist_spinner, LV_OBJ_FLAG_HIDDEN);
  assist_state("拍照、看图中…");
}

/* 回答要求：中文、短、不要 Markdown。字库只有 GB2312 一级常用字，
 * 生僻字会显示成方块，短句能少碰到。
 */

#define ASSIST_STYLE "请用简体中文回答，不超过 120 字，不要用 Markdown 格式。"

static void agent_look_cb(lv_event_t *event)
{
  (void)event;
  agent_ask("这是桌面摄像头刚拍的照片。桌上有哪些主要物体？" ASSIST_STYLE,
            "桌上有什么？");
}

static void agent_items_cb(lv_event_t *event)
{
  (void)event;
  agent_ask("这是桌面摄像头刚拍的照片。画面里有钥匙和杯子吗？被挡住或"
            "看不清就说不确定，不要猜。" ASSIST_STYLE,
            "钥匙和杯子在吗？");
}

static void agent_tidy_cb(lv_event_t *event)
{
  (void)event;
  agent_ask("这是桌面摄像头刚拍的照片。桌面整洁吗？最多给三条具体的"
            "收拾建议。" ASSIST_STYLE,
            "桌面乱不乱？怎么收拾？");
}

static int guard_value(cJSON *root, const char *name)
{
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
  if (cJSON_IsBool(item))
    {
      return cJSON_IsTrue(item) ? 1 : 0;
    }

  return -1;
}

static const char *guard_word(int v)
{
  return v < 0 ? "不确定" : (v ? "在" : "不见了");
}

static bool guard_apply(const char *reply)
{
  const char *json = strchr(reply, '{');
  cJSON *root;
  int keys;
  int cup;
  bool alert = false;

  if (json == NULL)
    {
      return false;
    }

  root = cJSON_Parse(json);
  if (root == NULL)
    {
      return false;
    }

  keys = guard_value(root, "keys");
  cup = guard_value(root, "cup");
  if (keys >= 0)
    {
      alert |= g_guard_keys == 1 && keys == 0;
      g_guard_keys = keys;
    }

  if (cup >= 0)
    {
      alert |= g_guard_cup == 1 && cup == 0;
      g_guard_cup = cup;
    }

  cJSON_Delete(root);
  lv_label_set_text_fmt(g_guard_state, "%s钥匙：%s，杯子：%s",
                        alert ? "注意：有东西不见了！" : "上次检查 ",
                        guard_word(g_guard_keys), guard_word(g_guard_cup));
  lv_obj_set_style_text_color(g_guard_state,
                              lv_color_hex(alert ? UI_BAD : UI_DIM), 0);
  if (alert)
    {
      assist_bubble(false, "桌面守护：刚才还在的东西现在看不到了，"
                           "请看一下桌面。");
    }

  return true;
}

static void guard_switch_cb(lv_event_t *event)
{
  g_guard_enabled = lv_obj_has_state(lv_event_get_target(event),
                                     LV_STATE_CHECKED);
  g_guard_next = time(NULL);
  if (!g_guard_enabled)
    {
      g_guard_keys = -1;
      g_guard_cup = -1;
      lv_label_set_text(g_guard_state, "桌面守护已关闭。");
    }
  else
    {
      lv_label_set_text(g_guard_state, "桌面守护已开启，马上检查一次。");
    }
}

static void agent_poll(void)
{
  static char reply[sizeof(g_agent_reply)];
  static time_t shown_elapsed = -1;
  time_t elapsed;
  bool ready;

  pthread_mutex_lock(&g_agent_lock);
  ready = g_agent_reply_ready;
  if (ready)
    {
      memcpy(reply, g_agent_reply, sizeof(reply));
      g_agent_reply_ready = false;
    }

  pthread_mutex_unlock(&g_agent_lock);

  elapsed = time(NULL) - g_agent_started;
  if (ready)
    {
      reply[sizeof(reply) - 1] = '\0';
      syslog(LOG_INFO, "界面: agent 回复（%ld 秒，%zu 字节）: %s\n",
             (long)elapsed, strlen(reply), reply);
      if (g_guard_request)
        {
          if (!guard_apply(reply))
            {
              lv_label_set_text(g_guard_state,
                                "这次没看清，状态保持不变。");
            }

          g_guard_request = false;
          g_guard_next = time(NULL) + 60;
          assist_state("空闲");
        }
      else
        {
          strip_markdown(reply);
          assist_bubble(false, reply);
          lv_label_set_text_fmt(g_assist_state, "已回答，用时 %ld 秒",
                                (long)elapsed);
          if (g_tts_enabled)
            {
              tts_start(reply);
            }
        }

      lv_obj_add_flag(g_assist_spinner, LV_OBJ_FLAG_HIDDEN);
      g_agent_busy = false;
      shown_elapsed = -1;
    }
  else if (g_agent_busy && elapsed >= UI_AGENT_TIMEOUT_S)
    {
      assist_bubble(false, "这次等了很久还没有回答，已放弃。"
                           "可以再问一次。");
      assist_state("超时");
      lv_obj_add_flag(g_assist_spinner, LV_OBJ_FLAG_HIDDEN);
      g_agent_busy = false;
      shown_elapsed = -1;
    }
  else if (g_agent_busy && elapsed != shown_elapsed)
    {
      /* 每秒刷一次，不是每个 tick —— 文本没变就不重绘 */

      shown_elapsed = elapsed;
      lv_label_set_text_fmt(g_assist_state,
                            "%s %ld 秒（拍照 → 看图作答，通常十几秒）",
                            g_guard_request ? "桌面守护检查中" : "思考中",
                            (long)elapsed);
    }

  if (g_guard_enabled && !g_agent_busy && time(NULL) >= g_guard_next)
    {
      g_guard_request = true;
      agent_ask("This is a fresh photo of the desk. Return one JSON "
                "object only: {\"keys\":true|false|null,"
                "\"cup\":true|false|null}. Use null when obscured or "
                "uncertain. Do not guess.", NULL);
      if (!g_agent_busy)
        {
          g_guard_request = false;
          g_guard_next = time(NULL) + 10;
        }
    }
}

/* 语音唤醒 */

static lv_obj_t *g_wake_state;
static lv_obj_t *g_wake_bar;
static char      g_wake_shown[96];

static void wake_switch_cb(lv_event_t *event)
{
  if (lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED))
    {
      if (k7_wake_start() < 0)
        {
          lv_label_set_text(g_wake_state, "语音唤醒启动失败。");
        }
    }
  else
    {
      k7_wake_stop();
      lv_bar_set_value(g_wake_bar, 0, LV_ANIM_OFF);
      lv_label_set_text(g_wake_state, "语音唤醒已关闭。");
      g_wake_shown[0] = '\0';
    }
}

static void wake_show(const char *text)
{
  if (strcmp(text, g_wake_shown) != 0)
    {
      snprintf(g_wake_shown, sizeof(g_wake_shown), "%s", text);
      lv_label_set_text(g_wake_state, text);
    }
}

/* 语音里的固定指令（录制等）在本地处理，不交给模型。定义在"工作"页。 */

static bool work_voice(const char *text, const char *question);
static void memo_poll(void);

static void wake_poll(void)
{
  static char heard[160];
  static time_t heard_until;
  struct k7_wake_status_s st;
  char text[512];
  char question[512];
  char prompt[1024];
  int ev;

  k7_wake_get_status(&st);
  if (!st.running && !st.linking)
    {
      return;
    }

  lv_bar_set_value(g_wake_bar, st.level, LV_ANIM_OFF);

  ev = k7_wake_get_event(text, sizeof(text), question, sizeof(question));
  switch (ev)
    {
      case K7_WAKE_ASK:
        if (work_voice(text, question))
          {
            break;
          }

        if (g_agent_busy)
          {
            assist_bubble(true, text);
            assist_bubble(false, "上一个问题还在处理，稍后再问我。");
            break;
          }

        snprintf(prompt, sizeof(prompt),
                 "这是桌面摄像头刚拍的照片。用户说：%s。请回答用户的问题；"
                 "如果问题和照片无关，就直接回答。" ASSIST_STYLE, question);
        agent_ask(prompt, text);
        break;

      case K7_WAKE_ONLY:
        assist_bubble(true, text);
        assist_bubble(false, "我在，请说。");
        break;

      case K7_WAKE_HEARD:
        snprintf(heard, sizeof(heard), "听到：%.120s（没有唤醒词）", text);
        heard_until = time(NULL) + 3;
        break;

      default:
        break;
    }

  if (st.linking)
    {
      wake_show("正在连接 A72 上的 Linux（端点检测在那边跑）…");
    }
  else if (st.woken)
    {
      wake_show("我在，请说问题…");
    }
  else if (st.recognizing)
    {
      wake_show("识别中…");
    }
  else if (st.speaking)
    {
      wake_show("在听你说…");
    }
  else if (time(NULL) <= heard_until)
    {
      wake_show(heard);
    }
  else
    {
      wake_show("说「你好 openvela」叫醒我，可以接着说问题。");
    }
}

/* 下面几个排版小工具定义在今日页那边，助手页先用到 */

static lv_obj_t *icon_label(lv_obj_t *parent, const char *icon,
                            const lv_font_t *font, uint32_t color);
static lv_obj_t *flex_box(lv_obj_t *parent, lv_flex_flow_t flow);
static lv_obj_t *panel_create(lv_obj_t *parent);

/* 提问按钮：整行宽的胶囊，左边文字、右边小箭头。第一个用主色实心，
 * 其余是次级底色 —— 和今日页"喝了一杯"那类按钮一个路子，不用主题默认
 * 的亮蓝按钮。
 */

static lv_obj_t *assist_button(lv_obj_t *parent, const char *text,
                               lv_event_cb_t cb, bool primary)
{
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_t *label;

  lv_obj_remove_style_all(button);
  lv_obj_set_size(button, LV_PCT(100), 64);
  lv_obj_set_style_radius(button, 32, 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(button,
                            lv_color_hex(primary ? UI_WATER : UI_SURFACE2), 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(primary ? UI_ACCENT :
                                                 UI_DIM), LV_STATE_PRESSED);
  lv_obj_set_style_pad_hor(button, 24, 0);
  lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(button, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);

  label = lv_label_create(button);
  lv_obj_set_style_text_color(label, lv_color_hex(primary ? UI_ON_COLOR :
                                                  UI_TEXT), 0);
  lv_label_set_text(label, text);

  icon_label(button, ICON_RIGHT, &lv_font_k7_icons_32,
             primary ? UI_ON_COLOR : UI_DIM);
  return button;
}

/* 助手页右栏的功能行：图标 | 名称 + 一行状态（返回状态标签）| 开关。
 * 和设置页的行同一套样式，只是状态字放在名称下面。
 */

static lv_obj_t *assist_feature(lv_obj_t *panel, const char *icon,
                                uint32_t color, const char *name,
                                const char *state, lv_event_cb_t cb,
                                bool on, lv_obj_t **col_out)
{
  lv_obj_t *row = flex_box(panel, LV_FLEX_FLOW_ROW);
  lv_obj_t *col;
  lv_obj_t *lbl;
  lv_obj_t *sw;

  lv_obj_set_style_pad_column(row, 16, 0);
  lv_obj_set_style_pad_ver(row, 14, 0);
  lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(row, 1, 0);
  lv_obj_set_style_border_color(row, lv_color_hex(UI_SURFACE2), 0);

  lbl = icon_label(row, icon, &lv_font_k7_icons_32, color);
  lv_obj_set_width(lbl, 40);
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);

  col = flex_box(row, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_width(col, 0);
  lv_obj_set_flex_grow(col, 1);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(col, 4, 0);

  lbl = lv_label_create(col);
  lv_obj_set_style_text_font(lbl, &lv_font_k7_ui_28, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_TEXT), 0);
  lv_label_set_text(lbl, name);

  lbl = lv_label_create(col);
  lv_obj_set_width(lbl, LV_PCT(100));
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_DIM), 0);
  lv_label_set_text(lbl, state);

  sw = lv_switch_create(row);
  lv_obj_set_style_bg_color(sw, lv_color_hex(color),
                            LV_PART_INDICATOR | LV_STATE_CHECKED);
  if (on)
    {
      lv_obj_add_state(sw, LV_STATE_CHECKED);
    }

  lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);

  if (col_out != NULL)
    {
      *col_out = col;
    }

  return lbl;
}

/* 小标题：放在卡片里第一行，灰色 */

static void assist_caption(lv_obj_t *panel, const char *text)
{
  lv_obj_t *lbl = lv_label_create(panel);

  lv_obj_set_width(lbl, LV_PCT(100));

  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_DIM), 0);
  lv_label_set_text(lbl, text);
}

static void assist_build(lv_obj_t *tab)
{
  lv_obj_t *left;
  lv_obj_t *right;
  lv_obj_t *panel;
  lv_obj_t *row;
  lv_obj_t *col;
  lv_obj_t *lbl;
  lv_obj_t *button;

  /* 横屏：左边对话（占满高度），右边提问 + 功能开关。竖屏同样两块，
   * 上下叠。
   */

  lv_obj_set_flex_flow(tab, UI_ROTATE == 0 ? LV_FLEX_FLOW_COLUMN :
                                             LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_all(tab, 24, 0);
  lv_obj_set_style_pad_gap(tab, 24, 0);
  lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

  /* 对话卡片：标题行（图标 + 桌面助手 + 转圈 + 状态）+ 气泡列表 */

  left = panel_create(tab);
  lv_obj_set_style_pad_row(left, 16, 0);
#if UI_ROTATE == 0
  lv_obj_set_height(left, 0);
  lv_obj_set_flex_grow(left, 1);
#else
  lv_obj_set_size(left, 0, LV_PCT(100));
  lv_obj_set_flex_grow(left, 1);
#endif

  row = flex_box(left, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 14, 0);
  icon_label(row, ICON_BRAIN, &lv_font_k7_icons_32, UI_EYE);
  lbl = lv_label_create(row);
  lv_obj_set_style_text_font(lbl, &lv_font_k7_ui_28, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_TEXT), 0);
  lv_label_set_text(lbl, "桌面助手");
  lv_obj_set_flex_grow(lbl, 1);

  g_assist_spinner = lv_spinner_create(row);
  lv_obj_set_size(g_assist_spinner, 28, 28);
  lv_obj_set_style_arc_width(g_assist_spinner, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_width(g_assist_spinner, 4, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(g_assist_spinner, lv_color_hex(UI_EYE),
                             LV_PART_INDICATOR);
  lv_obj_add_flag(g_assist_spinner, LV_OBJ_FLAG_HIDDEN);

  g_assist_state = lv_label_create(row);
  lv_obj_set_style_max_width(g_assist_state, 360, 0);
  lv_label_set_long_mode(g_assist_state, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(g_assist_state, lv_color_hex(UI_DIM), 0);
  lv_label_set_text(g_assist_state, "空闲");

  /* 对话区：卡片里剩下的高度，自己滚动 */

  g_assist_list = lv_obj_create(left);
  lv_obj_remove_style_all(g_assist_list);
  lv_obj_set_width(g_assist_list, LV_PCT(100));
  lv_obj_set_height(g_assist_list, 0);
  lv_obj_set_flex_grow(g_assist_list, 1);
  lv_obj_set_style_pad_row(g_assist_list, 14, 0);
  lv_obj_set_flex_flow(g_assist_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(g_assist_list, LV_DIR_VER);

  assist_bubble(false, "你好，我是桌面助手。点右边的问题，我会用摄像头"
                       "看一眼桌面再回答，一般十几秒。");

  /* 右栏 */

  right = flex_box(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(right, 24, 0);
#if UI_ROTATE != 0
  lv_obj_set_size(right, 420, LV_PCT(100));
  lv_obj_add_flag(right, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(right, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(right, LV_SCROLLBAR_MODE_OFF);
#endif

  /* 提问卡片拉伸补满右栏，让右栏底边和左边的对话卡片对齐 */

  panel = panel_create(right);
  lv_obj_set_style_pad_all(panel, 24, 0);
  lv_obj_set_style_pad_row(panel, 12, 0);
#if UI_ROTATE != 0
  lv_obj_set_height(panel, 0);
  lv_obj_set_flex_grow(panel, 1);
  lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_SPACE_EVENLY,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
#endif
  assist_caption(panel, "问问桌面");
  assist_button(panel, "桌上有什么？", agent_look_cb, true);
  assist_button(panel, "钥匙和杯子在吗？", agent_items_cb, false);
  assist_button(panel, "桌面乱不乱？", agent_tidy_cb, false);

  panel = panel_create(right);
  lv_obj_set_style_pad_all(panel, 24, 0);
  lv_obj_set_style_pad_ver(panel, 12, 0);
  lv_obj_set_style_pad_row(panel, 0, 0);

  g_guard_state = assist_feature(panel, ICON_EYE, UI_EYE, "桌面守护",
                                 "每 60 秒看一次钥匙和杯子", guard_switch_cb,
                                 false, NULL);
  /* 状态字的父是名称列，名称列的父是整行，开关是行的最后一个子项 */

  g_guard_switch = lv_obj_get_child(lv_obj_get_parent(
                     lv_obj_get_parent(g_guard_state)), -1);

  lbl = assist_feature(panel, ICON_VOLUME, UI_ACCENT, "语音播报",
                       "回答读出来", tts_switch_cb, true, &col);
  (void)lbl;

  /* "再读一遍" 做成开关左边的小按钮 */

  button = lv_button_create(lv_obj_get_parent(col));
  lv_obj_remove_style_all(button);
  lv_obj_set_style_radius(button, 20, 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(UI_SURFACE2), 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(UI_DIM), LV_STATE_PRESSED);
  lv_obj_set_style_pad_hor(button, 16, 0);
  lv_obj_set_style_pad_ver(button, 8, 0);
  lv_obj_add_event_cb(button, tts_again_cb, LV_EVENT_CLICKED, NULL);
  lbl = lv_label_create(button);
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_TEXT), 0);
  lv_label_set_text(lbl, "再读");
  lv_obj_move_to_index(button, -2);

  g_wake_state = assist_feature(panel, ICON_MIC, UI_SIT, "语音唤醒",
                                "语音唤醒已关闭。", wake_switch_cb, false,
                                &col);
  lv_obj_set_style_border_width(lv_obj_get_parent(col), 0, 0);

  /* 唤醒时的音量条：细线，挂在状态字下面 */

  g_wake_bar = lv_bar_create(col);
  lv_obj_set_size(g_wake_bar, LV_PCT(100), 6);
  lv_bar_set_range(g_wake_bar, 0, 100);
  lv_obj_set_style_bg_color(g_wake_bar, lv_color_hex(UI_SURFACE2),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_color(g_wake_bar, lv_color_hex(UI_SIT),
                            LV_PART_INDICATOR);
}

static void camera_build(lv_obj_t *tab)
{
  lv_obj_t *card;

  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(tab, 8, 0);

  card = card_create(tab, "桌面摄像头");
  g_camera_status = lv_label_create(card);
  lv_label_set_text(g_camera_status,
                    "「打开相机」看实时画面，画面里可以拍照。");
  lv_label_set_long_mode(g_camera_status, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(g_camera_status, LV_PCT(100));

  g_camera_button = lv_button_create(card);
  lv_label_set_text(lv_label_create(g_camera_button), "打开相机");
  lv_obj_add_event_cb(g_camera_button, camera_click_cb,
                      LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(g_camera_button, camera_click_cb,
                      LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(g_camera_button, camera_click_cb,
                      LV_EVENT_RELEASED, NULL);
  lv_obj_add_event_cb(g_camera_button, camera_click_cb,
                      LV_EVENT_PRESS_LOST, NULL);

  g_camera_image = lv_image_create(card);
  lv_obj_set_width(g_camera_image, LV_PCT(100));
}

/****************************************************************************
 * VOICE 页：录音机
 *
 * 录 5 秒 → 看波形和电平 → 放出来听。展示用，同时也是麦克风链路的
 * 自检：`mic` 命令给的是数字，这里给的是"录进去的就是我刚说的话"。
 *
 * ★ 录音和放音都在工作线程里做
 *
 *   音频 ioctl 会阻塞（每个缓冲区约 43ms，整段 5 秒），放在 LVGL 线程里
 *   界面就冻住了。工作线程只写 g_rec 里的几个数，界面在 tick 里读。
 *
 * ★ 录的是哪个声道
 *
 *   ES8388 按立体声采，但板上麦克风接的是 LINE2 的一路，另一路可能只有
 *   底噪。两路都存下来，录完按平均幅度挑大的那一路当作单声道。
 *
 * ★ 放音自动放大
 *
 *   麦克风电平偏低（`mic` 实测底噪平均 117/32767），原样放几乎听不见。
 *   按电平归一化，最多放大 4 倍（录音已有 ALC），倍数显示在界面上。
 ****************************************************************************/

#define REC_SECONDS    5
#define REC_FRAMES     (REC_SECONDS * K7A_RATE)
#define REC_WAVE_PTS   100
#define REC_WAV_FILE   "/tmp/k7-rec.wav"

enum rec_state_e
{
  REC_IDLE = 0,
  REC_RECORDING,
  REC_PLAYING,
  REC_BEEP,
};

struct rec_s
{
  pthread_mutex_t lock;
  volatile int    state;
  int16_t        *st;          /* 交错立体声，REC_FRAMES 帧 */
  int16_t        *mono;        /* 挑出来的那一路 */
  size_t          frames;      /* 已录帧数 */
  int             level;       /* 最近一块的电平 0..100，录音时刷新 */
  int             peak;        /* 所选声道的峰值 */
  int             avg[2];      /* 两个声道的平均幅度 */
  int             chan;        /* 选中的声道 0 = 左 1 = 右 */
  int             gain;        /* 放音放大倍数 */
  int             result;      /* 最近一次操作的返回值 */
  int             hum[3];      /* 50/100/150Hz 幅度（诊断） */
  bool            done;        /* 一次操作刚结束，界面还没处理 */
  int             done_what;   /* 刚结束的是 REC_RECORDING 还是 REC_PLAYING */
};

static struct rec_s g_rec =
{
  .lock = PTHREAD_MUTEX_INITIALIZER,
};

static lv_obj_t          *g_rec_state;
static lv_obj_t          *g_rec_bar;
static lv_obj_t          *g_rec_chart;
static lv_chart_series_t *g_rec_series;
static lv_obj_t          *g_rec_btn_rec;
static lv_obj_t          *g_rec_btn_play;
static int                g_rec_shown_sec = -1;
static lv_obj_t          *g_vol_slider;
static lv_obj_t          *g_vol_label;

static void vol_show(int percent)
{
  lv_label_set_text_fmt(g_vol_label, "%d%%", percent);
}

static void vol_slider_cb(lv_event_t *e)
{
  int v = (int)lv_slider_get_value(lv_event_get_target(e));

  vol_show(v);

  /* 拖动过程中只改显示，松手再下发 —— 每一格都写一次 codec 寄存器没必要 */

  if (lv_event_get_code(e) == LV_EVENT_RELEASED)
    {
      k7a_set_volume(v);
      syslog(LOG_INFO, "音量: %d%%\n", v);
    }
}

static bool rec_capture_cb(const int16_t *st, size_t frames, void *priv)
{
  int peak = 0;
  size_t n;
  size_t i;

  (void)priv;
  n = REC_FRAMES - g_rec.frames;
  if (frames < n)
    {
      n = frames;
    }

  memcpy(g_rec.st + 2 * g_rec.frames, st, n * 4);
  for (i = 0; i < n * 2; i++)
    {
      int v = st[i] < 0 ? -st[i] : st[i];
      if (v > peak)
        {
          peak = v;
        }
    }

  g_rec.frames += n;
  g_rec.level = peak * 100 / 32768;
  return g_rec.frames < REC_FRAMES;
}

static void rec_write_wav(const int16_t *pcm, size_t frames)
{
  uint32_t data = frames * 2;
  uint8_t  hdr[44];
  FILE    *fp;

  memcpy(hdr, "RIFF", 4);
  hdr[4]  = (data + 36) & 0xff;         hdr[5]  = ((data + 36) >> 8) & 0xff;
  hdr[6]  = ((data + 36) >> 16) & 0xff; hdr[7]  = ((data + 36) >> 24) & 0xff;
  memcpy(hdr + 8, "WAVEfmt ", 8);
  hdr[16] = 16; hdr[17] = 0; hdr[18] = 0; hdr[19] = 0;   /* fmt 块长度 */
  hdr[20] = 1;  hdr[21] = 0;                             /* PCM */
  hdr[22] = 1;  hdr[23] = 0;                             /* 单声道 */
  hdr[24] = K7A_RATE & 0xff; hdr[25] = (K7A_RATE >> 8) & 0xff;
  hdr[26] = 0;  hdr[27] = 0;
  hdr[28] = (K7A_RATE * 2) & 0xff; hdr[29] = ((K7A_RATE * 2) >> 8) & 0xff;
  hdr[30] = ((K7A_RATE * 2) >> 16) & 0xff; hdr[31] = 0;
  hdr[32] = 2;  hdr[33] = 0;                             /* 块对齐 */
  hdr[34] = 16; hdr[35] = 0;                             /* 位深 */
  memcpy(hdr + 36, "data", 4);
  hdr[40] = data & 0xff;         hdr[41] = (data >> 8) & 0xff;
  hdr[42] = (data >> 16) & 0xff; hdr[43] = (data >> 24) & 0xff;

  fp = fopen(REC_WAV_FILE, "wb");
  if (fp != NULL)
    {
      fwrite(hdr, 1, sizeof(hdr), fp);
      fwrite(pcm, 2, frames, fp);
      fclose(fp);
    }
}

/* 录完：挑声道、算电平和放大倍数、存 WAV
 *
 * ★ 电平用 99.5 百分位，不用峰值。
 *
 *   实测录音里会有个别满幅（32767）的尖峰：丢掉 START 后第一块之后
 *   仍然有。按峰值归一化的话，一个尖峰就把放大倍数压到 1 倍、把波形
 *   图压成"一根柱子加一条平线"——用户看到的就是"包络动一下就不动了，
 *   播放没反应"。百分位对零星尖峰不敏感。
 */

static void rec_analyse(void)
{
  static uint32_t hist[512];            /* |x| 按 64 分桶 */
  int64_t sum[2] = { 0, 0 };
  uint32_t want;
  uint32_t acc = 0;
  size_t peak_at = 0;
  int peak = 0;
  int p995 = 0;
  size_t i;

  for (i = 0; i < g_rec.frames; i++)
    {
      sum[0] += g_rec.st[2 * i] < 0 ? -g_rec.st[2 * i] : g_rec.st[2 * i];
      sum[1] += g_rec.st[2 * i + 1] < 0 ? -g_rec.st[2 * i + 1]
                                        : g_rec.st[2 * i + 1];
    }

  g_rec.avg[0] = g_rec.frames ? (int)(sum[0] / (int64_t)g_rec.frames) : 0;
  g_rec.avg[1] = g_rec.frames ? (int)(sum[1] / (int64_t)g_rec.frames) : 0;
  g_rec.chan = g_rec.avg[1] > g_rec.avg[0] ? 1 : 0;

  memset(hist, 0, sizeof(hist));
  for (i = 0; i < g_rec.frames; i++)
    {
      int16_t v = g_rec.st[2 * i + g_rec.chan];
      int a = v < 0 ? -v : v;

      g_rec.mono[i] = v;
      hist[a >> 6]++;
      if (a > peak)
        {
          peak = a;
          peak_at = i;
        }
    }

  want = (uint32_t)(g_rec.frames - g_rec.frames / 200);
  for (i = 0; i < 512; i++)
    {
      acc += hist[i];
      if (acc >= want)
        {
          p995 = (int)((i + 1) << 6);
          break;
        }
    }

  g_rec.peak = p995 > 0 ? p995 : 1;
  g_rec.gain = 20000 / g_rec.peak;
  if (g_rec.gain < 1)
    {
      g_rec.gain = 1;
    }
  else if (g_rec.gain > 4)
    {
      /* ALC 已经把录音电平拉到目标值，这里只做小幅补偿。上限原来是
       * 16 倍，底噪被一起放大，回放噪声很大。
       */

      g_rec.gain = 4;
    }

  syslog(LOG_INFO, "录音机: 最大尖峰 %d 在第 %zu 帧（%zu ms），99.5%% 电平 %d\n",
         peak, peak_at, peak_at * 1000 / K7A_RATE, p995);

  /* 诊断：工频干扰强度。Goertzel 算 50/100/150Hz 三个频点的幅度，和
   * 整段 RMS 放一起看 —— 电流声就是这几个频点。
   */

  {
    static const int hz[3] = { 50, 100, 150 };
    double rms = 0;
    int h;

    for (i = 0; i < g_rec.frames; i++)
      {
        rms += (double)g_rec.mono[i] * g_rec.mono[i];
      }

    rms = sqrt(rms / (g_rec.frames ? g_rec.frames : 1));
    for (h = 0; h < 3; h++)
      {
        double c = 2 * cos(2 * M_PI * hz[h] / K7A_RATE);
        double s0;
        double s1 = 0;
        double s2 = 0;

        for (i = 0; i < g_rec.frames; i++)
          {
            s0 = g_rec.mono[i] + c * s1 - s2;
            s2 = s1;
            s1 = s0;
          }

        g_rec.hum[h] = (int)(2 * sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) /
                             (g_rec.frames ? g_rec.frames : 1));
      }

    syslog(LOG_INFO, "录音机: RMS %d，工频幅度 50Hz %d / 100Hz %d / "
           "150Hz %d\n", (int)rms, g_rec.hum[0], g_rec.hum[1], g_rec.hum[2]);
  }

  /* 诊断：每 100ms 一个峰值，看噪声是均匀的底噪还是周期性的突发 */

  {
    char line[400];
    int off = 0;
    size_t seg = K7A_RATE / 10;
    size_t k;

    for (k = 0; k + seg <= g_rec.frames && off < (int)sizeof(line) - 8;
         k += seg)
      {
        int pk = 0;
        size_t j;

        for (j = 0; j < seg; j++)
          {
            int a = g_rec.mono[k + j] < 0 ? -g_rec.mono[k + j]
                                          : g_rec.mono[k + j];
            if (a > pk)
              {
                pk = a;
              }
          }

        off += snprintf(line + off, sizeof(line) - off, " %d", pk);
      }

    syslog(LOG_INFO, "录音机包络(每100ms峰值):%s\n", line);
  }
  rec_write_wav(g_rec.mono, g_rec.frames);
}

static void *rec_worker(void *arg)
{
  int what = (int)(intptr_t)arg;
  int ret;

  k7_wake_pause(true);            /* 录音机要独占音频设备 */

  if (what == REC_RECORDING)
    {
      g_rec.frames = 0;
      g_rec.level = 0;
      ret = k7a_capture(rec_capture_cb, NULL, (REC_SECONDS + 3) * 1000);
      if (g_rec.frames > 0)
        {
          rec_analyse();
        }
    }
  else if (what == REC_BEEP)
    {
      int16_t *out = malloc(K7A_RATE * 2);
      size_t i;

      ret = -ENOMEM;
      if (out != NULL)
        {
          /* 1kHz 方波的 48 点周期近似正弦：用查表太啰嗦，三角波足够听 */

          for (i = 0; i < K7A_RATE; i++)
            {
              int ph = (int)(i % 48);
              int tri = ph < 24 ? ph * 2 - 24 : 72 - ph * 2;

              out[i] = (int16_t)(tri * 800);
            }

          ret = k7a_play_mono(out, K7A_RATE);
          free(out);
        }
    }
  else
    {
      int16_t *out = malloc(g_rec.frames * 2);
      size_t i;

      if (out == NULL)
        {
          ret = -ENOMEM;
        }
      else
        {
          for (i = 0; i < g_rec.frames; i++)
            {
              int32_t v = (int32_t)g_rec.mono[i] * g_rec.gain;

              out[i] = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
            }

          ret = k7a_play_mono(out, g_rec.frames);
          free(out);
        }
    }

  k7_wake_pause(false);

  pthread_mutex_lock(&g_rec.lock);
  g_rec.result = ret;
  g_rec.done_what = what;
  g_rec.done = true;
  g_rec.state = REC_IDLE;
  pthread_mutex_unlock(&g_rec.lock);
  return NULL;
}

static void rec_start(int what)
{
  pthread_attr_t attr;
  pthread_t tid;

  if (g_rec.state != REC_IDLE)
    {
      return;
    }


  if (g_rec.st == NULL)
    {
      g_rec.st = malloc(REC_FRAMES * 4);
      g_rec.mono = malloc(REC_FRAMES * 2);
      if (g_rec.st == NULL || g_rec.mono == NULL)
        {
          lv_label_set_text(g_rec_state, "内存不够，录不了。");
          return;
        }
    }

  if (what == REC_PLAYING && g_rec.frames == 0)
    {
      lv_label_set_text(g_rec_state, "还没录过，先点“录音”。");
      return;
    }

  g_rec.state = what;
  g_rec_shown_sec = -1;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 8192);
  if (pthread_create(&tid, &attr, rec_worker, (void *)(intptr_t)what) != 0)
    {
      g_rec.state = REC_IDLE;
      lv_label_set_text(g_rec_state, "起不了工作线程。");
    }
  else
    {
      pthread_detach(tid);
      lv_label_set_text(g_rec_state, what == REC_RECORDING ?
                        "录音中…对着麦克风说话" : "播放中…");
    }

  pthread_attr_destroy(&attr);
}

static void rec_click_cb(lv_event_t *e)
{
  rec_start((int)(intptr_t)lv_event_get_user_data(e));
}

/* 录完后画包络：每个点是一段里的峰值，按本次峰值缩放到 0..100 */

static void rec_draw_wave(void)
{
  size_t seg = g_rec.frames / REC_WAVE_PTS;
  int i;

  for (i = 0; i < REC_WAVE_PTS; i++)
    {
      int peak = 0;
      size_t j;

      for (j = 0; j < seg; j++)
        {
          int16_t v = g_rec.mono[i * seg + j];
          int a = v < 0 ? -v : v;

          if (a > peak)
            {
              peak = a;
            }
        }

      peak = g_rec.peak ? peak * 100 / g_rec.peak : 0;
      lv_chart_set_value_by_id(g_rec_chart, g_rec_series, i,
                               peak > 100 ? 100 : peak);
    }

  lv_chart_refresh(g_rec_chart);
}

static void rec_poll(void)
{
  bool done;
  int what;
  int result;

  pthread_mutex_lock(&g_rec.lock);
  done = g_rec.done;
  what = g_rec.done_what;
  result = g_rec.result;
  g_rec.done = false;
  pthread_mutex_unlock(&g_rec.lock);

  if (g_rec.state == REC_RECORDING)
    {
      int sec = (int)(g_rec.frames / K7A_RATE);

      lv_bar_set_value(g_rec_bar, g_rec.level, LV_ANIM_OFF);
      if (sec != g_rec_shown_sec)
        {
          g_rec_shown_sec = sec;
          lv_label_set_text_fmt(g_rec_state, "录音中… %d / %d 秒",
                                sec, REC_SECONDS);
        }
    }

  if (!done)
    {
      return;
    }

  lv_bar_set_value(g_rec_bar, 0, LV_ANIM_OFF);
  if (what == REC_RECORDING)
    {
      if (g_rec.frames == 0)
        {
          lv_label_set_text_fmt(g_rec_state, "录音失败（%d）", result);
          return;
        }

      rec_draw_wave();
      lv_label_set_text_fmt(g_rec_state,
                            "录好了 %d.%d 秒。用的是%s声道（平均 左 %d / "
                            "右 %d），电平 %d，放音放大 %d 倍。",
                            (int)(g_rec.frames / K7A_RATE),
                            (int)(g_rec.frames % K7A_RATE * 10 / K7A_RATE),
                            g_rec.chan ? "右" : "左",
                            g_rec.avg[0], g_rec.avg[1], g_rec.peak,
                            g_rec.gain);
      syslog(LOG_INFO, "录音机: %zu 帧 声道=%d 平均 L=%d R=%d 峰值=%d "
             "增益=%d ret=%d，已存 %s\n", g_rec.frames, g_rec.chan,
             g_rec.avg[0], g_rec.avg[1], g_rec.peak, g_rec.gain, result,
             REC_WAV_FILE);
    }
  else
    {
      lv_label_set_text(g_rec_state, result == 0 ? "播放完毕。" :
                        "播放出错了。");
      syslog(LOG_INFO, "录音机: %s结束 ret=%d\n",
             what == REC_BEEP ? "测试音" : "放音", result);
    }
}

static lv_obj_t *voice_button(lv_obj_t *parent, const char *text,
                              lv_event_cb_t cb, int arg)
{
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_t *label = lv_label_create(button);

  lv_obj_set_style_text_font(label, &lv_font_k7_cjk_20, 0);
  lv_label_set_text(label, text);
  lv_obj_set_style_pad_ver(button, 16, 0);
  lv_obj_set_style_pad_hor(button, 28, 0);
  lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, (void *)(intptr_t)arg);
  return button;
}

/* 喇叭音量：对耳机、喇叭、朗读、录音回放统一生效。产品界面里放在
 * "设置"页，录音机留在开发者页 —— 两处共用一个滑块，/tmp/k7-vol
 * 调试入口才只需要同步这一个。
 */

static void vol_build(lv_obj_t *group)
{
  lv_obj_t *row = lv_obj_create(group);
  lv_obj_t *lbl;

  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), 88);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 24, 0);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  lbl = lv_label_create(row);
  lv_label_set_text(lbl, ICON_VOLUME);
  lv_obj_set_style_text_font(lbl, &lv_font_k7_icons_32, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_ACCENT), 0);
  lv_obj_set_width(lbl, 40);

  g_vol_slider = lv_slider_create(row);
  lv_obj_set_flex_grow(g_vol_slider, 1);
  lv_obj_set_height(g_vol_slider, 14);
  lv_obj_set_style_bg_color(g_vol_slider, lv_color_hex(UI_SURFACE2),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_color(g_vol_slider, lv_color_hex(UI_ACCENT),
                            LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(g_vol_slider, lv_color_white(), LV_PART_KNOB);
  lv_obj_set_style_pad_all(g_vol_slider, 10, LV_PART_KNOB);
  lv_slider_set_range(g_vol_slider, 0, 100);
  lv_slider_set_value(g_vol_slider, k7a_get_volume(), LV_ANIM_OFF);
  lv_obj_add_event_cb(g_vol_slider, vol_slider_cb, LV_EVENT_VALUE_CHANGED,
                      NULL);
  lv_obj_add_event_cb(g_vol_slider, vol_slider_cb, LV_EVENT_RELEASED, NULL);

  g_vol_label = lv_label_create(row);
  lv_obj_set_width(g_vol_label, 80);
  lv_obj_set_style_text_align(g_vol_label, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_style_text_font(g_vol_label, &lv_font_k7_ui_28, 0);
  lv_obj_set_style_text_color(g_vol_label, lv_color_hex(UI_TEXT), 0);
  vol_show(k7a_get_volume());
}

static void voice_build(lv_obj_t *tab)
{
  lv_obj_t *card;
  lv_obj_t *row;
  lv_obj_t *label;

  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(tab, 8, 0);

  card = card_create(tab, NULL);
  label = lv_label_create(card);
  lv_obj_set_style_text_font(label, &lv_font_k7_cjk_20, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(UI_ACCENT), 0);
  lv_label_set_text(label, "录音机（ES8388 麦克风 → 扬声器）");

  row = lv_obj_create(card);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 16, 0);
  g_rec_btn_rec = voice_button(row, "录音 5 秒", rec_click_cb, REC_RECORDING);
  g_rec_btn_play = voice_button(row, "播放", rec_click_cb, REC_PLAYING);

  label = lv_label_create(card);
  lv_obj_set_style_text_font(label, &lv_font_k7_cjk_20, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(UI_DIM), 0);
  lv_label_set_text(label, "实时电平");

  g_rec_bar = lv_bar_create(card);
  lv_obj_set_size(g_rec_bar, LV_PCT(100), 18);
  lv_bar_set_range(g_rec_bar, 0, 100);

  label = lv_label_create(card);
  lv_obj_set_style_text_font(label, &lv_font_k7_cjk_20, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(UI_DIM), 0);
  lv_label_set_text(label, "波形（包络）");

  g_rec_chart = lv_chart_create(card);
  lv_obj_set_size(g_rec_chart, LV_PCT(100), 200);
  lv_chart_set_type(g_rec_chart, LV_CHART_TYPE_BAR);
  lv_chart_set_point_count(g_rec_chart, REC_WAVE_PTS);
  lv_chart_set_range(g_rec_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
  lv_chart_set_div_line_count(g_rec_chart, 0, 0);
  lv_obj_set_style_pad_column(g_rec_chart, 1, 0);
  lv_obj_set_style_bg_color(g_rec_chart, lv_color_hex(UI_BG), 0);
  lv_obj_set_style_border_width(g_rec_chart, 0, 0);
  g_rec_series = lv_chart_add_series(g_rec_chart, lv_color_hex(UI_ACCENT),
                                     LV_CHART_AXIS_PRIMARY_Y);

  g_rec_state = lv_label_create(card);
  lv_obj_set_width(g_rec_state, LV_PCT(100));
  lv_label_set_long_mode(g_rec_state, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(g_rec_state, &lv_font_k7_cjk_20, 0);
  lv_label_set_text(g_rec_state, "点“录音 5 秒”，然后对着麦克风说话。");
}

/****************************************************************************
 * ABOUT 页
 ****************************************************************************/

static void touch_event_cb(lv_event_t *e)
{
  lv_indev_t *indev = lv_indev_active();
  lv_point_t  p;

  if (indev == NULL)
    {
      return;
    }

  lv_indev_get_point(indev, &p);

  if (lv_event_get_code(e) == LV_EVENT_PRESSED)
    {
      g_touch_count++;
    }

  lv_obj_set_pos(g_dot, p.x - 15, p.y - 15);
  lv_label_set_text_fmt(g_touch_label, "X=%d  Y=%d   按下 %u 次",
                        (int)p.x, (int)p.y, g_touch_count);
}

static void about_build(lv_obj_t *tab)
{
  lv_display_t *disp = lv_display_get_default();
  lv_obj_t *card;
  lv_obj_t *pad;

  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(tab, 8, 0);

  card = card_create(tab, "板卡");
  lv_label_set_text(kv_create(card, "芯片"), "Rockchip RK3576");
  lv_label_set_text(kv_create(card, "板子"), "KICKPI-K7");
  lv_label_set_text_fmt(kv_create(card, "屏幕分辨率"), "%d × %d",
                        (int)lv_display_get_horizontal_resolution(disp),
                        (int)lv_display_get_vertical_resolution(disp));
  lv_label_set_text(kv_create(card, "编译时间"), __DATE__ " " __TIME__);

  /* 触摸自检。原来这就是整个程序 —— 一个动作同时证明 VOP2 出图和
   * FT8756 上报都通了，比只看日志可靠，所以留着。
   */

  card = card_create(tab, "触摸测试（在框里拖动）");
  pad = lv_obj_create(card);
  lv_obj_set_width(pad, LV_PCT(100));
  lv_obj_set_height(pad, 220);
  lv_obj_set_style_bg_color(pad, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_color(pad, lv_color_hex(UI_ACCENT), 0);
  lv_obj_set_style_border_width(pad, 1, 0);
  lv_obj_remove_flag(pad, LV_OBJ_FLAG_SCROLLABLE);

  g_dot = lv_obj_create(pad);
  lv_obj_set_size(g_dot, 30, 30);
  lv_obj_set_style_radius(g_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(g_dot, lv_color_hex(UI_BAD), 0);
  lv_obj_set_style_border_width(g_dot, 0, 0);
  lv_obj_set_pos(g_dot, -100, -100);

  g_touch_label = lv_label_create(card);
  lv_obj_set_style_text_color(g_touch_label, lv_color_hex(UI_DIM), 0);
  lv_label_set_text(g_touch_label, "还没有触摸");

  lv_obj_add_flag(pad, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(pad, touch_event_cb, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(pad, touch_event_cb, LV_EVENT_PRESSING, NULL);
}

/****************************************************************************
 * ai_agent 开机自启动
 *
 * ★ 为什么由界面来拉起、而且 stdin 接 /dev/null
 *
 *   ai_agent 最后会起一个 CLI 线程，fgets(stdin) 读控制台。开机在后台
 *   跑的话它和 nsh 抢同一个串口，敲的字谁先读到算谁的。把它的 stdin
 *   接到 /dev/null，CLI 线程第一次读就拿到 EOF 退出，其余服务照常运行，
 *   控制台仍归 nsh。界面和 agent 在同一个地址空间，走消息总线通信。
 *
 * ★ Key 不进仓库
 *
 *   /data 是 tmpfs，重启即失，所以每次开机由这里调 set_llm。Key 来自
 *   本地的 k7_agent_key.h（.gitignore 已忽略，模板见
 *   k7_agent_key.h.example）。没有这个文件就只启动 agent、不配后端，
 *   界面上会显示 Agent 请求失败，而不是编进一个假 Key。
 ****************************************************************************/

#if __has_include("k7_agent_key.h")
#  include "k7_agent_key.h"
#endif

extern bool message_bus_ready(void);
extern void cmd_set_llm(int argc, char **argv);
extern void cmd_set_vision_llm(int argc, char **argv);

static void *agent_boot_thread(void *arg)
{
  posix_spawn_file_actions_t fa;
  char *argv[] = { "ai_agent", NULL };
  pid_t pid;
  int ret;
  int i;

  (void)arg;

  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  ret = posix_spawn(&pid, "ai_agent", &fa, NULL, argv, NULL);
  posix_spawn_file_actions_destroy(&fa);
  if (ret != 0)
    {
      syslog(LOG_ERR, "界面: 启动 ai_agent 失败 %d\n", ret);
      return NULL;
    }

  for (i = 0; i < 100 && !message_bus_ready(); i++)
    {
      usleep(100 * 1000);
    }

  if (!message_bus_ready())
    {
      syslog(LOG_ERR, "界面: ai_agent 10 秒内没有就绪\n");
      return NULL;
    }

#ifdef K7_AGENT_LLM_KEY
  {
    /* Token Plan 的 Key 只认它自己的地址（token-plan-cn.xiaomimimo.com），
     * 打到预设的 api.xiaomimimo.com 一律 401；模型也只有它列出的那几个
     * （2026-11 起是 mimo-v2.6-pro / flash，v2.5 已下线）。
     * 所以地址和模型都跟着 Key 放在本地头文件里。
     */

    char *llm[] = { "set_llm", K7_AGENT_LLM_URL, K7_AGENT_LLM_MODEL,
                    K7_AGENT_LLM_KEY, NULL };
    char *vis[] = { "set_vision_llm", K7_AGENT_LLM_HOST, K7_AGENT_VISION_MODEL,
                    K7_AGENT_LLM_KEY, NULL };

    /* 等 router 初始化完（agent_main 的 P3 在消息总线之后） */

    sleep(2);
    cmd_set_llm(4, llm);
    cmd_set_vision_llm(4, vis);
    syslog(LOG_INFO, "界面: ai_agent 已启动，后端 %s（%s）已配置\n",
           K7_AGENT_LLM_HOST, K7_AGENT_LLM_MODEL);
  }
#else
  syslog(LOG_WARNING, "界面: ai_agent 已启动，但没有 k7_agent_key.h，"
         "未配置 LLM 后端\n");
#endif

  return NULL;
}

static void agent_autostart(void)
{
  pthread_attr_t attr;
  pthread_t tid;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 16384);
  if (pthread_create(&tid, &attr, agent_boot_thread, NULL) == 0)
    {
      pthread_detach(tid);
    }

  pthread_attr_destroy(&attr);
}

/****************************************************************************
 * 产品界面：今日 / 家居 / 设置，以及健康提醒
 *
 * 规划见 docs/ui-product-plan.md。原来的工程页（双核、设备、曲线、
 * 音视频、关于）整体收进"设置 → 开发者工具"，代码一行没改，只是换了
 * 挂载位置。
 *
 * ★ 本期的提醒全部是**纯计时**，界面上也照实写明。
 *
 *   座位感知（摄像头判断人在不在）还没做：在座时长从界面启动或上次
 *   点"我起来了"起算，人离开了计时也不会停。假装"检测到你坐了 45 分钟"
 *   比老实计时更糟 —— 用户一次离座回来就会发现它在瞎说。
 *
 * ★ 时间分两套，别混用
 *
 *   提醒间隔用单调时钟：RTC 没校准、或者中途被人改了时间，都不能让提醒
 *   提前或永远不来。墙钟只用来显示时间和判断"新的一天"（喝水计数清零）。
 *   没开 CONFIG_LIBC_LOCALTIME，北京时间按 UTC+8 手动换算。
 *
 * ★ 墙钟只认 NTP，不认 RTC
 *
 *   板上 HYM8563 里的时间实测是错的（2026-10-30 上板读到 2026-02-28），
 *   而且错得"像真的" —— 按"早于某年就不可信"判断拦不住它，首页会大大方方
 *   显示一个错误的日期。所以只有 NTP 拿到过样本才显示时钟，否则显示
 *   "时间未同步"，喝水计数也不按天清零。
 ****************************************************************************/

#define UI_TZ_OFFSET_S    (8 * 3600)
#define UI_SNOOZE_S       (10 * 60)
#define UI_DAYOFF_S       (12 * 3600)    /* "今天不再提醒" */
#define UI_EYE_TOAST_S    20             /* 20-20-20 的那 20 秒 */
#define UI_WATER_GOAL     8


enum remind_e
{
  REMIND_SIT = 0,
  REMIND_WATER,
  REMIND_EYE,
  REMIND_COUNT
};

struct remind_s
{
  const char *title;
  const char *text;
  const char *speech;      /* NULL 表示不朗读 */
  const char *ok_text;     /* 提醒卡片的主按钮；NULL 表示只出角标不出卡片 */
  const char *icon;
  uint32_t    color;
  int         interval_min;
  bool        enabled;
  time_t      due;         /* 单调时钟秒 */
  time_t      off_until;   /* 单调时钟秒，"今天不再提醒"到期时间 */
};

static struct remind_s g_remind[REMIND_COUNT] =
{
  [REMIND_SIT] =
  {
    "该起来活动了", "已经连续坐了很久，站起来走动几分钟。",
    "坐了很久了，起来活动一下吧。", "我起来了", ICON_CHAIR, UI_SIT, 45,
    true
  },
  [REMIND_WATER] =
  {
    "喝口水", "离上次喝水已经有一段时间了。",
    "该喝水了。", "喝了一杯", ICON_TINT, UI_WATER, 60, true
  },
  [REMIND_EYE] =
  {
    "看看远处", "看 6 米外的地方 20 秒，让眼睛放松。",
    NULL, NULL, ICON_EYE, UI_EYE, 20, true
  },
};

/* 设置页下拉框的取值，与选项文字一一对应 */

static const int g_sit_choices[]   = { 30, 45, 60, 90 };
static const int g_water_choices[] = { 30, 60, 90, 120 };

static bool       g_remind_voice = true;
static int        g_remind_active = -1;    /* 正在显示的提醒卡片 */
static lv_obj_t  *g_remind_modal;
static lv_obj_t  *g_eye_toast;
static time_t     g_eye_toast_until;

static time_t     g_sit_start;
static time_t     g_water_last;
static int        g_water_count;
static long       g_water_day = -1;

static lv_obj_t  *g_main_tabview;
static lv_obj_t  *g_dev_panel;

static lv_obj_t  *g_today_clock;
static lv_obj_t  *g_today_date;
static lv_obj_t  *g_sit_arc;
static lv_obj_t  *g_sit_minutes;
static lv_obj_t  *g_sit_hint;
static lv_obj_t  *g_sit_btn;
static lv_obj_t  *g_water_cups[UI_WATER_GOAL];
static lv_obj_t  *g_water_label;
static lv_obj_t  *g_eye_hint;

/* NTP 是否已经对上时间。一旦对上就记住：之后即使暂时断网，系统时钟
 * 也是由 NTP 校准过的，继续可信。
 */

static bool wall_clock_synced(void)
{
#ifdef CONFIG_NETUTILS_NTPCLIENT
  static bool synced;
  struct ntpc_status_s st;

  if (!synced && ntpc_status(&st) == 0 && st.nsamples > 0)
    {
      synced = true;
      syslog(LOG_INFO, "界面: NTP 已对时\n");
    }

  return synced;
#else
  return false;
#endif
}

static time_t mono_now(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec;
}

static void remind_reschedule(int idx, time_t base)
{
  g_remind[idx].due = base + (time_t)g_remind[idx].interval_min * 60;
}

static void sit_reset(void)
{
  g_sit_start = mono_now();
  remind_reschedule(REMIND_SIT, g_sit_start);
}

static void water_add(void)
{
  g_water_count++;
  g_water_last = mono_now();
  remind_reschedule(REMIND_WATER, g_water_last);
}

static void remind_close(void)
{
  if (g_remind_modal != NULL)
    {
      lv_obj_delete(g_remind_modal);
      g_remind_modal = NULL;
    }

  g_remind_active = -1;
}

enum remind_action_e
{
  REMIND_ACT_OK = 0,
  REMIND_ACT_SNOOZE,
  REMIND_ACT_DAYOFF
};

static void remind_action_cb(lv_event_t *e)
{
  int action = (int)(intptr_t)lv_event_get_user_data(e);
  int idx = g_remind_active;
  time_t now = mono_now();

  if (idx < 0)
    {
      return;
    }

  switch (action)
    {
      case REMIND_ACT_OK:
        if (idx == REMIND_SIT)
          {
            sit_reset();
          }
        else if (idx == REMIND_WATER)
          {
            water_add();
          }
        break;

      case REMIND_ACT_SNOOZE:
        g_remind[idx].due = now + UI_SNOOZE_S;
        break;

      default:
        g_remind[idx].off_until = now + UI_DAYOFF_S;
        remind_reschedule(idx, g_remind[idx].off_until);
        break;
    }

  syslog(LOG_INFO, "提醒: %s -> %d\n", g_remind[idx].title, action);
  remind_close();
}

/* ---- 产品界面的几个小部件 ---- */

static lv_obj_t *icon_label(lv_obj_t *parent, const char *icon,
                            const lv_font_t *font, uint32_t color)
{
  lv_obj_t *lbl = lv_label_create(parent);

  lv_label_set_text(lbl, icon);
  lv_obj_set_style_text_font(lbl, font, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
  return lbl;
}

/* 透明的横排 / 竖排容器，只用来排版 */

static lv_obj_t *flex_box(lv_obj_t *parent, lv_flex_flow_t flow)
{
  lv_obj_t *box = lv_obj_create(parent);

  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(box, flow);
  lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  return box;
}

/* 大圆角卡片。和工程页的 card_create 分开：那边信息密度高，不动它。 */

static lv_obj_t *panel_create(lv_obj_t *parent)
{
  lv_obj_t *panel = lv_obj_create(parent);

  lv_obj_set_size(panel, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(panel, lv_color_hex(UI_CARD), 0);
  lv_obj_set_style_border_width(panel, 0, 0);
  lv_obj_set_style_radius(panel, 28, 0);
  lv_obj_set_style_pad_all(panel, 28, 0);
  lv_obj_set_style_pad_row(panel, 20, 0);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  return panel;
}

/* 卡片标题行：彩色图标 + 标题，右侧返回一个灰色小字标签给调用者填 */

static lv_obj_t *panel_header(lv_obj_t *panel, const char *icon,
                              uint32_t color, const char *title)
{
  lv_obj_t *row = flex_box(panel, LV_FLEX_FLOW_ROW);
  lv_obj_t *lbl;

  lv_obj_set_style_pad_column(row, 14, 0);
  icon_label(row, icon, &lv_font_k7_icons_32, color);

  lbl = lv_label_create(row);
  lv_label_set_text(lbl, title);
  lv_obj_set_style_text_font(lbl, &lv_font_k7_ui_28, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_TEXT), 0);
  lv_obj_set_flex_grow(lbl, 1);

  lbl = lv_label_create(row);
  lv_label_set_text(lbl, "");
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_DIM), 0);
  return lbl;
}

/* 胶囊按钮。filled：主色实底深色字；否则次级底色、主色字 */

static void pill_set(lv_obj_t *btn, uint32_t color, bool filled)
{
  lv_obj_set_style_bg_color(btn, lv_color_hex(filled ? color : UI_SURFACE2),
                            0);
  lv_obj_set_style_text_color(btn,
                              lv_color_hex(filled ? UI_ON_COLOR : color), 0);
}

static lv_obj_t *pill_button(lv_obj_t *parent, const char *text,
                             uint32_t color, bool filled,
                             lv_event_cb_t cb, void *user_data)
{
  lv_obj_t *btn = lv_button_create(parent);
  lv_obj_t *lbl;

  lv_obj_set_size(btn, LV_PCT(100), 76);
  lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  pill_set(btn, color, filled);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

  lbl = lv_label_create(btn);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_font(lbl, &lv_font_k7_ui_28, 0);
  lv_obj_center(lbl);
  return btn;
}

/* 全屏提醒卡片。挂在顶层图层上，盖住任何页面（包括开发者工具）；
 * 半透明底板吃掉触摸，免得点穿到下面的控件。
 */

static void remind_show(int idx)
{
  struct remind_s *r = &g_remind[idx];
  lv_obj_t *card;
  lv_obj_t *ring;
  lv_obj_t *lbl;
  lv_obj_t *btn;

  g_remind_active = idx;

  g_remind_modal = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(g_remind_modal);
  lv_obj_set_size(g_remind_modal, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(g_remind_modal, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(g_remind_modal, LV_OPA_80, 0);
  lv_obj_add_flag(g_remind_modal, LV_OBJ_FLAG_CLICKABLE);

  card = panel_create(g_remind_modal);
  lv_obj_set_width(card, LV_PCT(88));
  lv_obj_center(card);
  lv_obj_set_style_radius(card, 36, 0);
  lv_obj_set_style_pad_all(card, 36, 0);
  lv_obj_set_style_pad_row(card, 18, 0);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  /* 主色淡底的圆，中间大图标 */

  ring = lv_obj_create(card);
  lv_obj_remove_style_all(ring);
  lv_obj_set_size(ring, 136, 136);
  lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(ring, lv_color_hex(r->color), 0);
  lv_obj_set_style_bg_opa(ring, LV_OPA_20, 0);
  lv_obj_set_style_margin_bottom(ring, 8, 0);
  lv_obj_center(icon_label(ring, r->icon, &lv_font_k7_icons_64, r->color));

  lbl = lv_label_create(card);
  lv_label_set_text(lbl, r->title);
  lv_obj_set_style_text_font(lbl, &lv_font_k7_ui_28, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_TEXT), 0);

  lbl = lv_label_create(card);
  lv_label_set_text(lbl, r->text);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(lbl, LV_PCT(100));
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_DIM), 0);
  lv_obj_set_style_margin_bottom(lbl, 12, 0);

  pill_button(card, r->ok_text, r->color, true, remind_action_cb,
              (void *)(intptr_t)REMIND_ACT_OK);
  pill_button(card, "10 分钟后", r->color, false, remind_action_cb,
              (void *)(intptr_t)REMIND_ACT_SNOOZE);

  /* 第三个按钮做成纯文字：要用户多想一下才点 */

  btn = pill_button(card, "今天不再提醒", UI_DIM, false, remind_action_cb,
                    (void *)(intptr_t)REMIND_ACT_DAYOFF);
  lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
  lv_obj_set_height(btn, 56);

  /* 朗读走云端合成，网络不通时只是不出声，卡片照常显示 */

  if (g_remind_voice && r->speech != NULL)
    {
      tts_start(r->speech);
    }

  syslog(LOG_INFO, "提醒: 显示「%s」\n", r->title);
}

static void eye_toast_cb(lv_event_t *e)
{
  (void)e;
  g_eye_toast_until = 0;
}

/* 用眼休息只出一条顶部横幅，20 秒后自己消失：一天要出现十几次，
 * 每次都弹全屏卡片会被直接关掉。
 */

static void eye_toast_show(void)
{
  struct remind_s *r = &g_remind[REMIND_EYE];
  lv_obj_t *col;
  lv_obj_t *lbl;

  g_eye_toast = lv_obj_create(lv_layer_top());
  lv_obj_set_size(g_eye_toast, LV_PCT(92), LV_SIZE_CONTENT);
  lv_obj_align(g_eye_toast, LV_ALIGN_TOP_MID, 0, 24);
  lv_obj_set_style_bg_color(g_eye_toast, lv_color_hex(UI_SURFACE2), 0);
  lv_obj_set_style_border_width(g_eye_toast, 0, 0);
  lv_obj_set_style_radius(g_eye_toast, 28, 0);
  lv_obj_set_style_pad_all(g_eye_toast, 24, 0);
  lv_obj_set_style_pad_column(g_eye_toast, 20, 0);
  lv_obj_set_style_shadow_width(g_eye_toast, 40, 0);
  lv_obj_set_style_shadow_color(g_eye_toast, lv_color_black(), 0);
  lv_obj_set_style_shadow_opa(g_eye_toast, LV_OPA_60, 0);
  lv_obj_set_flex_flow(g_eye_toast, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(g_eye_toast, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(g_eye_toast, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(g_eye_toast, eye_toast_cb, LV_EVENT_CLICKED, NULL);

  icon_label(g_eye_toast, r->icon, &lv_font_k7_icons_32, r->color);

  col = flex_box(g_eye_toast, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_grow(col, 1);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(col, 4, 0);
  lv_obj_add_flag(col, LV_OBJ_FLAG_EVENT_BUBBLE);

  lbl = lv_label_create(col);
  lv_label_set_text(lbl, r->title);
  lv_obj_set_style_text_font(lbl, &lv_font_k7_ui_28, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_TEXT), 0);

  lbl = lv_label_create(col);
  lv_label_set_text_fmt(lbl, "%s点一下关闭。", r->text);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(lbl, LV_PCT(100));
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_DIM), 0);

  g_eye_toast_until = mono_now() + UI_EYE_TOAST_S;
}

static void remind_tick(void)
{
  time_t now = mono_now();
  int i;

  if (g_eye_toast != NULL && now >= g_eye_toast_until)
    {
      lv_obj_delete(g_eye_toast);
      g_eye_toast = NULL;
    }

  for (i = 0; i < REMIND_COUNT; i++)
    {
      struct remind_s *r = &g_remind[i];

      if (!r->enabled || now < r->off_until || now < r->due)
        {
          continue;
        }

      if (r->ok_text == NULL)
        {
          /* 角标式提醒：已有卡片时也照出，二者不冲突 */

          if (g_eye_toast == NULL)
            {
              eye_toast_show();
            }

          remind_reschedule(i, now);
          continue;
        }

      /* 同一时间只出一张卡片，其余的到期了就排队等着 */

      if (g_remind_active < 0)
        {
          remind_show(i);
        }
    }
}

static void today_refresh(void)
{
  static const char *const week[] =
  {
    "日", "一", "二", "三", "四", "五", "六"
  };

  struct remind_s *sit = &g_remind[REMIND_SIT];
  time_t now = mono_now();
  time_t wall = time(NULL);
  int mins = (int)((now - g_sit_start) / 60);
  bool over;
  int pct;
  int i;

  if (wall_clock_synced())
    {
      time_t local = wall + UI_TZ_OFFSET_S;
      struct tm tm;
      long day = (long)(local / 86400);

      gmtime_r(&local, &tm);
      lv_label_set_text_fmt(g_today_clock, "%02d:%02d",
                            tm.tm_hour, tm.tm_min);
      lv_label_set_text_fmt(g_today_date, "%d月%d日  星期%s",
                            tm.tm_mon + 1, tm.tm_mday, week[tm.tm_wday]);

      if (day != g_water_day)
        {
          g_water_day = day;
          g_water_count = 0;
        }
    }
  else
    {
      lv_label_set_text(g_today_clock, "--:--");
      lv_label_set_text(g_today_date, "时间未同步");
    }

  pct = mins * 100 / (sit->interval_min > 0 ? sit->interval_min : 1);
  over = sit->enabled && pct >= 100;
  lv_arc_set_value(g_sit_arc, pct > 100 ? 100 : pct);
  lv_obj_set_style_arc_color(g_sit_arc,
                             lv_color_hex(over ? UI_SIT_OVER : UI_SIT),
                             LV_PART_INDICATOR);
  lv_label_set_text_fmt(g_sit_minutes, "%d", mins);

  /* 超时后按钮变实底，一眼就知道该点它 */

  pill_set(g_sit_btn, over ? UI_SIT_OVER : UI_SIT, over);

  if (!sit->enabled)
    {
      lv_label_set_text(g_sit_hint, "提醒已关闭");
    }
  else if (now < sit->off_until)
    {
      lv_label_set_text(g_sit_hint, "今天不再提醒");
    }
  else if (now < sit->due)
    {
      lv_label_set_text_fmt(g_sit_hint, "%d 分钟后提醒",
                            (int)((sit->due - now + 59) / 60));
    }
  else
    {
      lv_label_set_text(g_sit_hint, "该起来活动了");
    }

  for (i = 0; i < UI_WATER_GOAL; i++)
    {
      bool full = i < g_water_count;

      lv_obj_set_style_bg_color(g_water_cups[i],
                                lv_color_hex(full ? UI_WATER : UI_SURFACE2),
                                0);
      lv_obj_set_style_text_color(g_water_cups[i],
                                  lv_color_hex(full ? UI_ON_COLOR : UI_DIM),
                                  0);
    }

  lv_label_set_text_fmt(g_water_label, "%d / %d 杯",
                        g_water_count, UI_WATER_GOAL);

  if (!g_remind[REMIND_EYE].enabled)
    {
      lv_label_set_text(g_eye_hint, "已关闭");
    }
  else
    {
      time_t left = g_remind[REMIND_EYE].due - now;

      lv_label_set_text_fmt(g_eye_hint, "%d 分钟后",
                            (int)((left > 0 ? left : 0) + 59) / 60);
    }
}

static void sit_reset_cb(lv_event_t *e)
{
  (void)e;
  sit_reset();
  today_refresh();
}

static void water_add_cb(lv_event_t *e)
{
  (void)e;
  water_add();
  today_refresh();
}

static void today_build(lv_obj_t *tab)
{
  lv_obj_t *left = tab;     /* 竖屏时两栏都是页面本身，从上往下排 */
  lv_obj_t *right = tab;
  lv_obj_t *panel;
  lv_obj_t *box;
  lv_obj_t *lbl;
  int i;

  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab, 24, 0);
  lv_obj_set_style_pad_row(tab, 20, 0);

#if UI_ROTATE != 0
  /* 横屏只有 720 高，竖着叠要滚两屏。分两栏：左边时钟、喝水、用眼，
   * 右边久坐圆环（它最高，单独一栏正好）。
   */

  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_column(tab, 24, 0);

  /* 两栏都撑满整页高度，各有一张卡片拉伸补满（左边喝水、右边久坐），
   * 两栏的上下沿对齐，中间不留空洞。
   */

  left = flex_box(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_width(left, 0);
  lv_obj_set_height(left, LV_PCT(100));
  lv_obj_set_flex_grow(left, 1);
  lv_obj_set_style_pad_row(left, 20, 0);

  right = flex_box(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_width(right, 440);
  lv_obj_set_height(right, LV_PCT(100));
#endif

  /* 时钟：不装卡片，直接放在页面左上 */

  box = flex_box(left, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_hor(box, 8, 0);
  lv_obj_set_style_pad_ver(box, 12, 0);
  lv_obj_set_style_pad_row(box, 8, 0);
  g_today_clock = lv_label_create(box);
  lv_obj_set_style_text_font(g_today_clock, &lv_font_k7_digits_72, 0);
  lv_obj_set_style_text_color(g_today_clock, lv_color_hex(UI_TEXT), 0);
  lv_label_set_text(g_today_clock, "--:--");
  g_today_date = lv_label_create(box);
  lv_obj_set_style_text_font(g_today_date, &lv_font_k7_ui_28, 0);
  lv_obj_set_style_text_color(g_today_date, lv_color_hex(UI_DIM), 0);

  /* 久坐：大圆环，中间是已坐分钟数 */

  panel = panel_create(right);
#if UI_ROTATE != 0
  lv_obj_set_flex_grow(panel, 1);
  lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
#endif
  g_sit_hint = panel_header(panel, ICON_CHAIR, UI_SIT, "久坐");

  g_sit_arc = lv_arc_create(panel);
  lv_obj_set_size(g_sit_arc, 300, 300);
  lv_arc_set_rotation(g_sit_arc, 135);
  lv_arc_set_bg_angles(g_sit_arc, 0, 270);
  lv_arc_set_range(g_sit_arc, 0, 100);
  lv_obj_remove_flag(g_sit_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_opa(g_sit_arc, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_arc_width(g_sit_arc, 22, LV_PART_MAIN);
  lv_obj_set_style_arc_width(g_sit_arc, 22, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(g_sit_arc, lv_color_hex(UI_SURFACE2),
                             LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(g_sit_arc, true, LV_PART_INDICATOR);

  box = flex_box(g_sit_arc, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_width(box, LV_SIZE_CONTENT);
  lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_center(box);
  g_sit_minutes = lv_label_create(box);
  lv_obj_set_style_text_font(g_sit_minutes, &lv_font_k7_digits_48, 0);
  lv_obj_set_style_text_color(g_sit_minutes, lv_color_hex(UI_TEXT), 0);
  lbl = lv_label_create(box);
  lv_label_set_text(lbl, "分钟");
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_DIM), 0);

  /* 圆环底部留着缺口，按钮往上收一点 */

  g_sit_btn = pill_button(panel, "我起来活动了", UI_SIT, false,
                          sit_reset_cb, NULL);
  lv_obj_set_style_margin_top(g_sit_btn, -36, 0);

  /* 喝水：八个杯子 */

  panel = panel_create(left);
#if UI_ROTATE != 0
  lv_obj_set_flex_grow(panel, 1);
  lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
#endif
  g_water_label = panel_header(panel, ICON_TINT, UI_WATER, "喝水");
  lv_obj_set_style_text_font(g_water_label, &lv_font_k7_ui_28, 0);

  box = flex_box(panel, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(box, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  for (i = 0; i < UI_WATER_GOAL; i++)
    {
      lv_obj_t *cup = lv_label_create(box);

      lv_obj_set_size(cup, 64, 64);
      lv_obj_set_style_radius(cup, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_opa(cup, LV_OPA_COVER, 0);
      lv_obj_set_style_text_font(cup, &lv_font_k7_icons_32, 0);
      lv_obj_set_style_text_align(cup, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_pad_top(cup, 16, 0);
      lv_label_set_text(cup, ICON_TINT);
      g_water_cups[i] = cup;
    }

  pill_button(panel, "喝了一杯", UI_WATER, false, water_add_cb, NULL);

  /* 用眼休息：只是个倒计时，到点出顶部横幅 */

  panel = panel_create(left);
  g_eye_hint = panel_header(panel, ICON_EYE, UI_EYE, "用眼休息");
  lv_obj_set_style_text_font(g_eye_hint, &lv_font_k7_ui_28, 0);

  lbl = lv_label_create(left);
  lv_obj_set_width(lbl, LV_PCT(100));
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_DIM), 0);
  lv_obj_set_style_text_opa(lbl, LV_OPA_70, 0);
  lv_label_set_text(lbl,
                    "提醒按计时进行，在座时长从开机或上次点"
                    "“我起来活动了”算起。");

#ifdef CONFIG_NETUTILS_NTPCLIENT
  /* 后台对时。网络还没起来也没关系，客户端自己会重试 */

  ntpc_start();
#endif

  sit_reset();
  g_water_last = mono_now();
  remind_reschedule(REMIND_WATER, g_water_last);
  remind_reschedule(REMIND_EYE, g_water_last);
  today_refresh();
}

/* 家居：经 MQTT 接局域网 broker（k7_home.c）。
 *
 * 磁贴只按设备回报的 state 着色。点一下发 set，磁贴先写"正在打开…"，
 * 等设备回报了才变色；3 秒没回报就恢复原样并提示没响应 —— 不做
 * "按下就当成功"的假反馈。broker 断开时整页变灰、不可点。
 */

#define HOME_N       6
#define HOME_WAIT_US (3 * 1000000ull)
#define HOME_NOTE_US (3 * 1000000ull)
#define HOME_BULB    0xffc857

static const struct
{
  const char *id;
  const char *icon;
  const char *name;
  uint32_t color;
}
g_home_dev[HOME_N] =
{
  { "living_light", ICON_BULB, "客厅灯", HOME_BULB },
  { "desk_lamp",    ICON_BULB, "台灯",   HOME_BULB },
  { "ac",           ICON_SNOW, "空调",   UI_ACCENT },
  { "fresh_air",    ICON_WIND, "新风",   UI_SIT },
  { "kettle",       ICON_PLUG, "饮水机", UI_WATER },
  { "tv",           ICON_TV,   "电视",   UI_EYE },
};

static const char *const g_home_scenes[][3] =
{
  { "home",  ICON_DOOR,  "回家" },
  { "away",  ICON_LEAVE, "离家" },
  { "focus", ICON_BRAIN, "专注" },
  { "rest",  ICON_MOON,  "休息" },
};

static const char *g_home_ids[HOME_N];
static lv_obj_t *g_home_tile[HOME_N];
static lv_obj_t *g_home_icon[HOME_N];
static lv_obj_t *g_home_name[HOME_N];
static lv_obj_t *g_home_word[HOME_N];
static lv_obj_t *g_home_scene[4];
static lv_obj_t *g_home_pill;
static lv_obj_t *g_home_dot;
static lv_obj_t *g_home_link;
static int8_t g_home_want[HOME_N];       /* -1 没有在等的指令 */
static uint64_t g_home_deadline[HOME_N];
static uint32_t g_home_gen = UINT32_MAX;
static char g_home_note[64];
static uint64_t g_home_note_until;
static bool g_home_dirty;

static void home_note(const char *fmt, const char *arg)
{
  snprintf(g_home_note, sizeof(g_home_note), fmt, arg);
  g_home_note_until = now_us() + HOME_NOTE_US;
  g_home_dirty = true;
}

static void home_paint(const struct k7_home_status_s *st)
{
  bool note = g_home_note_until > now_us();
  uint32_t dot;
  int i;

  for (i = 0; i < HOME_N; i++)
    {
      int v = k7_home_state(i);
      bool on = v == 1;
      const char *word;

      if (!st->connected)
        {
          word = "离线";
        }
      else if (g_home_want[i] >= 0)
        {
          word = g_home_want[i] ? "正在打开…" : "正在关闭…";
        }
      else if (v < 0)
        {
          word = "未知";
        }
      else
        {
          word = on ? "开着" : "关着";
        }

      lv_obj_set_style_bg_color(g_home_tile[i],
                                lv_color_hex(on ? g_home_dev[i].color
                                                : UI_CARD), 0);
      lv_obj_set_style_text_color(g_home_icon[i],
                                  lv_color_hex(on ? UI_ON_COLOR
                                                  : g_home_dev[i].color), 0);
      lv_obj_set_style_text_color(g_home_name[i],
                                  lv_color_hex(on ? UI_ON_COLOR : UI_TEXT),
                                  0);
      lv_obj_set_style_text_color(g_home_word[i],
                                  lv_color_hex(on ? UI_ON_COLOR : UI_DIM),
                                  0);
      lv_label_set_text(g_home_word[i], word);
      lv_obj_set_style_opa(g_home_tile[i],
                           st->connected ? LV_OPA_COVER : LV_OPA_50, 0);
      lv_obj_update_flag(g_home_tile[i], LV_OBJ_FLAG_CLICKABLE, st->connected);
    }

  for (i = 0; i < 4; i++)
    {
      lv_obj_set_style_opa(g_home_scene[i],
                           st->connected ? LV_OPA_COVER : LV_OPA_50, 0);
      lv_obj_update_flag(g_home_scene[i], LV_OBJ_FLAG_CLICKABLE,
                      st->connected);
    }

  if (note)
    {
      dot = UI_SIT_OVER;
      lv_label_set_text(g_home_link, g_home_note);
    }
  else if (st->connected)
    {
      dot = UI_OK;
      lv_label_set_text_fmt(g_home_link, "已连接  ·  %s", st->broker);
    }
  else if (st->err == ENOSYS)
    {
      dot = UI_DIM;
      lv_label_set_text(g_home_link, "未配置家庭网关");
    }
  else
    {
      dot = UI_BAD;
      lv_label_set_text_fmt(g_home_link, "离线  ·  正在重连 %s",
                            st->broker);
    }

  lv_obj_set_style_bg_color(g_home_dot, lv_color_hex(dot), 0);
}

static void home_poll(void)
{
  struct k7_home_status_s st;
  uint64_t now;
  int i;

  if (g_home_link == NULL)
    {
      return;
    }

  k7_home_status(&st);
  now = now_us();
  for (i = 0; i < HOME_N; i++)
    {
      if (g_home_want[i] < 0)
        {
          continue;
        }

      if (!st.connected || k7_home_state(i) == g_home_want[i])
        {
          g_home_want[i] = -1;
          g_home_dirty = true;
        }
      else if (now >= g_home_deadline[i])
        {
          g_home_want[i] = -1;
          home_note("%s没有响应", g_home_dev[i].name);
        }
    }

  if (g_home_note_until != 0 && now >= g_home_note_until)
    {
      g_home_note_until = 0;
      g_home_dirty = true;
    }

  if (st.gen != g_home_gen || g_home_dirty)
    {
      g_home_gen = st.gen;
      g_home_dirty = false;
      home_paint(&st);
    }
}

static void home_tile_cb(lv_event_t *e)
{
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  bool on;

  if (g_home_want[i] >= 0)
    {
      return;                            /* 上一次还在等回报 */
    }

  on = k7_home_state(i) != 1;
  if (k7_home_set(i, on) < 0)
    {
      home_note("%s：指令没发出去", g_home_dev[i].name);
      return;
    }

  g_home_want[i] = on;
  g_home_deadline[i] = now_us() + HOME_WAIT_US;
  g_home_dirty = true;
}

static void home_scene_cb(lv_event_t *e)
{
  int i = (int)(intptr_t)lv_event_get_user_data(e);

  home_note(k7_home_scene(g_home_scenes[i][0]) < 0 ? "%s：指令没发出去"
                                                   : "已发出场景：%s",
            g_home_scenes[i][2]);
}

static lv_obj_t *home_tile(lv_obj_t *parent, int32_t w, int32_t h,
                           lv_event_cb_t cb, int i)
{
  lv_obj_t *tile = lv_obj_create(parent);

  lv_obj_set_size(tile, w, h);
  lv_obj_set_style_bg_color(tile, lv_color_hex(UI_CARD), 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_80, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(tile, 0, 0);
  lv_obj_set_style_radius(tile, 28, 0);
  lv_obj_set_style_pad_all(tile, 24, 0);
  lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(tile, cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  return tile;
}

static void home_build(lv_obj_t *tab)
{
  lv_obj_t *row;
  lv_obj_t *grid;
  lv_obj_t *lbl;
  lv_obj_t *tile;
  int i;

  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab, 24, 0);
  lv_obj_set_style_pad_row(tab, 20, 0);

  /* 标题行：左边"家居"，右边连接状态胶囊 */

  row = flex_box(tab, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 14, 0);
  icon_label(row, ICON_HOME, &lv_font_k7_icons_32, UI_ACCENT);
  lbl = lv_label_create(row);
  lv_label_set_text(lbl, "家居");
  lv_obj_set_style_text_font(lbl, &lv_font_k7_ui_28, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_TEXT), 0);
  lv_obj_set_flex_grow(lbl, 1);

  g_home_pill = flex_box(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_width(g_home_pill, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(g_home_pill, lv_color_hex(UI_CARD), 0);
  lv_obj_set_style_bg_opa(g_home_pill, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(g_home_pill, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_pad_hor(g_home_pill, 20, 0);
  lv_obj_set_style_pad_ver(g_home_pill, 10, 0);
  lv_obj_set_style_pad_column(g_home_pill, 10, 0);

  g_home_dot = lv_obj_create(g_home_pill);
  lv_obj_remove_style_all(g_home_dot);
  lv_obj_set_size(g_home_dot, 12, 12);
  lv_obj_set_style_radius(g_home_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(g_home_dot, LV_OPA_COVER, 0);
  g_home_link = lv_label_create(g_home_pill);
  lv_obj_set_style_text_color(g_home_link, lv_color_hex(UI_DIM), 0);

  /* 设备：横屏 3x2，竖屏 2x3 */

  grid = flex_box(tab, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(grid, 16, 0);

  for (i = 0; i < HOME_N; i++)
    {
      tile = home_tile(grid, LV_PCT(UI_ROTATE == 0 ? 48 : 32),
                       UI_ROTATE == 0 ? 170 : 190, home_tile_cb, i);
      g_home_tile[i] = tile;
      g_home_icon[i] = icon_label(tile, g_home_dev[i].icon,
                                  &lv_font_k7_icons_32,
                                  g_home_dev[i].color);
      lv_obj_align(g_home_icon[i], LV_ALIGN_TOP_LEFT, 0, 0);

      g_home_name[i] = lv_label_create(tile);
      lv_label_set_text(g_home_name[i], g_home_dev[i].name);
      lv_obj_set_style_text_font(g_home_name[i], &lv_font_k7_ui_28, 0);
      lv_obj_align(g_home_name[i], LV_ALIGN_BOTTOM_LEFT, 0, -32);

      g_home_word[i] = lv_label_create(tile);
      lv_obj_align(g_home_word[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);

      g_home_ids[i] = g_home_dev[i].id;
      g_home_want[i] = -1;
    }

  /* 场景：broker 收到 home/scene/set 后由网关那边编排各设备 */

  lbl = lv_label_create(tab);
  lv_obj_set_width(lbl, LV_PCT(100));
  lv_label_set_text(lbl, "常用场景");
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_DIM), 0);
  lv_obj_set_style_pad_left(lbl, 8, 0);

  grid = flex_box(tab, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(grid, 16, 0);

  for (i = 0; i < 4; i++)
    {
      tile = home_tile(grid, LV_PCT(UI_ROTATE == 0 ? 48 : 23), 96,
                       home_scene_cb, i);
      lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER,
                            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(tile, 14, 0);
      icon_label(tile, g_home_scenes[i][1], &lv_font_k7_icons_32,
                 UI_ACCENT);
      lbl = lv_label_create(tile);
      lv_label_set_text(lbl, g_home_scenes[i][2]);
      lv_obj_set_style_text_font(lbl, &lv_font_k7_ui_28, 0);
      lv_obj_set_style_text_color(lbl, lv_color_hex(UI_TEXT), 0);
      g_home_scene[i] = tile;
    }

  /* 网络还没起来也没关系，客户端线程自己会重试 */

  k7_home_start(g_home_ids, HOME_N);
  home_poll();
}

/* 工作：录制模式（k7_studio）。速记在右栏，下一步加。 */

#define WORK_TAB        2                /* 在主页签里的位置 */
#define WORK_SHOW_MARKS 8

static lv_obj_t *g_work_btn;
static lv_obj_t *g_work_btn_icon;
static lv_obj_t *g_work_btn_text;
static lv_obj_t *g_work_time;
static lv_obj_t *g_work_hint;
static lv_obj_t *g_work_note;
static lv_obj_t *g_work_stat;
static lv_obj_t *g_work_marks;
static uint32_t g_work_gen = UINT32_MAX;
static uint32_t g_work_secs = UINT32_MAX;
static uint32_t g_work_rings;
static bool g_work_rings_seen;

static void work_paint(void)
{
  struct k7_studio_status_s st;
  struct k7_studio_mark_s m[K7_STUDIO_MAXMARK];
  char buf[WORK_SHOW_MARKS * 40];
  size_t o = 0;
  int n;
  int i;

  k7_studio_status(&st);
  g_work_gen = st.gen;
  g_work_secs = st.elapsed_s;

  lv_obj_set_style_bg_color(g_work_btn,
                            lv_color_hex(st.onair ? UI_BAD : UI_SURFACE2),
                            0);
  lv_obj_set_style_text_color(g_work_btn_icon,
                              lv_color_hex(st.onair ? UI_ON_COLOR : UI_BAD),
                              0);
  lv_obj_set_style_text_color(g_work_btn_text,
                              lv_color_hex(st.onair ? UI_ON_COLOR : UI_TEXT),
                              0);
  lv_label_set_text(g_work_btn_text, st.onair ? "结束录制" : "开始录制");
  if (st.onair)
    {
      lv_obj_remove_flag(g_work_time, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text_fmt(g_work_time, "%02u:%02u",
                            (unsigned)(st.elapsed_s / 60),
                            (unsigned)(st.elapsed_s % 60));
    }
  else
    {
      lv_obj_add_flag(g_work_time, LV_OBJ_FLAG_HIDDEN);
    }

  if (st.onair && !st.listening)
    {
      lv_label_set_text(g_work_hint, "连不上 Linux：不能噪声打点");
    }
  else
    {
      lv_label_set_text(g_work_hint, st.onair ? "录制中，语音不上传" : "");
    }

  lv_label_set_text(g_work_note, st.note[0] ? st.note :
                    "开始时打开台灯补光，关掉空调、新风、电视、饮水机，"
                    "门口显示\"录制中\"；结束后把动过的设备恢复原样。");

  if (st.onair || st.nmarks > 0)
    {
      lv_label_set_text_fmt(g_work_stat, "底噪 %d  ·  打点 %d 处  ·  门铃 %d 次",
                            st.floor, st.nmarks, st.rings);
    }
  else
    {
      lv_label_set_text(g_work_stat, "");
    }

  /* 最近几个点，最新的在上 */

  n = k7_studio_marks(m, K7_STUDIO_MAXMARK);
  buf[0] = '\0';
  for (i = n - 1; i >= 0 && i >= n - WORK_SHOW_MARKS; i--)
    {
      o += snprintf(buf + o, sizeof(buf) - o, "%s%02u:%02u   ",
                    o ? "\n" : "",
                    (unsigned)(m[i].at_ms / 60000),
                    (unsigned)(m[i].at_ms / 1000 % 60));
      if (o < sizeof(buf))
        {
          o += snprintf(buf + o, sizeof(buf) - o,
                        m[i].kind == K7_STUDIO_BELL ? "门铃" :
                        "噪声  %u.%u 秒",
                        (unsigned)(m[i].len_ms / 1000),
                        (unsigned)(m[i].len_ms / 100 % 10));
        }
      if (o >= sizeof(buf))
        {
          break;
        }
    }

  lv_label_set_text(g_work_marks, buf);
}

static void work_toggle(void)
{
  struct k7_studio_status_s st;

  k7_studio_status(&st);
  if (st.onair)
    {
      k7_studio_stop();
    }
  else
    {
      k7_studio_start();
    }

  work_paint();
}

static void work_btn_cb(lv_event_t *e)
{
  (void)e;
  work_toggle();
}

static void work_poll(void)
{
  struct k7_studio_status_s st;
  struct k7_home_status_s hs;
  FILE *fp;

  if (g_work_btn == NULL)
    {
      return;
    }

  k7_studio_poll();
  memo_poll();

  /* 门铃：录制中由 k7_studio 代答；不在录制就提示并念一句 */

  k7_home_status(&hs);
  if (!g_work_rings_seen)
    {
      g_work_rings = hs.rings;
      g_work_rings_seen = true;
    }

  while (g_work_rings != hs.rings)
    {
      g_work_rings++;
      if (!k7_studio_ring())
        {
          home_note("%s", "门铃响了");
          if (g_tts_enabled)
            {
              tts_start("有人按门铃。");
            }
        }
    }

  /* echo 1 / 0 > /tmp/k7-studio：开始 / 结束录制（调试入口） */

  fp = fopen("/tmp/k7-studio", "r");
  if (fp != NULL)
    {
      int on = 0;

      if (fscanf(fp, "%d", &on) == 1)
        {
          k7_studio_status(&st);
          if ((on != 0) != st.onair)
            {
              work_toggle();
            }
        }

      fclose(fp);
      unlink("/tmp/k7-studio");
    }

  k7_studio_status(&st);
  if (st.gen != g_work_gen || st.elapsed_s != g_work_secs)
    {
      work_paint();
    }
}


/* 速记：右栏列最近几条，点开看全文、可删除 */

#define MEMO_SHOW 6

static lv_obj_t *g_memo_list;
static lv_obj_t *g_memo_hint;
static lv_obj_t *g_memo_box;
static uint32_t g_memo_box_id;
static uint32_t g_memo_gen = UINT32_MAX;

static uint32_t memo_color(const char *type)
{
  if (strcmp(type, "选题") == 0)
    {
      return UI_ACCENT;
    }
  else if (strcmp(type, "待办") == 0)
    {
      return UI_SIT_OVER;
    }
  else if (strcmp(type, "客户") == 0)
    {
      return UI_EYE;
    }
  else if (strcmp(type, "灵感") == 0)
    {
      return UI_WATER;
    }

  return UI_DIM;
}

static void memo_when(char *buf, size_t cap, time_t ts)
{
  struct tm tm;
  time_t t = ts + UI_TZ_OFFSET_S;

  gmtime_r(&t, &tm);
  snprintf(buf, cap, "%02d-%02d %02d:%02d", tm.tm_mon + 1, tm.tm_mday,
           tm.tm_hour, tm.tm_min);
}

static void memo_box_cb(lv_event_t *e)
{
  bool del = (bool)(intptr_t)lv_event_get_user_data(e);

  if (del)
    {
      k7_memo_delete(g_memo_box_id);
    }

  lv_msgbox_close(g_memo_box);
  g_memo_box = NULL;
}

static void memo_row_cb(lv_event_t *e)
{
  uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
  struct k7_memo_s m;
  char when[48];
  char *body;
  lv_obj_t *btn;
  int i;

  for (i = 0; k7_memo_get(i, &m); i++)
    {
      if (m.id == id)
        {
          break;
        }
    }

  if (m.id != id || g_memo_box != NULL)
    {
      return;
    }

  body = malloc(sizeof(m.text) + 160);
  if (body == NULL)
    {
      return;
    }

  memo_when(when, sizeof(when), m.ts);
  snprintf(body, sizeof(m.text) + 160, "%s\n\n%s%s%s%s", m.text, when,
           m.tags[0] ? "  ·  " : "", m.tags,
           m.has_wav ? "  ·  有原始录音" : "");

  g_memo_box_id = id;
  g_memo_box = lv_msgbox_create(NULL);
  lv_obj_set_width(g_memo_box, LV_PCT(70));
  lv_msgbox_add_title(g_memo_box, m.title[0] ? m.title : "速记");
  lv_msgbox_add_text(g_memo_box, body);
  free(body);

  /* 默认的页脚按钮只有一指宽，放大到好按 */

  btn = lv_msgbox_add_footer_button(g_memo_box, "删除");
  lv_obj_set_size(btn, 180, 64);
  lv_obj_set_style_text_font(btn, &lv_font_k7_ui_28, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(UI_BAD), 0);
  lv_obj_add_event_cb(btn, memo_box_cb, LV_EVENT_CLICKED, (void *)1);
  btn = lv_msgbox_add_footer_button(g_memo_box, "关闭");
  lv_obj_set_size(btn, 180, 64);
  lv_obj_set_style_text_font(btn, &lv_font_k7_ui_28, 0);
  lv_obj_add_event_cb(btn, memo_box_cb, LV_EVENT_CLICKED, (void *)0);

  /* ★ 页脚在第一次 add_footer_button 时才创建，之前取到的是 NULL ——
   *   先取再设高度，板子当场空指针 panic（2026-11-02 实测整板挂死）。
   */

  lv_obj_set_height(lv_msgbox_get_footer(g_memo_box), LV_SIZE_CONTENT);
  lv_obj_set_style_pad_ver(lv_msgbox_get_footer(g_memo_box), 16, 0);
}

static void memo_paint(void)
{
  struct k7_memo_info_s info;
  struct k7_memo_s m;
  int i;

  k7_memo_info(&info);
  g_memo_gen = info.gen;

  if (!info.persistent)
    {
      lv_label_set_text(g_memo_hint, "/data 没挂上，重启会丢");
    }
  else if (info.pending > 0)
    {
      lv_label_set_text_fmt(g_memo_hint, "共 %d 条，%d 条整理中", info.count,
                            info.pending);
    }
  else
    {
      lv_label_set_text_fmt(g_memo_hint, "共 %d 条", info.count);
    }

  lv_obj_clean(g_memo_list);
  for (i = 0; i < MEMO_SHOW && k7_memo_get(i, &m); i++)
    {
      lv_obj_t *row = lv_obj_create(g_memo_list);
      lv_obj_t *line;
      lv_obj_t *lbl;
      char when[48];
      char head[80];
      const char *type = m.state == K7_MEMO_TAGGED ? m.type : "未分类";

      lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
      lv_obj_set_style_bg_color(row, lv_color_hex(UI_SURFACE2), 0);
      lv_obj_set_style_bg_opa(row, LV_OPA_70, LV_STATE_PRESSED);
      lv_obj_set_style_border_width(row, 0, 0);
      lv_obj_set_style_radius(row, 18, 0);
      lv_obj_set_style_pad_all(row, 14, 0);
      lv_obj_set_style_pad_row(row, 6, 0);
      lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
      lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_event_cb(row, memo_row_cb, LV_EVENT_CLICKED,
                          (void *)(uintptr_t)m.id);

      line = flex_box(row, LV_FLEX_FLOW_ROW);
      lv_obj_set_style_pad_column(line, 10, 0);
      lv_obj_add_flag(line, LV_OBJ_FLAG_EVENT_BUBBLE);

      lbl = lv_label_create(line);
      lv_label_set_text(lbl, type);
      lv_obj_set_style_bg_color(lbl, lv_color_hex(memo_color(type)), 0);
      lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
      lv_obj_set_style_text_color(lbl, lv_color_hex(UI_ON_COLOR), 0);
      lv_obj_set_style_radius(lbl, 8, 0);
      lv_obj_set_style_pad_hor(lbl, 8, 0);
      lv_obj_set_style_pad_ver(lbl, 2, 0);

      /* 标题没出来之前，用原话开头顶着 */

      if (m.state == K7_MEMO_TAGGED && m.title[0])
        {
          snprintf(head, sizeof(head), "%s", m.title);
        }
      else
        {
          utf8_copy(head, 40, m.text);
        }

      lbl = lv_label_create(line);
      lv_label_set_text(lbl, head);
      lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
      lv_obj_set_flex_grow(lbl, 1);
      lv_obj_set_style_text_color(lbl, lv_color_hex(UI_TEXT), 0);

      memo_when(when, sizeof(when), m.ts);
      lbl = lv_label_create(row);
      lv_obj_set_width(lbl, LV_PCT(100));
      lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
      lv_obj_set_style_text_color(lbl, lv_color_hex(UI_DIM), 0);
      lv_label_set_text_fmt(lbl, "%s%s%s", when,
                            m.state == K7_MEMO_PENDING ? "  ·  整理中…" :
                            m.tags[0] ? "  ·  " : "",
                            m.state == K7_MEMO_PENDING ? "" : m.tags);
    }

  if (i == 0)
    {
      lv_obj_t *lbl = lv_label_create(g_memo_list);

      lv_obj_set_width(lbl, LV_PCT(100));
      lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
      lv_obj_set_style_text_color(lbl, lv_color_hex(UI_DIM), 0);
      lv_label_set_text(lbl, "说\"你好 openvela，记一下……\"，"
                             "想法就记在这里，并自动分类。");
    }
}

static void memo_poll(void)
{
  struct k7_memo_info_s info;

  if (g_memo_list == NULL)
    {
      return;
    }

  /* echo 文字 > /tmp/k7-memo：直接记一条（调试入口，没有录音） */

  {
    FILE *fp = fopen("/tmp/k7-memo", "r");

    if (fp != NULL)
      {
        char buf[480];
        size_t n = fread(buf, 1, sizeof(buf) - 1, fp);

        fclose(fp);
        unlink("/tmp/k7-memo");
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
          {
            n--;
          }

        buf[n] = '\0';
        k7_memo_add(buf, NULL);
      }
  }

  k7_memo_info(&info);
  if (info.gen != g_memo_gen)
    {
      memo_paint();
    }
}

/* "记一下……"：从原话（带标点）里取关键词后面的部分。返回 false 表示
 * 不是速记指令。
 */

static bool memo_voice(const char *text, const char *question)
{
  static const char *const keys[] =
  {
    "记一下", "记下来", "帮我记", "备忘一下", "备忘", "记一笔", NULL
  };

  static const char *const skip[] =
  {
    "，", "。", "：", "、", "！", "？", ",", ".", ":", " ", NULL
  };

  char wav[32];
  const char *p = NULL;
  int k;

  for (k = 0; keys[k] != NULL; k++)
    {
      if (strstr(question, keys[k]) != NULL)
        {
          p = strstr(text, keys[k]);
          break;
        }
    }

  if (keys[k] == NULL)
    {
      return false;
    }

  assist_bubble(true, text);
  if (p != NULL)
    {
      p += strlen(keys[k]);
      for (k = 0; skip[k] != NULL; k++)
        {
          if (strncmp(p, skip[k], strlen(skip[k])) == 0)
            {
              p += strlen(skip[k]);
              k = -1;                      /* 连着几个标点都跳过 */
            }
        }
    }

  if (p == NULL || strlen(p) < 6)
    {
      assist_bubble(false, "要记什么？连着说，比如："
                           "你好 openvela，记一下，下期讲开箱。");
      return true;
    }

  k7_wake_event_wav(wav, sizeof(wav));
  if (k7_memo_add(p, wav) < 0)
    {
      assist_bubble(false, "没记上，请再说一次。");
      return true;
    }

  assist_bubble(false, "记下了，正在整理分类。");
  if (g_tts_enabled)
    {
      tts_start("记下了。");
    }

  lv_tabview_set_active(g_main_tabview, WORK_TAB, LV_ANIM_ON);
  return true;
}

static bool work_voice(const char *text, const char *question)
{
  struct k7_studio_status_s st;
  bool start = strstr(question, "开始录") != NULL;
  bool stop = strstr(question, "结束录") != NULL ||
              strstr(question, "停止录") != NULL;

  if (memo_voice(text, question))
    {
      return true;
    }

  if (!start && !stop)
    {
      return false;
    }

  k7_studio_status(&st);
  assist_bubble(true, text);
  if (start == st.onair)
    {
      assist_bubble(false, st.onair ? "已经在录了。" : "现在没在录。");
      return true;
    }

  work_toggle();
  k7_studio_status(&st);
  assist_bubble(false, st.note);
  lv_tabview_set_active(g_main_tabview, WORK_TAB, LV_ANIM_ON);
  return true;
}

static void work_build(lv_obj_t *tab)
{
  lv_obj_t *left = tab;
  lv_obj_t *right = tab;
  lv_obj_t *panel;

  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab, 24, 0);
  lv_obj_set_style_pad_row(tab, 20, 0);

#if UI_ROTATE != 0
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_column(tab, 24, 0);

  left = flex_box(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_width(left, 0);
  lv_obj_set_height(left, LV_PCT(100));
  lv_obj_set_flex_grow(left, 1);
  lv_obj_set_style_pad_row(left, 20, 0);

  right = flex_box(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_width(right, 440);
  lv_obj_set_height(right, LV_PCT(100));
  lv_obj_set_style_pad_row(right, 20, 0);
#endif

  /* 录制 */

  panel = panel_create(left);
#if UI_ROTATE != 0
  lv_obj_set_flex_grow(panel, 1);
#endif
  g_work_hint = panel_header(panel, ICON_VIDEO, UI_BAD, "录制");

  g_work_btn = lv_button_create(panel);
  lv_obj_set_size(g_work_btn, LV_PCT(100), 140);
  lv_obj_set_style_radius(g_work_btn, 28, 0);
  lv_obj_set_style_shadow_width(g_work_btn, 0, 0);
  lv_obj_set_style_pad_hor(g_work_btn, 36, 0);
  lv_obj_add_event_cb(g_work_btn, work_btn_cb, LV_EVENT_CLICKED, NULL);

  g_work_btn_icon = icon_label(g_work_btn, ICON_VIDEO, &lv_font_k7_icons_32,
                               UI_BAD);
  lv_obj_align(g_work_btn_icon, LV_ALIGN_LEFT_MID, 0, 0);
  g_work_btn_text = lv_label_create(g_work_btn);
  lv_obj_set_style_text_font(g_work_btn_text, &lv_font_k7_ui_28, 0);
  lv_obj_align(g_work_btn_text, LV_ALIGN_LEFT_MID, 60, 0);

  g_work_time = lv_label_create(g_work_btn);
  lv_obj_set_style_text_font(g_work_time, &lv_font_k7_digits_48, 0);
  lv_obj_set_style_text_color(g_work_time, lv_color_hex(UI_ON_COLOR), 0);
  lv_obj_align(g_work_time, LV_ALIGN_RIGHT_MID, 0, 0);

  g_work_note = lv_label_create(panel);
  lv_obj_set_width(g_work_note, LV_PCT(100));
  lv_label_set_long_mode(g_work_note, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(g_work_note, lv_color_hex(UI_TEXT), 0);

  g_work_stat = lv_label_create(panel);
  lv_obj_set_width(g_work_stat, LV_PCT(100));
  lv_obj_set_style_text_color(g_work_stat, lv_color_hex(UI_DIM), 0);

  g_work_marks = lv_label_create(panel);
  lv_obj_set_width(g_work_marks, LV_PCT(100));
  lv_obj_set_style_text_color(g_work_marks, lv_color_hex(UI_SIT_OVER), 0);

  /* 速记 */

  panel = panel_create(right);
#if UI_ROTATE != 0
  lv_obj_set_flex_grow(panel, 1);
#endif
  lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  g_memo_hint = panel_header(panel, ICON_NOTE, UI_WATER, "速记");
  g_memo_list = flex_box(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(g_memo_list, 12, 0);
  lv_obj_set_flex_grow(g_memo_list, 1);
  lv_obj_add_flag(g_memo_list, LV_OBJ_FLAG_SCROLLABLE);

  k7_memo_start();
  memo_paint();
  work_paint();
}

static void remind_switch_cb(lv_event_t *e)
{
  lv_obj_t *sw = lv_event_get_target(e);
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);

  if (idx < 0)
    {
      g_remind_voice = on;
      return;
    }

  g_remind[idx].enabled = on;
  g_remind[idx].off_until = 0;

  /* 重新打开时从现在起算，不要一打开就因为"早就到期"立刻弹出来。
   * 久坐例外：在座时长是真实累计的，按坐下时刻起算。
   */

  remind_reschedule(idx, idx == REMIND_SIT ? g_sit_start : mono_now());
}

static void remind_interval_cb(lv_event_t *e)
{
  lv_obj_t *dd = lv_event_get_target(e);
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  uint32_t sel = lv_dropdown_get_selected(dd);

  if (idx == REMIND_SIT && sel < sizeof(g_sit_choices) / sizeof(int))
    {
      g_remind[idx].interval_min = g_sit_choices[sel];
      remind_reschedule(idx, g_sit_start);
    }
  else if (idx == REMIND_WATER &&
           sel < sizeof(g_water_choices) / sizeof(int))
    {
      g_remind[idx].interval_min = g_water_choices[sel];
      remind_reschedule(idx, g_water_last);
    }
}

/* 设置页：分组列表。组标题在卡片外面，组内每行 88px，行间细分隔线 */

static lv_obj_t *settings_group(lv_obj_t *tab, const char *title)
{
  lv_obj_t *lbl = lv_label_create(tab);
  lv_obj_t *group;

  lv_label_set_text(lbl, title);
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_DIM), 0);
  lv_obj_set_style_pad_left(lbl, 12, 0);
  lv_obj_set_style_margin_top(lbl, 12, 0);

  group = panel_create(tab);
  lv_obj_set_style_pad_ver(group, 4, 0);
  lv_obj_set_style_pad_row(group, 0, 0);
  return group;
}

/* 组里最后一行不要分隔线 */

static void settings_group_end(lv_obj_t *group)
{
  lv_obj_t *last = lv_obj_get_child(group, -1);

  if (last != NULL)
    {
      lv_obj_set_style_border_width(last, 0, 0);
    }
}

static lv_obj_t *settings_row(lv_obj_t *group, const char *icon,
                              uint32_t color, const char *name)
{
  lv_obj_t *row = flex_box(group, LV_FLEX_FLOW_ROW);
  lv_obj_t *lbl;

  lv_obj_set_height(row, 88);
  lv_obj_set_style_pad_column(row, 18, 0);
  lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(row, 1, 0);
  lv_obj_set_style_border_color(row, lv_color_hex(UI_SURFACE2), 0);

  lbl = icon_label(row, icon, &lv_font_k7_icons_32, color);
  lv_obj_set_width(lbl, 40);
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);

  lbl = lv_label_create(row);
  lv_label_set_text(lbl, name);
  lv_obj_set_style_text_font(lbl, &lv_font_k7_ui_28, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_TEXT), 0);
  lv_obj_set_flex_grow(lbl, 1);
  return row;
}

/* 一行提醒设置：（可选的）间隔下拉框 + 开关 */

static void setting_row(lv_obj_t *group, const char *icon, uint32_t color,
                        const char *name, int idx, const char *options,
                        int selected, bool on)
{
  lv_obj_t *row = settings_row(group, icon, color, name);
  lv_obj_t *dd;
  lv_obj_t *sw;

  if (options != NULL)
    {
      dd = lv_dropdown_create(row);
      lv_dropdown_set_options(dd, options);
      lv_dropdown_set_selected(dd, selected);
      lv_obj_set_size(dd, 168, 56);
      lv_obj_set_style_bg_color(dd, lv_color_hex(UI_SURFACE2), 0);
      lv_obj_set_style_border_width(dd, 0, 0);
      lv_obj_set_style_radius(dd, 16, 0);
      lv_obj_set_style_text_color(dd, lv_color_hex(UI_TEXT), 0);
      lv_obj_add_event_cb(dd, remind_interval_cb, LV_EVENT_VALUE_CHANGED,
                          (void *)(intptr_t)idx);
    }

  sw = lv_switch_create(row);
  lv_obj_set_size(sw, 84, 46);
  lv_obj_set_style_bg_color(sw, lv_color_hex(UI_SURFACE2), LV_PART_MAIN);
  lv_obj_set_style_bg_color(sw, lv_color_hex(color),
                            LV_PART_INDICATOR | LV_STATE_CHECKED);
  if (on)
    {
      lv_obj_add_state(sw, LV_STATE_CHECKED);
    }

  lv_obj_add_event_cb(sw, remind_switch_cb, LV_EVENT_VALUE_CHANGED,
                      (void *)(intptr_t)idx);
}

static void dev_open_cb(lv_event_t *e)
{
  (void)e;
  lv_obj_remove_flag(g_dev_panel, LV_OBJ_FLAG_HIDDEN);
}

static void dev_close_cb(lv_event_t *e)
{
  (void)e;
  lv_obj_add_flag(g_dev_panel, LV_OBJ_FLAG_HIDDEN);
}

/* ---- 设置 → WLAN / 蓝牙 ----
 *
 * 一个全屏浮层（和开发者工具同一种做法），里面两块内容按入口切换。
 * 后台的启动 / 扫描 / 连接都在 k7_wireless.c 的线程里；这里只发请求、
 * 每个 tick 取一次状态快照，gen 变了才重画列表。
 *
 * 网络名、设备名是任意字符，用 lv_font_k7_ui_28（ASCII）退回全集的
 * lv_font_k7_cjk_20 显示。键盘按键要 LVGL 的几个符号，那几个字形放在
 * 图标字库里（scripts/gen-icon-font.sh），键盘用"图标字库 → 28px 界面
 * 字库"这一串。
 */

static lv_obj_t *g_wl_panel;
static lv_obj_t *g_wl_title;
static lv_obj_t *g_wl_wifi_box;
static lv_obj_t *g_wl_bt_box;
static lv_obj_t *g_wifi_sw;
static lv_obj_t *g_wifi_info;
static lv_obj_t *g_wifi_disc_btn;
static lv_obj_t *g_wifi_list;
static lv_obj_t *g_bt_sw;
static lv_obj_t *g_bt_info;
static lv_obj_t *g_bt_list;
static lv_obj_t *g_wifi_row_info;       /* 设置页那一行右边的状态字 */
static lv_obj_t *g_bt_row_info;
static lv_obj_t *g_pw_layer;
static lv_obj_t *g_pw_ta;
static char      g_pw_ssid[33];
static lv_font_t g_kb_font;
static bool      g_wifi_want;
static bool      g_bt_want;
static uint32_t  g_wifi_gen = UINT32_MAX;
static uint32_t  g_bt_gen = UINT32_MAX;
static struct k7_wifi_status_s g_wifi_st;
static struct k7_bt_status_s   g_bt_st;

static void wl_open(bool bt)
{
  lv_label_set_text(g_wl_title, bt ? "蓝牙" : "WLAN");
  if (bt)
    {
      lv_obj_add_flag(g_wl_wifi_box, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(g_wl_bt_box, LV_OBJ_FLAG_HIDDEN);
    }
  else
    {
      lv_obj_remove_flag(g_wl_wifi_box, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(g_wl_bt_box, LV_OBJ_FLAG_HIDDEN);
    }

  lv_obj_remove_flag(g_wl_panel, LV_OBJ_FLAG_HIDDEN);
}

static void wl_open_cb(lv_event_t *e)
{
  wl_open((bool)(intptr_t)lv_event_get_user_data(e));
}

static void wl_close_cb(lv_event_t *e)
{
  (void)e;
  lv_obj_add_flag(g_wl_panel, LV_OBJ_FLAG_HIDDEN);
}

/* 信号强弱的颜色：-60 dBm 以上算好，-75 以下算弱 */

static uint32_t wl_signal_color(int dbm)
{
  return dbm >= -60 ? UI_OK : dbm >= -75 ? UI_ACCENT : UI_DIM;
}

static const char *wl_signal_text(int dbm)
{
  return dbm >= -60 ? "信号强" : dbm >= -75 ? "信号中" : "信号弱";
}

/* 列表的一行：图标 + 名字（占满）+ 右侧若干小字。返回行对象。 */

static lv_obj_t *wl_list_row(lv_obj_t *list, const char *icon,
                             uint32_t color, const char *name)
{
  lv_obj_t *row = settings_row(list, icon, color, name);
  lv_obj_t *lbl = lv_obj_get_child(row, 1);

  lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_height(row, 76);
  return row;
}

static lv_obj_t *wl_tag(lv_obj_t *row, const char *text, uint32_t color)
{
  lv_obj_t *lbl = lv_label_create(row);

  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
  return lbl;
}

/* ---- 密码输入 ---- */

static void pw_close(void)
{
  if (g_pw_layer != NULL)
    {
      lv_obj_delete(g_pw_layer);
      g_pw_layer = NULL;
      g_pw_ta = NULL;
    }
}

static void pw_connect(void)
{
  const char *psk = lv_textarea_get_text(g_pw_ta);

  if (strlen(psk) < 8)
    {
      lv_textarea_set_placeholder_text(g_pw_ta, "密码至少 8 位");
      lv_textarea_set_text(g_pw_ta, "");
      return;
    }

  k7_wifi_connect(g_pw_ssid, psk);
  pw_close();
}

static void pw_btn_cb(lv_event_t *e)
{
  if ((bool)(intptr_t)lv_event_get_user_data(e))
    {
      pw_connect();
    }
  else
    {
      pw_close();
    }
}

static void pw_kb_cb(lv_event_t *e)
{
  if (lv_event_get_code(e) == LV_EVENT_READY)
    {
      pw_connect();
    }
  else if (lv_event_get_code(e) == LV_EVENT_CANCEL)
    {
      pw_close();
    }
}

static void pw_show_cb(lv_event_t *e)
{
  lv_obj_t *cb = lv_event_get_target(e);

  lv_textarea_set_password_mode(g_pw_ta,
                                !lv_obj_has_state(cb, LV_STATE_CHECKED));
}

/* 顶层图层上：上半是卡片（网络名、输入框、按钮），下半是键盘 */

static void pw_open(const char *ssid)
{
  lv_obj_t *card;
  lv_obj_t *row;
  lv_obj_t *lbl;
  lv_obj_t *cb;
  lv_obj_t *kb;
  lv_obj_t *btn;

  pw_close();
  strlcpy(g_pw_ssid, ssid, sizeof(g_pw_ssid));

  g_pw_layer = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(g_pw_layer);
  lv_obj_set_size(g_pw_layer, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(g_pw_layer, lv_color_hex(UI_BG), 0);
  lv_obj_set_style_bg_opa(g_pw_layer, LV_OPA_COVER, 0);
  lv_obj_add_flag(g_pw_layer, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_flex_flow(g_pw_layer, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(g_pw_layer, 24, 0);
  lv_obj_set_style_pad_row(g_pw_layer, 16, 0);

  card = panel_create(g_pw_layer);
  lv_obj_set_style_pad_all(card, 20, 0);
  lv_obj_set_style_pad_row(card, 14, 0);

  lbl = lv_label_create(card);
  lv_label_set_text_fmt(lbl, "输入「%s」的密码", ssid);
  lv_obj_set_style_text_font(lbl, &lv_font_k7_ui_28, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_TEXT), 0);

  row = flex_box(card, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 16, 0);

  g_pw_ta = lv_textarea_create(row);
  lv_textarea_set_one_line(g_pw_ta, true);
  lv_textarea_set_password_mode(g_pw_ta, true);
  lv_textarea_set_max_length(g_pw_ta, 63);
  lv_textarea_set_placeholder_text(g_pw_ta, "WLAN 密码");
  lv_obj_set_flex_grow(g_pw_ta, 1);
  lv_obj_set_style_text_font(g_pw_ta, &lv_font_k7_ui_28, 0);
  lv_obj_set_style_bg_color(g_pw_ta, lv_color_hex(UI_SURFACE2), 0);
  lv_obj_set_style_text_color(g_pw_ta, lv_color_hex(UI_TEXT), 0);
  lv_obj_set_style_border_width(g_pw_ta, 0, 0);
  lv_obj_add_state(g_pw_ta, LV_STATE_FOCUSED);

  cb = lv_checkbox_create(row);
  lv_checkbox_set_text(cb, "显示");
  lv_obj_set_style_text_color(cb, lv_color_hex(UI_TEXT), 0);
  lv_obj_add_event_cb(cb, pw_show_cb, LV_EVENT_VALUE_CHANGED, NULL);

  btn = lv_button_create(row);
  lv_obj_set_style_bg_color(btn, lv_color_hex(UI_SURFACE2), 0);
  lv_label_set_text(lv_label_create(btn), "取消");
  lv_obj_add_event_cb(btn, pw_btn_cb, LV_EVENT_CLICKED, (void *)0);

  btn = lv_button_create(row);
  lv_obj_set_style_bg_color(btn, lv_color_hex(UI_ACCENT), 0);
  lv_obj_set_style_text_color(btn, lv_color_hex(UI_ON_COLOR), 0);
  lv_label_set_text(lv_label_create(btn), "连接");
  lv_obj_add_event_cb(btn, pw_btn_cb, LV_EVENT_CLICKED, (void *)1);

  kb = lv_keyboard_create(g_pw_layer);
  lv_obj_set_width(kb, LV_PCT(100));
  lv_obj_set_flex_grow(kb, 1);
  lv_obj_set_style_text_font(kb, &g_kb_font, LV_PART_ITEMS);
  lv_keyboard_set_textarea(kb, g_pw_ta);
  lv_obj_add_event_cb(kb, pw_kb_cb, LV_EVENT_READY, NULL);
  lv_obj_add_event_cb(kb, pw_kb_cb, LV_EVENT_CANCEL, NULL);
}

/* ---- WLAN ---- */

static void wifi_sw_cb(lv_event_t *e)
{
  g_wifi_want = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
  k7_wifi_enable(g_wifi_want);
}

static void wifi_scan_cb(lv_event_t *e)
{
  (void)e;
  if (g_wifi_want)
    {
      k7_wifi_scan();
    }
}

static void wifi_disc_cb(lv_event_t *e)
{
  (void)e;
  k7_wifi_disconnect();
}

static void wifi_forget_cb(lv_event_t *e)
{
  int i = (int)(intptr_t)lv_event_get_user_data(e);

  if (i < g_wifi_st.nap)
    {
      if (g_wifi_st.state == K7_WL_CONNECTED &&
          strcmp(g_wifi_st.ssid, g_wifi_st.ap[i].ssid) == 0)
        {
          k7_wifi_disconnect();
        }

      k7_wifi_forget(g_wifi_st.ap[i].ssid);
    }
}

static void wifi_ap_cb(lv_event_t *e)
{
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  const struct k7_wifi_ap_s *ap;
  char psk[65];

  if (i >= g_wifi_st.nap)
    {
      return;
    }

  ap = &g_wifi_st.ap[i];
  if (g_wifi_st.state == K7_WL_CONNECTED &&
      strcmp(g_wifi_st.ssid, ap->ssid) == 0)
    {
      return;
    }

  if (!ap->secure)
    {
      k7_wifi_connect(ap->ssid, NULL);
    }
  else if (ap->saved && k7_wifi_saved_psk(ap->ssid, psk, sizeof(psk)))
    {
      k7_wifi_connect(ap->ssid, psk);
    }
  else
    {
      pw_open(ap->ssid);
    }
}

static void wifi_info_text(const struct k7_wifi_status_s *st, char *buf,
                           size_t len)
{
  static const char *const fail[] =
    {
      "", "无线模组启动失败（错误 %d）", "扫描失败（错误 %d）",
      "没找到「%s」，可能信号太弱", "连不上「%s」，请检查密码",
      "连上了但没拿到地址", "连接断开了"
    };

  switch (st->state)
    {
      case K7_WL_OFF:
        strlcpy(buf, "已关闭", len);
        break;

      case K7_WL_STARTING:
        strlcpy(buf, "正在启动无线模组，大约需要 40 秒…", len);
        break;

      case K7_WL_SCANNING:
        strlcpy(buf, "正在扫描附近的网络…", len);
        break;

      case K7_WL_CONNECTING:
        snprintf(buf, len, "正在连接「%s」…", st->ssid);
        break;

      case K7_WL_DHCP:
        snprintf(buf, len, "正在从「%s」获取地址…", st->ssid);
        break;

      case K7_WL_CONNECTED:
        snprintf(buf, len, "已连接「%s」  地址 %s", st->ssid, st->ip);
        break;

      default:
        if (st->fail == K7_FAIL_NOTFOUND || st->fail == K7_FAIL_AUTH)
          {
            snprintf(buf, len, fail[st->fail], st->ssid);
          }
        else if (st->fail != K7_FAIL_NONE && st->fail < 7)
          {
            snprintf(buf, len, fail[st->fail], st->err);
          }
        else
          {
            strlcpy(buf, "未连接，点一个网络连接", len);
          }
        break;
    }
}

static void wifi_list_fill(void)
{
  const struct k7_wifi_status_s *st = &g_wifi_st;
  int i;

  lv_obj_clean(g_wifi_list);

  for (i = 0; i < st->nap; i++)
    {
      const struct k7_wifi_ap_s *ap = &st->ap[i];
      bool cur = (st->state == K7_WL_CONNECTED ||
                  st->state == K7_WL_CONNECTING ||
                  st->state == K7_WL_DHCP) &&
                 strcmp(st->ssid, ap->ssid) == 0;
      lv_obj_t *row = wl_list_row(g_wifi_list, ICON_WIFI,
                                  wl_signal_color(ap->signal), ap->ssid);

      lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(row, wifi_ap_cb, LV_EVENT_CLICKED,
                          (void *)(intptr_t)i);

      if (cur)
        {
          wl_tag(row, st->state == K7_WL_CONNECTED ? "已连接" : "连接中",
                 UI_OK);
        }

      if (ap->saved)
        {
          lv_obj_t *btn = lv_button_create(row);

          lv_obj_set_style_bg_color(btn, lv_color_hex(UI_SURFACE2), 0);
          lv_obj_set_style_shadow_width(btn, 0, 0);
          lv_label_set_text(lv_label_create(btn), "忘记");
          lv_obj_add_event_cb(btn, wifi_forget_cb, LV_EVENT_CLICKED,
                              (void *)(intptr_t)i);
        }

      wl_tag(row, ap->band ? "5G" : "2.4G", UI_DIM);
      wl_tag(row, wl_signal_text(ap->signal), UI_DIM);
      icon_label(row, ap->secure ? ICON_LOCK : " ", &lv_font_k7_icons_32,
                 UI_DIM);
    }

  if (st->nap == 0 && st->state == K7_WL_IDLE)
    {
      wl_tag(g_wifi_list, "没有扫到网络", UI_DIM);
    }

  settings_group_end(g_wifi_list);
}

/* ---- 蓝牙 ---- */

static void bt_sw_cb(lv_event_t *e)
{
  g_bt_want = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
  k7_bt_enable(g_bt_want);
}

static void bt_scan_cb(lv_event_t *e)
{
  (void)e;
  if (g_bt_want)
    {
      k7_bt_scan();
    }
}

static void bt_info_text(const struct k7_bt_status_s *st, char *buf,
                         size_t len)
{
  switch (st->state)
    {
      case K7_WL_OFF:
        strlcpy(buf, "已关闭", len);
        break;

      case K7_WL_STARTING:
        strlcpy(buf, "正在启动蓝牙…", len);
        break;

      case K7_WL_SCANNING:
        snprintf(buf, len, "正在扫描（15 秒），已发现 %d 个设备", st->ndev);
        break;

      case K7_WL_FAILED:
        snprintf(buf, len, "蓝牙启动失败（错误 %d）", st->err);
        break;

      default:
        snprintf(buf, len, "扫描完成，发现 %d 个设备", st->ndev);
        break;
    }
}

static void bt_list_fill(void)
{
  const struct k7_bt_status_s *st = &g_bt_st;
  int i;

  lv_obj_clean(g_bt_list);

  for (i = 0; i < st->ndev; i++)
    {
      const struct k7_bt_dev_s *d = &st->dev[i];
      char addr[24];
      lv_obj_t *row;

      snprintf(addr, sizeof(addr), "%02X:%02X:%02X:%02X:%02X:%02X",
               d->addr[5], d->addr[4], d->addr[3], d->addr[2], d->addr[1],
               d->addr[0]);
      row = wl_list_row(g_bt_list, ICON_BLUETOOTH, wl_signal_color(d->rssi),
                        d->name[0] ? d->name : "未命名设备");
      wl_tag(row, addr, UI_DIM);
      wl_tag(row, wl_signal_text(d->rssi), UI_DIM);
    }

  if (st->ndev == 0 && st->state == K7_WL_IDLE)
    {
      wl_tag(g_bt_list, "附近没有发现设备", UI_DIM);
    }

  settings_group_end(g_bt_list);
}

/* 每个 tick：取快照，变了才重画；设置页那两行的状态字也在这里更新 */

static void wl_poll(void)
{
  char buf[128];

  k7_wifi_status(&g_wifi_st);
  if (g_wifi_st.gen != g_wifi_gen)
    {
      g_wifi_gen = g_wifi_st.gen;
      wifi_info_text(&g_wifi_st, buf, sizeof(buf));
      lv_label_set_text(g_wifi_info, buf);
      lv_label_set_text(g_wifi_row_info,
                        g_wifi_st.state == K7_WL_CONNECTED ? g_wifi_st.ssid :
                        g_wifi_st.state == K7_WL_OFF ? "关闭" :
                        g_wifi_st.state == K7_WL_STARTING ? "启动中" :
                        "未连接");
      if (g_wifi_st.state == K7_WL_CONNECTED)
        {
          lv_obj_remove_flag(g_wifi_disc_btn, LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          lv_obj_add_flag(g_wifi_disc_btn, LV_OBJ_FLAG_HIDDEN);
        }

      wifi_list_fill();
    }

  k7_bt_status(&g_bt_st);
  if (g_bt_st.gen != g_bt_gen)
    {
      g_bt_gen = g_bt_st.gen;
      bt_info_text(&g_bt_st, buf, sizeof(buf));
      lv_label_set_text(g_bt_info, buf);
      lv_label_set_text(g_bt_row_info,
                        g_bt_st.state == K7_WL_OFF ? "关闭" :
                        g_bt_st.state == K7_WL_FAILED ? "启动失败" : "开启");
      bt_list_fill();
    }
}

/* 一块内容：状态卡片（开关 + 状态字 + 按钮）+ 列表 */

static lv_obj_t *wl_box(lv_obj_t *body, const char *icon, uint32_t color,
                        const char *name, lv_event_cb_t sw_cb,
                        lv_obj_t **sw, lv_obj_t **info)
{
  lv_obj_t *box = flex_box(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_t *group;
  lv_obj_t *row;

  lv_obj_set_style_pad_row(box, 12, 0);

  group = panel_create(box);
  lv_obj_set_style_pad_ver(group, 8, 0);
  lv_obj_set_style_pad_row(group, 4, 0);
  row = settings_row(group, icon, color, name);
  lv_obj_set_style_border_width(row, 0, 0);

  *sw = lv_switch_create(row);
  lv_obj_set_size(*sw, 84, 46);
  lv_obj_set_style_bg_color(*sw, lv_color_hex(UI_SURFACE2), LV_PART_MAIN);
  lv_obj_set_style_bg_color(*sw, lv_color_hex(color),
                            LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_add_event_cb(*sw, sw_cb, LV_EVENT_VALUE_CHANGED, NULL);

  *info = lv_label_create(group);
  lv_obj_set_width(*info, LV_PCT(100));
  lv_label_set_long_mode(*info, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(*info, lv_color_hex(UI_DIM), 0);
  lv_obj_set_style_pad_left(*info, 58, 0);
  lv_obj_set_style_pad_bottom(*info, 12, 0);
  return box;
}

static lv_obj_t *wl_small_button(lv_obj_t *parent, const char *text,
                                 lv_event_cb_t cb)
{
  lv_obj_t *btn = lv_button_create(parent);
  lv_obj_t *lbl;

  lv_obj_set_height(btn, 56);
  lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_set_style_pad_hor(btn, 28, 0);
  pill_set(btn, UI_ACCENT, false);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

  lbl = lv_label_create(btn);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_font(lbl, &lv_font_k7_ui_28, 0);
  lv_obj_center(lbl);
  return btn;
}

static void wl_panel_build(lv_obj_t *scr)
{
  lv_obj_t *bar;
  lv_obj_t *btn;
  lv_obj_t *body;
  lv_obj_t *row;
  lv_obj_t *lbl;

  /* 键盘字体：先找图标字库里的按键符号，没有再退回 28px 界面字库 */

  g_kb_font = lv_font_k7_icons_32;
  g_kb_font.fallback = &lv_font_k7_ui_28;

  g_wl_panel = lv_obj_create(scr);
  lv_obj_remove_style_all(g_wl_panel);
  lv_obj_set_size(g_wl_panel, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(g_wl_panel, lv_color_hex(UI_BG), 0);
  lv_obj_set_style_bg_opa(g_wl_panel, LV_OPA_COVER, 0);
  lv_obj_set_flex_flow(g_wl_panel, LV_FLEX_FLOW_COLUMN);

  bar = flex_box(g_wl_panel, LV_FLEX_FLOW_ROW);
  lv_obj_set_height(bar, 80);
  lv_obj_set_style_pad_hor(bar, 24, 0);
  lv_obj_set_style_pad_column(bar, 16, 0);

  btn = lv_button_create(bar);
  lv_obj_set_style_bg_color(btn, lv_color_hex(UI_SURFACE2), 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_add_event_cb(btn, wl_close_cb, LV_EVENT_CLICKED, NULL);
  lbl = icon_label(btn, ICON_BACK, &lv_font_k7_icons_32, UI_TEXT);

  g_wl_title = lv_label_create(bar);
  lv_obj_set_style_text_font(g_wl_title, &lv_font_k7_ui_28, 0);
  lv_obj_set_style_text_color(g_wl_title, lv_color_hex(UI_TEXT), 0);

  body = lv_obj_create(g_wl_panel);
  lv_obj_remove_style_all(body);
  lv_obj_set_width(body, LV_PCT(100));
  lv_obj_set_flex_grow(body, 1);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_hor(body, 24, 0);
  lv_obj_set_style_pad_bottom(body, 24, 0);
  lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);

  /* WLAN */

  g_wl_wifi_box = wl_box(body, ICON_WIFI, UI_ACCENT, "WLAN", wifi_sw_cb,
                         &g_wifi_sw, &g_wifi_info);
  row = flex_box(g_wl_wifi_box, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 16, 0);
  lbl = lv_label_create(row);
  lv_label_set_text(lbl, "附近的网络");
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_DIM), 0);
  lv_obj_set_style_pad_left(lbl, 12, 0);
  lv_obj_set_flex_grow(lbl, 1);
  g_wifi_disc_btn = wl_small_button(row, "断开", wifi_disc_cb);
  wl_small_button(row, "重新扫描", wifi_scan_cb);
  g_wifi_list = panel_create(g_wl_wifi_box);
  lv_obj_set_style_pad_ver(g_wifi_list, 4, 0);
  lv_obj_set_style_pad_row(g_wifi_list, 0, 0);

  lbl = lv_label_create(g_wl_wifi_box);
  lv_obj_set_width(lbl, LV_PCT(100));
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_pad_hor(lbl, 12, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_DIM), 0);
  lv_obj_set_style_text_opa(lbl, LV_OPA_70, 0);
  lv_label_set_text(lbl, "支持不加密和 WPA2 个人版网络。连上过的网络会记住"
                    "密码，下次点一下就连；开机不会自动打开 WLAN。"
                    "网线和 WLAN 在同一网段时，数据仍优先走网线。");

  /* 蓝牙 */

  g_wl_bt_box = wl_box(body, ICON_BLUETOOTH, UI_WATER, "蓝牙", bt_sw_cb,
                       &g_bt_sw, &g_bt_info);
  row = flex_box(g_wl_bt_box, LV_FLEX_FLOW_ROW);
  lbl = lv_label_create(row);
  lv_label_set_text(lbl, "附近的设备");
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_DIM), 0);
  lv_obj_set_style_pad_left(lbl, 12, 0);
  lv_obj_set_flex_grow(lbl, 1);
  wl_small_button(row, "重新扫描", bt_scan_cb);
  g_bt_list = panel_create(g_wl_bt_box);
  lv_obj_set_style_pad_ver(g_bt_list, 4, 0);
  lv_obj_set_style_pad_row(g_bt_list, 0, 0);

  lbl = lv_label_create(g_wl_bt_box);
  lv_obj_set_width(lbl, LV_PCT(100));
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_pad_hor(lbl, 12, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_DIM), 0);
  lv_obj_set_style_text_opa(lbl, LV_OPA_70, 0);
  lv_label_set_text(lbl, "目前能发现附近的低功耗蓝牙（BLE）设备；配对和连接"
                    "还没有做。WLAN 和蓝牙在同一颗模组上，关掉开关只是"
                    "停止扫描，模组不断电。");

  lv_obj_add_flag(g_wl_panel, LV_OBJ_FLAG_HIDDEN);
}

/* 设置页"网络与连接"那一组 */

static void wl_settings_group(lv_obj_t *tab)
{
  lv_obj_t *group = settings_group(tab, "网络与连接");
  lv_obj_t *row;

  row = settings_row(group, ICON_WIFI, UI_ACCENT, "WLAN");
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(row, wl_open_cb, LV_EVENT_CLICKED, (void *)0);
  g_wifi_row_info = wl_tag(row, "关闭", UI_DIM);
  icon_label(row, ICON_RIGHT, &lv_font_k7_icons_32, UI_DIM);

  row = settings_row(group, ICON_BLUETOOTH, UI_WATER, "蓝牙");
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(row, wl_open_cb, LV_EVENT_CLICKED, (void *)1);
  g_bt_row_info = wl_tag(row, "关闭", UI_DIM);
  icon_label(row, ICON_RIGHT, &lv_font_k7_icons_32, UI_DIM);
  settings_group_end(group);
}

static void settings_build(lv_obj_t *tab)
{
  lv_obj_t *group;
  lv_obj_t *row;
  lv_obj_t *lbl;

  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(tab, 24, 0);
  lv_obj_set_style_pad_row(tab, 12, 0);

  wl_settings_group(tab);

  group = settings_group(tab, "健康提醒");
  setting_row(group, ICON_CHAIR, UI_SIT, "久坐", REMIND_SIT,
              "30 分钟\n45 分钟\n60 分钟\n90 分钟", 1, true);
  setting_row(group, ICON_TINT, UI_WATER, "喝水", REMIND_WATER,
              "30 分钟\n60 分钟\n90 分钟\n120 分钟", 1, true);
  setting_row(group, ICON_EYE, UI_EYE, "用眼休息", REMIND_EYE,
              NULL, 0, true);
  setting_row(group, ICON_BELL, UI_ACCENT, "语音播报", -1, NULL, 0, true);
  settings_group_end(group);

  lbl = lv_label_create(tab);
  lv_obj_set_width(lbl, LV_PCT(100));
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_pad_hor(lbl, 12, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_DIM), 0);
  lv_obj_set_style_text_opa(lbl, LV_OPA_70, 0);
  lv_label_set_text(lbl, "用眼休息按 20-20-20：每 20 分钟看 6 米外 20 秒。");

  group = settings_group(tab, "声音");
  vol_build(group);

  group = settings_group(tab, "其他");
  row = settings_row(group, ICON_CODE, UI_DIM, "开发者工具");
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(row, dev_open_cb, LV_EVENT_CLICKED, NULL);
  icon_label(row, ICON_RIGHT, &lv_font_k7_icons_32, UI_DIM);
  settings_group_end(group);
}

/* 开发者工具：原来的六页工程界面原样搬进来，盖在主界面上面。
 * ★ 页签顺序别动：tick_cb 里"曲线"页按下标 2 判断。
 */

static void dev_panel_build(lv_obj_t *scr)
{
  lv_obj_t *bar;
  lv_obj_t *btn;
  lv_obj_t *lbl;
  lv_obj_t *media;

  g_dev_panel = lv_obj_create(scr);
  lv_obj_remove_style_all(g_dev_panel);
  lv_obj_set_size(g_dev_panel, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(g_dev_panel, lv_color_hex(UI_BG), 0);
  lv_obj_set_style_bg_opa(g_dev_panel, LV_OPA_COVER, 0);
  lv_obj_set_flex_flow(g_dev_panel, LV_FLEX_FLOW_COLUMN);

  bar = lv_obj_create(g_dev_panel);
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, LV_PCT(100), 64);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_hor(bar, 12, 0);
  lv_obj_set_style_pad_column(bar, 16, 0);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  btn = lv_button_create(bar);
  lv_obj_add_event_cb(btn, dev_close_cb, LV_EVENT_CLICKED, NULL);
  lv_label_set_text(lv_label_create(btn), "返回");
  lbl = lv_label_create(bar);
  lv_label_set_text(lbl, "开发者工具");
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_ACCENT), 0);

  g_tabview = lv_tabview_create(g_dev_panel);
  lv_tabview_set_tab_bar_size(g_tabview, 48);
  lv_obj_set_width(g_tabview, LV_PCT(100));
  lv_obj_set_flex_grow(g_tabview, 1);
  lv_obj_set_style_bg_color(g_tabview, lv_color_hex(UI_BG), 0);

  amp_build(lv_tabview_add_tab(g_tabview, "双核"));
  dev_build(lv_tabview_add_tab(g_tabview, "设备"));
  g_tab_live = lv_tabview_add_tab(g_tabview, "曲线");
  live_build(g_tab_live);

  /* 音视频：录音机 + 桌面摄像头 */

  media = lv_tabview_add_tab(g_tabview, "音视频");
  voice_build(media);
  camera_build(media);

  about_build(lv_tabview_add_tab(g_tabview, "关于"));

  lv_obj_add_flag(g_dev_panel, LV_OBJ_FLAG_HIDDEN);
}

static bool dev_panel_visible(void)
{
  return !lv_obj_has_flag(g_dev_panel, LV_OBJ_FLAG_HIDDEN);
}

/****************************************************************************
 * 定时刷新
 ****************************************************************************/

static void kickpi_screenshot(const char *path);

/* 逻辑坐标 (lx, ly) 对应的面板坐标。pw/ph 是面板（帧缓冲）宽高。
 * 必须是 LVGL indev_pointer_proc 那个变换的逆，否则触摸会点歪。
 */

static inline void ui_to_panel(int32_t lx, int32_t ly, int32_t pw,
                               int32_t ph, int32_t *px, int32_t *py)
{
  (void)pw;
  (void)ph;

#if UI_ROTATE == 90
  *px = ly;
  *py = ph - 1 - lx;
#elif UI_ROTATE == 270
  *px = pw - 1 - ly;
  *py = lx;
#else
  *px = lx;
  *py = ly;
#endif
}

static inline lv_display_rotation_t ui_rotation(void)
{
#if UI_ROTATE == 90
  return LV_DISPLAY_ROTATION_90;
#elif UI_ROTATE == 270
  return LV_DISPLAY_ROTATION_270;
#else
  return LV_DISPLAY_ROTATION_0;
#endif
}

/* 调试用的虚拟指针：echo "x y" > /tmp/k7-tap 在 (x, y) 点一下，
 * echo "x1 y1 x2 y2" 从前一点拖到后一点（滑条、翻页）。坐标是界面的
 * 逻辑像素（横屏 1280x720，见 UI_ROTATE）。走的是 LVGL 正常的输入处理，和手指按下去一样触发
 * 按下 / 点击 / 拖动事件 —— 验证的是界面逻辑，不是触摸屏硬件。
 *
 * 按 indev 读回调的次数计步（默认 30ms 一次）：按住 UI_TAP_HOLD 次再
 * 松开；拖动时在 UI_TAP_DRAG 次里线性插值走完。
 */

#define UI_TAP_HOLD  3
#define UI_TAP_DRAG  20

static struct
{
  int x0;
  int y0;
  int x1;
  int y1;
  int step;              /* 已走的步数；-1 表示空闲 */
  int steps;
} g_tap = { .step = -1 };

static void tap_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
  lv_display_t *disp = lv_indev_get_display(indev);
  int32_t lw = lv_display_get_horizontal_resolution(disp);   /* 逻辑 */
  int32_t lh = lv_display_get_vertical_resolution(disp);
  int32_t pw = UI_ROTATE == 0 ? lw : lh;                     /* 面板 */
  int32_t ph = UI_ROTATE == 0 ? lh : lw;
  int32_t lx;
  int32_t ly;

  /* 读回调交给 LVGL 的是"触摸屏原始坐标"，LVGL 会按显示方向再转一次；
   * 注入的是逻辑坐标，所以先转成面板坐标，两次一抵就是原样。
   */

  if (g_tap.step < 0)
    {
      ui_to_panel(g_tap.x1, g_tap.y1, pw, ph, &data->point.x,
                  &data->point.y);
      data->state = LV_INDEV_STATE_RELEASED;
      return;
    }

  lx = g_tap.x0 + (g_tap.x1 - g_tap.x0) * g_tap.step / g_tap.steps;
  ly = g_tap.y0 + (g_tap.y1 - g_tap.y0) * g_tap.step / g_tap.steps;
  ui_to_panel(lx, ly, pw, ph, &data->point.x, &data->point.y);
  data->state = LV_INDEV_STATE_PRESSED;

  if (++g_tap.step > g_tap.steps)
    {
      g_tap.step = -1;
    }
}

static void tap_poll(void)
{
  FILE *fp = fopen("/tmp/k7-tap", "r");
  int v[4];
  int n;

  if (fp == NULL)
    {
      return;
    }

  n = fscanf(fp, "%d %d %d %d", &v[0], &v[1], &v[2], &v[3]);
  fclose(fp);
  unlink("/tmp/k7-tap");

  if (n != 2 && n != 4)
    {
      syslog(LOG_WARNING, "界面: /tmp/k7-tap 要 \"x y\" 或 "
             "\"x1 y1 x2 y2\"，读到 %d 个数\n", n);
      return;
    }

  g_tap.x0 = v[0];
  g_tap.y0 = v[1];
  g_tap.x1 = n == 4 ? v[2] : v[0];
  g_tap.y1 = n == 4 ? v[3] : v[1];
  g_tap.steps = n == 4 ? UI_TAP_DRAG : UI_TAP_HOLD;
  g_tap.step = 0;
  syslog(LOG_INFO, "界面: 注入%s (%d,%d)->(%d,%d)\n",
         n == 4 ? "拖动" : "点击", g_tap.x0, g_tap.y0, g_tap.x1, g_tap.y1);
}

#ifdef CONFIG_RK3576_FRAME_SHM
/* 调试用：echo 1 > /tmp/k7-frame 把 Linux 放进帧共享区的最新一帧全屏
 * 显示出来，点一下关闭。图像直接指向共享内存，不拷贝。
 *
 * ★ 显示前 invalidate 那个槽：本端映成 cacheable，不 invalidate 会显示
 *   cache 里的旧内容。Linux 要再发三帧才会绕回这个槽，一次性显示足够；
 *   以后接连续预览时要按帧重新 invalidate、换槽。
 */

static lv_obj_t      *g_frame_view;
static lv_image_dsc_t g_frame_dsc;

static void frame_view_close_cb(lv_event_t *e)
{
  (void)e;
  lv_obj_delete(g_frame_view);
  g_frame_view = NULL;
}

static void frame_view_show(void)
{
  uintptr_t base = CONFIG_RK3576_FRAME_SHM_BASE;
  FAR const struct k7f_hdr_s *h = (FAR const void *)base;
  lv_obj_t *img;
  lv_obj_t *lbl;
  uint32_t off;

  up_invalidate_dcache(base, base + K7F_HDR_SIZE);

  if (h->magic != K7F_MAGIC || h->version != K7F_VERSION || h->seq == 0 ||
      h->slot >= K7F_SLOTS || h->format != K7F_FMT_XRGB8888 ||
      h->width == 0 || h->width > 4096 || h->height == 0 ||
      h->height > 4096 || h->stride < h->width * 4 ||
      h->bytes != h->stride * h->height || h->bytes > h->slot_size ||
      (uint64_t)K7F_HDR_SIZE + (uint64_t)h->slot_size * K7F_SLOTS >
      CONFIG_RK3576_FRAME_SHM_SIZE)
    {
      syslog(LOG_WARNING, "界面: 帧共享区没有可显示的帧（magic=0x%08"
             PRIx32 " seq=%" PRIu32 "）\n", h->magic, h->seq);
      return;
    }

  off = k7f_slot_offset(h, h->slot);
  up_invalidate_dcache(base + off, base + off + h->bytes);

  lv_image_cache_drop(&g_frame_dsc);
  memset(&g_frame_dsc, 0, sizeof(g_frame_dsc));
  g_frame_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  g_frame_dsc.header.cf = LV_COLOR_FORMAT_XRGB8888;
  g_frame_dsc.header.w = h->width;
  g_frame_dsc.header.h = h->height;
  g_frame_dsc.header.stride = h->stride;
  g_frame_dsc.data_size = h->bytes;
  g_frame_dsc.data = (FAR const uint8_t *)(base + off);

  if (g_frame_view != NULL)
    {
      lv_obj_delete(g_frame_view);
    }

  g_frame_view = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(g_frame_view);
  lv_obj_set_size(g_frame_view, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(g_frame_view, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(g_frame_view, LV_OPA_COVER, 0);
  lv_obj_add_flag(g_frame_view, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(g_frame_view, frame_view_close_cb, LV_EVENT_CLICKED,
                      NULL);

  img = lv_image_create(g_frame_view);
  lv_image_set_src(img, &g_frame_dsc);
  lv_obj_center(img);

  lbl = lv_label_create(g_frame_view);
  lv_label_set_text_fmt(lbl, "Linux 帧共享区  seq=%" PRIu32 "  %" PRIu32
                        "x%" PRIu32 "（点一下关闭）",
                        h->seq, h->width, h->height);
  lv_obj_set_style_bg_color(lbl, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(lbl, LV_OPA_60, 0);
  lv_obj_set_style_pad_all(lbl, 8, 0);
  lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 16);

  syslog(LOG_INFO, "界面: 显示帧共享区 seq=%" PRIu32 " 槽=%" PRIu32 "\n",
         h->seq, h->slot);
}
#endif

#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_423_K7OTA
/* OTA 的启动确认（app/k7ota，等同 `bootctl`）。
 *
 * 界面建好并连续跑过 OTA_CONFIRM_MS 才算这次启动成功：新镜像要是在显示、
 * 触摸或界面初始化里挂住，就走不到这里，下次进 U-Boot 会回到原来的槽。
 * 从救援 FIT 启动、或已确认过，k7ab_success() 什么都不写。
 */

#define OTA_CONFIRM_MS 15000

static void ota_confirm_cb(lv_timer_t *t)
{
  int ret;

  (void)t;

  ret = k7ab_success();
  if (ret == 1)
    {
      syslog(LOG_INFO, "OTA: 本次启动已确认（界面运行 %d s）\n",
             OTA_CONFIRM_MS / 1000);
    }
  else if (ret < 0)
    {
      syslog(LOG_ERR, "OTA: 启动确认失败: %d\n", ret);
    }
}
#endif

static void tick_cb(lv_timer_t *t)
{
  (void)t;

  tap_poll();
#ifdef CONFIG_RK3576_FRAME_SHM
  if (unlink("/tmp/k7-frame") == 0)
    {
      frame_view_show();
    }
#endif

  camera_poll();
  agent_poll();
  wake_poll();
  tts_poll();
  rec_poll();
  home_poll();
  work_poll();
  wl_poll();

  /* 串口调试入口：echo 1 > /tmp/k7-open-camera 等同于点 Open camera */

  if (unlink("/tmp/k7-open-camera") == 0)
    {
      live_page_open();
    }

  /* echo 1 > /tmp/k7-ask 等同于点 "What is on my desk?" */

  if (unlink("/tmp/k7-ask") == 0)
    {
      syslog(LOG_INFO, "界面: 串口触发 agent 看桌面\n");
      agent_look_cb(NULL);
    }

  /* echo 1 > /tmp/k7-rec / k7-play 等同于点录音机的两个按钮 */

  if (unlink("/tmp/k7-rec") == 0)
    {
      rec_start(REC_RECORDING);
    }

  if (unlink("/tmp/k7-play") == 0)
    {
      rec_start(REC_PLAYING);
    }

  /* echo N > /tmp/k7-shot：截屏到 /tmp/k7-shot.jpg。先切页：
   *   N = 0~3    主界面的 今日 / 家居 / 助手 / 设置
   *   N = 10~14  开发者工具的 双核 / 设备 / 曲线 / 音视频 / 关于
   *   N = 20 / 21 设置里的 WLAN / 蓝牙 页
   */

  {
    FILE *fp = fopen("/tmp/k7-shot", "r");

    if (fp != NULL)
      {
        int tab = -1;

        if (fscanf(fp, "%d", &tab) == 1)
          {
            if (tab >= 0 && tab < 5)
              {
                lv_obj_add_flag(g_dev_panel, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(g_wl_panel, LV_OBJ_FLAG_HIDDEN);
                lv_tabview_set_active(g_main_tabview, tab, LV_ANIM_OFF);
              }
            else if (tab >= 10 && tab < 15)
              {
                lv_obj_remove_flag(g_dev_panel, LV_OBJ_FLAG_HIDDEN);
                lv_tabview_set_active(g_tabview, tab - 10, LV_ANIM_OFF);
              }
            else if (tab == 20 || tab == 21)
              {
                lv_obj_add_flag(g_dev_panel, LV_OBJ_FLAG_HIDDEN);
                wl_open(tab == 21);
              }

            lv_refr_now(NULL);
          }

        fclose(fp);
        unlink("/tmp/k7-shot");
        kickpi_screenshot("/tmp/k7-shot.jpg");
      }
  }

  /* echo 1 / 0 > /tmp/k7-wake：开 / 关语音唤醒（调试入口） */

  {
    FILE *fp = fopen("/tmp/k7-wake", "r");

    if (fp != NULL)
      {
        int on = 0;

        if (fscanf(fp, "%d", &on) == 1)
          {
            if (on)
              {
                k7_wake_start();
              }
            else
              {
                k7_wake_stop();
              }
          }

        fclose(fp);
        unlink("/tmp/k7-wake");
      }
  }

  /* echo 数值 > /tmp/k7-vol：设音量（0~100），等同于拖滑块 */

  {
    FILE *fp = fopen("/tmp/k7-vol", "r");

    if (fp != NULL)
      {
        int v = -1;

        if (fscanf(fp, "%d", &v) == 1)
          {
            k7a_set_volume(v);
            lv_slider_set_value(g_vol_slider, k7a_get_volume(), LV_ANIM_OFF);
            vol_show(k7a_get_volume());
            syslog(LOG_INFO, "音量: %d%%（调试入口）\n", k7a_get_volume());
          }

        fclose(fp);
        unlink("/tmp/k7-vol");
      }
  }

  /* echo 1 > /tmp/k7-beep：同一条放音路径放 1 秒 1kHz，单独验证喇叭 */

  if (unlink("/tmp/k7-beep") == 0)
    {
      rec_start(REC_BEEP);
    }

  /* echo N > /tmp/k7-remind：让第 N 个提醒（0 久坐 / 1 喝水 / 2 用眼）
   * 立刻到期。验收提醒卡片用，不必真的等 45 分钟。
   */

  {
    FILE *fp = fopen("/tmp/k7-remind", "r");

    if (fp != NULL)
      {
        int idx = -1;

        if (fscanf(fp, "%d", &idx) == 1 && idx >= 0 && idx < REMIND_COUNT)
          {
            g_remind[idx].due = 0;
            g_remind[idx].off_until = 0;
          }

        fclose(fp);
        unlink("/tmp/k7-remind");
      }
  }

  /* 今日页和提醒每秒一次就够，没必要跟着 250ms 的节拍刷 */

  {
    static unsigned n;

    if (n++ % (1000 / TICK_MS) == 0)
      {
        remind_tick();
        today_refresh();
      }
  }

  amp_refresh();

  /* LIVE 页不可见时不算、不画。曲线是"看得见才有意义"的东西，后台跑
   * 只会白占 CPU —— 这块板上 LVGL 和 rpmsg 抢的是同几颗核。
   */

  if (dev_panel_visible() && lv_tabview_get_tab_active(g_tabview) == 2)
    {
      live_refresh();
    }
  else
    {
      dev_refresh();
    }
}

/* 底部页签栏：去掉默认主题的选中底色和下划线，只用颜色区分 */

static void main_tabbar_style(lv_obj_t *bar)
{
  uint32_t i;

  lv_obj_set_style_bg_color(bar, lv_color_hex(UI_CARD), 0);
  lv_obj_set_style_pad_ver(bar, UI_ROTATE == 0 ? 8 : 40, 0);

  for (i = 0; i < lv_obj_get_child_count(bar); i++)
    {
      lv_obj_t *btn = lv_obj_get_child(bar, i);
      lv_obj_t *lbl = lv_obj_get_child(btn, 0);

      lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
      lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_STATE_CHECKED);
      lv_obj_set_style_border_width(btn, 0, LV_STATE_CHECKED);
      lv_obj_set_style_text_color(btn, lv_color_hex(UI_DIM), 0);
      lv_obj_set_style_text_color(btn, lv_color_hex(UI_WATER),
                                  LV_STATE_CHECKED);
      lv_obj_set_style_text_font(lbl, &lv_font_k7_icons_32, 0);
      lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_text_line_space(lbl, 6, 0);
    }
}

/* 开机动画：主界面建好后盖在最上层放一遍，然后淡出、删掉。
 *
 *   三道圆环依次画满，颜色是三类提醒的主色（久坐、喝水、用眼），中间是
 *   家的图标 —— 正好是这台设备的两件事：家居中控 + 工作健康助手。随后
 *   副标题从下往上浮出，停一会儿整层淡出。
 *
 * ★ 第一帧与 U-Boot 的开机 logo 是同一张图（scripts/gen-boot-logo.py
 *   照这里的布局画：暗色底轨 + 图标 + 标题）。logo 一直留在屏上直到这里
 *   画出第一帧，两边对得上就看不出切换，像是 logo 自己动了起来。所以
 *   图标和标题一开始就在、不做淡入。改布局要两边一起改，再重新烧 logo。
 *
 *   动的只有屏幕中间 240x240 和两行小字，横屏软件旋转下每帧也只刷这一
 *   小块；只有最后淡出那 300ms 是整屏重画。整层可点击，播放期间吞掉
 *   触摸，免得点到下面看不见的按钮。时长按动画本身排死（约 2.6 秒），
 *   不等任何外部事件，主界面的定时器照常在下面跑。
 */

#define SPLASH_RING_MS  900       /* 每道圆环画满的时间 */
#define SPLASH_STAGGER  160       /* 相邻两道圆环的起步间隔 */
#define SPLASH_TEXT_AT  620       /* 文字开始浮出 */
#define SPLASH_OUT_AT   2300      /* 开始淡出 */
#define SPLASH_OUT_MS   300

static void splash_arc_cb(void *obj, int32_t v)
{
  lv_arc_set_angles(obj, 0, v);
}

static void splash_opa_cb(void *obj, int32_t v)
{
  lv_obj_set_style_opa(obj, v, 0);
}

static void splash_rise_cb(void *obj, int32_t v)
{
  lv_obj_set_style_translate_y(obj, v, 0);
}

static void splash_done_cb(lv_anim_t *a)
{
  lv_obj_delete(a->var);
}

static void splash_anim(lv_obj_t *obj, lv_anim_exec_xcb_t cb,
                        int32_t from, int32_t to,
                        uint32_t delay, uint32_t ms)
{
  lv_anim_t a;

  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_exec_cb(&a, cb);
  lv_anim_set_values(&a, from, to);
  lv_anim_set_delay(&a, delay);
  lv_anim_set_duration(&a, ms);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

static void splash_show(void)
{
  static const uint32_t colors[] =
  {
    UI_SIT, UI_WATER, UI_EYE
  };

  lv_obj_t *layer;
  lv_obj_t *mark;
  lv_obj_t *obj;
  lv_anim_t a;
  int i;

  layer = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(layer);
  lv_obj_set_size(layer, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(layer, lv_color_hex(UI_BG), 0);
  lv_obj_set_style_bg_opa(layer, LV_OPA_COVER, 0);
  lv_obj_add_flag(layer, LV_OBJ_FLAG_CLICKABLE);

  /* 圆环和图标放在一个 240x240 的框里，框的中心在屏幕中线上方 70 */

  mark = lv_obj_create(layer);
  lv_obj_remove_style_all(mark);
  lv_obj_set_size(mark, 240, 240);
  lv_obj_align(mark, LV_ALIGN_CENTER, 0, -70);
  lv_obj_remove_flag(mark, LV_OBJ_FLAG_CLICKABLE);

  for (i = 0; i < 3; i++)
    {
      int32_t d = 240 - i * 52;

      obj = lv_arc_create(mark);
      lv_obj_remove_style(obj, NULL, LV_PART_KNOB);
      lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_size(obj, d, d);
      lv_obj_center(obj);
      lv_arc_set_rotation(obj, 270);           /* 从正上方起笔 */
      lv_arc_set_bg_angles(obj, 0, 360);
      lv_arc_set_angles(obj, 0, 0);
      lv_obj_set_style_arc_width(obj, 14, LV_PART_MAIN);
      lv_obj_set_style_arc_width(obj, 14, LV_PART_INDICATOR);
      lv_obj_set_style_arc_color(obj, lv_color_hex(UI_SURFACE2),
                                 LV_PART_MAIN);
      lv_obj_set_style_arc_color(obj, lv_color_hex(colors[i]),
                                 LV_PART_INDICATOR);
      lv_obj_set_style_arc_rounded(obj, true, LV_PART_INDICATOR);
      splash_anim(obj, splash_arc_cb, 0, 360, i * SPLASH_STAGGER,
                  SPLASH_RING_MS);
    }

  obj = lv_label_create(mark);
  lv_obj_set_style_text_font(obj, &lv_font_k7_icons_64, 0);
  lv_obj_set_style_text_color(obj, lv_color_hex(UI_TEXT), 0);
  lv_label_set_text(obj, ICON_HOME);
  lv_obj_center(obj);

  obj = lv_label_create(layer);
  lv_obj_set_style_text_font(obj, &lv_font_k7_ui_28, 0);
  lv_obj_set_style_text_color(obj, lv_color_hex(UI_TEXT), 0);
  lv_label_set_text(obj, "K7 桌面助手");
  lv_obj_align(obj, LV_ALIGN_CENTER, 0, 100);

  /* 副标题和底部一行：淡入，副标题再上浮 24 像素 */

  obj = lv_label_create(layer);
  lv_obj_set_style_text_color(obj, lv_color_hex(UI_DIM), 0);
  lv_label_set_text(obj, "家居中控  ·  久坐  ·  喝水  ·  用眼");
  lv_obj_align(obj, LV_ALIGN_CENTER, 0, 148);
  lv_obj_set_style_opa(obj, LV_OPA_TRANSP, 0);
  splash_anim(obj, splash_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER,
              SPLASH_TEXT_AT, 500);
  splash_anim(obj, splash_rise_cb, 24, 0, SPLASH_TEXT_AT, 500);

  obj = lv_label_create(layer);
  lv_obj_set_style_text_color(obj, lv_color_hex(UI_DIM), 0);
  lv_label_set_text(obj, "openvela  ·  RK3576");
  lv_obj_align(obj, LV_ALIGN_BOTTOM_MID, 0, -36);
  lv_obj_set_style_opa(obj, LV_OPA_TRANSP, 0);
  splash_anim(obj, splash_opa_cb, LV_OPA_TRANSP, LV_OPA_60,
              SPLASH_TEXT_AT + 200, 500);

  /* 整层淡出，结束时删掉 */

  lv_anim_init(&a);
  lv_anim_set_var(&a, layer);
  lv_anim_set_exec_cb(&a, splash_opa_cb);
  lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
  lv_anim_set_delay(&a, SPLASH_OUT_AT);
  lv_anim_set_duration(&a, SPLASH_OUT_MS);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
  lv_anim_set_completed_cb(&a, splash_done_cb);
  lv_anim_start(&a);
}

static void build_ui(void)
{
  lv_obj_t *scr = lv_screen_active();
  lv_indev_t *tap;
  lv_display_t *disp = lv_display_get_default();

  /* ★ 整屏用中文字库：换的是**主题**的字体，不是在屏幕上设。
   *
   *   上一版在屏幕根对象上 set_style_text_font，以为会沿对象树继承 ——
   *   但默认主题给页签按钮、按钮、标签等各自直接挂了主题字体
   *   （LV_FONT_DEFAULT = Montserrat 14），继承根本轮不到，中文全成了
   *   方框（页签、第一页、最后一页、"打开相机"……）。
   *
   *   这里按 LVGL 建显示时的主色（蓝 / 红，见 lv_display_create）重建
   *   默认主题，字体换成中文字库，并用**深色**模式：界面底色是深色卡片，
   *   浅色模式下没单独设颜色的文字是黑的，截图里几乎看不见（开关旁的
   *   说明、录音机和摄像头的提示），曲线图也是白底。中文字库带
   *   Montserrat 14 后备，LV_SYMBOL_* 图标照样显示。必须在创建任何控件
   *   之前做。
   */

  lv_display_set_theme(disp,
                       lv_theme_default_init(disp,
                                             lv_palette_main(LV_PALETTE_BLUE),
                                             lv_palette_main(LV_PALETTE_RED),
                                             true, &lv_font_k7_cjk_20));

  /* 相机全屏页挂在 lv_layer_top() 上。换主题只刷新当前屏幕，顶层图层在
   * 换主题之前就建好了，还是旧主题的 Montserrat —— 相机页全是方框。
   * 单独给它设字体（不能对它 lv_theme_apply：主题会给它铺不透明底色，
   * 把整屏盖住）。
   */

  lv_obj_set_style_text_font(lv_layer_top(), &lv_font_k7_cjk_20, 0);

  lv_obj_set_style_bg_color(scr, lv_color_hex(UI_BG), LV_PART_MAIN);

  /* 主界面五个页签。竖屏放底部（拇指够得着下沿）；横屏放左边 ——
   * 720 高本来就紧，底栏 112 再吃掉一块，左边 1280 宽有富余。
   */

  g_main_tabview = lv_tabview_create(scr);
#if UI_ROTATE == 0
  lv_tabview_set_tab_bar_position(g_main_tabview, LV_DIR_BOTTOM);
  lv_tabview_set_tab_bar_size(g_main_tabview, 112);
#else
  lv_tabview_set_tab_bar_position(g_main_tabview, LV_DIR_LEFT);
  lv_tabview_set_tab_bar_size(g_main_tabview, 128);
#endif
  lv_obj_set_size(g_main_tabview, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(g_main_tabview, lv_color_hex(UI_BG), 0);

  /* 页签名写成 图标 + 换行 + 文字：标签用图标字库，汉字走它的后备 */

  today_build(lv_tabview_add_tab(g_main_tabview, ICON_SUN "\n" "今日"));
  home_build(lv_tabview_add_tab(g_main_tabview, ICON_HOME "\n" "家居"));
  work_build(lv_tabview_add_tab(g_main_tabview, ICON_VIDEO "\n" "工作"));
  assist_build(lv_tabview_add_tab(g_main_tabview, ICON_MIC "\n" "助手"));
  settings_build(lv_tabview_add_tab(g_main_tabview, ICON_COG "\n" "设置"));
  main_tabbar_style(lv_tabview_get_tab_bar(g_main_tabview));

  /* 后建的在上层：开发者工具打开时盖住主界面 */

  dev_panel_build(scr);
  wl_panel_build(scr);

  /* 调试注入用的第二个指针设备，见 tap_read_cb。显示此时已建好，
   * lv_indev_create() 会绑到它上面。
   */

  tap = lv_indev_create();
  lv_indev_set_type(tap, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(tap, tap_read_cb);

  lv_timer_create(tick_cb, TICK_MS, NULL);

#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_423_K7OTA
  lv_timer_set_repeat_count(lv_timer_create(ota_confirm_cb, OTA_CONFIRM_MS,
                                            NULL), 1);
#endif

  splash_show();
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

struct kickpi_fb_s
{
  int       fd;
  uint8_t  *mem;
  uint32_t  stride;
  uint32_t  xres;
  uint32_t  yres;
  size_t    fblen;
  size_t    drawbytes;
  int       nbuf;         /* 帧缓冲块数 = yres_virtual / yres */
};

static struct kickpi_fb_s g_kfb;

/* ★ 相机页的双缓冲翻页（2026-11-02）
 *
 *   平时是 PARTIAL：LVGL 分块画进小缓冲，flush 直接拷进 VOP 正在扫的那一块
 *   帧缓冲。仪表盘一秒刷几次、每次只改一小块，看不出问题。
 *
 *   相机页每帧整屏 1280x720，按 202 行一块分 4 块画，画完一帧要几十毫秒，
 *   跨好几次扫描：扫的时候上面几块已是新画面、下面还是旧的。画面一动，
 *   块与块的交界就错开 —— 用户看到的是"固定位置、固定间隔的横纹，像画面
 *   没跟上"。
 *
 *   所以相机页开着时（g_fb_dbuf）改成：整帧画进 VOP **没在扫**的那一半
 *   （驱动本来就分了两块，yres_virtual = 2*yres），刷 cache 后
 *   FBIOPAN_DISPLAY 翻过去。
 *
 *   - 只有**整屏**的那一轮刷新（相机新帧：画面控件铺满全屏）才画进后
 *     缓冲再翻页；别的小块刷新照旧直接写进正在显示的那块。这样两块缓冲
 *     不需要逐帧同步（以前 DIRECT 双缓冲的老问题）：下一帧整屏会把后缓冲
 *     整个重写一遍。
 *
 *     第一版把相机页里每次标脏都扩成整屏来保证一致，结果被盖在下面的
 *     电平条、时钟这些小块更新全变成整屏重画，3 秒刷屏 342 次、只显示了
 *     45 个新帧，取帧线程也被挤得少拿帧（22fps）。
 *   - 翻页在下一个 VSYNC 才生效。驱动会等 FS 标志，但那个标志实测靠不住，
 *     所以再兜一层：写 CFG_DONE 之后至少过 FB_LATCH_US 才往换下来的那块
 *     写，60Hz 一帧 16.7ms 内必定 latch。
 *
 *     只有两块时这一等每帧要白等约 10ms（拷贝线程化之后实测 26fps）。
 *     驱动改成三块后轮着写：要写的那块是**两次**翻页之前换下来的，
 *     等的起点是倒数第二次翻页，一帧 30ms 量级时基本不用等。
 *   - 平时（g_fb_dbuf 为假）照旧写 g_fb_front 那一块，翻到第二块之后
 *     关掉相机页也不用拷回去。
 */

#define FB_LATCH_US 17000

static bool     g_fb_dbuf;
static uint8_t  g_fb_front;
static uint8_t  g_fb_prev = 1;  /* 翻到 g_fb_front 之前显示的那块 */
static bool     g_fb_full;      /* 下一轮刷新里有整屏的标脏 */
static bool     g_fb_drawing;   /* 一轮刷新已经开始写 */
static bool     g_fb_back;      /* 这一轮写后缓冲、结束时翻页 */
static uint64_t g_fb_pan_us;    /* 最近一次翻页 */
static uint64_t g_fb_pan_prev_us;

/* 后缓冲写哪一块：三块时取既不是正在显示、也不是刚换下来的那块 */

static int kickpi_fb_backbuf(const struct kickpi_fb_s *fb)
{
  return fb->nbuf >= 3 ? 3 - g_fb_front - g_fb_prev : g_fb_front ^ 1;
}

static uint8_t *kickpi_fb_half(const struct kickpi_fb_s *fb, int half)
{
  return fb->mem + (size_t)half * fb->stride * fb->yres;
}

static void kickpi_fb_dbuf(bool on)
{
  if (g_fb_dbuf == on)
    {
      return;
    }

  g_fb_dbuf = on;
  syslog(LOG_INFO, "界面: 相机页双缓冲 %s（当前显示第 %d 块）\n",
         on ? "开" : "关", g_fb_front);
}

static void kickpi_refr_cb(lv_event_t *e)
{
  if (lv_event_get_code(e) == LV_EVENT_REFR_START)
    {
      g_st_refr_t0 = now_us();
    }
  else if (g_st_refr_t0 != 0)
    {
      if (g_st_refr_drew)
        {
          g_st_refr_us += now_us() - g_st_refr_t0;
          g_st_refr_n++;
        }

      g_st_refr_t0 = 0;
      g_st_refr_drew = false;
    }
}

static void kickpi_inv_cb(lv_event_t *e)
{
  lv_area_t *a = lv_event_get_param(e);
  lv_display_t *disp = lv_event_get_target(e);

  /* 只看不改。lv_refr.c 的 get_max_row() 也拿这个事件探分块高度
   * （x1=x2=y1=0 的一列），改了会把分块算坏。
   */

  if (g_fb_dbuf && a != NULL && a->x1 <= 0 && a->y1 <= 0 &&
      a->x2 >= lv_display_get_horizontal_resolution(disp) - 1 &&
      a->y2 >= lv_display_get_vertical_resolution(disp) - 1)
    {
      g_fb_full = true;
    }
}

/* 调试截屏：把帧缓冲缩小一半编成 JPEG（/tmp/k7-shot.jpg，几十 KB），
 * 经 telnet 的 hexdump 取回主机看。开发时看不到屏幕，全靠它核对界面
 * （上一版"整屏换中文字库"就是凭推测改错、满屏方框才被发现的）。
 */

static void kickpi_screenshot(const char *path)
{
  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr jerr;
  unsigned char *row;
  int32_t pw = g_kfb.xres;
  int32_t ph = g_kfb.yres;
  uint32_t w = (UI_ROTATE == 0 ? pw : ph) / 2;  /* 按界面方向出图 */
  uint32_t h = (UI_ROTATE == 0 ? ph : pw) / 2;
  uint32_t y;
  FILE *fp;

  if (g_kfb.mem == NULL || g_kfb.mem == MAP_FAILED)
    {
      return;
    }

  fp = fopen(path, "wb");
  row = malloc(w * 3);
  if (fp == NULL || row == NULL)
    {
      if (fp != NULL)
        {
          fclose(fp);
        }

      free(row);
      return;
    }

  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);
  jpeg_stdio_dest(&cinfo, fp);
  cinfo.image_width = w;
  cinfo.image_height = h;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, 70, TRUE);
  jpeg_start_compress(&cinfo, TRUE);

  for (y = 0; y < h; y++)
    {
      uint32_t x;

      for (x = 0; x < w; x++)
        {
          const uint8_t *px;
          int32_t fx;
          int32_t fy;

          ui_to_panel(x * 2, y * 2, pw, ph, &fx, &fy);
          px = kickpi_fb_half(&g_kfb, g_fb_front) +
               (size_t)fy * g_kfb.stride + fx * 4;           /* B G R A */

          row[3 * x]     = px[2];
          row[3 * x + 1] = px[1];
          row[3 * x + 2] = px[0];
        }

      jpeg_write_scanlines(&cinfo, &row, 1);
    }

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  fclose(fp);
  free(row);
  syslog(LOG_INFO, "界面: 截屏 %" PRIu32 "x%" PRIu32 " → %s\n", w, h, path);
}

/* ★ 绘制缓冲必须在堆上分配，不能写成静态数组。
 *
 *   第一版写成 static uint32_t g_draw[720*60]，两块共 345KB 全进了 BSS，
 *   镜像从 4993 扇区涨到 5665 扇区。而 U-Boot 的 bootamp 读 FIT 用的是
 *   **写死的扇区数 5120**（cmd/bootamp.c 的编译期宏），于是读进来的是
 *   截断的镜像，校验直接失败：
 *
 *     Verifying Hash Integrity ... sha256 Bad hash: ...
 *     Bad Data Hash / AMP Error: Load loadables, ret=-13
 *
 *   现象是"板子起不来"，但根因既不在代码逻辑也不在 LVGL —— 是镜像超过了
 *   引导器愿意读的长度。运行期的缓冲放堆上，镜像一个字节都不会涨。
 */

/* 360 行：相机页 640x360 的画面一块画完。原来 60 行要分 6 块。
 * 两块共 720x360x4x2 = 2MB，系统堆放得下。横屏后逻辑宽 1280，整屏宽的
 * 区域一块 202 行；LVGL 按"缓冲字节数 / 区域宽"分块，640 宽的相机画面
 * 一块仍能画 405 行，不用加大。
 */

#define KICKPI_DRAW_LINES 360
static uint32_t *g_draw1;
static uint32_t *g_draw2;

/* ★ flush 交给另一个线程做（2026-11-02 查相机页帧率）
 *
 *   相机页每帧整屏 1280x720，分段计时：LVGL 把画面画进绘制缓冲约 13ms，
 *   flush 里转 90° 拷进帧缓冲约 18ms，刷 cache + 翻页约 6ms —— 全在界面
 *   线程里串着做，一帧 42ms，只有 22fps，而 Linux 那边稳定给 30fps。
 *
 *   两块绘制缓冲本来就有（g_draw1/g_draw2）。flush_cb 只把这一块交给
 *   kickpi_flushd 线程就返回，LVGL 立刻去另一块里画下一块，画和拷在两个
 *   核上重叠。LVGL 每次调 flush_cb 之前都会先等上一块拷完
 *   （lv_refr.c 的 wait_for_flushing，等法是下面的 kickpi_flush_wait），
 *   所以单槽交接就够，不会有两块同时在拷。
 *
 *   写哪一半（g_fb_front / 后缓冲）仍在界面线程里定；翻页、改 g_fb_front
 *   在拷贝线程里，界面线程下一次读它之前必然已经等过这一块拷完。
 */

struct kickpi_flush_job_s
{
  lv_display_t *disp;
  lv_area_t     area;
  uint8_t      *px_map;
  int           target;     /* 写帧缓冲的哪一半 */
  bool          first;      /* 一轮刷新的第一块：写后缓冲前要等 latch */
  bool          back;       /* 这一轮写后缓冲，最后一块之后翻页 */
  bool          last;
  uint64_t      free_us;    /* back 时：从这一刻起再过一帧，这块就没人扫了 */
};

static struct kickpi_flush_job_s g_fjob;
static sem_t          g_fjob_sem;     /* 有一块要拷 */
static sem_t          g_fdone_sem;    /* 拷完一块 */
static volatile bool  g_fbusy;

static void kickpi_fb_copy(struct kickpi_fb_s *fb, uint8_t *mem,
                           const lv_area_t *area, const uint8_t *px_map)
{
  int32_t w = lv_area_get_width(area);
  int32_t y;

#if UI_ROTATE == 0
  for (y = area->y1; y <= area->y2; y++)
    {
      memcpy(mem + (size_t)y * fb->stride + (size_t)area->x1 * 4,
             px_map + (size_t)(y - area->y1) * w * 4,
             (size_t)w * 4);
    }
#else
  /* area 是逻辑坐标。逻辑的一列正好是面板的一行：按面板行写（连续写），
   * 源按列读（跨 w*4 字节）。一块最多 360 行，一列读到的 360 个缓存行
   * 下一列接着用，L1 放得下，不用再分块。
   */

  {
    const uint32_t *src = (const uint32_t *)px_map;
    int32_t h = lv_area_get_height(area);
    int32_t x;

    for (x = area->x1; x <= area->x2; x++)
      {
        int32_t px0;
        int32_t py;
        int32_t px1;
        int32_t dummy;
        uint32_t *dst;
        const uint32_t *s = src + (x - area->x1);

        ui_to_panel(x, area->y1, fb->xres, fb->yres, &px0, &py);
        ui_to_panel(x, area->y2, fb->xres, fb->yres, &px1, &dummy);
        dst = (uint32_t *)(mem + (size_t)py * fb->stride);

        if (px1 >= px0)          /* 90：逻辑 y 增大，面板 x 增大 */
          {
            for (y = 0; y < h; y++)
              {
                dst[px0 + y] = s[(size_t)y * w];
              }
          }
        else                     /* 270：面板 x 减小 */
          {
            for (y = 0; y < h; y++)
              {
                dst[px0 - y] = s[(size_t)y * w];
              }
          }
      }
  }
#endif
}

static void *kickpi_flushd(void *arg)
{
  struct kickpi_fb_s *fb = arg;

  for (; ; )
    {
      struct kickpi_flush_job_s *j = &g_fjob;
      uint64_t t0;

      while (sem_wait(&g_fjob_sem) < 0)
        {
        }

      /* 写后缓冲之前确认上次翻页已经 latch：后缓冲就是上一帧 VOP 还在
       * 扫的那块。
       */

      if (j->first && j->back)
        {
          uint64_t dt = now_us() - j->free_us;

          if (dt < FB_LATCH_US)
            {
              usleep(FB_LATCH_US - dt);
              g_st_latch_us += FB_LATCH_US - dt;
            }
        }

      t0 = now_us();
      kickpi_fb_copy(fb, kickpi_fb_half(fb, j->target), &j->area,
                     j->px_map);
      g_st_copy_us += now_us() - t0;

      /* ★ 一帧只在最后一块时刷一次。
       *
       *   驱动的 updatearea 每次都把半个帧缓冲（3.6MB）整块刷回 DRAM
       *   （见 rk3576_fb.c 的说明）。原来每块都调一次，一帧画面被分成
       *   几块就刷几次 —— 640x360 的相机画面按 60 行分块，一帧刷 6 次，
       *   帧率被它压着。PARTIAL 模式下前几块写进去就在，最后统一刷一次
       *   就够了。驱动按 up.y 落在哪一半决定刷哪一块。
       */

      if (j->last)
        {
          struct fb_area_s up;

          t0 = now_us();
          up.x = 0;
          up.y = j->target * fb->yres;
          up.w = fb->xres;
          up.h = fb->yres;
          ioctl(fb->fd, FBIO_UPDATE, (unsigned long)&up);

          if (j->back)
            {
              struct fb_planeinfo_s pi;

              memset(&pi, 0, sizeof(pi));
              pi.stride       = fb->stride;
              pi.yres_virtual = fb->yres * fb->nbuf;
              pi.xres_virtual = fb->xres;
              pi.yoffset      = j->target * fb->yres;
              g_fb_pan_prev_us = g_fb_pan_us;
              g_fb_pan_us = now_us();
              ioctl(fb->fd, FBIOPAN_DISPLAY, (unsigned long)&pi);
              g_fb_prev  = g_fb_front;
              g_fb_front = j->target;
            }

          g_st_flush_us += now_us() - t0;
          g_st_flush_n++;
        }

      g_fbusy = false;
      lv_display_flush_ready(j->disp);
      sem_post(&g_fdone_sem);
    }

  return NULL;
}

static void kickpi_flush_wait(lv_display_t *disp)
{
  (void)disp;

  /* 没有在拷的时候 LVGL 也会调进来，这时不能等。多出来的 post 只会让
   * 下一次多转一圈再检查 g_fbusy，不会提前放行。
   */

  while (g_fbusy)
    {
      sem_wait(&g_fdone_sem);
    }
}

static void kickpi_flush_cb(lv_display_t *disp, const lv_area_t *area,
                            uint8_t *px_map)
{
  struct kickpi_flush_job_s *j = &g_fjob;

  /* 一轮刷新的第一块决定写哪一半 */

  j->first = !g_fb_drawing;
  if (j->first)
    {
      g_fb_drawing = true;
      g_fb_back = g_fb_dbuf && g_fb_full && g_kfb.nbuf >= 2;
      g_fb_full = false;
    }

  j->disp   = disp;
  j->area   = *area;
  j->px_map = px_map;
  j->back   = g_fb_back;
  j->target = g_fb_back ? kickpi_fb_backbuf(&g_kfb) : g_fb_front;
  j->free_us = g_kfb.nbuf >= 3 ? g_fb_pan_prev_us : g_fb_pan_us;
  j->last   = lv_display_flush_is_last(disp);
  if (j->last)
    {
      g_fb_drawing = false;
    }

  g_st_refr_drew = true;
  g_fbusy = true;
  sem_post(&g_fjob_sem);
}

static lv_display_t *kickpi_disp_create(const char *path)
{
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  lv_display_t         *disp;

  g_kfb.fd = open(path, O_RDWR);
  if (g_kfb.fd < 0)
    {
      return NULL;
    }

  memset(&pinfo, 0, sizeof(pinfo));
  if (ioctl(g_kfb.fd, FBIOGET_VIDEOINFO, (unsigned long)&vinfo) < 0 ||
      ioctl(g_kfb.fd, FBIOGET_PLANEINFO, (unsigned long)&pinfo) < 0)
    {
      close(g_kfb.fd);
      return NULL;
    }

  g_kfb.xres   = vinfo.xres;
  g_kfb.yres   = vinfo.yres;
  g_kfb.stride = pinfo.stride;
  g_kfb.fblen  = pinfo.fblen;
  g_kfb.nbuf   = pinfo.yres_virtual >= 2 * vinfo.yres ?
                 pinfo.yres_virtual / vinfo.yres : 1;
  g_kfb.mem    = mmap(NULL, pinfo.fblen, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_FILE, g_kfb.fd, 0);
  if (g_kfb.mem == MAP_FAILED)
    {
      close(g_kfb.fd);
      return NULL;
    }

  {
    size_t bufbytes = (size_t)vinfo.xres * KICKPI_DRAW_LINES * 4;

    g_draw1 = malloc(bufbytes);
    g_draw2 = malloc(bufbytes);
    if (g_draw1 == NULL || g_draw2 == NULL)
      {
        printf("绘制缓冲分配失败 (%zu x2)\n", bufbytes);
        close(g_kfb.fd);
        return NULL;
      }

    g_kfb.drawbytes = bufbytes;
  }

  disp = lv_display_create(vinfo.xres, vinfo.yres);
  if (disp == NULL)
    {
      return NULL;
    }

  lv_display_set_driver_data(disp, &g_kfb);
  lv_display_set_rotation(disp, ui_rotation());
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
  lv_display_set_flush_cb(disp, kickpi_flush_cb);

  /* 拷贝线程和界面同优先级，SMP 下自然落在另一个核上 */

  {
    pthread_attr_t attr;
    struct sched_param sp;
    pthread_t tid;

    sem_init(&g_fjob_sem, 0, 0);
    sem_init(&g_fdone_sem, 0, 0);
    sched_getparam(0, &sp);
    pthread_attr_init(&attr);
    pthread_attr_setschedparam(&attr, &sp);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setstacksize(&attr, 4096);
    if (pthread_create(&tid, &attr, kickpi_flushd, &g_kfb) != 0)
      {
        pthread_attr_destroy(&attr);
        printf("起不了 flush 线程\n");
        return NULL;
      }

    pthread_attr_destroy(&attr);
    pthread_setname_np(tid, "ui_flush");
  }

  lv_display_set_flush_wait_cb(disp, kickpi_flush_wait);
  lv_display_set_buffers(disp, g_draw1, g_draw2, g_kfb.drawbytes,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_add_event_cb(disp, kickpi_inv_cb, LV_EVENT_INVALIDATE_AREA,
                          NULL);
  lv_display_add_event_cb(disp, kickpi_refr_cb, LV_EVENT_REFR_START, NULL);
  lv_display_add_event_cb(disp, kickpi_refr_cb, LV_EVENT_REFR_READY, NULL);

  printf("\u663e\u793a: %" PRIu32 "x%" PRIu32 " PARTIAL, \u7ed8\u5236\u7f13\u51b2 %d \u884c\n",
         g_kfb.xres, g_kfb.yres, KICKPI_DRAW_LINES);
  return disp;
}

/* ★ 触摸自己读，不用 lv_nuttx_touchscreen 的读回调。
 *
 *   它把面板原始坐标按 lv_display_get_horizontal/vertical_resolution()
 *   钳位，而这两个返回的是**旋转后**的逻辑分辨率（1280x720）；原始坐标
 *   却是竖屏面板的（720x1280）。转 90° 后面板 y>719 全被压成 719，换算
 *   出来的逻辑 x 最小只有 560 —— 左边整片（页签栏就在这里）点不到，
 *   看起来就是"触摸卡住了"。
 *
 *   LVGL 的旋转换算在读回调之后（indev_pointer_proc），所以这里交出的
 *   是原始坐标，按面板尺寸钳位。只取第一个触点，和原驱动的单点路径一样。
 *   一次读一个样本、读到就让 LVGL 接着读，按下和抬起挨得再近也不会丢。
 */

static int                    g_touch_fd = -1;
static uint8_t                g_touch_maxpoint;
static struct touch_sample_s *g_touch_sample;
static lv_indev_state_t       g_touch_state = LV_INDEV_STATE_RELEASED;
static lv_point_t             g_touch_pt;

static void kickpi_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
  ssize_t len = SIZEOF_TOUCH_SAMPLE_S(g_touch_maxpoint);

  (void)indev;

  if (read(g_touch_fd, g_touch_sample, len) == len)
    {
      const struct touch_point_s *tp = &g_touch_sample->point[0];

      if (tp->flags & (TOUCH_DOWN | TOUCH_MOVE))
        {
          g_touch_pt.x  = LV_CLAMP(0, tp->x, (int32_t)g_kfb.xres - 1);
          g_touch_pt.y  = LV_CLAMP(0, tp->y, (int32_t)g_kfb.yres - 1);
          g_touch_state = LV_INDEV_STATE_PRESSED;
        }
      else if (tp->flags & TOUCH_UP)
        {
          g_touch_state = LV_INDEV_STATE_RELEASED;
        }

      data->continue_reading = true;
    }

  data->point = g_touch_pt;
  data->state = g_touch_state;
}

static lv_indev_t *kickpi_touch_create(const char *path)
{
  lv_indev_t *indev;

  g_touch_fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (g_touch_fd < 0)
    {
      return NULL;
    }

  if (ioctl(g_touch_fd, TSIOC_GETMAXPOINTS,
            (unsigned long)&g_touch_maxpoint) < 0 ||
      g_touch_maxpoint == 0 ||
      (g_touch_sample =
         malloc(SIZEOF_TOUCH_SAMPLE_S(g_touch_maxpoint))) == NULL)
    {
      close(g_touch_fd);
      g_touch_fd = -1;
      return NULL;
    }

  indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, kickpi_touch_read_cb);
  return indev;
}

int main(int argc, char *argv[])
{
  lv_nuttx_dsc_t    dsc;
  lv_nuttx_result_t result;
  lv_display_t     *disp_self;

  (void)argc;
  (void)argv;

  g_t0 = time(NULL);

  lv_init();

  /* ★ 不用 lv_nuttx_init() 建显示，只借它建触摸。
   *
   *   lv_nuttx_fbdev.c 写死了 LV_DISPLAY_RENDER_MODE_DIRECT，而且只要
   *   驱动报了 yres_virtual = 2*yres 就自动走双缓冲 + FBIOPAN_DISPLAY。
   *   这条路在这块板子上一直不干净：
   *
   *     - DIRECT + 双缓冲要求两块缓冲逐帧同步（LVGL 用
   *       refr_sync_areas() 把上一帧脏区从前缓冲拷到后缓冲），
   *       任何一环对不上，屏上就是"上一帧没清干净" ——
   *       实测滑动时闪烁并出现黑色短线条；
   *     - 翻页的 CFG_DONE 要等 VSYNC 才 latch，而 VOP2 的中断我们
   *       没接，只能轮询 VP1 的 FS 标志。实测那个标志是**粘滞**
   *       的（120ms 里只跳变 3 次，60Hz 本该 ~14 次），拿它当帧
   *       边界并不可靠，k7diag anim 仍跑到 78fps（屏是 60Hz）。
   *
   *   PARTIAL 模式把这一整类问题从根上去掉：LVGL 只往一块很小的
   *   RAM 缓冲里画，flush_cb 负责把它拷进帧缓冲。帧缓冲**始终是
   *   一份完整一致的画面**，没有两块缓冲要同步，也就不需要翻页和
   *   vsync 门控。代价是每帧多一次内存拷贝，对这个仪表盘可以忽略。
   *
   *   这是绝大多数嵌入式 LVGL 的标准接法 —— 我们绕了远路才回到它。
   */

  /* ★ 顺序不能反：先建显示，再建触摸。
   *
   *   lv_indev_create() 会把输入设备绑到**当时的默认显示**上。先建触摸
   *   的话那时还没有显示，indev 的 display 是空的，随后
   *   process_single_touch() 里 lv_indev_get_display() 取到 NULL，
   *   一点屏幕就崩。第一版就是这么写的，板子直接起不来。
   */

  disp_self = kickpi_disp_create("/dev/fb0");
  if (disp_self == NULL)
    {
      printf("建立显示失败\n");
      return -1;
    }

  lv_nuttx_dsc_init(&dsc);
  dsc.fb_path    = NULL;               /* 显示上面已经建好 */
  dsc.input_path = NULL;               /* 触摸也自己建，见 kickpi_touch_read_cb */

  lv_nuttx_init(&dsc, &result);

  /* ★ lv_nuttx_init() 会把 result 整个重置。
   *
   *   我们的显示是上面自己建的（PARTIAL 模式），而 dsc.fb_path 传的是
   *   NULL，于是 lv_nuttx_init() 里那句 `if (dsc && dsc->fb_path)` 不成立，
   *   它**不建显示**，同时把 result.disp 清成了 NULL —— 先前填进去的指针
   *   被覆盖掉了。板上现象是"显示建好了，紧接着报打不开 (null)"，然后
   *   程序退出、屏幕全黑。
   *
   *   所以自建的 disp 要用局部变量存住，等 lv_nuttx_init() 返回之后再填
   *   回 result，并把输入设备绑上去。
   */

  result.disp  = disp_self;
  result.indev = kickpi_touch_create("/dev/input0");

  if (result.indev != NULL)
    {
      lv_indev_set_display(result.indev, result.disp);
    }

  if (result.disp == NULL)
    {
      printf("显示未就绪\n");
      return EXIT_FAILURE;
    }

  if (result.indev == NULL)
    {
      /* 触摸没起来不影响看画面，继续跑并明确说出来，不要让"界面出来了"
       * 掩盖掉输入其实没通。
       */

      printf("警告：打不开 /dev/input0 —— 触摸不可用，仅显示\n");
    }

  printf("LVGL 就绪 %dx%d，触摸 %s\n",
         (int)lv_display_get_horizontal_resolution(result.disp),
         (int)lv_display_get_vertical_resolution(result.disp),
         result.indev != NULL ? "可用" : "不可用");

  build_ui();
  agent_autostart();

#ifdef CONFIG_LV_USE_BUILTIN_MALLOC
  {
    lv_mem_monitor_t monitor;

    lv_mem_monitor(&monitor);
    printf("LVGL heap: used=%zu total=%zu peak=%zu\n",
           monitor.total_size - monitor.free_size,
           monitor.total_size, monitor.max_used);
  }
#endif

  /* openvela 的 LVGL 分支没有 lv_nuttx_run()，循环要自己写。
   * 和 apps/examples/lvgldemo 一样：lv_timer_handler() 返回距离下一个
   * 定时器还有多久，按它睡，最少 1ms。
   */

  for (; ; )
    {
      uint64_t t0 = now_us();
      uint32_t idle = lv_timer_handler();

      g_st_loop_us += now_us() - t0;
      g_st_loop_n++;

      usleep((idle ? idle : 1) * 1000);
    }

  lv_nuttx_deinit(&result);
  lv_deinit();
  return EXIT_SUCCESS;
}
