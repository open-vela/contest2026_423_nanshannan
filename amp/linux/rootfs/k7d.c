/*
 * k7d：A72 簇上 Linux 的用户态服务，经 rpmsg 替 openvela 执行命令。
 *
 * Copyright (c) 2026 contest2026_423_nanshannan
 * SPDX-License-Identifier: Apache-2.0
 *
 * ★ 链路是怎么建起来的
 *
 *   openvela 那边的 `ampctl exec` 建一个名为 "rpmsg-raw" 的端点。它的
 *   名字服务宣告到了 Linux，rpmsg_ns 就建一条 channel，rpmsg_char
 *   （id_table 里正是 "rpmsg-raw"）接手并生成 /dev/rpmsgN。
 *
 *   本进程轮询 /dev/rpmsg*，打开后**先发一帧 HELLO**。这一帧不只是寒暄：
 *   Linux 这边的端点地址是动态分的，openvela 只有收到过一帧之后才知道
 *   往哪回（OpenAMP 在第一帧到达时把 dest_addr 填成对方的 src）。所以
 *   openvela 在收到 HELLO 之前**不能**发命令。
 *
 *   openvela 用完就销毁端点 → 名字服务宣告撤销 → channel 删掉 →
 *   这里的 read() 返回 EPIPE。本进程关掉设备，回去接着等下一次。
 *
 * ★ 帧格式（一帧就是一次 rpmsg_send，首字节是类型）
 *
 *   openvela → Linux
 *     'X' <命令行>      用 /bin/sh -c 执行
 *   Linux → openvela
 *     'H' <文本>        握手：内核版本、运行时间
 *     'O' <字节>        命令输出（stdout+stderr 合在一起），可能分很多帧
 *     'E' <十进制>      结束，带退出码；超时被杀是 124（和 timeout(1) 一致）
 *
 *   一帧最多 496 字节（virtio_rpmsg 缓冲 512 减去 16 字节头），这里留余量
 *   取 480。
 *
 * ★ 音频模式：语音唤醒的端点检测（VAD）在这里做
 *
 *   openvela 握手后的第一帧如果是 'A'，这条通道就进入音频模式：
 *
 *   openvela → Linux
 *     'A'               进入音频模式（16kHz、单声道、16 位）
 *     'P' <样本>        PCM，小端 int16，通常 160 个样本（10ms）
 *   Linux → openvela
 *     'L' <十进制>      最近 100ms 的峰值 0~32767，给界面画电平
 *     'S' <起点>        开始说话（样本序号，从 'A' 之后第一个样本记 0）
 *     'V' <起点> <终点> 一句话说完，[起点, 终点) 这段送去识别
 *
 *   openvela 那边保留最近十几秒的环形缓冲，按样本序号取段 —— 语音不回传，
 *   rpmsg 上只走 PCM 一个方向。
 *
 * ★ 帧共享测试：k7d frame
 *
 *   不当守护进程用，而是一次性的命令（经 ampctl exec 从 openvela 调）：
 *     k7d frame [帧数] [间隔毫秒] [corrupt]
 *   往 /dev/k7frame 写测试图案并按 amp/include/k7_frame.h 的协议发布，
 *   openvela 那边 `ampctl frame` 逐像素核对。见 frame_main()。
 *
 * ★ 多条通道并发
 *
 *   唤醒的音频通道常驻，ampctl exec 随时还会再开一条。每发现一个新的
 *   /dev/rpmsgN 就 fork 一个子进程服务它，互不阻塞。
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "k7_frame.h"

#define FRAME_MAX     480
#define EXEC_TMO_S    60
#define POLL_DEV_MS   50
#define MAX_CHILD     8

/* VAD 参数（16kHz，每帧 10ms = 160 个样本） */

