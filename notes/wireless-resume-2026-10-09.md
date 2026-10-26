# 蓝牙 / WiFi 续开发记录 — 2026-10-09

## 接手结论

工作仓库 `contest2026_423_nanshannan`，当前分支 `report-appendix`，接手 HEAD
`d4b9d07`。本轮是在既有 SKW6621S 需求、设计和流程 A/B/C 已确认状态下
继续实现共用 SDIO 基础。用户本次明确把 WiFi 也列为后续开发目标。
原 `chip/rk3576/skw6621s/{requirements,design,tasks}.md` 属于用户未跟踪文件，
本轮保留原样；该目录需求清单的勾选表示需求选择，不能当作实现完成证据。

旧交接中的 `rk3576-bsp` 是历史分支描述；本轮未切换分支或提交代码。

截至 09-20 的已有实板结果：

- AMP 双系统、rpmsg、MIPI 显示/触摸、摄像头、音频、有线网络已工作。
- xTS 最新总表为 A 31 / B 1 / C 3；`HANDOFF.md` 中更早的数字已过时。
- SKW6621S 实焊型号以原厂 Android 的 `1ffe:6621` / `SV6160LITE` 为依据，
  不能按旧 DTS 的 `ap6256` 字符串选择 Broadcom 驱动。
- SDIO 枚举仍未通过：旧记录为 CMD5/CMD52 的 RE 和零响应、DAT0 持续低。
  本次修复的是后续传输问题，不能宣称已解决这个硬件现象。
- `bt init` 仍返回 `-ENOSYS`。固件、HCI、BLE 扫描、WiFi 扫描/联网均未实现。
- 旧交接提到的 GMAC 多余 25MHz 输出，当前源码已取消复用，不能再次当成待修项。

当前环境没有 `/dev/ttyUSB*` 或 `/dev/ttyACM*`；本轮没有进行刷写、复位或
实板测试。保留原有本地修改、配置、日志和密钥文件。

## 本轮代码

### 共用 host：`chip/rk3576/rk3576_dwmmc.c`

1. 运行时调速使用实例硬件表。SDIO 是 CLKSEL_CON104，TF/SDMMC0 是
   CLKSEL_CON105；此前 SDIO 调速也会改 TF 时钟。
2. 固定安装的 SDIO 模组按不可拔设备报告 PRESENT；无效 host 基址拒绝初始化。
3. 保存当前命令，在 CMD53 的 EVENTWAIT 中补齐 CMD_DONE/WAITRESPONSE。
   普通 SD 已完成响应的事务继续使用缓存结果，不重复等待已清除的完成位。
4. PIO 按字节搬运，以局部 32 位字和 memcpy 处理非对齐缓冲区及尾部。
   BYTCNT 保留实际长度，FIFO 最后一次仍访问完整 32 位。
5. FIFO 与 DATA_OVER 共用 PIO 截止时间；错误优先于成功，超时返回
   SDIOWAIT_TIMEOUT。失败清理 FIFO/缓冲区。

PIO 仍是有界忙轮询。不能在 FIFO 未就绪时简单睡一个调度 tick：高速接收下
一个 tick 可能长于 FIFO 可容纳数据的时间。真正减少 CPU 占用需 IRQ/DMA。
命令等待另有自己的上界，不能把 PIO 的 1000ms 称为整个 CMD53 的总上界。

### 公共 SDIO：`nuttx/drivers/mmcsd/sdio.c`

- 按 DMASUPPORTED 能力选择 DMA 或 PIO；DMABEFOREWRITE 仅控制 setup 顺序。
  原助手无条件调用 DMA 宏，而当前板配置没有 SDIO_DMA，返回 ENOSYS 被忽略。
- 检查 CMD52/CMD53 取锁、setup、发命令等返回值；CMD53 提前失败 cancel 并解锁。
- CMD53 R5 与后续 CMD52 响应分开存放，避免原错误状态被覆盖。
- 补查 R5 的 CRC/非法命令标志。
- 补齐 CMD5 READY 轮询、功能/电压校验与取锁失败返回。
- 拒绝越界 function/address、无效 CMD53 长度/块数和递增地址越界，避免
  字段静默截断或生成无限块传输。

已独立归档 `bsp/upstream/sdio-cmd53-pio-errors.patch`，并接入
`scripts/setup-workspace.sh`，使重新建立工作区也能复现修复。
公共助手原有的 CMD53 后续 CMD52 序列保留；其设备侧语义与错误恢复仍需实测。

### SKW：`chip/rk3576/skw6621s/skw6621s_sdio.c`

读取芯片 ID 的 F1 0x0f 直访窗口改为地址递增 CMD53，与原厂
`skw_sdio_dt_read -> sdio_memcpy_fromio` 一致。0x20 包 FIFO 才使用固定地址。
保持手动 `bt probe` 路径，不把未验证的固件初始化加入启动流程。

## 依据

SDK 根目录：
`/home/dministrator/openvela-rk3576/docs/Rk3576-SDK/1-SDK软件源码/Linux/sdk/20260320/rk3576-linux-20260320/rk3576-linux`

- TRM Part1 V1.2：印刷页 92–93 的 CLKSEL_CON104/105；第 39 章，页 1397
  明确 15 字节事务应访问 16 字节 FIFO；页 1399–1401 区分 CMD_DONE/DTO。
