# NPU 提频到 950MHz（2026-11-02）

接 `notes/npu-2026-11-02/`：NPU 跑通了，但没开 devfreq，时钟停在开机的 198MHz。
本轮把它提到 OPP 表的最高档 950MHz。

**结果**：MobileNet v1 单次 NPU 耗时 **8.6ms → 2.0ms**（4.3 倍；频率比 4.8 倍），
top-5 不变。连续 20000 次（约 40 秒）全部在 2.0~2.1ms，结果一致，openvela 正常。

## 要解决的三件事

### 1. vdd_npu 归谁、现在多少伏

- vdd_npu_s0 是 RK806 的 DCDC2（BUCK2），RK806 在 i2c1 的 0x23。
- **i2c1 归 Linux**：rk3x-i2c 驱动绑着它（`/sys/bus/i2c/devices/1-0023`），空闲时
  还会把 i2c1 的时钟关掉（CRU CLKGATE_CON12 = 0x1081，PCLK_I2C1、CLK_I2C1 都关）。
  这就是 openvela 的 `/dev/i2c1` 什么都扫不到的原因（`notes/npu-2026-11-02` 第 2 条），
  不是总线坏了。openvela 不该再去碰这条总线。
- 从 Linux 的 `/dev/i2c-1` 用 I2C_RDWR 读（`run1/`、`run2/` 的 _netsh）：

  | 寄存器 | 值 | 含义 |
  |---|---|---|
  | 0x5a/0x5b CHIP_NAME | 0x80 0x6a | RK806，A 版 |
  | 0x00 POWER_EN0 | 0x0f | DCDC1~4 都开 |
  | 0x1b BUCK2_ON_VSEL | 0x28 | 500 + 40 × 6.25 = **750mV**（和原理图一致） |

- **重启后回到 750mV**（`reboot` 会复位 PMIC 的这个寄存器），所以每次开机都要重新升压。

### 2. 电压升到多少

rk3576.dtsi `npu_opp_table` 的 950MHz 档：L0/L1 875mV，往下按漏电分档逐级降到
L10 的 762.5mV。AMP 内核没有 OPP/稳压器框架，也没读芯片的分档，**按最差的
L0 取 875mV**（同时也是表里所有档的上限，低于原理图 vdd_npu 的 950mV）。

工具 `amp/linux/npu/k7pmic.c`（静态链接，进 lxroot）：
- `k7pmic npu-mv` 只读；
- `k7pmic npu-mv <mV>` 只写 0x1b，只认 725~875mV，写前确认芯片是 RK806、DCDC2
  开着，向上取整到 6.25mV 一档，写完回读。

### 3. 时钟怎么设

clk_npu 是 SCMI 的 CLK_RKNN_DSU0（时钟由 trust 固件管，PVTPLL）。rknpu 不开 devfreq
时从不设频率。在 `0004-rknpu-as-module.patch` 加了第三处：模块参数 `npu_hz`。

试了两次才对：

| 版本 | 做法 | 结果 |
|---|---|---|
| a | probe 里 `clk_set_rate`（insmod 带参数） | 日志 `198000000 -> 198000000`。后来发现**参数根本没传进去**：initramfs 的 busybox 没开 `FEATURE_CMDLINE_MODULE_OPTIONS`，insmod 静默丢掉 `npu_hz=`（`/sys/module/rknpu/parameters/npu_hz` 读出 0） |
| b | 挪到 `rknpu_power_on` 里时钟开着之后设（厂商 `rockchip_opp_config_clks` 也只在 NPU 时钟开着时设 SCMI 时钟）；参数改 0644，k7npu 在 insmod 之后写 sysfs | `clk_npu 198000000 -> 950000000 Hz (0)`。clk-scmi 是 `CLK_GET_RATE_NOCACHE`，这个数是固件回读 |

probe 最后会把 NPU 关电，所以 insmod 之后第一次推理就是"下一次上电"，参数在那时生效。

关于 a 版：参数是 0，那次 `clk_set_rate` 根本没调用，所以"时钟关着时固件不理"
这一点**没有被直接验证**。b 版两处都改了，目前不知道单改哪一处是否够用。

## 用法

```
k7npu load [MHz]   # 默认 950；支持 950/900/800/700/600/500，0 = 不动
k7npu test [次数]  # 没装会先 load（即 950）
k7npu status       # 多打一行 vdd_npu 电压
```

