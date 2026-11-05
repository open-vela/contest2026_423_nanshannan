/****************************************************************************
 * board/kickpi-k7/src/kickpi_k7_mpu6050.c
 * SPDX-License-Identifier: Apache-2.0
 *
 * MPU-6050（六轴，加速度 + 陀螺）接到 40 针扩展口的 I2C7，按 uORB 传感器
 * 框架注册成 /dev/accel0 与 /dev/gyro0。
 *
 * ★ 为什么不用树里现成的驱动
 *
 *   drivers/sensors/mpu60x0.c 是老式字符设备（register_driver 出 /dev/imuN，
 *   读回私有结构），不走 uORB；mpu9250_uorb.c 走 uORB，但绑死 AK8963 磁力计
 *   （MPU6050 没有）。xTS 1.3.7 的 cmocka_driver_i2c_spi 打开的是
 *   /dev/accel0（注意是 accel 而不是 imu）并按 struct sensor_accel 读，
 *   所以这里补一层 uORB 适配。
 *
 * ★ 只实现 fetch
 *
 *   drivers/sensors/sensor.c 的 sensor_read()：lower->ops->fetch 非空时直接
 *   走它取一帧，不需要缓冲区与推送线程（sensor_register 里 fetch 非空会把
 *   nbuffer 置 0）。用例是"打开后连读 100 次"，fetch 就够；省掉一个常驻
 *   工作线程。
 *
 * ★ 接线（厂商规格书《40Pin 引脚定义》）
 *
 *     MPU6050 VCC -> 1 脚 VCC_3V3      SCL -> 22 脚 GPIO3_A0 = I2C7_SCL_M1
 *     MPU6050 GND -> 6 脚 GND          SDA -> 24 脚 GPIO3_A1 = I2C7_SDA_M1
 *     AD0 -> GND（地址 0x68；接 VCC 则为 0x69）
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/nuttx.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/fs/fs.h>
#include <nuttx/sensors/bmi160.h>
#include <nuttx/sensors/sensor.h>

#include "rk3576_i2c.h"
#include "kickpi_k7.h"

#ifdef CONFIG_KICKPI_K7_MPU6050

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MPU_ADDR        0x68
#define MPU_FREQ        400000

#define MPU_SMPLRT_DIV  0x19
#define MPU_CONFIG      0x1a
#define MPU_GYRO_CONFIG 0x1b
#define MPU_ACCEL_CONFIG 0x1c
#define MPU_ACCEL_XOUT  0x3b   /* 之后依次是 accel(6) temp(2) gyro(6) */
#define MPU_PWR_MGMT_1  0x6b
#define MPU_WHO_AM_I    0x75

#define MPU_WHOAMI_VAL  0x68

/* 量程：加速度 ±2g（16384 LSB/g）、陀螺 ±250°/s（131 LSB/(°/s)） */