#define VAD_RATE        16000
#define VAD_FRAME       160
#define VAD_START_N     3       /* 连续 3 帧有声算开口            */
#define VAD_END_N       70      /* 连续 700ms 无声算说完（句中停顿别切断） */
#define VAD_PREROLL     50      /* 起点往前补 500ms，免得吃掉首字 */
#define VAD_MIN_N       30      /* 短于 300ms 的不送（咳嗽、敲桌） */
#define VAD_MAX_N       800     /* 最长 8s 强制切断               */
#define VAD_ABS_MIN     120     /* 有声的绝对下限（RMS）
                                 *
                                 * 原来 300：用户 20cm 说话峰值 1000~2300、RMS 只有
                                 * 几百，只有最响的尾音"…ela"过线，识别成
                                 * "Yeah./诶。"。开着噪声门时安静 RMS 仅个位数，
                                 * 120 仍有余量。
                                 */

static void klog(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int fd, n;

	n = snprintf(buf, sizeof(buf), "k7d: ");
	va_start(ap, fmt);
	n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
	va_end(ap);
	if (n >= (int)sizeof(buf))
		n = sizeof(buf) - 1;

	/* 没有控制台，日志只能进内核环形缓冲 */
	fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (fd >= 0) {
		(void)!write(fd, buf, n);
		close(fd);
	}
}

static int send_frame(int fd, char type, const void *data, size_t len)
{
	char frame[FRAME_MAX];

	if (len > FRAME_MAX - 1)
		len = FRAME_MAX - 1;

	frame[0] = type;
	memcpy(frame + 1, data, len);

	for (;;) {
		ssize_t n = write(fd, frame, len + 1);

		if (n >= 0)
			return 0;
		if (errno != EINTR)
			return -errno;
	}
}

static int send_text(int fd, char type, const char *fmt, ...)
{
	char buf[FRAME_MAX];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	if (n < 0)
		return -EINVAL;
	if (n >= (int)sizeof(buf))
		n = sizeof(buf) - 1;

	return send_frame(fd, type, buf, n);
}

/*
 * 执行一条命令，输出逐帧转发。
 *
 * 返回 0 表示命令本身跑完了（不管退出码是多少）；负数表示 rpmsg 那头
 * 断了，调用方应该关设备重来。
 */
static int run_command(int fd, const char *cmd)
{
	char buf[FRAME_MAX - 1];
	int pipefd[2];
	time_t deadline;
	int status = 0;
	int ret = 0;
	pid_t pid;

	klog("exec: %s\n", cmd);

	if (pipe(pipefd) < 0)
		return send_text(fd, 'E', "%d", 127);

	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return send_text(fd, 'E', "%d", 127);
	}

	if (pid == 0) {
		int devnull = open("/dev/null", O_RDONLY);

		if (devnull >= 0)
			dup2(devnull, 0);
		dup2(pipefd[1], 1);
		dup2(pipefd[1], 2);
		close(pipefd[0]);
		close(pipefd[1]);

		/* 自成一个进程组，超时时能把整棵子树一起杀掉 */
		setpgid(0, 0);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}

	close(pipefd[1]);
	deadline = time(NULL) + EXEC_TMO_S;

	for (;;) {
		struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
		int left = (int)(deadline - time(NULL));
		ssize_t n;

		if (left <= 0) {
			kill(-pid, SIGKILL);
			status = -1;
			break;
		}

		if (poll(&pfd, 1, left * 1000) <= 0)
			continue;

		n = read(pipefd[0], buf, sizeof(buf));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;

		ret = send_frame(fd, 'O', buf, n);
		if (ret < 0) {
			/* openvela 那头走了，命令也没必要再跑 */
			kill(-pid, SIGKILL);
			break;
		}
	}

	close(pipefd[0]);
	waitpid(pid, status < 0 ? NULL : &status, 0);

	if (ret < 0)
		return ret;

	if (status < 0)
		return send_text(fd, 'E', "%d", 124);
	if (WIFEXITED(status))
		return send_text(fd, 'E', "%d", WEXITSTATUS(status));
	return send_text(fd, 'E', "%d", 128 + WTERMSIG(status));
}

