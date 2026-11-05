/****************************************************************************
 * chip/rk3576/rk3576_video.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * RK3576 VICAP/CIF 的 V4L2 下半部（imgdata）。
 *
 * 把已有的 rk3576_cif_* 接口包成 NuttX 视频框架要求的 imgdata_ops_s，
 * 由 capture_register() 与 imgsensor 一起注册成 /dev/videoN。
 *
 * ★ 为什么要做这一层：
 *
 *   ai_agent 的视觉能力（摄像头拍照 + Vision LLM 分析）只认 V4L2 设备，
 *   我们原来的 `cam` 命令是直接调 rk3576_cif_* 的私有接口，agent 用不了。
 *
 * ★ 与原有取图路径的关键差别：中断，而不是轮询。
 *
 *   rk3576_cif_wait_frame() 是**轮询** CIF_MIPI_INTSTAT 的，适合
 *   `cam cap` 那种"要一帧就等一帧"的用法。而 V4L2 的 start_capture()
 *   交给你一个回调，要求帧结束时主动通知上层 —— 没法用轮询实现，
 *   否则得常驻一个线程空转。
 *
 *   所以这里挂真正的 CIF 中断（GIC_SPI 318 -> IRQ 350，出处
 *   rk3576.dtsi 的 rkcif@27c10000），在中断里清标志、算时间戳、
 *   调回调。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <debug.h>
#include <string.h>
#include <sys/time.h>

#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include <nuttx/wqueue.h>
#include <nuttx/kmalloc.h>
#include <nuttx/video/imgdata.h>
#include <nuttx/video/v4l2_cap.h>
#include <sys/videoio.h>

#include <arch/irq.h>

#include "rk3576_cif.h"
#include "rk3576_video.h"
#include "hardware/rk3576_memorymap.h"

#ifdef CONFIG_RK3576_VIDEO

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* CIF 中断。出处 rk3576.dtsi：
 *   rkcif: rkcif@27c10000 {
 *     interrupts = <GIC_SPI 318 IRQ_TYPE_LEVEL_HIGH>;
 *   };
 * SPI 号加 32 得到中断号，与本端口其它外设一致。
 */

#define RK3576_IRQ_CIF        (318 + 32)

/* 我们只用 host3（cam3 走的那一路，dtb 记载 imx415_3@0x37）。
 * 换接口时这里和板级的 sensor 一起改。
 */

#define RK3576_VIDEO_HOST     3

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_video_s
{
  struct imgdata_s     data;        /* 必须是第一个成员 */

  bool                 capturing;
  bool                 irq_attached;

  uint16_t             width;
  uint16_t             height;
  uint32_t             framesize;   /* 一帧的字节数，回调要报 */

  uint8_t             *buf;         /* 上层给的目标缓冲区 */

  imgdata_capture_t    callback;
  void                *arg;

  /* ---- JPEG 模式 ----
   *
   * CIF 只出 Bayer RAW，而 ai_agent 的视觉工具向 /dev/video0 请求的是
   * V4L2_PIX_FMT_JPEG（见 packages/ai_agent/src/tools/tool_camera.c）。
   * 所以 JPEG 只能软件生成：CIF 先 DMA 进内部 RAW 缓冲，再转换成 JPEG
   * 写进上层给的 buffer。
   */

  bool                 jpeg_mode;
  FAR uint16_t        *rawbuf;      /* 内部 RAW 缓冲，仅 JPEG 模式用 */
  size_t               rawbytes;
  int                  stride_pix;  /* RAW 行跨距，以 uint16 计 */
  uint32_t             outlen;      /* 上层 buffer 的容量 */
  struct work_s        work;        /* 编码转到工作队列，不在 ISR 里做 */

  /* ★ 按需编码：上层排队一个缓冲区（set_buf）才编一帧。
   *
   *   原来每个帧结束中断都排一次编码。30fps 进帧、一帧编码一两秒，
   *   LPWORK（优先级 100）编完一帧立刻又有下一帧，永远不空 ——
   *   ai_agent 的拍照线程（优先级 60）拿不到 CPU，DQBUF 之后的
   *   STREAMOFF 永远执行不到，camera_capture 不返回。
   *   v4l2cap 在 nsh（优先级 100）里跑能成功，正是因为能和它轮转。
   */

  bool                 want;        /* 上层在等一帧 */
  bool                 cif_running; /* JPEG 模式下 CIF 当前在搬运 */
  int                  skip;        /* (重)启动后要丢掉的帧数 */
  struct timeval       ts;
};

