# Linux 对照与 SDIO 引脚上拉修复（2026-10-22）

## 当前板卡状态

板卡已回到 openvela AMP，修正镜像已写入并回读一致。网络 NSH
192.168.1.50:2323、串口 /dev/ttyUSB0@1500000 可用，串口已释放。
`bt probe` 仍返回 -5：CMD52/CMD5 为响应超时，枚举、固件、HCI、BLE、
WiFi 联网均未通过验收，`bt init` 仍未实现。

## 正常 Linux 基线

在同一个恢复后的 AMP U-Boot 下，用 Fastboot **仅下载到 RAM** 的方式
启动本机 `/home/dministrator/rk3576-kernel/arch/arm64/boot/Image`。
没有向 eMMC 写入完整 Linux。Image 的 CRC32 为 193fee4c，下载后和
搬运后分别核对一致。使用原有完整 Linux DTB/initramfs，临时参数
`console=ttyFIQ0 rdinit=/bin/sh`，避开根文件系统 NFS 等待。

Linux 6.1.75 #4 Sep30 再次枚举 `mmc2:0001:1`，ID `1ffe:6621`，
SDR104、4 bit、实际 198MHz。这说明同板模组能够正常应答，不能将
openvela 的失败直接归咎于硬件。采样时 SDIO runtime PM 设为 on。

| 寄存器/状态 | Linux 正常态 | openvela 修正前 |
|---|---|---|
| GPIO1_B pull 0x26046114 | 0000ffd7 | 000055d7 |
| GPIO1_C pull 0x26046118 | 00007f0f | 000055f5 |
| SDIO GPIO1_12..17 pull 两位编码 | 全部 3 | 全部 1 |
| GPIO1 数据口 0x2ae10070 | 017df9ff | 00ed09ff（另一次采样00ed0bff） |
| drive 0x2604602c/630 | 2222/6622 | 6666/6666 |
| CRU 0x272004a0 | 2f02（高速态） | 2fbb（初始化） |
| SDIO CLKENA | 00010001 | 00000001 |
| SDIO RST_N 0x2a320078 | 1 | 1 |
| RTC 0x51 reg0x0d | b0 | b0（前轮） |

Linux 上述快照是枚举后的高速态，不能直接视为 CMD5 时的配置。
特别是 CLKENA 的低功耗位在该快照中为 1，旧注释“Linux 从不置位”不准确。
GPIO1_D0 在 Linux 是扩展口 SPI CS，不能仅凭数据口不同推断它是 WiFi 电源。

证据：[Linux 快照](wireless-debug-2026-10-22/linux-snapshot.txt)。

## 确认的代码错误和修复

本机 Linux `drivers/pinctrl/pinctrl-rockchip.c`：

- `RK3576_PIN_BANK` 对所有 bank 选择 `PULL_TYPE_IO_1`。
- `rockchip_pull_list[1]` 为 0/2=关闭、1=下拉、3=上拉，没有 bus hold。
- K7 使用的 `sdmmc1m0_bus4/clk/cmd` 六脚都指定上拉，包括 CLK。

此前 `rk3576_pinmux.h` 错用了其他 Rockchip 型号的编码，把 1 定义成上拉。
现已将 UP 改为 3、DOWN 改为 1、删除无调用者的 HOLD；setpull 只接受
NONE/UP/DOWN，getpull 明确返回硬件原始编码。SDIO probe 显式给六脚配置
上拉，避免继承引导器留下的下拉。TF 控制器路径没有新增配置。

现有 SPI、触摸 IRQ、GPIO 输入调用都使用宏，修正后也得到真实上拉；
GMAC 使用 NONE，不受影响。独立跨文件审查未发现新增阻塞缺陷。

## 实板对照结果

首先只用 mw 改 SDIO 六脚的 pull 字段，并核对写回：