/* 端点检测。ES8388 开了 ALC 和噪声门，安静时样本基本是 0~50，说话时
 * RMS 上千，靠"比底噪高 4 倍且过绝对下限"分开就够用。底噪跟踪：比
 * 当前估计低就快速跟下去，高就极慢地往上爬（说话时不会被带高）。
 */

struct vad_s
{
  unsigned long pos;       /* 已处理的样本数（下一帧的起点序号） */
  unsigned long seg_start;
  int           floor;     /* 底噪 RMS 估计 */
  int           run;       /* 连续有声 / 无声帧数 */
  int           in_speech;
  int           speech_n;  /* 本段已持续的帧数 */
  int           peak;      /* 这 100ms 里的峰值 */
  int           nframe;
  int16_t       buf[VAD_FRAME];
  int           fill;
};

static int isqrt(unsigned long v)
{
  unsigned long r = 0;
  unsigned long b = 1ul << 30;

  while (b > v)
    b >>= 2;
  while (b) {
    if (v >= r + b) {
      v -= r + b;
      r = (r >> 1) + b;
    } else {
      r >>= 1;
    }
    b >>= 2;
  }

  return (int)r;
}

static void vad_frame(int fd, struct vad_s *v)
{
  unsigned long acc = 0;
  int voiced;
  int rms;
  int i;

  for (i = 0; i < VAD_FRAME; i++) {
    int x = v->buf[i];
    int a = x < 0 ? -x : x;

    acc += (unsigned long)(x * x);
    if (a > v->peak)
      v->peak = a;
  }

  rms = isqrt(acc / VAD_FRAME);

  if (v->floor == 0 || rms < v->floor)
    v->floor = v->floor == 0 ? rms + 1 : (v->floor * 7 + rms) / 8 + 1;
  else if (!v->in_speech)
    v->floor += (rms - v->floor) / 256;

  voiced = rms > VAD_ABS_MIN && rms > v->floor * 4;

  if (!v->in_speech) {
    v->run = voiced ? v->run + 1 : 0;
    if (v->run >= VAD_START_N) {
      unsigned long back = (VAD_START_N + VAD_PREROLL) * VAD_FRAME;

      v->in_speech = 1;
      v->speech_n = v->run;
      v->run = 0;
      v->seg_start = v->pos > back ? v->pos - back : 0;
      send_text(fd, 'S', "%lu", v->seg_start);
    }
  } else {
    v->speech_n++;
    v->run = voiced ? 0 : v->run + 1;
    if (v->run >= VAD_END_N || v->speech_n >= VAD_MAX_N) {
      unsigned long end = v->pos + VAD_FRAME;

      if (v->speech_n - v->run >= VAD_MIN_N)
        send_text(fd, 'V', "%lu %lu", v->seg_start, end);
      else
        klog("vad: 丢掉 %d0ms 的短声\n", v->speech_n - v->run);

      v->in_speech = 0;
      v->run = 0;
    }
  }

  v->pos += VAD_FRAME;
  if (++v->nframe >= 10) {
    send_text(fd, 'L', "%d", v->peak);
    v->peak = 0;
    v->nframe = 0;
  }
}

static void serve_audio(int fd)
{
  struct vad_s v;
  char frame[FRAME_MAX + 1];

  memset(&v, 0, sizeof(v));
  klog("audio: VAD 开始\n");

  for (;;) {
    ssize_t n = read(fd, frame, FRAME_MAX);
    int ns;
    int i;

    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    if (frame[0] != 'P')
      continue;

    /* 样本从 frame+1 开始，未对齐：逐个拷 */

    ns = (int)(n - 1) / 2;
    for (i = 0; i < ns; i++) {
      int16_t x;

      memcpy(&x, frame + 1 + 2 * i, 2);
      v.buf[v.fill++] = x;
      if (v.fill == VAD_FRAME) {
        vad_frame(fd, &v);
        v.fill = 0;
      }
    }
  }

  klog("audio: VAD 结束，共 %lu 样本\n", v.pos);
}

