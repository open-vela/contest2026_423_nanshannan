/*
 * k7cam：A72 上 Linux 的摄像头取图验证工具（lxroot 镜像里的 /usr/bin/k7cam）。
 *
 *   k7cam cap [帧数 1~300] [show|-] [曝光行数] [模拟增益]
 *   k7cam isp [帧数 1~300] [show|-] [宽 高]
 *   k7cam live [秒数 1~600] [曝光行数] [模拟增益]
 *   k7cam live-isp [秒数 1~600]
 *
 * 从 rkcif 的 RAW 节点（stream_cif_mipi_id0）取若干帧，逐帧报序号、
 * 帧间隔、字节数和按 Bayer 四个相位分开的亮度均值；带 show 时把最后一帧
 * 做成彩色预览写进帧共享区（amp/include/k7_frame.h），openvela 那边
 * `echo 1 > /tmp/k7-frame` 显示。
 *
 * live 是给 openvela 界面的相机页用的：同一条 RAW 链持续开流，**每一帧**
 * 都发布到帧共享区（30fps，传感器 4K 下的上限），不逐帧打印。停止条件
 * 三选一：到了给定秒数、出现 /tmp/k7live.stop（openvela 的 `ampctl cam
 * stop` 建它）、取帧出错。原先界面每一帧都要重新起一次 `cap 1 show`，
 * 光起流就要 80ms 以上，加上进程启动只有 3~4fps。
 *
 * 曝光和增益直接写传感器子设备的 V4L2 控件（不给就不动）。没有 rkaiq
 * 时驱动的默认曝光只有 8 行、增益最小，拍出来是一片黑电平。
 *
 * 这一步不经过 ISP、没有 3A：证明的是 sensor -> D-PHY -> CSI-2 -> CIF ->
 * DDR 这一段是通的。预览是每个 2x2 Bayer 块直接取 R、G、B，按灰色世界
 * 假设配白平衡，再按 99% 分位拉伸 —— 没有 ISP 时能做到的最低限度的彩色，
 * 不是成像质量（没有去马赛克插值、没有降噪和伽马）。
 *
 * live-isp 是 live 的 ISP 版：经 ISP 出 1280x720 NV12，每帧转 XRGB 发布，
 * 停止条件同 live。曝光、白平衡交给 rkaiq（要先 k7aiq start），画质比
 * RAW 直出好得多。取不到帧时退出码非 0，ampctl cam start 据此退回 live。
 *
 * isp 从 rkisp_mainpath 取 NV12（经 ISP，宽高默认 1280x720，由 mainpath
 * 缩放），逐帧报 Y/U/V 均值；带 show 时把最后一帧转成彩色发布。3A 要先用
 * k7aiq start 起 rkaiq_3A_server，否则 ISP 用的是驱动默认参数（曝光 8 行），
 * 画面同样是黑的 —— Y 均值随帧数爬升就是 AE 在收敛。
 *
 * ★ 每次等待都有上限（poll 2 秒），整个运行还有一个总期限，到了就带着
 *   已经拿到的结果退出 —— 取图卡住时这个工具必须自己回来。
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>

#include "k7_frame.h"

/* include/uapi/linux/rkcif-config.h */
#define RKCIF_CMD_SET_CSI_MEMORY_MODE _IOW('V', BASE_VIDIOC_PRIVATE + 1, int)
#define CSI_LVDS_MEM_WORD_LOW_ALIGN   1

#define CIF_NAME        "stream_cif_mipi_id0"
#define ISP_NAME        "rkisp_mainpath"
#define SENSOR_PREFIX   "m02_b_imx415"
#define NBUF            4
#define MAX_FRAMES      300
#define LIVE_MAX_S      600
#define LIVE_STOP       "/tmp/k7live.stop"
#define POLL_MS         2000
#define FRAME_DEV       "/dev/k7frame"
#define FRAME_SHM_SIZE  (64u << 20)
#define PREVIEW_W       1280      /* openvela 相机页整屏 1280x720 */

/* live 模式下不逐帧打印（共享区映射、发布这些每帧都有的日志） */

static int g_quiet;

/* show() 上一帧的白平衡系数和拉伸分位，live 用来平滑 */

static unsigned g_kr, g_kb, g_p99;

struct buf_s {
  void *p;
  size_t len;
};

