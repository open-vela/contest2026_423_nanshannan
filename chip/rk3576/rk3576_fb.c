/****************************************************************************
 * arch/arm64/src/rk3576/rk3576_fb.c
 *
 * RK3576 VOP2 帧缓冲，接到 NuttX 的 fb 框架上（/dev/fb0）。
 *
 * ★ 本驱动**不初始化显示链路**，只接管。
 *
 *   出厂 U-Boot 已经完成面板上电、发送厂商初始化序列、配置 D-PHY / DSI /
 *   VOP2 并在 VP1 上出图。那份面板初始化序列在厂商 dtsi 里，我们拿不到，
 *   一旦复位面板就无法恢复（表现为背光亮但永远无显示）。
 *
 *   因此这里只做三件事：从 VP1 的时序寄存器读出真实分辨率、分配帧缓冲、
 *   把 ESMART1 图层指过去。时序、DSI、D-PHY、面板一律不碰。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/video/fb.h>
#include <nuttx/kmalloc.h>

#include "arm64_internal.h"
#include "rk3576_vop2.h"

#ifdef CONFIG_RK3576_FB

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define FB_NBUF 3                  /* 帧缓冲块数，见 up_fbinitialize() */

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct fb_vtable_s   g_fb_vtable;
static struct fb_videoinfo_s g_videoinfo;
static struct fb_planeinfo_s g_planeinfo;
static void                *g_fbmem;
static uint32_t             g_fbstride;
static uint32_t             g_fbheight;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int rk3576_getvideoinfo(struct fb_vtable_s *vtable,
                               struct fb_videoinfo_s *vinfo)
{
  if (vtable == NULL || vinfo == NULL)
    {
      return -EINVAL;
    }

  *vinfo = g_videoinfo;
  return OK;
}

static int rk3576_getplaneinfo(struct fb_vtable_s *vtable, int planeno,
                               struct fb_planeinfo_s *pinfo)
{
  if (vtable == NULL || pinfo == NULL || planeno != 0)
    {
      return -EINVAL;
    }

  *pinfo = g_planeinfo;
  return OK;
}

#ifdef CONFIG_FB_UPDATE

/****************************************************************************
 * Name: rk3576_updatearea
 *
 * Description:
 *   把被改动的那几行从 D-cache 刷回 DRAM。
 *
 * ★ 这一步是必需的，但它**不是**黑色细横条纹的原因
 *
 *   帧缓冲在 openvela 自己的堆里，那段 DRAM 按 MT_NORMAL（写回式可缓存）
 *   映射，而 VOP2 直接读 DRAM。不刷回，它取到的就是旧数据。所以这个回调
 *   必须有 —— LVGL 每帧都会调 ioctl(FBIO_UPDATE)，我们最初没实现它，
 *   ioctl 返回 -ENOTTY，而 LVGL 的错误日志因为 LV_USE_LOG 默认关着也看
 *   不到，两边都不报错。
 *
 *   但把它实现之后条纹依旧。后来用 k7diag 绕开 LVGL 直接写图案
 *   （ramp / bars），走的是**完全相同的** mmap + FBIO_UPDATE 路径，
 *   屏上完全正常 —— 这就把"刷 cache 不够"这个怀疑彻底排除了，
 *   真正的原因是没有双缓冲，见 rk3576_pandisplay()。
 *
 *   这条记录留在这里：一个"看起来能解释现象、改完现象还在"的修复，
 *   如果不显式否定，下一轮就会被当成已知正确的前提。
 *
 *   只刷改动的行，不是整屏：720x1280x4 = 3.6MB，整屏刷会把每帧的开销
 *   都花在 cache 维护上。
 *
 ****************************************************************************/