#define ACCEL_LSB_PER_G 16384.0f
#define GYRO_LSB_PER_DPS 131.0f
#define STANDARD_G      9.80665f
#define DEG_TO_RAD      0.017453292f

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct mpu6050_dev_s
{
  struct sensor_lowerhalf_s   accel;
  struct sensor_lowerhalf_s   gyro;
  FAR struct i2c_master_s    *i2c;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct mpu6050_dev_s g_mpu6050;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int mpu_read(FAR struct mpu6050_dev_s *dev, uint8_t reg,
                    FAR uint8_t *buf, size_t len)
{
  struct i2c_msg_s msg[2];

  msg[0].frequency = MPU_FREQ;
  msg[0].addr      = MPU_ADDR;
  msg[0].flags     = 0;
  msg[0].buffer    = &reg;
  msg[0].length    = 1;

  msg[1].frequency = MPU_FREQ;
  msg[1].addr      = MPU_ADDR;
  msg[1].flags     = I2C_M_READ;
  msg[1].buffer    = buf;
  msg[1].length    = len;

  return I2C_TRANSFER(dev->i2c, msg, 2);
}

static int mpu_write(FAR struct mpu6050_dev_s *dev, uint8_t reg, uint8_t val)
{
  struct i2c_msg_s msg;
  uint8_t buf[2];

  buf[0] = reg;
  buf[1] = val;

  msg.frequency = MPU_FREQ;
  msg.addr      = MPU_ADDR;
  msg.flags     = 0;
  msg.buffer    = buf;
  msg.length    = 2;

  return I2C_TRANSFER(dev->i2c, &msg, 1);
}

/* 原始 14 字节：accel x/y/z、temp、gyro x/y/z，全是大端有符号 */

static int mpu_sample(FAR struct mpu6050_dev_s *dev, FAR int16_t *raw)
{
  uint8_t buf[14];
  int ret;
  int i;

  ret = mpu_read(dev, MPU_ACCEL_XOUT, buf, sizeof(buf));
  if (ret < 0)
    {
      return ret;
    }

  for (i = 0; i < 7; i++)
    {
      raw[i] = (int16_t)((buf[i * 2] << 8) | buf[i * 2 + 1]);
    }

  return OK;
}

/* 温度换算见数据手册 4.18：T(℃) = raw/340 + 36.53 */

static float mpu_temp(int16_t raw)
{
  return (float)raw / 340.0f + 36.53f;
}

static int mpu_accel_fetch(FAR struct sensor_lowerhalf_s *lower,
                           FAR struct file *filep,
                           FAR char *buffer, size_t buflen)
{
  FAR struct mpu6050_dev_s *dev =
    container_of(lower, struct mpu6050_dev_s, accel);
  struct sensor_accel data;
  int16_t raw[7];
  int ret;

  if (buflen < sizeof(data))
    {
      return -EINVAL;
    }

  ret = mpu_sample(dev, raw);
  if (ret < 0)
    {
      return ret;
    }

  memset(&data, 0, sizeof(data));
  data.timestamp   = sensor_get_timestamp();
  data.x           = raw[0] / ACCEL_LSB_PER_G * STANDARD_G;
  data.y           = raw[1] / ACCEL_LSB_PER_G * STANDARD_G;
  data.z           = raw[2] / ACCEL_LSB_PER_G * STANDARD_G;
  data.temperature = mpu_temp(raw[3]);

  memcpy(buffer, &data, sizeof(data));
  return sizeof(data);
}

static int mpu_gyro_fetch(FAR struct sensor_lowerhalf_s *lower,
                          FAR struct file *filep,
                          FAR char *buffer, size_t buflen)
{
  FAR struct mpu6050_dev_s *dev =
    container_of(lower, struct mpu6050_dev_s, gyro);
  struct sensor_gyro data;
  int16_t raw[7];
  int ret;

  if (buflen < sizeof(data))
    {
      return -EINVAL;
    }

  ret = mpu_sample(dev, raw);
  if (ret < 0)
    {
      return ret;
    }

  memset(&data, 0, sizeof(data));
  data.timestamp   = sensor_get_timestamp();
  data.x           = raw[4] / GYRO_LSB_PER_DPS * DEG_TO_RAD;
  data.y           = raw[5] / GYRO_LSB_PER_DPS * DEG_TO_RAD;
  data.z           = raw[6] / GYRO_LSB_PER_DPS * DEG_TO_RAD;
  data.temperature = mpu_temp(raw[3]);

  memcpy(buffer, &data, sizeof(data));
  return sizeof(data);
}

/* activate / set_interval：器件常开、按需取数，这里只需如实返回 OK，
 * 否则上层 SNIOC_ACTIVATE 会拿到 -ENOTSUP。
 */

static int mpu_activate(FAR struct sensor_lowerhalf_s *lower,
                        FAR struct file *filep, bool enable)
{
  UNUSED(lower);
  UNUSED(filep);
  UNUSED(enable);
  return OK;
}

static int mpu_set_interval(FAR struct sensor_lowerhalf_s *lower,
                            FAR struct file *filep,
                            FAR uint32_t *period_us)
{
  UNUSED(lower);
  UNUSED(filep);
  UNUSED(period_us);
  return OK;
}

static const struct sensor_ops_s g_mpu_accel_ops =
{
  .activate     = mpu_activate,
  .set_interval = mpu_set_interval,
  .fetch        = mpu_accel_fetch,
};

static const struct sensor_ops_s g_mpu_gyro_ops =
{
  .activate     = mpu_activate,
  .set_interval = mpu_set_interval,
  .fetch        = mpu_gyro_fetch,
};

/****************************************************************************
 * Name: mpu_chr_read
 *
 * Description:
 *   /dev/accel0 的字符设备读：返回一帧 struct accel_gyro_st_s。
 *
 * ★ 为什么还要这个节点
 *
 *   xTS 1.3.7 的 cmocka_driver_i2c_spi 是按 **BMI160 字符设备**写的：
 *   它 include <nuttx/sensors/bmi160.h>，以阻塞方式 open 后按
 *   sizeof(struct accel_gyro_st_s)（16 字节：陀螺 3xint16 + 加速度 3xint16
 *   + 时间戳）连读 100 次，并断言每次返回值等于该长度。
 *
 *   uORB 节点满足不了它，有两处对不上：
 *     一、数据格式是 struct sensor_accel（28 字节，浮点 m/s^2）；
 *     二、drivers/sensors/sensor.c 的 sensor_read() 在**阻塞模式**下
 *         先等 user->buffersem，而 fetch 型底层不会去 post 它 ——
 *         用例 open 时没带 O_NONBLOCK，于是永远等在那里（实测卡住，
 *         既不返回也不报错）。
 *
 *   所以这里按用例期待的布局另开一个字符节点：同一颗器件、同一次采样，
 *   只是把原始计数按 BMI160 的结构摆好。两个节点并存，uORB 那对用于
 *   框架集成，这个用于跑原文用例。
 *
 ****************************************************************************/

static ssize_t mpu_chr_read(FAR struct file *filep, FAR char *buffer,
                            size_t buflen)
{
  FAR struct mpu6050_dev_s *dev = filep->f_inode->i_private;
  struct accel_gyro_st_s out;
  int16_t raw[7];
  int ret;

  if (buflen < sizeof(out))
    {
      return -EINVAL;
    }

  ret = mpu_sample(dev, raw);
  if (ret < 0)
    {
      return ret;
    }

  memset(&out, 0, sizeof(out));
  out.accel.x     = raw[0];
  out.accel.y     = raw[1];
  out.accel.z     = raw[2];
  out.gyro.x      = raw[4];
  out.gyro.y      = raw[5];
  out.gyro.z      = raw[6];
  out.sensor_time = (uint32_t)clock_systime_ticks();

  memcpy(buffer, &out, sizeof(out));
  return sizeof(out);
}

static const struct file_operations g_mpu_chr_fops =
{
  NULL,            /* open  */
  NULL,            /* close */
  mpu_chr_read,    /* read  */
  NULL,            /* write */
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int kickpi_k7_mpu6050_initialize(void)
{
  FAR struct mpu6050_dev_s *dev = &g_mpu6050;
  uint8_t id = 0;
  int ret;

  dev->i2c = rk3576_i2cbus_initialize(CONFIG_KICKPI_K7_MPU6050_I2C_BUS);
  if (dev->i2c == NULL)
    {
      syslog(LOG_ERR, "ERROR: MPU6050 所在 I2C%d 初始化失败\n",
             CONFIG_KICKPI_K7_MPU6050_I2C_BUS);
      return -ENODEV;
    }

  /* ★ 先核对 WHO_AM_I 再注册。
   *
   *   注册成功不等于器件在位 —— 本板的触摸、RTC 都踩过这一条。
   *   0x75 读回 0x68 才是 MPU-6050 本人（同系列 MPU-6500 读回 0x70、
   *   MPU-9250 读回 0x71，寄存器布局兼容但不是同一颗）。
   */

  ret = mpu_read(dev, MPU_WHO_AM_I, &id, 1);
  if (ret < 0 || id != MPU_WHOAMI_VAL)
    {
      syslog(LOG_ERR,
             "ERROR: MPU6050 @I2C%d:0x%02x 无应答或 ID 不符"
             "（ret=%d WHO_AM_I=0x%02x，应为 0x%02x）\n",
             CONFIG_KICKPI_K7_MPU6050_I2C_BUS, MPU_ADDR, ret, id,
             MPU_WHOAMI_VAL);
      return ret < 0 ? ret : -ENODEV;
    }

  /* 上电默认 SLEEP=1，必须清掉才会采样；时钟源选陀螺 X 轴（手册推荐，
   * 比内部 8MHz RC 稳）。
   */

  ret = mpu_write(dev, MPU_PWR_MGMT_1, 0x01);
  if (ret < 0)
    {
      return ret;
    }

  up_mdelay(10);

  mpu_write(dev, MPU_SMPLRT_DIV, 0x07);    /* 1kHz/(1+7) = 125Hz */
  mpu_write(dev, MPU_CONFIG, 0x03);        /* DLPF 44Hz，抑制抖动 */
  mpu_write(dev, MPU_GYRO_CONFIG, 0x00);   /* ±250 °/s */
  mpu_write(dev, MPU_ACCEL_CONFIG, 0x00);  /* ±2 g */

  dev->accel.ops  = &g_mpu_accel_ops;
  dev->accel.type = SENSOR_TYPE_ACCELEROMETER;
  dev->gyro.ops   = &g_mpu_gyro_ops;
  dev->gyro.type  = SENSOR_TYPE_GYROSCOPE;

  ret = sensor_register(&dev->accel, 0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 注册 /dev/accel0 失败: %d\n", ret);
      return ret;
    }

  ret = sensor_register(&dev->gyro, 0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 注册 /dev/gyro0 失败: %d\n", ret);
      sensor_unregister(&dev->accel, 0);
      return ret;
    }

  ret = register_driver("/dev/accel0", &g_mpu_chr_fops, 0444, dev);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: 注册 /dev/accel0 失败: %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO,
         "MPU6050: /dev/accel0 与 /dev/uorb/sensor_accel0|gyro0 就绪"
         "（I2C%d:0x%02x，WHO_AM_I=0x%02x）\n",
         CONFIG_KICKPI_K7_MPU6050_I2C_BUS, MPU_ADDR, id);
  return OK;
}

#endif /* CONFIG_KICKPI_K7_MPU6050 */
