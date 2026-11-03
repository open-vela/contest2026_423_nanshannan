# openvela × Rockchip RK3576（KICKPI-K7）BSP 适配

**队伍**：`contest2026_423_nanshannan`　**赛道**：新硬件适配

在 KICKPI-K7（Rockchip RK3576）上完成 openvela 从 0 到 1 的首次适配 ——
启动引导、GICv2 中断、串口控制台、基础外设驱动、显示与 LVGL 交互界面，
**openvela(A53×4) + Linux(A72×4) 异构双 OS（AMP）**，
并在上面做成一台**桌面家庭中控 + 工作健康助手**：家居 MQTT 控制、语音唤醒与拍照问答、
语音速记、工作室录制模式、久坐/喝水/用眼提醒；摄像头经 ISP 出图，NPU 跑在 Linux 一侧。

开机 **2.8 秒**进系统；xTS 通用自测 35 项中 **34 项按原文严格口径通过并留了原始日志**（
见 [`notes/xts-final-2026-11-02.md`](notes/xts-final-2026-11-02.md)）。

## 作品简介

openvela 官方已在大赛分支上把 KICKPI-K7 挂为**待适配目标**：
`vendor_rockchip` 的 `boards/rk3576/kickpi-k7/` 目录下只有一份适配指引 README，
明文标注「尚未适配、本目录不包含任何板级支持代码、欢迎提交可用的板级适配 PR」。
本作品即针对该目标完成 openvela 在 RK3576 平台上的首次适配。

| 核查项 | 结论 |
|---|---|
| openvela `arch/arm64/src/` | 有 a64 / rk3399 / imx8 / imx9 / vdk / zynq-mpsoc，**无 rk3576** |
| 上游 Apache NuttX | 有 rk3399、rk3588，**无 rk3576** |
| `vendor_rockchip/boards/rk3576/kickpi-k7/` | **仅有 README，无任何板级代码** |

## 选题方向

**新硬件适配**。openvela 官方仓把 KICKPI-K7 挂为待适配目标而无任何板级代码（见上表），
RK3576 在 openvela 与上游 NuttX 里都没有芯片层 —— 这是一块**真正从零开始**的板子：
先把系统启起来，再用它做出一个能说话、能看东西的产品形态，
最后用大赛的 xTS 用例逐条验收。三步都落在这个仓里。

## 当前状态

**openvela 已在 KICKPI-K7 上启动并运行，核心外设与端侧 AI 能力均已上板验证。**

```
nuttx.bin        3,482,592 字节（含 LVGL 界面、CJK 字库与 AI 能力）
启动             reboot 2.76 s / 冷启动 2.84 s（到 NuttShell 横幅，10 次均值）
CPU              openvela 4× Cortex-A53 SMP ＋ Linux 6.1 4× Cortex-A72（AMP 双系统）
中断控制器       GIC-400 / GICv2（两个 OS 共享同一分发器）
```

### 已上板跑通

| 子系统 | 验证方式 |
|---|---|
| 启动 / MMU / 异常向量 / GICv2 / Generic Timer / PSCI | 稳定启动至 `nsh>` |
| **SMP**（4× Cortex-A53） | `ps` 四核在线、`cmocka_sched_test` 16/16、`ostest` 退出 0、`smp_call_test` |
| 串口（16550，12 路 UART 已勘察） | 控制台 1.5 Mbaud（与厂商 U-Boot 一致） |
| **千兆网口**（GMAC + Maxio MAE0621A PHY） | 双向 ping 0% 丢包、1000M 全双工、TFTP 收发文件 |
| eMMC（dwcmshc） | 单系统下挂载读写；**AMP 双系统下暂未开启**（需在 Linux DTB 声明中断归属），`/data` 改用 tmpfs |
| TF 卡（dw-mshc） | `mkfatfs` + FAT32 挂载、2 MB 读写复验、`fstest` 20/20 |
| GPIO / I2C / SPI / RNG / 看门狗 / RTC(HYM8563) | 对应 cmocka 驱动用例 |
| 显示（VOP2 + MIPI DSI 2.0 + D-PHY + 触摸 FT8756） | 720×1280 出图，LVGL 可交互 GUI |
| 音频（SAI） | `cmocka_driver_audio` |
| **摄像头**（IMX415 → CSI D-PHY → CIF） | 取实帧，`INTSTAT=0x300`，动态范围正常 |
| **图像处理**（去马赛克 GBRG + 灰世界 AWB + 面积平均缩放 + JPEG） | 1932×1096 与 1280×720 实拍图，白平衡 G/R=1.02 |
| **V4L2**（`/dev/video0`） | `v4l2cap` 复刻 ai_agent 相机工具的调用序列，出 1280×720 JPEG |
| **ai_agent** | 接 MiMo 后端（OpenAI 兼容），TLS 握手 + 真实对话，`status=ok` |
| HDMI | 控制器探测：`CORE_ID` 读出 `"HQTX"`，电源域/时钟到位（尚未出图） |