| 条件 | DAT0 高电平采样 | CMD5 |
|---|---|---|
| 原始下拉 | 0/64 | 初始 STS=2，settled=6，RESP0=0 |
| 仅改正确上拉 | 64/64 | 初始 STS=100，settled=104，RESP0=0 |
| 上拉 + Linux drive=2 | 64/64 | 仍超时 |
| 上述配置 + CRU div=29 | 高 | 原始 CMD5 STS=104，RESP0=0 |
| 再保持 BT 复位、仅释放 WiFi | 高 | 原始 CMD5 STS=104，RESP0=0 |

STS 均为十六进制。0x104 是 RTO+CMD_DONE，不是有效响应。
这次上拉修复解决了错误 idle 电平，但不能独立解决枚举失败。
后续组合实验没有引入驱动默认参数；重启清除了临时 drive/clock/reset 改动。

日志：
- [上拉单变量](wireless-debug-2026-10-22/pull-baseline.txt)
- [驱动强度和时钟](wireless-debug-2026-10-22/pull-drive-clock.txt)
- [复位组合](wireless-debug-2026-10-22/pull-reset-clock.txt)
- [新镜像重启后 probe](wireless-debug-2026-10-22/flashed-probe-uart.txt)

## 构建、烧录和回归

- K7 配置 `make -C nuttx -j8` 成功，相关引脚调用者和板级文件均重新编译。
- `tests/pinmux/run.py` 编译真实 pinmux.c，使用独立寄存器向量验证五个
  bank、分段边界、写掩码、上/下拉、关闭和非法参数，UBSan 通过。
- `tests/sdio/run.py` 原有 PIO/DMA 两种配置均通过。
- 全文件 checkpatch 仍报告既有路径标头和中文长行，不能宣称全文件风格
  检查通过；本次新增 C 代码未引入这些长行。git diff --check 通过。
- 使用现有 `scripts/flash.sh --fit ... --stay`，布局校验、旧 FIT 备份、
  写入、回读 cmp 均通过，仅更新 LBA24576..31592。复位后进入 NSH。
- 新镜像 `bt probe` 自动读回六脚 pull=3，DAT0=64/64；没有先手工 mw。
- AMP 握手完成，`ampctl exec uname -a` 返回 A72 Linux #17 Sep19。
- 从 TF `/dev/mmcsd1` 同一位置读两次到 /tmp，每次32768字节，cmp=0；
  没有写 TF 数据区。这是有限只读回归，未代表全部外设验收。

镜像哈希：

| 文件 | SHA-256 |
|---|---|
| nuttx.bin | a5956630e8bfa3fc7eef17c1f0d49c161535f8d93644efd7217233a201851604 |
| FIT / 回读 FIT | e34dbdf643192be242873d4fec06e5a2b2c3faada28e7c2214cf7651a9298034 |

最终回读：[寄存器、AMP、TF](wireless-debug-2026-10-22/final-net.txt)。
源码中仍有旧调试假设注释，判断进展应以本记录和实测为准。

## 接续入口

当前瓶颈已从错误下拉导致的响应格式错误收敛为真正的 CMD5 无应答。
下一步采集 Linux **首次 CMD5 时**的控制器/时钟/供电初始化状态，
与修正后的 openvela 比较；不要把 SDR104 的高速状态直接套到初始化。
仍需核对时钟实际输出、输入选择和电源/复位时序，不能宣布硬件故障。

持久本地辅助脚本、原始日志、构建日志、镜像及备份位于工作区
`.driver-workflow/artifacts/sdio-linuxdiag-20261022/`。`serial_cmd.py` 提供
互斥串口访问，`net_cmd.py` 提供逐条等待提示符的网络 NSH。
`fastboot-stage.c` 只发送 download/continue，不发送 flash/erase。
RAM Linux 操作流程留在 ram-verify.txt 和 boot_linux.py。
镜像含本地应用配置，仅本地保存，不加入公开仓库。

旧记录中的部分 /tmp 文件已在宿主机重启时丢失。本轮 FIT 备份已另存到
上述持久目录的 fit/lba24576-8192-before.bin，避免依赖 /tmp。