static void serve(const char *path)
{
	char frame[FRAME_MAX + 1];
	struct utsname uts;
	struct sysinfo si;
	int fd;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		/* channel 刚建好，设备节点可能还没就绪；下一轮再来 */
		return;
	}

	klog("opened %s\n", path);

	uname(&uts);
	sysinfo(&si);
	if (send_text(fd, 'H', "Linux %s %s on %s, up %lds, %d procs",
		      uts.release, uts.machine, uts.nodename,
		      si.uptime, si.procs) < 0)
		goto out;

	for (;;) {
		ssize_t n = read(fd, frame, FRAME_MAX);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;         /* EPIPE：openvela 撤掉了端点 */

		frame[n] = '\0';
		if (frame[0] == 'A') {
			serve_audio(fd);
			break;
		}
		if (frame[0] == 'X') {
			if (run_command(fd, frame + 1) < 0)
				break;
		} else {
			send_text(fd, 'E', "%d", 2);
		}
	}

out:
	klog("closed %s\n", path);
	close(fd);
}

/* 正在被子进程服务的设备 */

static struct
{
  char  name[256];
  pid_t pid;
} g_child[MAX_CHILD];

static int child_busy(const char *name)
{
  int i;

  for (i = 0; i < MAX_CHILD; i++)
    if (g_child[i].pid > 0 && strcmp(g_child[i].name, name) == 0)
      return 1;

  return 0;
}

static void reap(void)
{
  pid_t pid;
  int i;

  while ((pid = waitpid(-1, NULL, WNOHANG)) > 0)
    for (i = 0; i < MAX_CHILD; i++)
      if (g_child[i].pid == pid)
        g_child[i].pid = 0;
}

/* ---- k7d frame：往帧共享区写测试图案 ----
 *
 * 摄像头接到 Linux 之前，先单独证明"共享区 + 发布协议 + cache 契约"
 * 是对的：这里写一张公式算得出来的图，openvela 按同一个公式逐像素比。
 *
 * corrupt：每帧发布前把像素 (100, 200) 的蓝色字节翻一位。openvela 必须
 * 报出这一处不一致 —— 用来证明那边的核对真能发现错误。
 */

#define FRAME_DEV       "/dev/k7frame"
#define FRAME_SHM_SIZE  (64u << 20)    /* = DTS 里 k7_frame_shm 的大小 */
#define FRAME_W         720
#define FRAME_H         1280
#define FRAME_MAX_N     1000
#define FRAME_MAX_GAP   10000          /* 毫秒 */