### 产品形态：桌面家庭中控 + 工作健康助手

LVGL 界面开机自启，五个页签。规划与取舍见 [`docs/ui-product-plan.md`](docs/ui-product-plan.md)。

| 页签 | 能力 | 做法 / 实测 |
|---|---|---|
| **今日** | 时钟、久坐计时、喝水记录、用眼 20-20-20 | 到点弹全屏提醒卡片（用眼只出顶部横幅）；"10 分钟后"连按三次自动降频 |
| **家居** | 灯、空调、窗帘、新风等设备与场景 | 自写 MQTT 3.1.1 客户端；**板上自带 broker `k7mqttd`**，设备直连面板即可。只按设备回报的状态着色，3 s 没回应提示"X 没有响应" |
| **工作** | 工作室录制模式；语音速记 | 录制：按预设布景（补光开、空调/电视/新风关），门口牌子亮"录制中"，有人按门铃由面板代答并打点，录制期间语音不出板，结束只恢复被改过的设备。速记：说一句记一条，后台由大模型打标签 |
| **助手** | 语音唤醒、拍照问答、语音播报 | 唤醒 → 识别 → 拍照 → 看图作答 → TTS 念出来；端点检测在 A72 的 Linux 上做 |
| **设置** | 网络、音量、开发者页（录音机、相机） | 相机页经 ISP 出 1280×720 NV12，**约 29 fps** |

摄像头和 NPU 走 Linux 一侧（见下节）：

| 能力 | 做法 | 实测 |
|---|---|---|
| **摄像头 + ISP + 3A** | IMX415 整条链（CSI/CIF/ISP + rkaiq）在 Linux 上跑，帧经共享内存交给 openvela 显示 | 30 fps 出图；修了 rkaiq 两处 bug 后 3A 生效。为消拖影与暗处横纹关 BAY3D、增益封顶 16 倍（`amp/linux/rkaiq/tune-iq.py`） |
| **NPU** | rknpu 编成模块，电源域由 openvela 开、驱动由 Linux 装 | MobileNet v1 单次 **2.0 ms（950 MHz）/ 2.2 ms（默认 800 MHz）**，原 198 MHz 时 8.6 ms |
| **调压 + 温控** | vdd_npu（RK806 DCDC2）由 Linux 的 `k7pmic` 按 OPP 表升压；TSADC 温度驱动 + `k7npu guard` 软件降频 | 85°C 降到 600 MHz、95°C 降到 300 MHz；115°C 关机、120°C 硬件复位兜底 |

### 异构双 OS（AMP）—— openvela 与 Linux 同时在跑

四核 A53 跑 openvela、四核 A72 跑 Linux，**共享同一片 DDR 与同一个 GIC-400 分发器**
（GICv2 只有一个分发器，没有硬件隔离），经 mailbox 门铃 + rpmsg/virtio 通信。

```
nsh> ampctl status
  已注册: 是   握手完成: 是   收到门铃: 1
  最后收到: cmd=00000003 data=524d5347     ← Linux 侧 "RMSG"
  /dev/rpmsg/linux 存在
nsh> ps
  0..3  CPU0..CPU3 IDLE        ← openvela 四核 A53
  7     nsh_main
  8     kickpi_ui              ← LVGL 仪表盘，开机自启
  10    rpmsg-linux-0
```