static int rk3576_updatearea(struct fb_vtable_s *vtable,
                             const struct fb_area_s *area)
{
  uintptr_t start;
  uintptr_t end;

  UNUSED(vtable);

  if (g_fbmem == NULL || area == NULL)
    {
      return -ENODEV;
    }

  /* 调用方传进来的 area->y 已经含了 yoffset（见
   * lv_nuttx_fbdev.c: fb_area.y = dirty_area.y1 + yoffset），
   * 取值范围 0..FB_NBUF*yres-1，据此判断这一帧画的是哪一块。
   */

  /* ★ 刷**整块**后缓冲，不能只刷 LVGL 报的脏区。
   *
   *   双缓冲 + DIRECT 模式下，LVGL 每帧只把脏区**画**进后缓冲，其余部分
   *   是从前缓冲**拷**过来的（上一帧的脏区）。拷贝也是 CPU 写，同样停在
   *   cache 里，但它不在本帧 FBIO_UPDATE 报的那个脏区范围内 —— 只按脏区
   *   刷，这些行就永远不会写回 DRAM，VOP2 取到的是旧内容。
   *
   *   板上现象：平时正常，**一刷新就冒出黑色细横线**，位置跟着上一帧改
   *   过的地方走。这正是"只刷了画的、没刷拷的"。
   *
   *   所以按 area->y 落在哪一半，整半刷。一半 3.6MB，在这块 A53 上约 1ms，
   *   而这是个每秒刷几次的界面，代价可以接受。真要收窄，得刷"本帧脏区 ∪
   *   上一帧脏区"，而上一帧的范围只有 LVGL 自己知道 —— 驱动这层拿不到，
   *   所以整半刷不是偷懒，是这一层能做的最窄的正确范围。
   */

  {
    uintptr_t base = (uintptr_t)g_fbmem;
    size_t    half = (size_t)g_fbstride * g_fbheight;

    if (g_fbstride == 0 || half == 0)
      {
        return -EINVAL;
      }

    if (area->y >= g_fbheight * FB_NBUF)
      {
        return -EINVAL;
      }

    start = base + half * (area->y / g_fbheight);
    end = start + half;
  }

  up_flush_dcache(start, end);
  return OK;
}

#endif /* CONFIG_FB_UPDATE */

/****************************************************************************
 * Name: rk3576_pandisplay
 *
 * Description:
 *   翻页。LVGL 画完一帧后用 FBIOPAN_DISPLAY 把 yoffset 交过来，这里把
 *   ESMART1 的取数地址切到对应那一块。
 *
 * ★ 为什么结尾要调 fb_remove_paninfo()
 *
 *   NuttX 的 fb.c 把每次 pan 排进一个深度为 fbcount 的队列，用它给
 *   poll(POLLOUT) 做流控：队列满了就不再报可写，LVGL 的刷新定时器
 *   （display_refr_timer_cb 里的 poll）随之停摆。正常出队的时机是
 *   VSYNC 中断里调 fb_remove_paninfo()。
 *
 *   RK3576 的 VOP2 中断还没接，队列没人消费 —— 两帧之后 LVGL 就再也
 *   收不到 POLLOUT，界面会**整个卡住**。所以这里自己出队一次。
 *
 *   注意顺序：fb.c 是先调 pandisplay() 再 fb_add_paninfo()，所以这里
 *   出的是**上一次**那一笔，队列深度稳定在 1，不会溢出。
 *
 *   代价是失去了"等硬件真的翻完页"这层背压。对这块 60Hz 面板 + 4Hz
 *   刷新的仪表盘来说，CFG_DONE 在 17ms 内就 latch 完了，远早于下一帧，
 *   不构成问题。接上 VOP2 的 VSYNC 中断后应当改回标准做法。
 *
 ****************************************************************************/

static int rk3576_pandisplay(struct fb_vtable_s *vtable,
                             struct fb_planeinfo_s *pinfo)
{
  uintptr_t addr;
  uint32_t  yoffset;

  if (vtable == NULL || pinfo == NULL || g_fbmem == NULL)
    {
      return -EINVAL;
    }

  yoffset = pinfo->yoffset;
  if (yoffset % g_fbheight != 0 || yoffset >= g_fbheight * FB_NBUF)
    {
      return -EINVAL;
    }

  addr = (uintptr_t)g_fbmem + (uintptr_t)yoffset * g_fbstride;

  fb_remove_paninfo(vtable, FB_NO_OVERLAY);

  return rk3576_vop2_fb_pan(addr);
}


/****************************************************************************
 * Public Functions
 ****************************************************************************/