- SDK `kernel-6.1/drivers/clk/rockchip/clk-rk3576.c`：SDIO/SDMMC0 时钟字段。
- SDK `kernel-6.1/drivers/mmc/host/dw_mmc.c`：32 位 FIFO 的尾字节处理。
- SDK `external/rkwifibt/drivers/skw6621s/seekwaveplatform_lite/sdio/skw_sdio_main.c`：
  `check_chipid`、`skw_sdio_dt_read`；Linux `drivers/mmc/core/sdio_io.c` 的
  `sdio_memcpy_fromio` 使用递增地址。

SDK 只作为协议和寄存器参考，未复制 GPL 驱动实现。

## 验证

- `tests/sdio/run.py`：实际源码 + 模拟寄存器/时钟，PIO/DMA 两种编译条件
  均通过；覆盖短包、尾字节、R5、失败清理、超时、已完成响应的 SD 路径、
  两 host 的时钟隔离、SKW 地址锁存与 CMD53 参数；开启 UBSan 和编译警告即错误。
- `run.py --baseline`：09-20 的 HEAD 代码在首个 CMD53 事务断言失败，证明
  回归用例能检出原问题。
- 当前 K7 AMP 配置的完整 `make -C nuttx -j8` 编译、链接通过，包含
  CONFIG_RK3576_DWMMC=y / CONFIG_RK3576_SKW6621S=y。
- 公共 SDIO 和 SKW 源文件 checkpatch 通过；DWMMC 全文件存在历史格式问题，
  本次新增/修改行 nxstyle 检查通过，未为了格式清理重写旧驱动。
- 独立寄存器核对及跨文件调用链检查已完成。
- 实板枚举、SD 卡回归、固件/HCI/WiFi 都是待验证。完整 SKW 工作流 D/E
  仍未完成，不能因本次构建通过而把整个驱动标记完成。

## 后续顺序与验收点

1. **恢复 SDIO 枚举证据**：确认串口独占和板上测试状态后，保留镜像/配置
   标识与原始日志，执行 `bt probe`。仍为 RE/DAT0 低时量供电、REG_ON 和
   CLK/CMD/DAT0，不能靠继续堆固件代码解释电气层失败。
2. **标准枚举和 chip ID**：`sdio_probe()` 已补充 CMD5 READY 轮询及 OCR
   功能/电压校验，防止 CMD0 再次复位后过早发 CMD3。模拟就绪/超时/错误
   场景通过，实板判据为有效 OCR、CCCR、1ffe:6621、SV6160LITE 和重复 CMD53。
   同步验证 TF 卡。
3. **共用 SKW transport / 固件**：增加实例锁，覆盖 address-latch + CMD53
   整个事务，逐段有界加载 IRAM/DRAM/boot NV，建立服务仲裁和错误恢复。
4. **蓝牙**：模块上电与服务 ACK → HCI Reset/Local Version → NVDS →
   BLE 被动扫描。验收保留原要求的 100 次 Reset、20 次开关、30 分钟扫描。
5. **WiFi**：复用固件/通道/流控，引入 SKW 命令及事件分发，接 NuttX netdev
   和无线 ioctl。参考树内 `bcm43xxx/bcmf_netdev.c` 的 SIOCSIWSCAN /
   SIOCGIWSCAN 等接口，不能直接搬 Linux cfg80211。先验收扫描，再实现
   关联/认证、密钥、数据收发，最后 DHCP/ping 和蓝牙共存。

## 其他外设候选

| 优先级 | 目标 | 当前事实 / 首个验收点 |
|---|---|---|
| 次于无线 | HDMI | 已有芯片驱动与应用；接显示器后先验证 HPD/EDID、PHY 锁定和出图 |
| 次于无线 | AMP 下 eMMC 恢复 | 单系统驱动已存在，双系统配置未启用；先明确 Linux DTB/中断归属，再只读探测和读盘，禁止覆盖启动区域 |
| 后续新驱动 | USB Host | RK3576 芯片层尚无 Host glue；核对 DWC3/xHCI、PHY/供电与现有 USB 框架后，以枚举设备为首个目标 |
| 较大独立任务 | PCIe + NVMe | 已有勘察，缺 RK3576 PCIe host 和 NVMe 驱动；需实际 SSD，先 LTSSM 链路，再枚举、只读 Identify/读块 |

本轮不同时打开多个未经验证的启动路径；无线共用地基优先。

## 本地构建标识

- `nuttx/nuttx.bin` SHA-256: `96910acb489778835149ce241125c45e38b26c1ead7f9ded4d331e5babd7747f`
- `nuttx/.config` SHA-256: `57108eeb9bf4643601ff6c8831caf899d2ab085154400a41dcd6a108b51ed1d9`

镜像只留本地，未刷入板子；当前配置可能含本地应用密钥，不能公开上传镜像。

## 后续一轮：固件格式和旧假设纠正

新增 `scripts/skw-firmware-inspect.py`，真实 SDK 的 ALONE/SHARE 均通过
结构检查，7 条模块记录、28 字节 CMFG，输出两份仅含元数据/哈希的 JSON。
9 个合成测试覆盖截断、未知模块、地址/文件越界、NV溢出、重复标记、
PNFG记录不完整/超过20条，以及CRC16已知向量。

[固件协议核对](../docs/skw6621s-boot-protocol.md) 记录几个关键纠正：
BT模块编号=3（不是2）；0x180用于模块RAM上电，不是首次引导完成；
首次成功依赖RX的BSPREADY→RDVERSION→版本消息；0x1b0 bit4是CP assert。
固件检查器不下载、不选天线配置、不把结构检查当成固件已启动。