| 组成 | 做法 |
|---|---|
| 分核引导 | U-Boot 的 FIT 按 `cpu = <MPIDR>` 分核：PSCI 拉 A72 跑 Linux，当前核 `armv8_switch_to_el2()` 进 openvela |
| GIC 共享 | openvela 侧不复位 SPI、不动 `GICD_CTLR`，按字节认领自己的中断；Linux 侧用 `rockchip,amp` 的 `amp-irqs` 声明归属 |
| 通信 | mailbox0 group0/3 门铃 + 0x47800000 起的 vring 与缓冲池，地址两侧逐字对齐 |
| Linux 瘦身 | 43 MB → 7.6 MB，裁掉的都是 AMP 下归 openvela 的外设 —— 既省地方，也是资源划分本身 |
| 资源划分 | 见 [`amp/OWNERSHIP.md`](amp/OWNERSHIP.md)：显示、音频、网络、存储归 openvela；**摄像头整链、ISP、NPU、i2c1/RK806 归 Linux**。Linux 不开 regulator 与电源域框架（会关掉 openvela 正在用的电源） |
| 用户态与镜像 | 内核内嵌最小 initramfs；摄像头驱动、rkaiq、NPU 运行库放在 eMMC 上的 lxroot（cpio，U-Boot 读进内存、校验 sha256），openvela 经网络 4 MB/s 更新它（`scripts/lxroot-push.py`） |
| 跨核调用 | `ampctl exec "<命令>"`：openvela 一侧发命令，Linux 的 `k7d` 执行并回传输出 |

细节见 [`amp/README.md`](amp/README.md)、[`amp/FLASH.md`](amp/FLASH.md)、[`docs/amp.md`](docs/amp.md)。

### 进行中

| 项 | 状态 |
|---|---|
| 蓝牙 / WiFi（SKW6621S，SDIO） | **已实板打通**：BLE 扫描；WiFi WPA2 关联、DHCP、ping。蓝牙连接/GATT 未测 |
| USB Host（DWC3 xHCI + 板载 hub） | 给 NuttX xHCI 补了平台总线与 hub 支持（`bsp/upstream/xhci-platform-hub.patch`）；控制器能起、能检测到 hub，**xHC DMA 不通，未枚举成功** |
| 红外接收 | 驱动与 `ir` 命令已写（`/dev/lirc0`，手动 `ir init`），未完成上板验证 |
| 座位感知 | 久坐目前按界面计时；用 NPU 做人脸检测判断在座/离座还没接 |
| HDMI 输出 | 控制器探测通过，未出图 |
| M.2 SSD（PCIe + NVMe） | 勘察完成，见 `docs/pcie-nvme.md`；NuttX 无 NVMe 驱动 |
| xTS 剩余 1 项 | 1.3.14 时间一致性：要关 NTP 静置 24 h 以上 |

### 沉淀

271 次提交；`notes/DEBUG-CASES.md` **29 例**排查记录（现象—歧路—根因—修复—教训），
其中多例是**上游缺陷**并已归档为可提交的补丁（见 `bsp/upstream/` 与 `amp/`）；
**3 个可复用 Skill**（硬件参数勘察 / 板级时钟排查 / 有界 bring-up），
均从本项目的真实返工中提炼，不绑定 RK3576。

主机环境有两个会让配置系统**静默失效**的坑（`olddefconfig` 失败却不影响 `make`、
LVGL 被原版顶替），单独记在 [`scripts/setup-host.md`](scripts/setup-host.md)。

### 已定位并修复的上游缺陷