int up_fbinitialize(int display)
{
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  size_t   fbsize;
  int      ret;

  if (display != 0)
    {
      return -ENODEV;
    }

  if (g_fbmem != NULL)
    {
      return OK;              /* 已初始化 */
    }

  /* 分辨率从 VP1 的时序寄存器读，不写死 —— 那是 U-Boot 按厂商 dtsi
   * 配好的值，比文档可靠。
   */

  ret = rk3576_vop2_get_mode(&width, &height);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 读不到显示模式: %d\n", ret);
      return ret;
    }

  /* U-Boot 用的是 ARGB8888，沿用之，避免改格式牵连一串寄存器。 */

  stride = width * 4;
  fbsize = (size_t)stride * height;

  /* ★ 分配**两倍**：双缓冲。
   *
   *   单缓冲时 LVGL 边画、VOP2 边扫同一块内存，扫描线撞上正在重画的
   *   控件就会扫出一条"只有背景没有内容"的横带 —— 板上那几条黑色细
   *   横条纹就是这么来的。详见 rk3576_vop2.c 里 rk3576_vop2_fb_pan()
   *   的说明。
   *
   *   几块缓冲首尾相连（consecutive），LVGL 的 lv_nuttx_fbdev.c 会自己
   *   识别这种布局：display 0 和 display 1 报同一个 fbmem 时它按
   *   offset = yres * stride 去 mmap 第二块。
   *
   * ★ 后来改成三块（2026-11-02，相机页帧率）。
   *
   *   两块时，翻页之后要等 VOP 在下一个 VSYNC 真正切过去，才能往换下来
   *   的那块写，相机页每帧白等约 10ms（只到 26fps）。三块轮着用，要写的
   *   那块是两次翻页之前换下来的，早就 latch 完了。kickpi_ui 自己管翻页；
   *   lv_nuttx_fbdev.c 只认 yres_virtual == 2*yres，三块时它会退回单缓冲，
   *   本项目没有用它建显示。
   */

  fbsize *= FB_NBUF;

  g_fbmem = kmm_memalign(256, fbsize);
  if (g_fbmem == NULL)
    {
      syslog(LOG_ERR, "ERROR: 帧缓冲分配失败 %zu 字节\n", fbsize);
      return -ENOMEM;
    }

  memset(g_fbmem, 0, fbsize);
  up_flush_dcache((uintptr_t)g_fbmem, (uintptr_t)g_fbmem + fbsize);

  ret = rk3576_vop2_fb_setup((uintptr_t)g_fbmem, width, height, stride);
  if (ret < 0)
    {
      kmm_free(g_fbmem);
      g_fbmem = NULL;
      return ret;
    }

  g_videoinfo.fmt     = FB_FMT_RGB32;
  g_videoinfo.xres    = width;
  g_videoinfo.yres    = height;
  g_videoinfo.nplanes = 1;

  g_planeinfo.fbmem        = g_fbmem;
  g_planeinfo.fblen        = fbsize;          /* 几块之和 */
  g_planeinfo.stride       = stride;
  g_planeinfo.display      = display;
  g_planeinfo.bpp          = 32;

  /* ★ 这两行就是开关。
   *
   *   lv_nuttx_fbdev.c:
   *     double_buffer = (pinfo.yres_virtual == vinfo.yres * 2)
   *
   *   不填 yres_virtual（=0）时 LVGL 只能走单缓冲那条会撕裂的路，而且
   *   两边都不报错 —— 这正是之前查不出来的原因。
   *   NuttX 自己的 fb.c 也用它算缓冲块数：
   *     fbcount = yres_virtual == 0 ? 1 : yres_virtual / yres
   */

  g_planeinfo.xres_virtual = width;
  g_planeinfo.yres_virtual = height * FB_NBUF;
  g_planeinfo.yoffset      = 0;

  g_fb_vtable.getvideoinfo = rk3576_getvideoinfo;
  g_fb_vtable.getplaneinfo = rk3576_getplaneinfo;
#ifdef CONFIG_FB_UPDATE
  g_fb_vtable.updatearea    = rk3576_updatearea;
#endif
  g_fb_vtable.pandisplay    = rk3576_pandisplay;

  g_fbstride = stride;
  g_fbheight = height;

  syslog(LOG_INFO,
         "FB: %" PRIu32 "x%" PRIu32 " ARGB8888 %d 块缓冲 %zu 字节 @%p\n",
         width, height, FB_NBUF, fbsize, g_fbmem);
  return OK;
}

struct fb_vtable_s *up_fbgetvplane(int display, int vplane)
{
  if (display != 0 || vplane != 0 || g_fbmem == NULL)
    {
      return NULL;
    }

  return &g_fb_vtable;
}

void up_fbuninitialize(int display)
{
  UNUSED(display);

  /* 不释放帧缓冲，也不关显示 —— 关掉之后无法重新初始化面板。 */
}

#endif /* CONFIG_RK3576_FB */