static long now_ms(void)
{
  struct timespec t;

  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* /sys/class/video4linux/<节点>/name 等于 want（或以它开头）的那个节点 */

static int find_node(const char *want, int prefix, char *path, size_t len)
{
  struct dirent *de;
  DIR *d = opendir("/sys/class/video4linux");
  int found = 0;

  if (d == NULL)
    return 0;

  while (!found && (de = readdir(d)) != NULL) {
    char fn[300];
    char name[64] = "";
    FILE *f;

    if (de->d_name[0] == '.')
      continue;
    snprintf(fn, sizeof(fn), "/sys/class/video4linux/%s/name", de->d_name);
    f = fopen(fn, "r");
    if (f == NULL)
      continue;
    if (fgets(name, sizeof(name), f) != NULL) {
      name[strcspn(name, "\n")] = 0;
      if (prefix ? strncmp(name, want, strlen(want)) == 0
                 : strcmp(name, want) == 0) {
        snprintf(path, len, "/dev/%s", de->d_name);
        found = 1;
      }
    }
    fclose(f);
  }

  closedir(d);
  return found;
}

/* 映射帧共享区，必要时初始化帧头，返回下一个要写的槽 */

static uint8_t *shm_begin(struct k7f_hdr_s **hdp, uint32_t *slotp,
                          uint32_t **pxp)
{
  struct k7f_hdr_s *hd;
  uint8_t *base;
  int fd;

  fd = open(FRAME_DEV, O_RDWR);
  if (fd < 0) {
    printf("打不开 %s: %s\n", FRAME_DEV, strerror(errno));
    return NULL;
  }
  base = mmap(NULL, FRAME_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (base == MAP_FAILED) {
    printf("mmap %s 失败: %s\n", FRAME_DEV, strerror(errno));
    return NULL;
  }
  if (!g_quiet)
    printf("帧共享区: %p + %u\n", (void *)base, FRAME_SHM_SIZE);

  hd = (struct k7f_hdr_s *)base;
  if (hd->magic != K7F_MAGIC || hd->version != K7F_VERSION) {
    memset(hd, 0, K7F_HDR_SIZE);
    hd->slot_size = ((FRAME_SHM_SIZE - K7F_HDR_SIZE) / K7F_SLOTS) &
                    ~(K7F_HDR_SIZE - 1);
    hd->version = K7F_VERSION;
    __asm__ volatile("dsb sy" ::: "memory");
    hd->magic = K7F_MAGIC;
  }

  *slotp = hd->seq == 0 ? 0 : (hd->slot + 1) % K7F_SLOTS;
  *pxp = (uint32_t *)(base + k7f_slot_offset(hd, *slotp));
  *hdp = hd;
  return base;
}

/* 槽写完之后再更新帧头，seq 最后加：openvela 看到新 seq 时像素已经在了 */

static void shm_commit(uint8_t *base, struct k7f_hdr_s *hd, uint32_t slot,
                       unsigned ow, unsigned oh)
{
  __asm__ volatile("dsb sy" ::: "memory");
  hd->slot = slot;
  hd->width = ow;
  hd->height = oh;
  hd->stride = ow * 4;
  hd->format = K7F_FMT_XRGB8888;
  hd->bytes = ow * 4 * oh;
  hd->source = K7F_SRC_CAMERA;
  hd->seed = 0;
  __asm__ volatile("dsb sy" ::: "memory");
  hd->seq++;
  __asm__ volatile("dsb sy" ::: "memory");
  if (!g_quiet)
    printf("预览已发布：seq=%u 槽=%u %ux%u\n", hd->seq, slot, ow, oh);
  munmap(base, FRAME_SHM_SIZE);
}

/* 把一帧 RAW 做成 PREVIEW_W 宽的彩色图发布到帧共享区。
 *
 * GBRG：2x2 块里 [0]=G [1]=B 下一行 [0]=R [1]=G。每个输出像素取一个块，
 * 不插值。白平衡按灰色世界：R、B 各乘一个系数，使三色均值相等；亮度按
 * 配平后最亮通道的 99% 分位拉伸到 255。系数是整数 Q8，逐帧重算。
 */

static void show(const uint16_t *raw, unsigned w, unsigned h, unsigned bpl)
{
  unsigned ow = PREVIEW_W;
  unsigned oh = (unsigned)((uint64_t)h * ow / w) & ~1u;
  uint32_t hist[1024] = { 0 };
  unsigned stride16 = bpl / 2;
  unsigned x, y, p99 = 0, total = 0, acc = 0;
  uint64_t sr = 0, sg = 0, sb = 0;
  unsigned kr, kb;
  struct k7f_hdr_s *hd;
  uint32_t *px;
  uint32_t slot;
  uint8_t *base;

#define BLOCK(x, y) (raw + (((y) * h / oh) & ~1u) * stride16 + \
                     (((x) * w / ow) & ~1u))

  for (y = 0; y < oh; y += 4)
    for (x = 0; x < ow; x += 4) {
      const uint16_t *q = BLOCK(x, y);

      sg += q[0] + q[stride16 + 1];
      sb += q[1];
      sr += q[stride16];
    }
  sg /= 2;
  kr = sr ? (unsigned)(sg * 256 / sr) : 256;
  kb = sb ? (unsigned)(sg * 256 / sb) : 256;
  if (kr > 1024)
    kr = 1024;
  if (kb > 1024)
    kb = 1024;

  /* live 每帧都发布：白平衡系数和下面的拉伸分位逐帧独立算会跟着画面
   * 内容跳（手一挥过，整幅亮度和色调都抖），所以按 3/4 旧 + 1/4 新平滑。
   */

  if (g_quiet && g_kr != 0) {
    kr = (g_kr * 3 + kr) / 4;
    kb = (g_kb * 3 + kb) / 4;
  }
  g_kr = kr;
  g_kb = kb;

  for (y = 0; y < oh; y += 2)
    for (x = 0; x < ow; x += 2) {
      const uint16_t *q = BLOCK(x, y);
      unsigned r = q[stride16] * kr >> 8, b = q[1] * kb >> 8;
      unsigned g = (q[0] + q[stride16 + 1]) / 2;
      unsigned m = r > g ? r : g;

      m = m > b ? m : b;
      hist[m > 1023 ? 1023 : m]++;
      total++;
    }
  for (p99 = 0; p99 < 1023 && acc < total * 99 / 100; p99++)
    acc += hist[p99];
  if (p99 < 16)
    p99 = 16;
  if (g_quiet && g_p99 != 0)
    p99 = (g_p99 * 3 + p99) / 4;
  g_p99 = p99;

  base = shm_begin(&hd, &slot, &px);
  if (base == NULL)
    return;

  for (y = 0; y < oh; y++)
    for (x = 0; x < ow; x++) {
      const uint16_t *q = BLOCK(x, y);
      unsigned r = (q[stride16] * kr >> 8) * 255 / p99;
      unsigned g = ((q[0] + q[stride16 + 1]) / 2) * 255 / p99;
      unsigned b = (q[1] * kb >> 8) * 255 / p99;

      px[y * ow + x] = 0xff000000u | (r > 255 ? 255 : r) << 16 |
                       (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
    }

#undef BLOCK

  if (!g_quiet)
    printf("白平衡 R x%u.%02u B x%u.%02u，99%% 分位 %u 拉到 255\n",
           kr >> 8, (kr & 255) * 100 / 256, kb >> 8, (kb & 255) * 100 / 256,
           p99);
  shm_commit(base, hd, slot, ow, oh);
}

static uint8_t clamp8(int v)
{
  return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* 把一帧 NV12 缩到 PREVIEW_W 宽、按 BT.601 有限范围转成 XRGB 发布 */

static void show_nv12(const uint8_t *yp, const uint8_t *uvp, unsigned w,
                      unsigned h, unsigned bpl)
{
  unsigned ow = w < PREVIEW_W ? w : PREVIEW_W;
  unsigned oh = (unsigned)((uint64_t)h * ow / w) & ~1u;
  struct k7f_hdr_s *hd;
  uint32_t *px;
  uint32_t slot;
  uint8_t *base;
  unsigned x, y;

  base = shm_begin(&hd, &slot, &px);
  if (base == NULL)
    return;

  for (y = 0; y < oh; y++)
    for (x = 0; x < ow; x++) {
      unsigned sy = y * h / oh, sx = x * w / ow;
      const uint8_t *uv = uvp + (sy / 2) * bpl + (sx & ~1u);
      int c = 298 * (yp[sy * bpl + sx] - 16);
      int d = uv[0] - 128, e = uv[1] - 128;

      px[y * ow + x] = 0xff000000u |
                       (clamp8((c + 409 * e + 128) >> 8) << 16) |
                       (clamp8((c - 100 * d - 208 * e + 128) >> 8) << 8) |
                       clamp8((c + 516 * d + 128) >> 8);
    }

  shm_commit(base, hd, slot, ow, oh);
}

/* 写一个子设备控件，并把驱动实际接受的值和范围读回来报告 */

static void set_ctrl(int sfd, unsigned id, const char *name, int val)
{
  struct v4l2_queryctrl qc = { .id = id };
  struct v4l2_control c = { .id = id, .value = val };

  if (ioctl(sfd, VIDIOC_QUERYCTRL, &qc) < 0) {
    printf("%s: 传感器没有这个控件（%s）\n", name, strerror(errno));
    return;
  }
  if (ioctl(sfd, VIDIOC_S_CTRL, &c) < 0)
    printf("%s=%d 失败: %s\n", name, val, strerror(errno));
  c.value = 0;
  ioctl(sfd, VIDIOC_G_CTRL, &c);
  printf("%s = %d（范围 %d~%d）\n", name, c.value, qc.minimum, qc.maximum);
}

/* live 的简易自动曝光。
 *
 * 没有 ISP、没有 rkaiq，曝光和增益只能自己调。被测量是 RAW 绿色通道
 * 均值减黑电平（实测 ~51），目标 AE_TARGET。IMX415 的模拟增益一级是
 * 0.3dB（倍率 1.0351，Q16 是 67839），偏差折算成"差几级"，每次只走一半
 * （传感器的新值要隔一两帧才生效，走满会来回振荡），一次最多 20 级。
 * 太暗先加曝光再加增益；太亮先减增益到 0 再减曝光。每两帧调一次。
 *
 * ★ 抗频闪：曝光超过 10ms 时只取 10ms 的整数倍。
 *
 *   50Hz 市电下灯光按 100Hz 明暗起伏。卷帘快门每一行的曝光窗口起点不同，
 *   窗口不是 10ms 整数倍时，各行积到的光量不一样 —— 画面上是横向的明暗
 *   条纹，而且条纹每帧挪一点（帧率 30 和 100Hz 不同步），看上去就是闪。
 *   原先一直用满一帧 2242 行（33.2ms），正好撞上。
 *   这个模式 VTS=2250 行 / 33.33ms，一行 14.81us，10ms = 675 行，可用
 *   675 / 1350 / 2025；凑不出的亮度由增益补。10ms 以下（很亮的场景）
 *   不受约束，那时条纹本来就弱。60Hz 地区要改成 8.33ms（562 行）。
 */

#define AE_BLACK    51
#define AE_TARGET   300
#define AE_BAND     675        /* 10ms */
#define AE_EXP_MAX  (3 * AE_BAND)
#define AE_EXP_MIN  8
#define AE_GAIN_MAX 240

static int ae_band(int e)
{
  return e >= AE_BAND ? e / AE_BAND * AE_BAND : e;
}

static void ae_step(int sfd, unsigned mean_g, int *exposure, int *gain)
{
  uint64_t net = mean_g > AE_BLACK + 1 ? mean_g - AE_BLACK : 1;
  uint64_t v = net << 16;
  struct v4l2_control c;
  int steps = 0;

  if (net * 100 > AE_TARGET * 90 && net * 100 < AE_TARGET * 110)
    return;                                   /* ±10% 以内不动 */

  if (net < AE_TARGET) {
    while (v < ((uint64_t)AE_TARGET << 16) && steps < 40) {
      v = v * 67839 >> 16;
      steps++;
    }
  } else {
    while (v > ((uint64_t)AE_TARGET << 16) && steps > -40) {
      v = v * 65536 / 67839;
      steps--;
    }
  }
  steps /= 2;
  if (steps == 0)
    steps = net < AE_TARGET ? 1 : -1;
  if (steps > 20)
    steps = 20;
  if (steps < -20)
    steps = -20;

  {
    uint64_t want = (uint64_t)*exposure * AE_TARGET / net;
    int e = ae_band(want > AE_EXP_MAX ? AE_EXP_MAX :
                    want < AE_EXP_MIN ? AE_EXP_MIN : (int)want);

    /* 曝光能往需要的方向挪一档就挪曝光，挪不动（已到头，或取整后原地
     * 不动）再动增益。变暗时增益先降到 0。
     */

    if (steps > 0 && e > *exposure)
      *exposure = e;
    else if (steps < 0 && *gain == 0 && e < *exposure)
      *exposure = e;
    else {
      *gain += steps;
      if (*gain < 0)
        *gain = 0;
      if (*gain > AE_GAIN_MAX)
        *gain = AE_GAIN_MAX;
    }
  }

  c.id = V4L2_CID_EXPOSURE;
  c.value = *exposure;
  ioctl(sfd, VIDIOC_S_CTRL, &c);
  c.id = V4L2_CID_ANALOGUE_GAIN;
  c.value = *gain;
  ioctl(sfd, VIDIOC_S_CTRL, &c);
}

static int cap_main(int frames, int want_show, int exposure, int gain,
                    int live)
{
  struct v4l2_subdev_format sf = { .which = V4L2_SUBDEV_FORMAT_ACTIVE };
  struct v4l2_requestbuffers rb = { 0 };
  struct v4l2_format fmt = { 0 };
  struct buf_s bufs[NBUF] = { 0 };
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  char vpath[300], spath[300];
  int fd = -1, sfd = -1, mode = CSI_LVDS_MEM_WORD_LOW_ALIGN;
  int got = 0, streaming = 0, ret = 1, i;
  long t_start, deadline, t_prev = 0, t_first = 0;
  unsigned w, h, bpl;

  if (!find_node(CIF_NAME, 0, vpath, sizeof(vpath))) {
    printf("找不到 %s（摄像头模块没装上？看 dmesg 里的 k7 camera）\n", CIF_NAME);
    return 1;
  }
  if (!find_node(SENSOR_PREFIX, 1, spath, sizeof(spath))) {
    printf("找不到 IMX415 的子设备节点\n");
    return 1;
  }

  /* 传感器当前输出多大，CIF 就按多大收 */

  sfd = open(spath, O_RDWR);
  if (sfd < 0 || ioctl(sfd, VIDIOC_SUBDEV_G_FMT, &sf) < 0) {
    printf("读 %s 格式失败: %s\n", spath, strerror(errno));
    if (sfd >= 0)
      close(sfd);
    return 1;
  }
  printf("传感器 %s: %ux%u code=0x%04x\n", spath, sf.format.width,
         sf.format.height, sf.format.code);
  if (exposure > 0)
    set_ctrl(sfd, V4L2_CID_EXPOSURE, "曝光", exposure);
  if (gain >= 0)
    set_ctrl(sfd, V4L2_CID_ANALOGUE_GAIN, "模拟增益", gain);

  /* live 要一直调曝光，子设备留着；其余模式到这里就用完了 */

  if (!live) {
    close(sfd);
    sfd = -1;
  } else {
    /* 起点放在抗频闪的档位上，先设下去再开流 */

    exposure = ae_band(exposure <= 0 || exposure > AE_EXP_MAX ?
                       AE_EXP_MAX : exposure);
    if (gain < 0)
      gain = 0;
    set_ctrl(sfd, V4L2_CID_EXPOSURE, "曝光（抗频闪档）", exposure);
  }

  fd = open(vpath, O_RDWR | O_NONBLOCK);
  if (fd < 0) {
    printf("打不开 %s: %s\n", vpath, strerror(errno));
    return 1;
  }

  if (ioctl(fd, RKCIF_CMD_SET_CSI_MEMORY_MODE, &mode) < 0)
    printf("设 16 位低对齐失败（%s），按驱动给的 bytesperline 走\n",
           strerror(errno));

  fmt.type = type;
  fmt.fmt.pix_mp.width = sf.format.width;
  fmt.fmt.pix_mp.height = sf.format.height;
  fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_SGBRG10;
  fmt.fmt.pix_mp.num_planes = 1;
  if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
    printf("S_FMT 失败: %s\n", strerror(errno));
    goto out;
  }
  w = fmt.fmt.pix_mp.width;
  h = fmt.fmt.pix_mp.height;
  bpl = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
  printf("CIF %s: %ux%u fourcc=%.4s bytesperline=%u sizeimage=%u\n", vpath,
         w, h, (char *)&fmt.fmt.pix_mp.pixelformat, bpl,
         fmt.fmt.pix_mp.plane_fmt[0].sizeimage);

  /* ★ 实际尺寸以 bytesperline / sizeimage 为准，不信 width / height。
   *
   *   IMX415 报 3864x2192，CIF 回的 width/height 也照抄这个数，但它按
   *   3840x2160 裁（bytesperline 7680、sizeimage 16588800）。按 2192 行
   *   去读，最后 32 行就越过缓冲区尾 —— 第一次跑就是这么 SIGBUS 的，
   *   出错地址正好是缓冲起点 + 16588800。
   */
  if (bpl / 2 < w)
    w = bpl / 2;
  if (fmt.fmt.pix_mp.plane_fmt[0].sizeimage / bpl < h)
    h = fmt.fmt.pix_mp.plane_fmt[0].sizeimage / bpl;
  printf("按 %ux%u（16 位/像素）解释\n", w, h);

  rb.count = NBUF;
  rb.type = type;
  rb.memory = V4L2_MEMORY_MMAP;
  if (ioctl(fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) {
    printf("REQBUFS 失败: %s（给了 %u 个）\n", strerror(errno), rb.count);
    goto out;
  }

  for (i = 0; i < (int)rb.count; i++) {
    struct v4l2_plane pl[VIDEO_MAX_PLANES] = { 0 };
    struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP,
                             .index = i, .m.planes = pl, .length = 1 };

    if (ioctl(fd, VIDIOC_QUERYBUF, &b) < 0) {
      printf("QUERYBUF %d 失败: %s\n", i, strerror(errno));
      goto out;
    }
    bufs[i].len = pl[0].length;
    bufs[i].p = mmap(NULL, pl[0].length, PROT_READ | PROT_WRITE, MAP_SHARED,
                     fd, pl[0].m.mem_offset);
    if (bufs[i].p == MAP_FAILED) {
      bufs[i].p = NULL;
      printf("mmap 缓冲 %d 失败: %s\n", i, strerror(errno));
      goto out;
    }
    printf("缓冲 %d: %p + %zu\n", i, bufs[i].p, bufs[i].len);
    if (ioctl(fd, VIDIOC_QBUF, &b) < 0) {
      printf("QBUF %d 失败: %s\n", i, strerror(errno));
      goto out;
    }
  }

  if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
    printf("STREAMON 失败: %s\n", strerror(errno));
    goto out;
  }
  streaming = 1;

  /* 总期限：每帧按 200ms 算（5fps 都够），再加 3 秒起流。live 的帧数
   * 是按 30fps 从秒数折算的，期限就按秒数给。
   */

  t_start = now_ms();
  deadline = t_start + 3000 + (live ? frames / 30 * 1000L : frames * 200L);
  unlink(LIVE_STOP);

  while (got < frames) {
    struct v4l2_plane pl[VIDEO_MAX_PLANES] = { 0 };
    struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP,
                             .m.planes = pl, .length = 1 };
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    long left = deadline - now_ms();
    uint64_t sum[4] = { 0 };
    unsigned n4 = 0, x, y, stride16 = bpl / 2;
    const uint16_t *raw;
    long t;
    int pr;

    if (left <= 0) {
      printf("到总期限了，拿到 %d/%d 帧\n", got, frames);
      break;
    }
    pr = poll(&pfd, 1, left < POLL_MS ? (int)left : POLL_MS);
    if (pr == 0) {
      printf("%d ms 内没有新帧（已拿到 %d 帧）\n", POLL_MS, got);
      break;
    }
    if (pr < 0 || ioctl(fd, VIDIOC_DQBUF, &b) < 0) {
      printf("等帧/DQBUF 失败: %s\n", strerror(errno));
      break;
    }

    t = now_ms();
    raw = bufs[b.index].p;

    /* 四个 Bayer 相位分开求均值：GBRG 下 [0]=G [1]=B [2]=R [3]=G */

    for (y = 0; y + 1 < h; y += 16)
      for (x = 0; x + 1 < w; x += 16) {
        const uint16_t *q = raw + y * stride16 + x;

        sum[0] += q[0];
        sum[1] += q[1];
        sum[2] += q[stride16];
        sum[3] += q[stride16 + 1];
        n4++;
      }

    got++;
    if (got == 1)
      t_first = t;
    if (!live || got % 300 == 1)
    printf("帧 %d: seq=%u 字节=%u 间隔=%ld ms  均值 G=%u B=%u R=%u G=%u "
           "(0~1023)%s\n", got, b.sequence, pl[0].bytesused,
           t_prev ? t - t_prev : 0, (unsigned)(sum[0] / n4),
           (unsigned)(sum[1] / n4), (unsigned)(sum[2] / n4),
           (unsigned)(sum[3] / n4), b.flags & V4L2_BUF_FLAG_ERROR ? " ERROR" : "");
    fflush(stdout);
    t_prev = t;

    if (live && got % 2 == 0)
      ae_step(sfd, (unsigned)((sum[0] + sum[3]) / 2 / n4), &exposure, &gain);
    if (live && got % 30 == 1)
      printf("帧 %d AE: G 均值 %u 曝光 %d 行 增益 %d\n", got,
             (unsigned)((sum[0] + sum[3]) / 2 / n4), exposure, gain);

    if ((live || got == frames) && want_show)
      show(raw, w, h, bpl);

    /* 停止文件每 15 帧（半秒）看一次 */

    if (live && got % 15 == 0 && access(LIVE_STOP, F_OK) == 0) {
      printf("看到 %s，停止（%d 帧）\n", LIVE_STOP, got);
      unlink(LIVE_STOP);
      frames = got;
    }

    if (ioctl(fd, VIDIOC_QBUF, &b) < 0) {
      printf("回填 QBUF 失败: %s\n", strerror(errno));
      break;
    }
  }

  if (got > 1)
    printf("共 %d 帧，平均 %.1f fps（首帧在起流后 %ld ms）\n", got,
           (got - 1) * 1000.0 / (t_prev - t_first > 0 ? t_prev - t_first : 1),
           t_first - t_start);
  ret = got == frames ? 0 : 1;

out:
  if (sfd >= 0)
    close(sfd);
  if (streaming)
    ioctl(fd, VIDIOC_STREAMOFF, &type);
  for (i = 0; i < NBUF; i++)
    if (bufs[i].p != NULL)
      munmap(bufs[i].p, bufs[i].len);
  if (fd >= 0)
    close(fd);
  return ret;
}

