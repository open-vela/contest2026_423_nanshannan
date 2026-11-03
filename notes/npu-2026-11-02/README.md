# NPU 归 Linux：离线准备与上板步骤（2026-11-02）

前半是离线准备；**当天下午上板跑通**，结果见文末"上板结果"。
第一个里程碑定为：不接摄像头，用一张事先转好的猫图跑一次 MobileNet v1，
拿到 top-5 和耗时。

## 方案：电归 openvela，驱动归 Linux

AMP 内核里没有电源域和稳压器框架（`amp/linux/amp-minimal.config`），因为它们会
把"没人用"的域和电源关掉，而 openvela 正在用（vdd_cpu_lit 和 VOP 都出过事）。
摄像头已经是这么分的：openvela 上电、给 MCLK，Linux 只跑驱动。NPU 照此办理。

| 资源 | 归谁 | 做法 |
|---|---|---|
| vdd_npu_s0 = RK806 DCDC2（I2C1@0x23） | 不动 | DTS 标了 `regulator-boot-on`，U-Boot 留着；`k7diag npu` 只读确认 |
| PD_NPU → NPUTOP → NPU0 / NPU1 | openvela | `k7diag npu on`，按父子顺序上电 |
| SCMI `CLK_RKNN_DSU0` 和 CRU 门控、复位 | Linux | rknpu 驱动自己开（Linux 的 CRU、SCMI 驱动本来就在） |
| rknpu_mmu、中断 SPI 247/248 | Linux | 中断不进 amp-irqs，走默认路由给 Linux |

**硬约束**：rknpu 一 probe 就读写 NPU 寄存器，域没电时这会把总线挂死。所以：

- rknpu.ko、drm.ko 不进 lxroot 的 `load.order`（开机不装），另写 `npu.order`；
- 由 Linux 上的 `/usr/bin/k7npu load` 手动装，装之前先用 `k7npu-test pd` 从
  `/dev/mem` 读 PMU，四个域没全开就拒绝。PMU 在常开域，读它是安全的。

## 改了什么

**openvela**

- `chip/rk3576/rk3576_power.c`：
  - 加入 NPU 四个域。参数照抄厂商 `pm_domains.c` 的 `rk3576_pm_domains[]`。
  - 补了几个字段：电源位偏移（NPUTOP/NPU0/NPU1 在 0x214）；PD_NPU 没有 repair
    位，改看 STATUS bit0（反逻辑）；NPUTOP 撤销 idle 后等 15µs；厂商的 mem 复位
    流程（chain → mem 关 → mem 开）。mem 复位只对 NPU 域开，已经验证过的域行为不变。
  - **顺手修了一个旧 bug**：解除时钟门控写的是 `0x140`，没有加每个域自己的
    r_offset（厂商是 `clk_ungate_offset + r_offset`）。r_offset 为 0x4 的域
    （NVM、SDGMAC、AUDIO）受影响，原来会写到别的域的门控位上。不过只在域原本
    断电、需要上电时才走这段，所以以前没暴露。**上板后要回归网口和音频。**
- `app/k7diag/k7diag_main.c`：新增 `k7diag npu [on]`。
  - 不带参数只读：RK806 的 CHIP_NAME、POWER_EN0、BUCK2_ON_VSEL（换算成 mV），
    PMU 的 PWR/STATUS/REPAIR/CHAIN/MEM/REQ/ACK/IDLE，以及四个域的开关状态。
  - `on`：DCDC2 开着、电压在 550~950mV 时，才按顺序上电，任何一步失败就停。
  - 绝不碰 NPU 寄存器（0x27700000）和 NPU QoS（0x27f08000）。

**Linux**（内核树 `~/rk3576-amp/kernel-6.1` 已打补丁、已编）

