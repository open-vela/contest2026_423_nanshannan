/*
 * k7pmic：从 Linux 读 RK806，必要时改 vdd_npu（DCDC2）的电压。
 *
 *   k7pmic r <寄存器> [个数]   只读，打印寄存器
 *   k7pmic npu-mv              只读，打印 DCDC2 当前电压
 *   k7pmic npu-mv <mV>         把 DCDC2 调到 <mV>（只允许 725~875）
 *
 * ★ RK806 挂在 i2c1（地址 0x23），这条总线归 Linux：rk3x-i2c 驱动绑着它，
 *   空闲时还会把 i2c1 的时钟关掉，所以 openvela 那边的 /dev/i2c1 什么都扫
 *   不到。这里用 /dev/i2c-1 的 I2C_RDWR 直接访问（AMP 内核没开稳压器框架，
 *   rk806 的 MFD 驱动只管中断和关机，不会去动 DCDC2）。
 *
 * ★ 写电压只认 DCDC2 的 ON_VSEL（0x1b），写之前确认芯片是 0x806、DCDC2 开着，
 *   写完回读。上限 875mV 是 rk3576.dtsi 里 NPU OPP 表的最高电压（950MHz，
 *   L0/L1 档），也低于原理图给 vdd_npu 定的 950mV。
 *
 * 静态链接，不依赖 glibc 运行库。
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define RK806_ADDR       0x23
#define RK806_POWER_EN0  0x00   /* bit1 = DCDC2 开 */
#define RK806_BUCK2_VSEL 0x1b   /* 500mV + n * 6.25mV（n < 0xa0） */
#define RK806_CHIP_NAME  0x5a   /* 0x80, 0x6x = "806x" */

#define NPU_MV_MIN       725
#define NPU_MV_MAX       875

static int g_fd;

static int rd(unsigned reg, uint8_t *v, unsigned n)
{
  uint8_t r = reg;
  struct i2c_msg m[2] =
  {
    { .addr = RK806_ADDR, .flags = 0, .len = 1, .buf = &r },
    { .addr = RK806_ADDR, .flags = I2C_M_RD, .len = n, .buf = v },
  };
  struct i2c_rdwr_ioctl_data x = { .msgs = m, .nmsgs = 2 };

  if (ioctl(g_fd, I2C_RDWR, &x) != 2)
    {
      perror("I2C_RDWR 读");
      return -1;
    }

  return 0;
}

static int wr(unsigned reg, uint8_t v)
{
  uint8_t b[2] = { reg, v };
  struct i2c_msg m = { .addr = RK806_ADDR, .flags = 0, .len = 2, .buf = b };
  struct i2c_rdwr_ioctl_data x = { .msgs = &m, .nmsgs = 1 };

  if (ioctl(g_fd, I2C_RDWR, &x) != 1)
    {
      perror("I2C_RDWR 写");
      return -1;
    }

  return 0;
}

static unsigned vsel_uv(uint8_t v)
{
  return 500000 + v * 6250;
}

static int npu_mv(int argc, char **argv)
{
  uint8_t id[2];
  uint8_t en;
  uint8_t v;
  long mv;

  if (rd(RK806_CHIP_NAME, id, 2) < 0 || rd(RK806_POWER_EN0, &en, 1) < 0 ||
      rd(RK806_BUCK2_VSEL, &v, 1) < 0)
    {
      return 1;
    }

  printf("RK806 %02x%02x，DCDC2 %s，VSEL 0x%02x = %u uV\n", id[0], id[1],
         en & 0x02 ? "开" : "关", v, vsel_uv(v));
  if (argc < 3)
    {
      return 0;
    }

  mv = strtol(argv[2], NULL, 0);
  if (mv < NPU_MV_MIN || mv > NPU_MV_MAX)
    {
      fprintf(stderr, "%ld mV 不在 %d~%d 之内，不写\n", mv, NPU_MV_MIN,
              NPU_MV_MAX);
      return 2;
    }

  if (id[0] != 0x80 || (id[1] & 0xf0) != 0x60 || (en & 0x02) == 0)
    {
      fprintf(stderr, "不是 RK806 或 DCDC2 没开，不写\n");
      return 3;
    }

  /* 向上取整到 6.25mV 一档：宁高半档，不低半档 */

  v = (mv * 1000 - 500000 + 6249) / 6250;
  if (wr(RK806_BUCK2_VSEL, v) < 0)
    {
      return 1;
    }

  /* RK806 自己按斜率爬升，最坏 150mV 在 1mV/us 下也只要 150us */

  usleep(2000);
  if (rd(RK806_BUCK2_VSEL, &en, 1) < 0)
    {
      return 1;
    }

  printf("写 0x%02x，回读 0x%02x = %u uV\n", v, en, vsel_uv(en));
  return en == v ? 0 : 4;
}

int main(int argc, char **argv)
{
  uint8_t v[64];
  unsigned reg;
  unsigned n;
  unsigned i;

  g_fd = open("/dev/i2c-1", O_RDWR);
  if (g_fd < 0)
    {
      perror("/dev/i2c-1");
      return 1;
    }

  if (argc >= 2 && strcmp(argv[1], "npu-mv") == 0)
    {
      return npu_mv(argc, argv);
    }

  if (argc >= 3 && strcmp(argv[1], "r") == 0)
    {
      reg = strtoul(argv[2], NULL, 0);
      n = argc > 3 ? strtoul(argv[3], NULL, 0) : 1;
      if (reg > 0xff || n < 1 || n > sizeof(v) || reg + n > 0x100)
        {
          fprintf(stderr, "寄存器或个数不对\n");
          return 2;
        }

      if (rd(reg, v, n) < 0)
        {
          return 1;
        }

      for (i = 0; i < n; i++)
        {
          printf("0x%02x = 0x%02x\n", reg + i, v[i]);
        }

      return 0;
    }

  fprintf(stderr, "用法: k7pmic r <寄存器> [个数] | npu-mv [mV]\n");
  return 2;
}