/* 经 ISP 取 NV12：rkisp_mainpath 一开流，内核经 sditf 把 CIF 也带起来，
 * ISP 回读 CIF 写进 DDR 的 RAW。3A 参数由 rkaiq_3A_server（k7aiq start）
 * 在起流事件里接上，这里只管取帧。
 */

static int isp_main(int frames, int want_show, unsigned w, unsigned h,
                    int live)
{
  struct v4l2_requestbuffers rb = { 0 };
  struct v4l2_format fmt = { 0 };
  struct buf_s bufs[NBUF] = { 0 };
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  char vpath[300];
  int fd = -1, got = 0, streaming = 0, ret = 1, i;
  long t_start, deadline, t_prev = 0, t_first = 0;
  unsigned bpl;

  if (!find_node(ISP_NAME, 0, vpath, sizeof(vpath))) {
    printf("找不到 %s（rkisp 模块没装上？）\n", ISP_NAME);
    return 1;
  }

  fd = open(vpath, O_RDWR | O_NONBLOCK);
  if (fd < 0) {
    printf("打不开 %s: %s\n", vpath, strerror(errno));
    return 1;
  }

  fmt.type = type;
  fmt.fmt.pix_mp.width = w;
  fmt.fmt.pix_mp.height = h;
  fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
  fmt.fmt.pix_mp.num_planes = 1;
  if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
    printf("S_FMT 失败: %s\n", strerror(errno));
    goto out;
  }
  w = fmt.fmt.pix_mp.width;
  h = fmt.fmt.pix_mp.height;
  bpl = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
  printf("ISP %s: %ux%u fourcc=%.4s planes=%u bytesperline=%u sizeimage=%u\n",
         vpath, w, h, (char *)&fmt.fmt.pix_mp.pixelformat,
         fmt.fmt.pix_mp.num_planes, bpl,
         fmt.fmt.pix_mp.plane_fmt[0].sizeimage);
  if (fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 ||
      fmt.fmt.pix_mp.num_planes != 1 || bpl < w ||
      fmt.fmt.pix_mp.plane_fmt[0].sizeimage < bpl * h * 3 / 2) {
    printf("驱动给的不是单平面 NV12，不按 NV12 解释\n");
    goto out;
  }

  rb.count = NBUF;
  rb.type = type;
  rb.memory = V4L2_MEMORY_MMAP;
  if (ioctl(fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) {
    printf("REQBUFS 失败: %s（给了 %u 个）\n", strerror(errno), rb.count);
    goto out;
  }

  for (i = 0; i < (int)rb.count && i < NBUF; i++) {
    struct v4l2_plane pl[VIDEO_MAX_PLANES] = { 0 };
    struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP,
                             .index = i, .m.planes = pl, .length = 1 };

    if (ioctl(fd, VIDIOC_QUERYBUF, &b) < 0) {
      printf("QUERYBUF %d 失败: %s\n", i, strerror(errno));
      goto out;
    }
    bufs[i].len = pl[0].length;
    bufs[i].p = mmap(NULL, pl[0].length, PROT_READ | PROT_WRITE, MAP_SHARED,
                     fd, pl[0].m.mem_offset);
    if (bufs[i].p == MAP_FAILED) {
      bufs[i].p = NULL;
      printf("mmap 缓冲 %d 失败: %s\n", i, strerror(errno));
      goto out;
    }
    if (ioctl(fd, VIDIOC_QBUF, &b) < 0) {
      printf("QBUF %d 失败: %s\n", i, strerror(errno));
      goto out;
    }
  }

  if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
    printf("STREAMON 失败: %s\n", strerror(errno));
    goto out;
  }
  streaming = 1;

  /* 总期限同 cap；ISP 起流要等 rkaiq 接上参数，多给 2 秒 */

  t_start = now_ms();
  deadline = t_start + 5000 + (live ? frames / 30 * 1000L : frames * 200L);
  if (live)
    unlink(LIVE_STOP);

  while (got < frames) {
    struct v4l2_plane pl[VIDEO_MAX_PLANES] = { 0 };
    struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP,
                             .m.planes = pl, .length = 1 };
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    long left = deadline - now_ms();
    uint64_t sy = 0, su = 0, sv = 0;
    unsigned ny = 0, nuv = 0, x, y;
    const uint8_t *yp, *uvp;
    long t;
    int pr;

    if (left <= 0) {
      printf("到总期限了，拿到 %d/%d 帧\n", got, frames);
      break;
    }
    pr = poll(&pfd, 1, left < POLL_MS ? (int)left : POLL_MS);
    if (pr == 0) {
      printf("%d ms 内没有新帧（已拿到 %d 帧）\n", POLL_MS, got);
      break;
    }
    if (pr < 0 || ioctl(fd, VIDIOC_DQBUF, &b) < 0) {
      printf("等帧/DQBUF 失败: %s\n", strerror(errno));
      break;
    }

    t = now_ms();
    yp = bufs[b.index].p;
    uvp = yp + bpl * h;
    for (y = 0; y < h; y += 16)
      for (x = 0; x < w; x += 16) {
        sy += yp[y * bpl + x];
        ny++;
      }
    for (y = 0; y < h / 2; y += 8)
      for (x = 0; x + 1 < w; x += 16) {
        su += uvp[y * bpl + x];
        sv += uvp[y * bpl + x + 1];
        nuv++;
      }

    got++;
    if (got == 1)
      t_first = t;
    if (!live || got % 300 == 1)
    printf("帧 %d: seq=%u 字节=%u 间隔=%ld ms  均值 Y=%u U=%u V=%u%s\n", got,
           b.sequence, pl[0].bytesused, t_prev ? t - t_prev : 0,
           (unsigned)(sy / ny), (unsigned)(su / nuv), (unsigned)(sv / nuv),
           b.flags & V4L2_BUF_FLAG_ERROR ? " ERROR" : "");
    t_prev = t;

    if ((live || got == frames) && want_show)
      show_nv12(yp, uvp, w, h, bpl);

    if (live && got % 15 == 0 && access(LIVE_STOP, F_OK) == 0) {
      printf("看到 %s，停止（%d 帧）\n", LIVE_STOP, got);
      unlink(LIVE_STOP);
      frames = got;
    }

    if (ioctl(fd, VIDIOC_QBUF, &b) < 0) {
      printf("回填 QBUF 失败: %s\n", strerror(errno));
      break;
    }
  }

  if (got > 1)
    printf("共 %d 帧，平均 %.1f fps（首帧在起流后 %ld ms）\n", got,
           (got - 1) * 1000.0 / (t_prev - t_first > 0 ? t_prev - t_first : 1),
           t_first - t_start);
  /* live 只在一帧都没拿到时算失败，ampctl cam start 靠它决定退回 RAW */
  ret = live ? got == 0 : got != frames;