/* 转换回调。
 *
 * ★ 去马赛克与 JPEG 编码放在板级（传感器的 Bayer 相位、黑白电平都是
 *   板级知识），芯片层不该反向依赖板级。所以做成注册式：板级在初始化
 *   时把自己的转换函数交给这一层。
 */

/* RAW 缓冲尾部的哨兵区，用来抓 DMA 越界写 */

#define RAW_GUARD_BYTES   (256 * 1024)
#define RAW_GUARD_BYTE    0xa5

static rk3576_video_conv_t g_conv;
static FAR void           *g_conv_arg;

/* 采集尺寸 = 传感器模式的尺寸，由板级在注册转换器时告知。
 * CIF 永远按这个尺寸搬运；上层请求的尺寸只决定 JPEG 的输出大小。
 */

static int                 g_capw;
static int                 g_caph;

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int rk3576_video_init(FAR struct imgdata_s *data);
static int rk3576_video_uninit(FAR struct imgdata_s *data);
static int rk3576_video_set_buf(FAR struct imgdata_s *data,
                                uint8_t nr_datafmts,
                                FAR imgdata_format_t *datafmts,
                                uint8_t *addr, uint32_t size);
static int rk3576_video_validate_frame_setting(
                                FAR struct imgdata_s *data,
                                uint8_t nr_datafmts,
                                FAR imgdata_format_t *datafmts,
                                FAR imgdata_interval_t *interval);
static int rk3576_video_start_capture(FAR struct imgdata_s *data,
                                uint8_t nr_datafmts,
                                FAR imgdata_format_t *datafmts,
                                FAR imgdata_interval_t *interval,
                                FAR imgdata_capture_t callback,
                                FAR void *arg);
