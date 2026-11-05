/****************************************************************************
 * chip/rk3576/rk3576_rng.c
 *
 * RK3576 硬件随机数（rng@2a410000，compatible "rockchip,rkrng"）。
 *
 * ★ 不是 crypto v1/v2 那两代。厂商驱动 rockchip-rng.c 里同时支持三代，
 *   寄存器完全不同：v1 在 CRYPTO 块的 0x0200，v2 在 0x0400 偏移，
 *   而 rkrng 是独立地址空间、CTRL 在 0x0010。照 v2 写不会报错，
 *   只是读到全 0。本板的 compatible 明确是 rockchip,rkrng。
 *
 * 时钟（clk-rk3576.c）：
 *   GATE(HCLK_TRNG_NS, "hclk_trng_ns", "hclk_secure_s",
 *        RK3576_NON_SECURE_GATING_CON00, 13, GFLAGS)
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

#include <nuttx/fs/fs.h>
#include <nuttx/drivers/drivers.h>

#include "arm64_internal.h"
#include "rk3576_rng.h"
#include "hardware/rk3576_memorymap.h"

#ifdef CONFIG_RK3576_RNG

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RKRNG_CTRL              0x0010
#define RKRNG_CTRL_SW_DRNG_REQ  (1 << 3)

#define RKRNG_STATE             0x0014
#define RKRNG_STATE_SW_DRNG_ACK (1 << 3)

#define RKRNG_DRNG_DATA_0       0x0070
#define RKRNG_MAX_BYTES         32          /* 8 个 32 位寄存器 */

/* ★ CTRL 用 Rockchip 惯例的高 16 位写使能掩码。忘记掩码时写入无效，
 * 但读回 STATE 永远等不到 ACK —— 表现为超时而非报错。
 */

#define RKRNG_HIWORD(val, mask) (((mask) << 16) | ((val) & (mask)))

#define RKRNG_POLL_US           50000

/* 时钟：非安全域门控寄存器 */

#define RKRNG_GATE_REG          0x0c48      /* CRU_NON_SECURE_GATING_CON00 */
#define RKRNG_GATE_BIT          13

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t rng_getreg(uint32_t off)
{
  return getreg32(RK3576_RNG_ADDR + off);
}

static inline void rng_putreg(uint32_t off, uint32_t val)
{
  putreg32(val, RK3576_RNG_ADDR + off);
}

/****************************************************************************
 * Name: rk3576_rng_fill
 *
 * Description:
 *   取一批随机数。一次最多 32 字节（8 个数据寄存器）。
 *
 ****************************************************************************/

static int rk3576_rng_fill(FAR uint8_t *buf, size_t len)
{
  uint32_t words[RKRNG_MAX_BYTES / 4];
  size_t   got = 0;
  int      i;
  int      us;

  while (got < len)
    {
      size_t chunk = len - got;

      if (chunk > RKRNG_MAX_BYTES)
        {
          chunk = RKRNG_MAX_BYTES;
        }

      rng_putreg(RKRNG_CTRL,
                 RKRNG_HIWORD(RKRNG_CTRL_SW_DRNG_REQ, 0xffff));

      for (us = 0; us < RKRNG_POLL_US; us++)
        {
          if (rng_getreg(RKRNG_STATE) & RKRNG_STATE_SW_DRNG_ACK)
            {
              break;
            }

          up_udelay(1);
        }

      if (us >= RKRNG_POLL_US)
        {
          syslog(LOG_ERR,
                 "ERROR: RNG 取数超时 STATE=0x%08" PRIx32 "\n",
                 rng_getreg(RKRNG_STATE));
          return -ETIMEDOUT;
        }

      /* 写 1 清 ACK，供下一轮使用。 */

      rng_putreg(RKRNG_STATE, RKRNG_STATE_SW_DRNG_ACK);

      for (i = 0; i < (int)(RKRNG_MAX_BYTES / 4); i++)
        {
          words[i] = rng_getreg(RKRNG_DRNG_DATA_0 + i * 4);
        }

      memcpy(buf + got, words, chunk);
      got += chunk;
    }

  return OK;
}

