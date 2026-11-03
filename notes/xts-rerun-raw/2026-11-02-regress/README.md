# xTS 回归重跑（2026-11-02）

目的：9 月 20 日的 A 档证据是在 `ecc7cba` 镜像上取的，之后固件改动很大（AMP 相机、
WiFi/蓝牙、界面重做、家居 MQTT 与板上 broker、网卡接收缓冲修复、TCP/回调配额调整）。
在当前固件上按原文命令把能在正常镜像上跑的用例重跑一遍，确认没有退步。

- 镜像：`uname`: `NuttX 0.0.0 ed8f69ad-dirty Nov  2 2026 00:28:35 arm64 kickpi-k7`
  （主体测试）；ramtest 重跑与开机日志用的是之后烧录的镜像，只多了 `k7diag sdpart / baud`
  两个手动子命令，不在任何用例路径上。
- 脚本：`run.py`（复制自 `../2026-09-20-btier/run.py`，补全了用例清单）、`summarize.py`。
- 每条用例两份原始记录：`<用例>-<命令>.net.txt`（telnet）与 `.serial.raw`（串口）；
  `events.jsonl` 记命令、耗时、两侧字节数；`run.log` 是整轮的运行输出。
- 开机串口日志：`boot-after-flash.serial.raw`。

## 结果

| 用例 | 命令 | 结果 | 与 09-20 比 |
|---|---|---|---|
| 1.1.2 调度 | `cmocka_sched_test` | PASSED 16/16 | 同 |
| 1.1.3 系统调用 | `cmocka_syscall_test` | 82 项全部运行，0 失败 | 同 |
| 1.1.5 getprime | `getprime` | 1230 个素数，148 ms | 149 ms |
| 1.1.6 内存 | `mm` | TEST COMPLETE | 同 |
| 1.1.7 scanf | `scanftest` | OK 164 / FAILED 0 | 同 |
| 1.1.8 / 1.1.9 | `hello` / `helloxx` | 正常输出 | 同 |
| 1.1.10 popen | `popen` | `popen("help")` 输出完整返回 | 同 |
| 1.1.11 pipe | `pipe` | 4 项全 PASSED | 同 |
| 1.1.12 MD5 | `echo xts-md5-test-file > /tmp/1.txt`；`md5_test -f /tmp/1.txt -c 100` | 100/100 均为 `e8561e8a…87ca`，与主机 `md5sum` 一致 | 同 |
| 1.1.13 C++ | `cxxtest` | vector / map / RTTI / Exception 均执行 | 同 |
| 1.2.3 RAM 占用 | `free`、`ampctl exec free` | openvela 总 62,468,096 B，开测时已用 11.2 MB；Linux 总 3,880,604 KB，已用 22,516 KB | openvela 总量少 1 MB（新功能的静态区） |
| 1.2.4 Flash 占用 | `df -h`、`ampctl exec df` | openvela 新增 `/data`（eMMC cache 分区，vfat 383 MB，已用 1152 KB）；Linux 根为 initramfs，不占 Flash（`df -h` 在 busybox 上不支持 `-h`，以不带参数的为准） | 多了 `/data` |
| 1.3.2 RAM 读写 | `fstest -n 10 -m /tmp` | OK 20 / FAILED 0 | 同 |
| 1.3.3 RAM 读写性能 | `ramtest -w/-h/-b -s 52245696` | 三种宽度各 6 阶段走完，无地址错误，串口 0 字节 | 同（见下） |
| 1.3.4 RAM 随机读写 | `mkrd -m 10 -s 1000 1024`；`cmocka_driver_block -m /dev/ram10` | PASSED 3/3 | 同 |
| 1.3.6 GPIO | `cmocka_driver_gpio -i /dev/gpio3 -o /dev/gpio2` | 4/4 OK（bool / loop / rw / interrupt） | 同 |
| 1.3.7 I²C/SPI | `cmocka_driver_i2c_spi` | **失败**：打不开 `/dev/accel0`（见下） | 退步，硬件原因 |
| 1.3.10 UART | `cmocka_driver_uart -d /dev/ttyS0` | PASSED 1/1 | 同 |
| 1.3.13 Timer | `cmocka_driver_oneshot -d /dev/oneshot` | PASSED 1/1 | 同 |
| 1.3.17 Crypto | des3cbc / aescbc / aesctr / aesxts / hmac / hash / crc32 / ecdsa | 全部 OK | 同 |

## 两处说明

**1.3.3 第一次 malloc 失败，重跑通过。** 主体那轮在开测前取的 maxfree（52,250,672）
到跑 ramtest 时已经要不到了（前面二十多条用例跑过，堆有碎片），三种宽度都报
`RAMTest: malloc failed`，原始记录保留在本目录 `1.3.3-*`。重启后紧挨着取 `free`
（maxfree 52,311,232），按 09-19 的做法减 64 KB 得 52,245,696，三种宽度全部通过，
记录在 `ramtest-retry/`。

**1.3.7 失败是传感器没接上。** `boot-after-flash.serial.raw` 里：
`I2C7: 空闲电平 SCL=1 SDA=1`、复用正确，但
`ERROR: MPU6050 @I2C7:0x68 无应答或 ID 不符（ret=-6 WHO_AM_I=0x00）`，因此没有
注册 `/dev/accel0`，用例在 setup 里 `fd > 0` 断言失败。驱动与配置未变；要按
`board/kickpi-k7/src/kickpi_k7_mpu6050.c` 顶部的接线（40 针 1/6/22/24 脚）重新插好
MPU-6050 后重跑。

## 本次没有覆盖的

- 1.1.4 ostest：要在单系统镜像（`flash.sh --solo`）上跑，本轮没换镜像。
- 1.3.12 RTC、1.3.16 RNG：用例只编在 `xts-driver-tests.config` 叠加镜像里。
- 1.2.1 / 1.2.2 / 2.1.3 / 2.1.4（重启、冷启动、启动时间）、3.1.1（12h KASAN）：需要专门的计时脚本或镜像。