- `amp/linux/amp-npu.config`：
  - `DRM=m`，`ROCKCHIP_RKNPU=m`，内存管理用 DRM GEM（走 IOMMU，不要连续内存）；
  - `DRM_ROCKCHIP` 明确关掉，因为显示归 openvela；
  - DMA heap 那条路要额外开 `DMABUF_HEAPS_ROCKCHIP` 再预留一块 CMA，不选。
- `amp/linux/0004-rknpu-as-module.patch`：导出 `iommu_get_dma_cookie`。不导出的话
  modpost 报 undefined，因为厂商一直把 rknpu 编进内核。
- `amp/linux/rk3576-kickpi-k7-amp.dts`：
  - `&rknpu` 删掉 power-domains、power-domain-names、rknpu-supply 后 okay；
  - `&rknpu_mmu` okay。开机时 rockchip_iommu.ko 会 probe 它，但 probe 只拿时钟
    （prepare）和中断，attach 在 `pm_runtime_get_if_in_use()==0` 时直接返回，
    不碰寄存器。这一点是读代码得出的，上板第 1 步要验证。
- `amp/linux/rootfs/build-rootfs.sh`：
  - 合并 amp-npu.config；
  - 检查 RKNPU 必须是 m、DRM_ROCKCHIP 必须没开。
- `amp/linux/lxroot/build-lxroot.sh`：
  - 拆出 npu.order；
  - 带上 librknnrt.so（SDK 2.0.0b0，只要 GLIBC_2.17、GLIBCXX_3.4.22，现有运行库够用）、
    k7npu、k7npu-test、mobilenet_v1.rknn，以及主机上用 PIL 转好的 cat.rgb。
- `amp/linux/npu/k7npu`、`k7npu-test.c`：手动装驱动、查状态、跑推理。

## 离线构建结果

| 产物 | 结果 |
|---|---|
| `nuttx.bin` | 3478496 字节，编过（同一版也带速记弹窗的空指针修复） |
| `Image-amp-rootfs` | 7754240 字节（窗口 8388608，余 634368），sha256 `190194fdbdbdd5c5`，含 `iommu_get_dma_cookie` |
| `rk3576-kickpi-k7-amp.dtb` | 272105 字节（上限 544 扇区 = 278528） |
| `lxroot.cpio` | 22329344 字节，开机装 19 个模块，NPU 3 个手动装，sha256 `c79af8923d961f24` |

旧产物备份在 `~/rk3576-amp/out/*.pre-npu`。

## 上板步骤（每一步过了再走下一步）

1. **只刷 openvela**（`scripts/flash.sh`），先不动 Linux。
   - 回归：网口、音频、屏幕（ungate 修复影响 SDGMAC/AUDIO 的路径）。
   - 再在 openvela 上跑 `k7diag npu`（只读）。要记下的是：
     - 开机时四个域的状态；
     - DCDC2 是否开着、电压多少。
   - **DCDC2 是关的就停**：开它要写 RK806，另做方案。
2. **刷 Linux 三块料**：
   - Image：`scripts/flash-kernel.sh`；
   - dtb：`rkdeveloptool wl 14336`，然后回读比对；
   - lxroot：`scripts/lxroot-push.py`。

   开机后先**不**给 NPU 上电，确认摄像头链照常（10-camera.sh 装 rockchip_iommu 时会
   probe rknpu_mmu），dmesg 里没有异常。`k7npu status` 应显示电源域关、rknpu 未装。
3. openvela：`k7diag npu on`。
   - 每一步都有 10ms 上界，失败会打印卡在哪一步和寄存器值。
   - 成功后再跑一次只读的 `k7diag npu`，确认四个域都开着。
4. Linux：`k7npu load`。
   - 它先自查 PMU，再 insmod。
   - 看 dmesg 里 rknpu 的版本行（驱动 0.9.8），以及 `/dev/dri/renderD128`。