| 位置 | 问题 |
|---|---|
| NuttX `arch/arm64` 启动路径 | 未清除 U-Boot 遗留的 `HCR_EL2.TGE`，`eret` 到 EL1 触发 Illegal Execution State |
| NuttX `drivers/input/ft5x06.c` | 缺 `TSIOC_GETMAXPOINTS`，LVGL 的触摸设备绑不上 —— **两边都不报错**，界面正常、点下去没反应 |
| Linux `drivers/irqchip/irq-gic-common.c` | `gic_dist_config()` 无条件清空全部 SPI 使能位；AMP 下会把对端 OS 已使能的中断一并关掉 |
| U-Boot `common/bootm.c` | `bootm_disable_interrupts()` 对 `env_get("devtype")` 没判空，返回 NULL 时空指针解引用 |
| NuttX `sched/task/task_posixspawn.c` | 找不到程序文件时按 ERROR 打日志。NSH 对**每条命令**都先当程序文件试一次，于是每条命令都多一行 `ERROR: exec failed: 2` —— 污染所有"日志无异常"的判据 |
| NuttX `libs/libc/stdio/lib_fopen.c` | 用 `_POSIX_STREAM_MAX`（POSIX 规定的**最低**保证值 16）当流数上限且不可配置。NIST 随机数统计要同时开 32 个日志流，跑到一半失败 |
| NuttX `apps/netutils/tftpc` | WRQ 首个 ACK 传 `blockno = NULL`，被无条件解引用 —— 任何一次握手成功的 TFTP put 都会 panic |
| openvela `packages/ai_agent` | 连接池里服务器已关掉的 TLS 连接，重连前发 `close_notify` 用阻塞 `send()`，对端不再 ACK，调用线程卡 8 分钟以上 |
| NuttX `drivers/usbhost/usbhost_xhci*` | `uint8_t no_erst = 1 << 15` 截成 0，ERSTSZ 写 0、事件环被关掉（DWC3 报 ERST Max=15 时必现）；另有只支持 PCI、不支持 hub、拒绝 64 字节上下文 |
| 瑞芯微 rkaiq | ① tuning socket 失败时 `LOGE` 参数顺序错，直接段错误；② `_isp_module_ens` 跨次起流不清零，第二次起流 3A 统计模块全关，画面偏暗偏绿 |
| 瑞芯微 rknpu | 不开 devfreq 时运行时 PM 回调返回 -EOPNOTSUPP，probe 失败（-95）；也从不设频率，NPU 停在 198 MHz |

### 启动时间

分两轮。第一轮用主机侧时间戳还原时间线（不改板子、不重编），AMP 配置下 **47 s → 9.3 s**：

| 区间 | 原 | 现 | 做法 |
|---|---:|---:|---|
| I2C | 31.8 s | 1.5 s | 起始条件用 100 ms 超时去等一个 10 µs 的事件，拆成 5 ms / 50 ms 两档 |
| GMAC | 7.5 s | 0 | 启动路径上阻塞等链路状态**只为打一行日志**，改成读一次如实报告 |

第二轮为 xTS 的启动时间用例（2026-09-19/20），**4.47 s → 2.76 s**：

| 项 | 省下 | 做法 |
|---|---:|---|
| U-Boot 按键倒计时 | 0.85 s | 倒计时每档 1000 ms 改 200 ms（`amp/uboot/0010`）。进 U-Boot 的退路不变：Rockchip 在倒计时前先查一次 Ctrl-C |
| 触摸 / TF 卡 / 摄像头 / 界面 | 0.85 s | 这些 nsh 一样都不用，挪到后台线程；界面仍等触摸就绪后再起，出现时刻不变 |

10 次 `reboot` 均值 **2.76 s**、10 次上下电均值 **2.84 s**（阈值分别是 6 s 和 4 s）。

结论比数字重要：**没有时间戳就不要谈耗时** —— 在补上时间戳之前，我对启动耗时的判断错了一个数量级。

## xTS 验收

大赛的 xTS 通用自测用例共 35 项。判定分三档，**不把内部功能进度当成严格通过**
（最新总表 [`notes/xts-final-2026-11-02.md`](notes/xts-final-2026-11-02.md)，09-20 的完整对照表 [`notes/xts-final-2026-09-20.md`](notes/xts-final-2026-09-20.md)，每项原始日志在 `notes/xts-rerun-raw/`）：

| 档 | 数量 | 含义 |
|---|---|---|
| A 严格通过 | **34** | 按原文步骤执行，保留原始串口/网络字节与镜像标识，判据未改 |
| B 部分通过 | **0** | |
| C 未达成 | **1** | 1.3.14 时间一致性（要静置 24 h 以上） |

09-20 之后补齐的三项：1.3.5（SD 卡末尾 64 MB 注册成独立块设备跑原文块测试）、
1.3.11（控制台运行时降到 115200 再用 `rb`/`sb` 双向传输）、1.3.15（复位原因改用重启模式寄存器，
断电上电后 -r 0→3 顺序跑完并全程录串口）。1.3.15 原文"打印堆栈"一条做不到（非安全态配不了 FIQ），已在总表注明。

