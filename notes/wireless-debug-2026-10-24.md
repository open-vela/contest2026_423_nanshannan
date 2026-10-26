# CMD5 低速阶段对照与时钟修正（2026-10-22～24）

## 本轮结论

**最新部署状态（10-24 06:28 更新）：时钟修正镜像已写入并回归，CMD5 仍超时。**
08:48Z 首次烧录在备份阶段 USB 掉线未写入；用户按 RESET 后 10:23Z 重刷成功，
旧区域整备份 sha 21d4deed…，新 FIT 回读 sha 245e83cb… 与构建一致。
启动后 `bt probe`：CLKSEL104=0x2f9d（与 Linux 相同，800kHz 源/400kHz 卡），
GPIO1_12..17 上拉均为 3（0x26046114=ffd7、0x26046118=55ff），空闲 CMD/D0 64/64 高；
CMD52 两条超时（Linux 也如此），CMD0 RINTSTS=4，**CMD5 RINTSTS=0x100、RESP0=0**，
probe 返回 -5。时钟二分频已排除为 CMD5 超时原因。
证据：`.driver-workflow/artifacts/sdio-cmd5-20261022/flashed-clock-probe.txt`、`flash-clock-rockusb.txt`。

已抓到同板 Linux 重新枚举 SDIO 时的首个有效 CMD5，返回 `90ffff00`。
这轮核对确认 SDIO 的 CIU 源时钟需要计入 CLKGEN 固定二分频，已修正
probe 和运行时调频计算。此修正不等同于解决 CMD5 超时；openvela 的
枚举仍未通过，固件、HCI 和 WiFi 数据通路仍未完成。

## 采样方法与边界

沿用前轮 RAM Fastboot 下载方式启动未改动的 Linux Image（CRC32
193fee4c），没有刷完整 Linux 镜像。为已构建的 Linux 6.1.75 内核编译
一个临时外部模块，通过 `mmc_request_start/done` tracepoint 观察命令。
模块只匹配父设备 `2a320000.mmc`，只读 MMIO、记录有限数量的请求，
不写控制器或介入返回值。上传到板上 /tmp 后校验 SHA-256 一致。

只解绑/重新绑定 `dwmmc_rockchip` 的 `2a320000.mmc`，没有解绑 eMMC
或 TF 控制器。模块在首次 CMD5 提交给 host ops 前读取寄存器，并记录
请求完成后的响应。这里是**重新枚举的首次 CMD5**，不是开机第一条请求。
重新绑定后 Linux 仍成功进入 SDR104。采样完成后卸载模块、重启返回
openvela；没有把临时模块加入任何系统的启动路径。

诊断模块使用 GPL 导出的 Linux tracepoint 接口，源码和构建产物单独放在
工作区 `.driver-workflow/artifacts/sdio-cmd5-20261022/module/`；它不是
NuttX 驱动实现的一部分。

## 有效低速基线

Linux `clock=400000, width=0, timing=0, signal=0, power=2`：

| 请求 | 参数 | 完成结果 | 完成后 CMD 寄存器 |
|---|---|---|---|
| CMD52 读 ABORT | 00000c00 | -110 / RESP0=0 | 2000c174 |
| CMD52 复位 | 80000c08 | -110 / RESP0=0 | 20004174 |
| CMD0 | 0 | 0 | 20004000 |
| CMD5 | 0 | 0 / RESP0=90ffff00 | 20000045 |
| CMD5 | 01300000 | 0 / RESP0=91ffff00 | 20000045 |
| CMD11 | 0 | 0 / RESP0=00001e00 | 3000014b |
| CMD3 | 0 | 0 / RESP0=00010000 | 20000143 |
| CMD7 | 00010000 | 0 / RESP0=00001e00 | 20000147 |

后续 CMD52 能读取 CCCR。**前两条 CMD52 超时在正常 Linux 中也出现，
不能单独将它们当作坏链路证据。** 真正的分界仍是 CMD5。

首次 CMD5 前的关键寄存器：

| 寄存器 | Linux |
|---|---|
| CRU CLKSEL104 / 0x272004a0 | 2f9d：xin24m / 30，源时钟800kHz |
| CLKDIV / CLKSRC | 0 / 0 |
| CLKENA | 00010001 |
| CTRL / INTMASK | 10 / b7ff |
| PWREN / CTYPE / UHS / RST_N | 1 / 0 / 0 / 1 |
| TMOUT / STATUS | ffffffff / 106 |
| CLKGEN_CON0/1 | 2 / 0：drive90°、sample0° |
| GPIO1_12..17 pull | 全部3，即上拉 |
| GPIO1_12..17 drive | 全部2 |
| GPIO1_C6 / C7 | 1 / 0 |