顺序是**先升压，后升频**：k7npu 先读 vdd_npu，低于该档电压就用 k7pmic 升，然后才
insmod、写 npu_hz。电压只升不降。换频率要先 `rmmod rknpu` 再 load。
load 会跳过已装的模块（rmmod rknpu 后 drm 还在）。

## 板上数据（run2/_netsh）

镜像：Linux 内核 `#28`（Image 未改），lxroot `2026-11-02T16:3x`，rknpu.ko 重编。

| 条件 | vdd_npu | clk_npu | NPU 耗时（10 次） | top-1 |
|---|---|---|---|---|
| `load 0` | 750mV | 198MHz | 8582~8758 us | 283 0.4153 |
| `load`（950） | 875mV | 950MHz | 2012~2191 us（首次 2191） | 283 0.4153 |
| 950，20000 次 | 875mV | 950MHz | 19999 次在 2000~2099 us，首次 2140 | 不变 |
| 不重启：950 → `rmmod rknpu; k7npu load 800` → 再回 950 | 875mV | 800MHz | 2071~2074 us（首次 2171） | 不变 |

固件回读依次是 `198→950`、`950→800`、`800→950`，换档可用。

**800MHz 只比 950MHz 慢 2.5%**：MobileNet v1（224×224）这么小的模型在 800MHz 以上
已经不受算力限制，主要卡在访存和每次提交的开销。如果以后在意发热，800MHz（L0 档
800mV）是更划算的点。

## 还没做 / 风险

- **温度没监控**：AMP 内核没开 thermal（`/sys/class/thermal` 没有 zone），厂商的
  950MHz 是配着温控降频用的。40 秒满载没出问题，长时间满载（比如摄像头每帧都跑
  NPU）之前要先有温度读数（tsadc 或 openvela 侧）。
- 没读芯片漏电分档，电压按最差档给，可能偏高 0~112mV，功耗和发热略多。
- busybox 的 `FEATURE_CMDLINE_MODULE_OPTIONS` 没开，以后给其它模块传参也会被静默丢掉。
  开它要重编 Image（initramfs 内嵌），本轮没动。
- rknpu.ko 重编后的 `0004` 补丁已更新；内核树里的源文件也已改动。

## 第二轮：默认 800MHz、温度读数与温控、刷内核（run3/_netsh）

### 改动

- **内核**（`amp/linux/amp-rootfs.config`）：`THERMAL`、`THERMAL_OF`、`ROCKCHIP_THERMAL` = y。
  tsadc 节点在 dtb 里本来就是 okay，dtb 不用重刷。Image 7823872 字节（内核 `#29`）。
- **busybox**：开 `FEATURE_CMDLINE_MODULE_OPTIONS`，`insmod x.ko a=1` 不再丢参数。
- **rknpu**（`0004` 第三节重写）：`npu_hz` 改成 `module_param_cb`，运行中写入时，
  NPU 开着就立刻设时钟，关着就等下次上电。
- **k7npu**：默认 800MHz；新增 `mhz`、`temp`、`guard`；`load` 后在后台起 guard。

### 板上结果

| 项 | 结果 |
|---|---|
| tsadc probe | `tsadc is probed successfully!`（另有一行 `Missing rockchip,grf property` 警告，RK3576 的数据里没用 grf，不影响） |
| 空闲温度 | soc/little/ddr/npu 42.5°C，bigcore/gpu 43.4°C |
| `k7npu test`（默认 800） | vdd_npu 800mV，`clk_npu 198000000 -> 800000000`，NPU 2187~2306 us，top-1 283 不变 |
| 温控降频 | guard 的拷贝，阈值临时改成 40/35°C：日志 `npu 41°C：800 -> 600 MHz`，固件回读 `800000000 -> 600000000`，NPU 2442 us |
| 温控恢复 | 换回正常阈值的 guard：`npu 41°C：600 -> 800 MHz` |
| `k7npu mhz 950` | 先升压 800 → 875mV，再 `-> 950000000`，NPU 2016~2110 us |

真正过热没试（没法加热）。降频与恢复的逻辑是用临时降低的阈值验证的。

刷写：第一次 `flash-kernel.sh` 备份读到 4.6MB 时 USB 掉线（Windows 显示"设备描述符请求失败"），
内核区没被写。断电、重新插好 USB 后再刷，一次成功，回读一致。
