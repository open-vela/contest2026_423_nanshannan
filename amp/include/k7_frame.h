/****************************************************************************
 * amp/include/k7_frame.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A72 上的 Linux（生产者）与 A53 上的 openvela（消费者）之间的帧共享区
 * 布局。两边同时包含这一份头文件：openvela 的 ampctl / kickpi_ui，Linux
 * 用户态的 k7d。布局只在这里定义一次。
 *
 * ★ 物理位置不在这里
 *
 *   基址和大小两边各配一处，必须相等（和 rpmsg 共享内存同一个规矩）：
 *     openvela  chip/rk3576/Kconfig 的 RK3576_FRAME_SHM_BASE / _SIZE
 *     Linux     amp/linux/rk3576-kickpi-k7-amp.dts 的 k7_frame 保留区
 *   Linux 用户态不需要知道物理地址，它 mmap /dev/k7frame。
 *
 *   0x50000000 + 64MB。原规划（amp/OWNERSHIP.md）是 0x4e400000，但
 *   U-Boot 把 Linux 的 DTB 放在 0x4f000000，正好在那 64MB 里面。
 *
 * ★ 布局
 *
 *   +0          帧头 struct k7f_hdr_s（占满 4KB）
 *   +4KB        槽 0
 *   +4KB+S      槽 1 ...                  S = hdr.slot_size
 *
 * ★ 发布协议（单生产者）
 *
 *   生产者写一个**不是当前已发布**的槽，写完以后：
 *     1. 屏障（Linux 映射是 write-combine，要把写缓冲排空）
 *     2. 填 slot / width / height / stride / format / bytes / seed
 *     3. 屏障
 *     4. seq 加一
 *   消费者先读 seq，再读其余字段和像素，最后再读一次 seq：两次不同就说明
 *   中途发布了新帧，这一帧作废重读。槽有 4 个，生产者要连写三帧才会绕回
 *   消费者正在读的槽，所以前后两次 seq 相差 < 3 时读到的槽仍然完整。
 *
 * ★ cache 契约
 *
 *   Linux 用户态的映射是 write-combine（Normal-NC），写完不需要 clean。
 *   openvela 把整段映成 cacheable，**每次读之前 invalidate 帧头和要读的
 *   那个槽** —— 否则读到的是上一轮留在 cache 里的旧数据，而且不报错。
 *   以后由 ISP / RGA 用 DMA 写入，同样只需要消费者 invalidate。
 ****************************************************************************/

#ifndef __AMP_INCLUDE_K7_FRAME_H
#define __AMP_INCLUDE_K7_FRAME_H

#include <stdint.h>

#define K7F_MAGIC         0x5246374bu   /* "K7FR"，小端存储 */
#define K7F_VERSION       1
#define K7F_HDR_SIZE      4096u
#define K7F_SLOTS         4

/* 像素格式：V4L2 的 fourcc，方便以后直接沿用摄像头那边的定义 */

#define K7F_FOURCC(a, b, c, d) \
  ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | \
   ((uint32_t)(d) << 24))
#define K7F_FMT_XRGB8888  K7F_FOURCC('X', 'R', '2', '4')   /* B,G,R,X 字节序 */

/* 帧内容的来源 */

#define K7F_SRC_PATTERN   1   /* k7d frame 写的测试图案，可逐字节核对 */
#define K7F_SRC_CAMERA    2

struct k7f_hdr_s
{
  uint32_t magic;
  uint32_t version;
  uint32_t seq;            /* 每发布一帧加一；0 表示还没有帧 */
  uint32_t slot;           /* 最新一帧在哪个槽 */
  uint32_t slot_size;      /* 每个槽的字节数 */
  uint32_t width;
  uint32_t height;
  uint32_t stride;         /* 每行字节数 */
  uint32_t format;         /* K7F_FMT_* */
  uint32_t bytes;          /* 这一帧有效字节数 = stride * height */
  uint32_t source;         /* K7F_SRC_* */
  uint32_t seed;           /* 测试图案的参数，见 k7f_pattern() */
  uint32_t reserved[4];
};

/* 槽 i 相对共享区起点的偏移 */

static inline uint32_t k7f_slot_offset(const struct k7f_hdr_s *h,
                                       uint32_t i)
{
  return K7F_HDR_SIZE + i * h->slot_size;
}

/* 测试图案：横向红色渐变、纵向绿色渐变，蓝色是 64px 棋盘格再随 seed
 * 变化。两边按同一个公式算，openvela 拿它逐像素核对。
 * 每个像素的值都依赖 x、y、seed，行错位、列错位、拿到上一帧都会被发现。
 */

static inline uint32_t k7f_pattern(uint32_t x, uint32_t y, uint32_t w,
                                   uint32_t h, uint32_t seed)
{
  uint32_t r = w > 1 ? x * 255 / (w - 1) : 0;
  uint32_t g = h > 1 ? y * 255 / (h - 1) : 0;
  uint32_t b = (seed * 37 + (((x >> 6) ^ (y >> 6)) & 1) * 128 +
                (x & 0x3f)) & 0xff;

  return 0xff000000u | (r << 16) | (g << 8) | b;
}

#endif /* __AMP_INCLUDE_K7_FRAME_H */