为通过用例做的改动，也都是板子本身的改进：TF 卡按厂商设备树跳过 CMD1（去掉每次开机的驱动报错）、
RTC 用系统定时器实现秒级闹钟（芯片硬件闹钟只到分钟，而用例要求 ±10 ms）、
`/dev/urandom` 指向硬件随机数（否则测的是软件伪随机数）、加开机 logo、
GPIO 输出脚读回改读 SWPORT_DR（EXT_PORT 在本板输出态恒读 0）。
1.3.7 用 MPU-6050 接 40 针口 I2C7 代替原文的 BMI160，新增的板级驱动同时注册
uORB 节点与 `/dev/accel0`，原文测试程序直接 PASSED。

判读时踩过三个坑，值得写下来：串口在 1.5 Mbaud 下主机侧会丢字；
NSH 横幅与另一个核的 syslog **逐字节交错**（`N0u]tt SGhMelAlC 0(:N SH2)`），
连续字符串匹配永远找不到；冷启动计时的起点要取上电后的第一行日志 ——
拔电源瞬间串口线掉电平会产生一个 0x00，拿它当起点会把"人插回电源前的等待"算进启动时间。

## ★ RK3576 的三个坑（都属于"填错不报错、上板无输出"）

| | RK3399 / RK3568 / RK3588 | **RK3576** |
|---|---|---|
| 中断控制器 | GIC-500 / GIC-600，**GICv3** | GIC-400，**GICv2** |
| DRAM 物理基址 | RK3568 从 `0x0` 起 | **`0x40000000`** |
| 外设地址段 | `0xfxxxxxxx` 高位 | **`0x22000000`–`0x2b060000` 低位** |

依据：Linux 主线 `rk3576.dtsi` 与 U-Boot 主线 `include/configs/rk3576_common.h`
（`CFG_SYS_SDRAM_BASE 0x40000000`、`kernel_addr_r=0x42000000`），均为 Rockchip 官方提交。

## 运行方式

```bash
# 1. 拉取工程
repo init -u https://github.com/open-vela/manifests -b dev-ai-contest-2026 \
  -m contest2026_423_nanshannan.xml
repo sync -c -j8

# 2. ★ 把本仓接进构建树（必须，见下方说明）
./contest2026_423_nanshannan/scripts/setup-workspace.sh

# 3. 编译（amp-dual = 完整作品：openvela + Linux 双系统 + 界面 + AI；
#         另有 nsh = 最小控制台基线，amp = 不带 Linux 的单系统）
source contest2026_423_nanshannan/scripts/env.sh
cd nuttx
cp ../contest2026_423_nanshannan/board/kickpi-k7/configs/amp-dual/defconfig .config
make olddefconfig && make olddefconfig      # 需要两遍，见下
make -j$(nproc)                             # 产出 nuttx.bin

# 4. 烧录（全自动，不需要碰板子；打 FIT → 进下载模式 → 写 → 回读校验 → 复位）
cd ../contest2026_423_nanshannan && ./scripts/flash.sh
```

要用 AI 能力，还需在 `app/kickpi_ui/k7_agent_key.h` 填模型 API Key
（模板见同目录 `.example`，该文件已 gitignore）。不填也能编译，界面照常，
只是问答/朗读/唤醒会提示未配置。

**xTS 的几个测试镜像**用叠加配置生成，不改主配置：
`cat board/kickpi-k7/configs/xts-driver-tests.config >> .config && make olddefconfig`
（RTC/随机数用例）、`xts-kasan-longrun.config`（12h 待机 + KASAN）、`xts-logging.config`（内存日志）。

烧 U-Boot 与开机 logo 用 `scripts/flash-uboot.sh` / `scripts/flash-logo.sh`（都带备份、回读与回滚）。

**第 2 步为什么必须。** 大赛 manifest 只为每队映射了三个**模板**目录
（`app/hello_app`、`quickapp/hello_quickapp`、`board/contest_board`），
而本作品的代码在 `board/kickpi-k7/`、`chip/rk3576/`、`app/{kickpi_ui,cam,v4l2cap,
ampctl,mic,hdmi,spi_selftest}` 下。manifest 在组委会仓里改不了，所以这些映射、以及公共仓
的适配补丁，由 `setup-workspace.sh` 建立（幂等，可重复执行）。跳过这一步
的表现是"配置里找不到板子"，而原因离现象很远。