out:
  if (streaming)
    ioctl(fd, VIDIOC_STREAMOFF, &type);
  for (i = 0; i < NBUF; i++)
    if (bufs[i].p != NULL)
      munmap(bufs[i].p, bufs[i].len);
  if (fd >= 0)
    close(fd);
  return ret;
}

/* SIGBUS/SIGSEGV 时报出出错的地址再退出：取图缓冲和帧共享区都是 mmap
 * 进来的，哪一块出的问题，看地址落在哪儿就知道（下面打印了各块的范围）。
 */

static void on_fault(int sig, siginfo_t *si, void *ctx)
{
  char msg[96];
  int n;

  (void)ctx;
  n = snprintf(msg, sizeof(msg), "\n★ 收到信号 %d，地址 %p，si_code %d\n",
               sig, si->si_addr, si->si_code);
  write(1, msg, n);
  _exit(128 + sig);
}

int main(int argc, char **argv)
{
  struct sigaction sa = { .sa_sigaction = on_fault, .sa_flags = SA_SIGINFO };
  int frames = argc > 2 ? atoi(argv[2]) : 10;
  int want_show = argc > 3 && strcmp(argv[3], "show") == 0;
  int isp = argc > 1 && strcmp(argv[1], "isp") == 0;
  int live = argc > 1 && strcmp(argv[1], "live") == 0;
  int live_isp = argc > 1 && strcmp(argv[1], "live-isp") == 0;

  if (live_isp && frames >= 1 && frames <= LIVE_MAX_S) {
    setvbuf(stdout, NULL, _IONBF, 0);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    g_quiet = 1;
    return isp_main(frames * 30, 1, PREVIEW_W, 720, 1);
  }

  if (live && frames >= 1 && frames <= LIVE_MAX_S) {
    setvbuf(stdout, NULL, _IONBF, 0);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    g_quiet = 1;
    return cap_main(frames * 30, 1, argc > 3 ? atoi(argv[3]) : 0,
                    argc > 4 ? atoi(argv[4]) : -1, 1);
  }

  /* ★ 用法错误退出码固定为 2：ampctl cam start 靠它判断板上的 k7cam
   *   是不是还不认识 live，是的话退回逐帧 cap 1 show 的老办法。
   */

  if (argc < 2 || (!isp && strcmp(argv[1], "cap") != 0) || frames < 1 ||
      frames > MAX_FRAMES) {
    printf("用法: k7cam cap [帧数 1~%d] [show|-] [曝光行数] [模拟增益]\n"
           "      k7cam isp [帧数 1~%d] [show|-] [宽 高]\n"
           "      k7cam live [秒数 1~%d] [曝光行数] [模拟增益]\n"
           "      k7cam live-isp [秒数 1~%d]\n",
           MAX_FRAMES, MAX_FRAMES, LIVE_MAX_S, LIVE_MAX_S);
    return 2;
  }

  /* 输出经 rpmsg 管道回到 openvela；不关缓冲的话，进程一出事最后几行就丢了 */

  setvbuf(stdout, NULL, _IONBF, 0);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGSEGV, &sa, NULL);

  if (isp)
    return isp_main(frames, want_show, argc > 5 ? atoi(argv[4]) : 1280,
                    argc > 5 ? atoi(argv[5]) : 720, 0);
  return cap_main(frames, want_show, argc > 4 ? atoi(argv[4]) : 0,
                  argc > 5 ? atoi(argv[5]) : -1, 0);
}