完整证据：[Linux 首次 CMD5](wireless-debug-2026-10-24/linux-cmd5.txt)。
Linux regulator_summary 另存为 linux-extra.txt，未测实际电压波形。

## 排除实验

每组从已知失败的 bt probe 开始，暂时配置 Linux 的 div29、drive2、
C7低，使用有界网络 NSH 命令；检查 CTRL reset 和 CMD START 自清，
读完成状态和 RESP0。完成后恢复 drive 原值并用 bt probe 恢复基线。

1. CLKENA 低功耗位开/关 × CMD0 使用 STOP/INIT，共四组。
   CMD0 均返回状态4，CMD5 均为104、RESP0=0。
2. GPIO1_C4（模组 UART RX 的主机端）从下拉改上拉，确认数据口由低变高，
   bt probe 仍 -5；之后恢复原下拉。
3. 独立源码复核发现 Linux 在释放 C6 并等待200ms后还会 CTRL 全复位，
   当前 probe 顺序不同。增加这一复位并重新更新 CIU 时钟的单变量实验
   仍得到 CMD5=104/RESP0=0。该因素也没有解释本次故障。
4. 只读补查 IE 输入使能 0x26046184/188=ff，CLKGEN=2/0，MISC=0。
   TRM 39.6.6 确认 init_state 的复位/配置/释放顺序本身有文档依据。

104 是十六进制 RTO+CMD_DONE，不能当作有效命令响应。以上实验没有
测到真实 CLK/CMD 电气波形，不能排除芯片外信号或供电/复位问题。

证据：[矩阵](wireless-debug-2026-10-24/matrix-results.json)、
[主机后复位](wireless-debug-2026-10-24/post-reset-results.json)、
[UART空闲电平](wireless-debug-2026-10-24/uart-idle-pull.txt)、
[输入使能](wireless-debug-2026-10-24/input-enable.txt)。

## 已落地的时钟修正

Linux 源码 `dw_mci_rk3288_set_ios()` 与本次低速读数一致；前轮高速读数
也显示396MHz源对应198MHz卡时钟。现有 SDIO 路径漏乘二，等效把目标
400kHz设成了200kHz，还把源频率误记成实际卡频率。

- probe 的 SDIO CRU 分频59改为29，得到800kHz源/400kHz卡时钟。
- `dw_set_clock_hz()` 仅对 SDIO 将源目标设为请求值乘二，并在记录
  `card_hz` 时除二。25MHz请求得到 GPLL/24/2=24.75MHz。
- 保留现有 TF 时钟策略；未据此声称 TF 频率已经测量校准。
- 清除有关“尚不确定是否二分频”的旧解释。低功耗位和其他未证实参数
  没有随实验改入默认路径。
- 新增测试验证400kHz、24.75MHz实际频率、实例寄存器隔离及关钟。
  真实函数的 PIO/DMA 两种 mock 构建均通过 UBSan；K7固件编译通过。
- 独立复核确认现有调用仅为0、400000、25000000，新增乘二不存在
  当前调用范围内溢出。全文件checkpatch仍有既有问题，未声称全通过。

新构建 SHA-256：

| 文件 | SHA-256 |
|---|---|
| nuttx.bin | d24be69648c59a112d7c3941ce422dc8e7eb4c16f9dd9d7a114d820fba2108d0 |
| FIT | 245e83cbca2a333ee5ed9c06ec5421b520b016b08d3615d6fe3d5db73c83b277 |

本轮辅助脚本和完整日志位于工作区
`.driver-workflow/artifacts/sdio-cmd5-20261022/`，目录日期为开始采样日期。
probe_matrix.py 和 probe_post_reset.py 是有恢复步骤的单次诊断程序；
不应在主机/SDIO已投入业务使用时直接运行。

本轮失败的局部备份位于
`/tmp/k7-amp-fit/flash-20261024T084837Z/lba24576-8192-before.bin`，
**长度不完整，禁止拿它回滚**。前轮完整备份仍保存在
`.driver-workflow/artifacts/sdio-linuxdiag-20261022/fit/`。
本轮镜像和准备校验产物位于本轮 artifacts 的 fit/；其中二进制只留本地。