/****************************************************************************
 * 字符设备接口
 ****************************************************************************/

static ssize_t rk3576_rng_read(FAR struct file *filep, FAR char *buffer,
                               size_t buflen)
{
  UNUSED(filep);

  if (buffer == NULL || buflen == 0)
    {
      return -EINVAL;
    }

  if (rk3576_rng_fill((FAR uint8_t *)buffer, buflen) < 0)
    {
      return -EIO;
    }

  return (ssize_t)buflen;
}

static const struct file_operations g_rng_fops =
{
  NULL,               /* open  */
  NULL,               /* close */
  rk3576_rng_read,    /* read  */
  NULL,               /* write */
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int rk3576_rng_initialize(void)
{
  uint8_t a[16];
  uint8_t b[16];
  int ret;

  /* 开时钟。非安全域门控同样是高 16 位写使能掩码。 */

  putreg32((1u << (RKRNG_GATE_BIT + 16)) | (0u << RKRNG_GATE_BIT),
           RK3576_CRU_ADDR + RKRNG_GATE_REG);

  /* ★ 自检要能证伪"取到的是随机数"。
   *
   *   只看"读到了数据"不够 —— 寄存器块没上电时读回的是全 0 或全 f，
   *   长度检查一样能通过。这里取两批比较：两批完全相同，说明拿到的
   *   是常量而非随机数（硬件没工作），此时宁可不注册也不能让上层
   *   拿着一串固定值当随机数用。
   */

  ret = rk3576_rng_fill(a, sizeof(a));
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_rng_fill(b, sizeof(b));
  if (ret < 0)
    {
      return ret;
    }

  if (memcmp(a, b, sizeof(a)) == 0)
    {
      syslog(LOG_ERR,
             "ERROR: RNG 两次取数完全相同（首字节 0x%02x）——"
             "不是随机数，硬件未工作\n", a[0]);
      return -EIO;
    }

  syslog(LOG_INFO,
         "RNG: 自检通过，两批取数不同（0x%02x%02x… vs 0x%02x%02x…）\n",
         a[0], a[1], b[0], b[1]);

  ret = register_driver("/dev/random", &g_rng_fops, 0444, NULL);

#ifdef CONFIG_DEV_URANDOM_ARCH
  /* /dev/urandom 指向同一个硬件（见下面 devurandom_register 的说明）。 */

  if (ret >= 0)
    {
      ret = register_driver("/dev/urandom", &g_rng_fops, 0444, NULL);
    }
#endif

  return ret;
}

#ifdef CONFIG_DEV_RANDOM
/****************************************************************************
 * Name: devrandom_register
 *
 * Description:
 *   选了 ARCH_HAVE_RNG 后 CONFIG_DEV_RANDOM 默认打开，drivers_initialize()
 *   会调它。留空，理由同下面的 devurandom_register：/dev/random 在
 *   rk3576_rng_initialize() 里开时钟、自检通过之后才注册。
 *
 ****************************************************************************/

void devrandom_register(void)
{
}
#endif

#ifdef CONFIG_DEV_URANDOM_ARCH
/****************************************************************************
 * Name: devurandom_register
 *
 * Description:
 *   CONFIG_DEV_URANDOM_ARCH 要求芯片层提供它。
 *
 * ★ 这里留空，/dev/urandom 由 rk3576_rng_initialize() 注册。
 *
 *   drivers_initialize() 在 OS 启动早期就调这个函数，那时 RNG 的时钟
 *   还没开、自检还没做。在这里注册的话，节点先于硬件就绪出现，而且
 *   自检失败（取到常量）时也挡不住 —— 那恰恰是 /dev/urandom 最不该
 *   交出去的东西。所以和 /dev/random 一起，自检通过后再注册。
 *
 *   不用 NuttX 自带的软件 /dev/urandom（xorshift128）：xTS 1.3.16 用
 *   nist_sts 读 /dev/urandom，测软件伪随机数证明不了本板的 RNG。
 *
 ****************************************************************************/

void devurandom_register(void)
{
}
#endif

#endif /* CONFIG_RK3576_RNG */