static int rk3576_video_stop_capture(FAR struct imgdata_s *data);
static void rk3576_video_encode_work(FAR void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct imgdata_ops_s g_rk3576_video_ops =
{
  .init                   = rk3576_video_init,
  .uninit                 = rk3576_video_uninit,
  .set_buf                = rk3576_video_set_buf,
  .validate_frame_setting = rk3576_video_validate_frame_setting,
  .start_capture          = rk3576_video_start_capture,
  .stop_capture           = rk3576_video_stop_capture,
};

static struct rk3576_video_s g_rk3576_video =
{
  .data =
    {
      .ops = &g_rk3576_video_ops,
    },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_video_encode_work
 *
 * Description:
 *   把刚收到的一帧 RAW 转成 JPEG 写进上层的 buffer，然后回调。
 *
 *   ★ 回调报的必须是**压缩后**的真实字节数，不是帧大小。上层按这个
 *     长度把数据交给 Vision LLM，报错了就是一张截断的图。
 *
 ****************************************************************************/

static void rk3576_video_encode_work(FAR void *arg)
{
  FAR struct rk3576_video_s *priv = (FAR struct rk3576_video_s *)arg;
  int len;
  FAR const uint8_t *g;
  size_t i;

  if (!priv->capturing || priv->callback == NULL || g_conv == NULL)
    {
      return;
    }

  rk3576_cif_stop(RK3576_VIDEO_HOST);
  priv->cif_running = false;

  /* DMA 刚写完这块内存，CPU 侧的缓存里可能是旧数据 */

  up_invalidate_dcache((uintptr_t)priv->rawbuf,
                       (uintptr_t)priv->rawbuf + priv->rawbytes);

  g = (FAR const uint8_t *)priv->rawbuf +
      priv->rawbytes;

  up_invalidate_dcache((uintptr_t)g, (uintptr_t)g + RAW_GUARD_BYTES);
  for (i = 0; i < RAW_GUARD_BYTES && g[i] == RAW_GUARD_BYTE; i++);
  if (i < RAW_GUARD_BYTES)
    {
      size_t last = i;
      size_t k;

      for (k = i; k < RAW_GUARD_BYTES; k++)
        {
          if (g[k] != RAW_GUARD_BYTE)
            {
              last = k;
            }
        }

      syslog(LOG_ERR, "CIF: ★ DMA 越界写 RAW 缓冲尾部：第 %zu..%zu 字节"
             "（缓冲 %zu 字节，一行 %d 字节）\n",
             i, last, priv->rawbytes, priv->stride_pix * 2);
    }

  len = g_conv(priv->rawbuf, priv->stride_pix, g_capw, g_caph,
               priv->width, priv->height,
               priv->buf, priv->outlen, g_conv_arg);
  if (len <= 0)
    {
      nerr("ERROR: JPEG 编码失败: %d\n", len);
      priv->callback(1, 0, &priv->ts, priv->arg);   /* result!=0 表示坏帧 */
      return;
    }

  priv->callback(0, (uint32_t)len, &priv->ts, priv->arg);
}

/****************************************************************************
 * Name: rk3576_video_interrupt
 *
 * Description:
 *   CIF 帧结束中断。清标志，把这一帧交给上层。
 *
 *   ★ 中断状态位必须在这里清 —— CIF 的中断是电平触发的（dtsi 写的是
 *     IRQ_TYPE_LEVEL_HIGH），不清就会立刻重入。GMAC 那边刚栽过同一个跟头。
 *
 ****************************************************************************/

static int rk3576_video_interrupt(int irq, FAR void *context, FAR void *arg)
{
  FAR struct rk3576_video_s *priv = (FAR struct rk3576_video_s *)arg;
  struct timeval ts;
  uint32_t status;

  status = rk3576_cif_status(RK3576_VIDEO_HOST);

  /* 清标志（写 1 清）。不清则中断线一直拉着。 */

  rk3576_cif_clear_status(RK3576_VIDEO_HOST, status);

  if (priv->capturing && priv->callback != NULL)
    {
      gettimeofday(&ts, NULL);

      if (priv->jpeg_mode)
        {
          /* ★ 编码要几十毫秒，绝不能在 ISR 里做 —— 那会把中断关到
           *   下一帧之后，直接丢帧，而且看门狗可能先复位。
           *   转到工作队列，时间戳在这里取（那才是这一帧的时刻）。
           */

          /* 刚(重)启动时 DMA 是在一帧中间接上的，那一帧不完整，丢掉 */

          if (priv->skip > 0)
            {
              priv->skip--;
              return OK;
            }

          if (!priv->want || !work_available(&priv->work))
            {
              return OK;
            }

          /* 停中断、交给工作队列：编码期间 DMA 必须停，否则它会在
           * 我们读 RAW 的同时写下一帧，编出来是撕裂的图。
           */

          priv->want = false;
          priv->ts = ts;
          up_disable_irq(RK3576_IRQ_CIF);
          work_queue(LPWORK, &priv->work, rk3576_video_encode_work,
                     priv, 0);
        }
      else
        {
          /* result=0 表示这一帧是好的。CIF 侧目前没有区分错误帧的判据，
           * 有了再补 —— 现在报成功而实际是坏帧，比谎称失败更容易被发现。
           */

          priv->callback(0, priv->framesize, &ts, priv->arg);
        }
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_video_init
 ****************************************************************************/

static int rk3576_video_init(FAR struct imgdata_s *data)
{
  FAR struct rk3576_video_s *priv = (FAR struct rk3576_video_s *)data;

  /* 时钟、D-PHY、CSI host 的上电在板级初始化里已经做过
   * （rk3576_cif_probe 那条路径），这里只做本层的状态复位。
   */

  priv->capturing = false;
  priv->callback  = NULL;
  priv->arg       = NULL;
  priv->buf       = NULL;

  return OK;
}

/****************************************************************************
 * Name: rk3576_video_uninit
 ****************************************************************************/

static int rk3576_video_uninit(FAR struct imgdata_s *data)
{
  FAR struct rk3576_video_s *priv = (FAR struct rk3576_video_s *)data;

  if (priv->capturing)
    {
      rk3576_video_stop_capture(data);
    }

  if (priv->irq_attached)
    {
      up_disable_irq(RK3576_IRQ_CIF);
      irq_detach(RK3576_IRQ_CIF);
      priv->irq_attached = false;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_video_set_buf
 *
 * Description:
 *   上层指定这一帧写到哪。
 *
 *   ★ CIF 是硬件乒乓（FRM0/FRM1 两个地址寄存器）。V4L2 一次只给一个
 *     缓冲区，所以两个槽都指向它 —— 等于退化成单缓冲。
 *     这会在高帧率下丢帧，但正确性没问题：DMA 始终写在上层给的地址上。
 *     要用满乒乓得让上层一次给两个 buffer，那是 V4L2 队列的事，
 *     等基本通路跑通再说。
 *
 ****************************************************************************/

static int rk3576_video_set_buf(FAR struct imgdata_s *data,
                                uint8_t nr_datafmts,
                                FAR imgdata_format_t *datafmts,
                                uint8_t *addr, uint32_t size)
{
  FAR struct rk3576_video_s *priv = (FAR struct rk3576_video_s *)data;
  int ret;

  if (addr == NULL || size == 0)
    {
      return -EINVAL;
    }

  priv->buf       = addr;
  priv->framesize = size;
  priv->outlen    = size;

  /* JPEG 模式下 CIF 写的是内部 RAW 缓冲，不是上层这块 —— 上层这块
   * 是编码结果的目的地。所以这里不把它交给 CIF。
   */

  if (priv->jpeg_mode)
    {
      priv->want = true;

      /* 上一帧编码时停掉了 DMA，这里为下一帧重新启动 */

      if (priv->capturing && !priv->cif_running)
        {
          ret = rk3576_cif_start(RK3576_VIDEO_HOST, (uintptr_t)priv->rawbuf,
                                 (uintptr_t)priv->rawbuf, g_capw, g_caph);
          if (ret < 0)
            {
              return ret;
            }

          priv->cif_running = true;
          priv->skip = 1;
          up_enable_irq(RK3576_IRQ_CIF);
        }

      return OK;
    }

  ret = rk3576_cif_set_buffer(RK3576_VIDEO_HOST, 0, (uintptr_t)addr);
  if (ret < 0)
    {
      return ret;
    }

  return rk3576_cif_set_buffer(RK3576_VIDEO_HOST, 1, (uintptr_t)addr);
}

/****************************************************************************
 * Name: rk3576_video_validate_frame_setting
 *
 * Description:
 *   上层问"这个格式/尺寸能不能拍"。
 *
 *   ★ CIF 在我们这条链路上是**透传**的：MIPI 收到什么就写什么，
 *     不做去马赛克也不做格式转换。IMX415 出的是 10-bit RAW，
 *     打包后按 2 字节/像素落盘，所以这里只认 SBGGR10 这一种。
 *     RGB/YUV 要等去马赛克做完才能声称支持 —— 现在谎报支持，
 *     上层会拿到一堆看不懂的字节。
 *
 ****************************************************************************/

static int rk3576_video_validate_frame_setting(
                                FAR struct imgdata_s *data,
                                uint8_t nr_datafmts,
                                FAR imgdata_format_t *datafmts,
                                FAR imgdata_interval_t *interval)
{
  if (nr_datafmts != 1 || datafmts == NULL)
    {
      return -EINVAL;
    }

  /* SBGGR10：CIF 透传，上层直接拿 RAW。
   * JPEG：软件转换，需要板级注册过转换器。
   *
   * ★ 没注册转换器就不声称支持 JPEG —— 谎报支持的话，上层会设成
   *   JPEG 然后拿到一堆 Bayer 原始字节，当成 JPEG 去解，
   *   错误会出现在很远的地方。
   */

  /* ★ 这里的 pixelformat 是 **IMGDATA_PIX_FMT_\*** 枚举，不是 V4L2 的
   *   fourcc（上半部的 convert_to_imgdatafmt 已经翻译过）。
   *
   * ★ 只支持 JPEG：这套枚举里没有 Bayer RAW，V4L2 那条路出不了 RAW。
   */

  if (datafmts[0].pixelformat != IMGDATA_PIX_FMT_JPEG)
    {
      return -EINVAL;
    }

  if (datafmts[0].width == 0 || datafmts[0].height == 0)
    {
      return -EINVAL;
    }

  if (g_conv == NULL || g_capw <= 0 || g_caph <= 0)
    {
      return -EINVAL;
    }

  /* 输出可以小于采集尺寸（转换器负责缩放），不能更大 —— 传感器给不出
   * 那么多信息（见 kickpi_imgproc_jpeg_scaled）。
   */

  if (datafmts[0].width > g_capw || datafmts[0].height > g_caph)
    {
      return -EINVAL;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_video_start_capture
 ****************************************************************************/

static int rk3576_video_start_capture(FAR struct imgdata_s *data,
                                uint8_t nr_datafmts,
                                FAR imgdata_format_t *datafmts,
                                FAR imgdata_interval_t *interval,
                                FAR imgdata_capture_t callback,
                                FAR void *arg)
{
  FAR struct rk3576_video_s *priv = (FAR struct rk3576_video_s *)data;
  int ret;
  uintptr_t dma;

  ret = rk3576_video_validate_frame_setting(data, nr_datafmts,
                                            datafmts, interval);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->buf == NULL)
    {
      nerr("ERROR: 还没设过缓冲区就要开始取图\n");
      return -EINVAL;
    }

  priv->width    = datafmts[0].width;
  priv->height   = datafmts[0].height;
  priv->callback = callback;
  priv->arg      = arg;
  priv->jpeg_mode = (datafmts[0].pixelformat == IMGDATA_PIX_FMT_JPEG);

  if (priv->jpeg_mode)
    {
      /* CIF 的行跨距要 256 字节对齐（见 rk3576_cif.c），
       * 每像素 2 字节。写成 "宽度 x 2" 是错的。
       */

      size_t line = (((size_t)g_capw * 2) + 255) & ~(size_t)255;
      size_t need = line * (size_t)g_caph;

      if (priv->rawbuf != NULL && priv->rawbytes != need)
        {
          kmm_free(priv->rawbuf);
          priv->rawbuf = NULL;
        }

      if (priv->rawbuf == NULL)
        {
          /* 尾部多留一段并填哨兵，检查 DMA 有没有越界写 */

          priv->rawbuf = kmm_memalign(64, need + RAW_GUARD_BYTES);
          if (priv->rawbuf == NULL)
            {
              nerr("ERROR: 分配 %zu 字节 RAW 缓冲失败\n", need);
              return -ENOMEM;
            }

          priv->rawbytes = need;
          memset((FAR uint8_t *)priv->rawbuf + need, RAW_GUARD_BYTE,
                 RAW_GUARD_BYTES);
          up_clean_dcache((uintptr_t)priv->rawbuf + need,
                          (uintptr_t)priv->rawbuf + need + RAW_GUARD_BYTES);
        }

      priv->stride_pix = (int)(line / 2);
    }

  if (!priv->irq_attached)
    {
      ret = irq_attach(RK3576_IRQ_CIF, rk3576_video_interrupt, priv);
      if (ret < 0)
        {
          nerr("ERROR: 挂 CIF 中断失败: %d\n", ret);
          return ret;
        }

      priv->irq_attached = true;
    }

  dma = priv->jpeg_mode ? (uintptr_t)priv->rawbuf
                        : (uintptr_t)priv->buf;

  /* ★ CIF 一律按采集尺寸搬运。JPEG 模式下 priv->width/height 是
   *   **输出**尺寸，喂给 CIF 会让它按错的跨距写内存。
   */

  int cifw = priv->jpeg_mode ? g_capw : (int)priv->width;
  int cifh = priv->jpeg_mode ? g_caph : (int)priv->height;

  ret = rk3576_cif_start(RK3576_VIDEO_HOST, dma, dma, cifw, cifh);
  if (ret < 0)
    {
      return ret;
    }

  priv->capturing = true;
  priv->cif_running = true;
  priv->skip = priv->jpeg_mode ? 1 : 0;

  /* V4L2 先 set_buf 再 start_capture，而 jpeg_mode 到这里才定下来 ——
   * 第一个缓冲区在 set_buf 里没能置位 want，这里补上。
   */

  priv->want = priv->jpeg_mode && priv->buf != NULL;
  up_enable_irq(RK3576_IRQ_CIF);

  return OK;
}

/****************************************************************************
 * Name: rk3576_video_stop_capture
 ****************************************************************************/

static int rk3576_video_stop_capture(FAR struct imgdata_s *data)
{
  FAR struct rk3576_video_s *priv = (FAR struct rk3576_video_s *)data;

  up_disable_irq(RK3576_IRQ_CIF);
  priv->capturing   = false;
  priv->callback    = NULL;
  priv->want        = false;
  priv->cif_running = false;

  return rk3576_cif_stop(RK3576_VIDEO_HOST);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_video_imgdata
 *
 * Description:
 *   拿到 imgdata 实例，交给板级去和 imgsensor 一起 capture_register()。
 *
 ****************************************************************************/

FAR struct imgdata_s *rk3576_video_imgdata(void)
{
  return &g_rk3576_video.data;
}

/****************************************************************************
 * Name: rk3576_video_set_converter
 *
 * Description:
 *   注册 RAW -> JPEG 的转换函数。不注册就不支持 JPEG 格式。
 *
 *   ★ 为什么要注册而不是直接调：去马赛克需要知道 Bayer 相位和黑白
 *     电平，那都是**传感器/板级**的知识。芯片层直接调板级函数就成了
 *     反向依赖，换一块板就得改芯片层。
 *
 ****************************************************************************/

void rk3576_video_set_converter(rk3576_video_conv_t fn,
                                int capw, int caph, FAR void *arg)
{
  g_conv     = fn;
  g_conv_arg = arg;
  g_capw     = capw;
  g_caph     = caph;
}

#endif /* CONFIG_RK3576_VIDEO */