**`olddefconfig` 为什么要跑两遍。** 第一遍解析 `select`/`depends on` 之后
才会显现出新的可见符号，第二遍才把它们的值定下来。只跑一遍会静默丢掉
一部分配置 —— 编译不报错，只是功能不存在。

烧录原理与恢复手段见 [`board/kickpi-k7/README_zh-cn.md`](board/kickpi-k7/README_zh-cn.md)。

## 目录说明

| 路径 | 内容 |
|---|---|
| `board/kickpi-k7/` | ★ **板级适配代码**（结构与最终 PR 目标 `vendor_rockchip/boards/rk3576/kickpi-k7/` 一一对应） |
| `chip/rk3576/` | ★ **芯片层**（原 `arch/arm64/src/rk3576`）。按《新平台适配指南》「不得修改核心代码」搬到树外，由 `CONFIG_ARCH_CHIP_CUSTOM_DIR` 加载 |
| `bsp/nuttx-drivers.patch` | 要提给上游 NuttX 的**通用驱动**：新增 `drivers/timers/hym8563.c`（I2C RTC）+ 修 `drivers/input/ft5x06.c` |
| `bsp/nuttx-history.patch` | 完整提交序列，开发过程记录（含芯片层搬迁前的历史） |
| `bsp/vendor-rockchip-toplevel/` | 获奖后 PR 到 `vendor_rockchip` 所需的顶层 Kconfig / Make.defs / Makefile |
| `docs/rk3576-soc-recon.md` | SoC 硬件参数勘察（GIC / CPU / 定时器 / 12 路 UART 全表） |
| `docs/xts-checklist.md` | xTS 必测项清单 = 开发路线图与验收标准 |
| `docs/smp-amp.md` | SMP 启用过程与 Linux+NuttX AMP 方案 |
| `docs/amp.md` | ★ **AMP 进展与三个「静默失败」的查法** |
| `amp/` | ★ **异构双 OS 的全部产物**：`README.md`（总览）、`FLASH.md`（烧写与启动，含 rkdeveloptool 32MB 限制）、`OWNERSHIP.md`（资源划分）、`uboot/*.patch`（分核引导、lxroot initrd、8 MB 内核窗口等）、`linux/`（A72 的 DTS、裁剪配置、内核补丁、`lxroot/`、`rkaiq/`、`npu/`）、`fit/amp.its`（AMP 的全部契约） |
| `app/kickpi_ui` | ★ **主界面**：今日 / 家居 / 工作 / 助手 / 设置五个页签（见上文"产品形态"）；开机自启 |
| `app/mqttd` | 板上 MQTT broker `k7mqttd`（家居设备直连面板） |
| `app/lxroot`、`app/ir` | lxroot 镜像网络更新；红外接收 bring-up 命令 |
| `scripts/setup-host.md` | ★ 主机环境两个**静默失效**的坑（kconfiglib、LVGL fork 被原版顶替） |
| `docs/ai-agent-setup.md` | ai_agent 后端配置与上板验证 |
| `docs/pcie-nvme.md` | M.2 SSD（PCIe + NVMe）勘察与移植方案 |
| `bsp/upstream/` | ★ **上游侧缺陷修复与适配补丁**（TFTP 空指针、agent 墙钟计时等），各自独立提 PR |
| `app/` | 板级验证程序：`cam`（摄像头/图像/帧率）、`v4l2cap`（V4L2 全链路）、`mic`（录音）、`hdmi`、`spi_selftest`、`bt`、`k7diag`、`ampctl`（AMP 状态、门铃自检、`exec` 到 Linux 侧执行命令） |
| `docs/m1a-gicv2-qemu.md` | M1a：QEMU 上 GICv2 路径验证与驱动分析 |
| `docs/refs/` | 官方文档离线副本 |
| `notes/DEBUG-CASES.md` | 踩坑记录 29 例（现象—排查—根因—修复—验证） |
| `notes/xts-final-2026-11-02.md` | ★ **xTS 35 项最新状态**（分 A/B/C 三档；09-20 版保留），逐项原始日志在 `notes/xts-rerun-raw/` |
| `notes/<功能>-<日期>/` | 各功能上板记录（相机 ISP、NPU 与提频、速记、录制模式等），原始输出在各自的 `run*/` |
| `notes/HANDOFF.md` | 交接说明：现状、铁律、坑；换人/换会话先读这份 |
| `scripts/` | `env.sh`、`flash*.sh`（FIT/U-Boot/内核/logo，均带备份回读回滚）、`gen-boot-logo.py`、`gen-cjk-font.sh`、`xts-*.py`（网络 NSH 取证、NIST 统计、长测）、`collect-logs.sh`（AI 日志白名单导出与脱敏）、`sync-bsp.sh` |
| `.claude/skills/soc-hw-recon` | ★ Skill：无寄存器手册时勘察并**验证**新 SoC 的硬件参数 |
| `.claude/skills/board-clock-bringup` | ★ Skill：板级时钟排查 —— 门控/分频/PCLK/功能时钟分层证伪，识别"整条链自洽地跑错频率" |
| `.claude/skills/bounded-bringup` | ★ Skill：有界 bring-up —— 上界界在整件事上，以及"串口没反应"的四种成因 |
| `logs/` | AI Coding 日志 |
| `archive/` | 前身项目（Amlogic A311Y2）产物，不参与构建 |
| `PLAN.md` / `RECON.md` / `VelaPort-K7-项目描述.md` | 方案、待核实问题、★ 报名正文 |