5. Linux：`k7npu test 5`。
   - 判据：rknn_init 成功；top-1 落在猫的几类（282~286 一带）；记下每次的 NPU 耗时。
   - 已知风险：SCMI 时钟、PVTPLL 在 AMP 下没人调过，若 init 卡住，先看 dmesg 里的时钟报错。

**回退**：
- openvela：`k7diag npu` 只在手动敲时才执行，不敲就等于没有。
- Linux：重刷 `*.pre-npu`。dtb 的旧版是 `~/rk3576-amp/out/rk3576-kickpi-k7-amp-cam.dtb`。

## 还不确定的

- U-Boot 启动后 NPU 四个域开没开、DCDC2 开没开：第 1 步读出来就知道。
- mem 复位那一步在冷启动的域上会不会卡：有上界，卡了会报 CHAIN/MEM 的值。
- NPU 跑起来之后，频率由谁定（devfreq 没开，固定在 `assigned-clock-rates` 198MHz，
  远低于标称）。先求能跑，再谈提频，提频要动 SCMI，属于下一轮。

## 上板结果（2026-11-02 下午）

镜像：openvela `09:11:14`，Linux 内核 `#27 Mon Nov 2 03:23:37`，lxroot `2026-11-02T14:23:46Z`。
原始输出在 `run1/_netsh/`（第一次 probe 失败）、`run2/_netsh/`（修复后跑通）。

1. **开机时 NPU 四个电源域本来就开着**（`k7diag npu`：PWR 第二字只有 bit9 = GPU 关；
   REPAIR bit22~24 全 1；STATUS bit0 = 0）。U-Boot 没关它们，`k7diag npu on` 不需要。
2. **openvela 读不到 RK806**：`/dev/i2c1` 扫不到任何设备（i2c2 正常，扫到 0x4e、0x51）。
   vdd_npu 的电压无从确认，以推理结果为准。未追。
3. 开机后、装 NPU 驱动前：摄像头模块照常装上、imx415 绑定；rknpu 没有被自动装；
   dmesg 里没有 rknpu 挂 IOMMU 的记录（IOMMU 的配置在驱动 probe 时才做），开机阶段确实
   没碰 NPU。
4. 第一次 `k7npu load`（run1）：驱动装上、IOMMU 模式、DRM 初始化都成功，但 probe 最后
   `failed to get pm runtime for rknpu, ret: -95`。原因：不开 `PM_DEVFREQ` 时
   `rknpu_devfreq_runtime_resume/suspend` 的空实现返回 -EOPNOTSUPP，被当成唤醒失败。
   补进 `0004-rknpu-as-module.patch`（第二处），只重编 rknpu.ko、Image 不变。
5. 修复后（run2）probe 成功，出现 `/dev/dri/card0`、`renderD128`，nbuf 在 0x3fe80000。
   `k7npu test 5`：

   ```
   rknn_init: 0，用时 13 ms
   api 2.0.0b0 / driver 0.9.8
   输入 0：1x224x224x3，150528 字节
   第 0~4 次：rknn_run 8 ms（NPU 8581~8691 us）
   top-5：283 0.4153 / 282 0.1758 / 286 0.1588 / 278 0.0603 / 279 0.0434
   ```

   283/282/286 是虎斑猫、花斑猫、埃及猫（1001 类，0 为背景）。NPU 中断 GIC 279 计数在涨。
   openvela 全程正常。
6. `k7npu` 脚本里 `tail -5` busybox 不认，已改成 `tail -n 5`（板上这一版还是旧的，
   只影响打印 dmesg 尾巴）。

频率仍是 198MHz（`assigned-clock-rates`，没开 devfreq）。
→ 提频见 `notes/npu-freq-2026-11-02/`：950MHz，单次 8.6ms → 2.0ms。

★ 同一天刷写时 USB 下载连续三次中途掉线（Windows 侧"设备描述符请求失败"，板子卡死要
断电）。重新插紧 USB 线之后一次成功，FIT、内核、dtb 连写都没再掉。