static int frame_main(int argc, char **argv)
{
  int count = argc > 2 ? atoi(argv[2]) : 1;
  int gap = argc > 3 ? atoi(argv[3]) : 0;
  int corrupt = argc > 4 && strcmp(argv[4], "corrupt") == 0;
  struct k7f_hdr_s *h;
  uint8_t *base;
  int fd;
  int n;

  if (count < 1 || count > FRAME_MAX_N || gap < 0 || gap > FRAME_MAX_GAP) {
    printf("用法: k7d frame [帧数 1~%d] [间隔毫秒 0~%d] [corrupt]\n",
           FRAME_MAX_N, FRAME_MAX_GAP);
    return 2;
  }

  fd = open(FRAME_DEV, O_RDWR);
  if (fd < 0) {
    printf("打不开 %s: %s\n", FRAME_DEV, strerror(errno));
    return 1;
  }

  base = mmap(NULL, FRAME_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
              fd, 0);
  close(fd);
  if (base == MAP_FAILED) {
    printf("mmap %s 失败: %s\n", FRAME_DEV, strerror(errno));
    return 1;
  }

  h = (struct k7f_hdr_s *)base;
  if (h->magic != K7F_MAGIC || h->version != K7F_VERSION) {
    memset(h, 0, K7F_HDR_SIZE);
    h->slot_size = ((FRAME_SHM_SIZE - K7F_HDR_SIZE) / K7F_SLOTS) &
                   ~(K7F_HDR_SIZE - 1);
    h->version = K7F_VERSION;
    __asm__ volatile("dsb sy" ::: "memory");
    h->magic = K7F_MAGIC;
    printf("初始化帧头: 槽 %d x %u 字节\n", K7F_SLOTS, h->slot_size);
  }

  for (n = 0; n < count; n++) {
    uint32_t slot = h->seq == 0 ? 0 : (h->slot + 1) % K7F_SLOTS;
    uint32_t seed = h->seq + 1;
    uint32_t *px = (uint32_t *)(base + k7f_slot_offset(h, slot));
    struct timespec t0, t1;
    uint32_t x, y;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (y = 0; y < FRAME_H; y++)
      for (x = 0; x < FRAME_W; x++)
        px[y * FRAME_W + x] = k7f_pattern(x, y, FRAME_W, FRAME_H, seed);

    if (corrupt)
      ((uint8_t *)px)[(200 * FRAME_W + 100) * 4] ^= 0x01;

    /* 发布：像素先落地，再填描述，最后 seq —— 见 k7_frame.h */

    __asm__ volatile("dsb sy" ::: "memory");
    h->slot = slot;
    h->width = FRAME_W;
    h->height = FRAME_H;
    h->stride = FRAME_W * 4;
    h->format = K7F_FMT_XRGB8888;
    h->bytes = FRAME_W * 4 * FRAME_H;
    h->source = K7F_SRC_PATTERN;
    h->seed = seed;
    __asm__ volatile("dsb sy" ::: "memory");
    h->seq = seed;
    __asm__ volatile("dsb sy" ::: "memory");
    clock_gettime(CLOCK_MONOTONIC, &t1);

    printf("发布 seq=%u 槽=%u %ux%u%s 用时 %ld ms\n", seed, slot,
           FRAME_W, FRAME_H, corrupt ? " (故意改坏 (100,200))" : "",
           (t1.tv_sec - t0.tv_sec) * 1000 +
           (t1.tv_nsec - t0.tv_nsec) / 1000000);
    fflush(stdout);

    if (gap > 0 && n + 1 < count)
      usleep((useconds_t)gap * 1000);
  }

  munmap(base, FRAME_SHM_SIZE);
  return 0;
}

int main(int argc, char **argv)
{
  struct dirent *de;
  DIR *d;

  if (argc > 1 && strcmp(argv[1], "frame") == 0)
    return frame_main(argc, argv);

  signal(SIGPIPE, SIG_IGN);
  klog("started, waiting for /dev/rpmsgN\n");

  for (;;) {
    reap();

    d = opendir("/dev");
    if (d != NULL) {
      while ((de = readdir(d)) != NULL) {
        char path[300];
        pid_t pid;
        int i;

        if (strncmp(de->d_name, "rpmsg", 5) != 0 ||
            de->d_name[5] < '0' || de->d_name[5] > '9' ||
            child_busy(de->d_name))
          continue;

        for (i = 0; i < MAX_CHILD && g_child[i].pid > 0; i++)
          ;
        if (i == MAX_CHILD)
          break;

        snprintf(path, sizeof(path), "/dev/%s", de->d_name);
        pid = fork();
        if (pid == 0) {
          closedir(d);
          serve(path);
          _exit(0);
        }

        if (pid > 0) {
          snprintf(g_child[i].name, sizeof(g_child[i].name), "%s",
                   de->d_name);
          g_child[i].pid = pid;
        }
      }

      closedir(d);
    }

    usleep(POLL_DEV_MS * 1000);
  }
}
