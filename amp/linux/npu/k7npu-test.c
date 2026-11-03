/*
 * k7npu-test：NPU 第一个里程碑 —— 不接摄像头，用一张事先转好的
 * 224x224 RGB 原始数据跑一次 MobileNet v1，打印 top-5 和耗时。
 *
 *   k7npu-test <model.rknn> <input.rgb> [次数]
 *   k7npu-test pd        只读 PMU：NPU 四个电源域都开着返回 0，否则 3
 *
 * 输入是 NHWC 的 uint8 RGB（150528 字节），由 build-lxroot.sh 在主机上用
 * PIL 从 SDK 自带的 cat_224x224.jpg 转出来。板上不需要 OpenCV。
 * MobileNet v1 有 1001 类，第 0 类是背景，猫在 282~286 一带。
 *
 * 动态链接 /lib64/librknnrt.so（glibc），只依赖 rknn_api.h。
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "rknn_api.h"

static void *load(const char *path, size_t *size)
{
  FILE *f = fopen(path, "rb");
  void *buf = NULL;
  long n;

  if (f == NULL)
    {
      perror(path);
      return NULL;
    }

  if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 &&
      fseek(f, 0, SEEK_SET) == 0 && (buf = malloc(n)) != NULL &&
      fread(buf, 1, n, f) == (size_t)n)
    {
      *size = n;
    }
  else
    {
      fprintf(stderr, "%s: 读取失败\n", path);
      free(buf);
      buf = NULL;
    }

  fclose(f);
  return buf;
}

/* PMU 在常开域，任何时候读都安全。位的含义见 openvela 侧
 * chip/rk3576/rk3576_power.c：PD_NPU 看 STATUS(0x230) bit0，0=开；
 * NPUTOP/NPU0/NPU1 看 REPAIR(0x570) bit22~24，1=开。
 */

static int check_pd(void)
{
  volatile uint32_t *pmu;
  uint32_t st;
  uint32_t rep;
  int fd;

  fd = open("/dev/mem", O_RDONLY | O_SYNC);
  if (fd < 0)
    {
      perror("/dev/mem");
      return 1;
    }

  pmu = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0x27380000);
  close(fd);
  if (pmu == MAP_FAILED)
    {
      perror("mmap PMU");
      return 1;
    }

  st = pmu[0x230 / 4];
  rep = pmu[0x570 / 4];
  munmap((void *)pmu, 4096);

  printf("PMU STATUS=0x%08x REPAIR=0x%08x：npu %s nputop %s npu0 %s npu1 %s\n",
         st, rep, (st & 1) ? "关" : "开", (rep & (1u << 22)) ? "开" : "关",
         (rep & (1u << 23)) ? "开" : "关", (rep & (1u << 24)) ? "开" : "关");
  return (st & 1) == 0 && (rep & (7u << 22)) == (7u << 22) ? 0 : 3;
}

static long ms_since(const struct timespec *t0)
{
  struct timespec t1;

  clock_gettime(CLOCK_MONOTONIC, &t1);
  return (t1.tv_sec - t0->tv_sec) * 1000 +
         (t1.tv_nsec - t0->tv_nsec) / 1000000;
}

int main(int argc, char *argv[])
{
  rknn_context ctx = 0;
  rknn_sdk_version ver;
  rknn_input_output_num io;
  rknn_tensor_attr attr;
  rknn_input in;
  rknn_output out;
  rknn_perf_run perf;
  struct timespec t0;
  void *model;
  void *rgb;
  size_t msize;
  size_t rsize;
  int loops = argc > 3 ? atoi(argv[3]) : 1;
  int ret;
  int i;
  int k;

  if (argc == 2 && strcmp(argv[1], "pd") == 0)
    {
      return check_pd();
    }

  if (argc < 3)
    {
      fprintf(stderr, "用法: %s <model.rknn> <input.rgb> [次数]\n", argv[0]);
      return 2;
    }

  model = load(argv[1], &msize);
  rgb = load(argv[2], &rsize);
  if (model == NULL || rgb == NULL)
    {
      return 1;
    }

  clock_gettime(CLOCK_MONOTONIC, &t0);
  ret = rknn_init(&ctx, model, msize, 0, NULL);
  printf("rknn_init: %d，用时 %ld ms\n", ret, ms_since(&t0));
  if (ret < 0)
    {
      return 1;
    }

  if (rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver)) == 0)
    {
      printf("api %s / driver %s\n", ver.api_version, ver.drv_version);
    }

  ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
  memset(&attr, 0, sizeof(attr));
  attr.index = 0;
  if (ret == 0)
    {
      ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &attr, sizeof(attr));
    }

  if (ret < 0)
    {
      printf("rknn_query: %d\n", ret);
      goto out;
    }

  printf("输入 %u 个、输出 %u 个；输入 0：%ux%ux%ux%u，%u 字节\n",
         io.n_input, io.n_output, attr.dims[0], attr.dims[1], attr.dims[2],
         attr.dims[3], attr.size);

  memset(&in, 0, sizeof(in));
  in.index = 0;
  in.buf   = rgb;
  in.size  = rsize;
  in.type  = RKNN_TENSOR_UINT8;
  in.fmt   = RKNN_TENSOR_NHWC;

  for (k = 0; k < loops; k++)
    {
      ret = rknn_inputs_set(ctx, 1, &in);
      clock_gettime(CLOCK_MONOTONIC, &t0);
      if (ret == 0)
        {
          ret = rknn_run(ctx, NULL);
        }

      if (ret < 0)
        {
          printf("第 %d 次推理失败：%d\n", k, ret);
          goto out;
        }

      printf("第 %d 次：rknn_run %ld ms", k, ms_since(&t0));
      if (rknn_query(ctx, RKNN_QUERY_PERF_RUN, &perf, sizeof(perf)) == 0)
        {
          printf("（NPU %lld us）", (long long)perf.run_duration);
        }

      printf("\n");
    }

  memset(&out, 0, sizeof(out));
  out.want_float = 1;
  ret = rknn_outputs_get(ctx, 1, &out, NULL);
  if (ret < 0)
    {
      printf("rknn_outputs_get: %d\n", ret);
      goto out;
    }

  /* top-5：每轮挑一个最大的，挑过的置成很小 */

  {
    float *p = out.buf;
    int n = out.size / sizeof(float);

    printf("输出 %d 类，top-5：\n", n);
    for (k = 0; k < 5; k++)
      {
        int best = 0;

        for (i = 1; i < n; i++)
          {
            if (p[i] > p[best])
              {
                best = i;
              }
          }

        printf("  %4d  %.4f\n", best, p[best]);
        p[best] = -1e30f;
      }
  }

  rknn_outputs_release(ctx, 1, &out);

out:
  rknn_destroy(ctx);
  free(model);
  free(rgb);
  return ret < 0 ? 1 : 0;
}
