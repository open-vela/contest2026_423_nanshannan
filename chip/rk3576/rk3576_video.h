/****************************************************************************
 * chip/rk3576/rk3576_video.h
 *
 * RK3576 VICAP/CIF 的 V4L2 下半部（imgdata）。
 *
 * 板级拿到这个 imgdata 实例后，与 IMX415 的 imgsensor 一起调
 * capture_register("/dev/video0", ...) 注册成 V4L2 设备。
 *
 ****************************************************************************/

#ifndef __CHIP_RK3576_RK3576_VIDEO_H
#define __CHIP_RK3576_RK3576_VIDEO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/video/imgdata.h>

#ifndef __ASSEMBLY__

/****************************************************************************
 * Name: rk3576_video_imgdata
 *
 * Description:
 *   取得 CIF 的 imgdata 实例。
 *
 *   ★ 调用前 CIF 必须已经由板级初始化过（时钟、D-PHY、CSI host）。
 *     本层只负责把取图接口包成 V4L2 要的形状，不做上电。
 *
 ****************************************************************************/

FAR struct imgdata_s *rk3576_video_imgdata(void);

/****************************************************************************
 * RAW -> JPEG 转换器
 *
 *   CIF 只出 Bayer RAW，而 ai_agent 的视觉工具向 /dev/video0 请求的是
 *   V4L2_PIX_FMT_JPEG，所以 JPEG 只能软件生成。
 *
 *   ★ 做成注册式而不是芯片层直接调：去马赛克要知道 Bayer 相位和黑白
 *     电平，那是传感器/板级的知识。芯片层直接依赖板级的话，换块板就
 *     得改芯片层。
 *
 *   返回写入 out 的字节数（>0），失败返回负值。
 ****************************************************************************/

typedef int (*rk3576_video_conv_t)(FAR const uint16_t *raw, int stride_pix,
                                   int srcw, int srch,
                                   int dstw, int dsth,
                                   FAR uint8_t *out, size_t outlen,
                                   FAR void *arg);

/* 注册转换器，同时告诉芯片层**采集尺寸**（传感器模式的尺寸）。
 *
 * ★ 采集尺寸和输出尺寸是两件事。传感器只有一个固定模式，CIF 必须按那个
 *   尺寸搬运；而上层请求的输出尺寸可以更小（ai_agent 要 1280x720 或
 *   320x180）。以前芯片层把请求尺寸直接喂给 CIF，只有两者恰好相等时
 *   才对 —— 一旦上层请求小图，CIF 就按错的尺寸算跨距，画面会斜。
 */

void rk3576_video_set_converter(rk3576_video_conv_t fn,
                                int capw, int caph, FAR void *arg);

#endif /* __ASSEMBLY__ */
#endif /* __CHIP_RK3576_RK3576_VIDEO_H */