## AI Coding 使用说明

本作品从第一行代码起就是**人机协作**完成的：我（参赛者）定方向、接板子、做判断，
Claude Code 做勘察、写代码、跑验证并记录。完整对话日志在 [`logs/`](logs/)（3 个会话、约 2.4 万条事件）。

**AI 实际承担的部分**

| 环节 | 怎么用的 | 例子 |
|---|---|---|
| 硬件勘察 | 没有寄存器手册，从 Linux 主线 dts、U-Boot、厂商 SDK 三处交叉比对推参数，并给出**可证伪的验证办法** | 认定 RK3576 是 GICv2 而非 GICv3、DRAM 基址 0x40000000、外设在低位地址段 |
| 写驱动 | 按"先造判据再动手"的方式写 | SD 卡写不进去：先把控制器两个字节计数器打进超时日志，一次读数就把"没进 FIFO / 没排到卡 / 卡没回应"三种可能分开，随后三个缺陷依次被读数指出 |
| 调试 | 现象唯一、原因多样时，先补观测再改代码 | 触摸能上报但 LVGL 收不到（缺一个 ioctl，两边都不报错）；Linux 的 GIC 初始化把 openvela 已使能的中断清掉 |
| 验收 | 按 xTS 原文逐条执行、保留原始字节、失败如实记录 | 串口丢字、日志交错、冷启动计时起点三个判读陷阱都是这样发现的 |
| 沉淀 | 把返工经验写成可复用的 Skill 与案例库 | `.claude/skills/` 三个 Skill；`notes/DEBUG-CASES.md` 29 例 |

**协作中定下的规矩**（写进 `notes/HANDOFF.md`，后续会话必须遵守）

- 密钥绝不进仓、不在命令行明文出现；公开仓导出日志前按白名单裁剪并脱敏。
- 动板子前先确认串口没被别的程序占用 —— 两个程序读同一个串口，表现和死机一模一样，为此误判过好几次。
- 写 eMMC 一律过 `amp/layout.sh` 的区域检查：曾按旧布局写坏 Linux 内核，"双系统全挂、单系统正常"查了一整天。
- 未验证的硬件初始化不放进启动路径，否则卡住就进不了 nsh，只能靠 MASKROM 救。

**AI 带来的实际帮助**：这块板子在 openvela 与上游 NuttX 里都没有任何代码，
从"串口没有输出"到"语音唤醒能对话"约两周。最省时间的不是写代码本身，
而是**在没有手册的情况下快速交叉比对出正确参数**，以及**每次失败都留下可复现的记录**，
使得同一个坑不会踩第二次。

## 提交路径

| 代码 | 比赛期间 | 获奖后 |
|---|---|---|
| 板级 `board/kickpi-k7/` | fork 本仓 → PR → 自行 review 合入 | PR 到 `vendor_rockchip` 的 `dev-ai-contest-2026` |
| 通用驱动 `bsp/nuttx-drivers.patch` | fork `open-vela/nuttx` → PR 到 `dev-ai-contest-2026`，组委会 review | — |
| 芯片层 `chip/rk3576/` | 随本仓提交；获奖后 PR 到 `vendor_rockchip` 的 `chips/rk3576/` | — |
