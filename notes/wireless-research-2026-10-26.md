# SKW6621S CMD5 无应答：重新调研（2026-10-26）

接 [10-24](wireless-debug-2026-10-24.md)。这一轮只读资料和只读寄存器，没有改代码、没有刷写。

## 新排除的方向

| 方向 | 依据 | 结论 |
|---|---|---|
| IO 电压域（io-domain） | SDK `drivers/soc/rockchip/io-domain.c` 不支持 RK3576；K7 设备树没有 io-domains 节点，而 Linux 照样能枚举 | 排除 |
| 模组供电 | 原理图 V2.1 第 22 页：VCCIO_WL ← R6219(0R) ← VCC_1V8_S3；VBAT ← R6221 ← VCC_3V3_S3。都是常开的 S3 电源，没有 GPIO 开关 | 排除 |
| 32K 低速时钟 | 第 13 页：HYM8563 CLKOUT（开漏）经 10K 上拉到 VCC_1V8_S3 → 32KOUT_RTC2CON → R6230(22R) → 模组 LPO。两个系统下相同 | 排除 |
| SDIO 信号电压 | SDIO_VSEL 经 R6218(10K) 上拉到 VCCIO_WL → 1.8V，与主机一致 | 排除 |
| SDMMC1 M0/M1 路由位 | Linux pinctrl 对 RK3576 没有 mux route 表；SDGMAC_GRF 里只有 GMAC/存储器配置；10-22 下拉实验时控制器报 RESP_ERR，说明它的 CMD 输入就接在 M0 引脚上 | 排除 |
| IO 保持（retention） | TRM PMU0_GRF_IO_RET_CON0/1（0x26024020/24）；板上只读读回均为 0 | 排除 |
| 双系统下 Linux 干扰 | AMP 内核没编 MMC / PWRSEQ / RFKILL，wifi/bt 节点虽在设备树里但没有驱动绑定，pinctrl 不会被应用 | 排除 |
| SKW 驱动在枚举前做了额外动作 | 做对照的 Linux 内核（~/rk3576-kernel/.config）`# CONFIG_SKW6621S is not set`；rfkill-wlan 在 K7 上没有 vbat/reset/poweren/clk_wifi，只把 GPIO1_D5 配成下拉 | 排除：Linux 能枚举只靠 pwrseq-simple + dw_mmc |

附带核对：SKW 驱动的 `MODEM_ENABLE_GPIO=54` = GPIO1_C6（bank1×32+22），与 pwrseq 的复位脚是同一根。

## 原理图要点（V2.1 第 22 页，丝印按 AP6398S 兼容封装画）

- WL_REG_ON（15 脚）= WIFI-EN = GPIO1_C6，无外部上下拉（R6233 DNP）。
- BT_REG_ON（38 脚）经 R6200(15K)/R6201(20K) 分压后接 GPIO1_C7。
- 22/23 脚是 `SDIO_DATA_0/HUSB_DP`、`SDIO_DATA_1/HUSB_DM`，SDIO 与 USB 复用；复位释放时 D0/D1 的电平可能影响模式选择。openvela 的 probe 先配上拉再释放 C6，顺序与 Linux 一致。
- 外部晶振 Y6200 标 NC，模组用内部晶振。

## 已逐项一致的静态配置（10-22～10-24 + 本轮复核）

CRU 时钟（800kHz 源 / 400kHz 卡）、门控、引脚复用（功能 2）、上拉（3）、驱动强度（2）、输入使能、
内部相位（驱动 90°/采样 0°）、CTRL/INTMASK/TMOUT/PWREN/RST_N、C6/C7 电平与上电时序、首条命令带 INIT。

## 结论与下一步

静态配置能对的都对过了，再比寄存器意义不大。**缺的是动态证据**：从没看过 CMD5 期间引脚上实际发生了什么。

下一步做"软件逻辑分析仪"：在 `bt probe` 发 CMD5 的同时，关中断、紧循环读 GPIO1 的引脚电平寄存器
（0x2ae10070，反映焊盘实际电平，与复用无关），记录 CLK(C1) / CMD(C0) / D0(B4) 几千个采样点，事后解码。可以回答：

1. CLK 是否真的出现在焊盘上；
2. CMD 线上的 48 位 CMD5 帧（含 CRC7）是否正确；
3. 模组有没有在 CMD 线上回应答。

三种结果分别指向主机输出通路、主机采样通路、模组初始化，能把问题一次切成两半。只读、采样数有上限、
只在手动 `bt probe` 里跑，不进启动路径。

## 结果：根因找到（同日）

软件逻辑分析仪（原始串口记录在 [wireless-debug-2026-10-26/](wireless-debug-2026-10-26/)）：

- CLK 确实出现在焊盘上，约 400kHz。
- CMD5 的 48 位帧逐位正确，CRC7 = 0101101。
- 模组在 CMD 线上**没有应答** → 问题在模组一侧的初始化，不在主机。

再把释放复位时的焊盘快照和 Linux 正常时逐位比，模组相关的只差两根：

| 引脚 | Linux | openvela（修前） |
|---|---|---|
| GPIO1_C4 = UART4_TX → 模组 UART_RXD | 复用成 UART，空闲为高 | 默认下拉的 GPIO，低 |
| GPIO1_C7 = BT_REG_ON | 低 | 高 |

同一次开机里 A/B（`ab0.txt` / `ab2.txt`）：

- C4 上拉 → CMD5 返回 R4 = `0x90ffff00`，与 Linux 相同。
- `mw 26046118=0f000500` 把 C4 改回下拉 → 超时（RINTSTS 0x104，ret -5）。
- BT_REG_ON 高低四种组合都不影响。

**根因：模组的蓝牙 UART_RXD 在复位释放时为低，它就不应答 SDIO。** 推测模组 ROM 在复位释放时
采样 RXD，低电平当作走 UART 下载。10-24 单独拉高 BT_REG_ON 等试验都没碰到 C4，所以一直没找到。

修法（`rk3576_dwmmc.c`）：释放 WL_REG_ON 之前把 C4/C5 配上拉，BT_REG_ON 保持低（与 Linux 一致）。
逻辑分析仪保留，默认关，`g_dw_la_enable` 置 1 打开。

## 第二个问题：CIS 找错了地方（-19）

CMD5 过了以后，CMD3 / CMD7 / CCCR（版本 0x43）都正常，读 MANFID 返回 -19。代码从**功能 1 的 CIS**
（FBR1 0x109 → 0x2000）里找 MANFID 元组（0x20），走到 0xFF 结束都没找到。按 SDIO 规范 MANFID 在
**公共 CIS**（指针在 CCCR 0x09–0x0B），Linux `sdio_read_common_cis` 也从那里读。改成 0x009 后：

```
SKW6621S: CIS pointer ret=0 0x1000
SKW6621S: CIS 0x01000 tuple 0x20
SKW6621S: CIS MANFID ret=0 1ffe:6621
SKW6621S: identity ret=0 53 56 36 31 36 30 4c 49
SKW6621S: ID 1ffe:6621, chip SV6160LITE, SDIO v2, block 512
```

`bt probe` 返回 0，**SDIO 枚举走通**（`wireless-debug-2026-10-26/serial-bt-probe-ok.txt`；
串口丢字，最后一行实际打的是 block 512）。

## 刷写时 USB 掉线（旁注）

本日两次刷写在 "Reading current AMP FIT region" 时 USB 掉线、板子卡在 loader，只能断电。两次之前
同一次开机里都跑过 `bt probe`；冷启动或看门狗复位后、没碰模组时刷写都成功。是否相关未证实。

## 固件启动成功（同日）

`bt init`（`chip/rk3576/skw6621s/skw6621s_boot.c`）按原厂 `skw_sdio_boot_cp` 的顺序：
F0 0x165 ← 1（原厂不论主机收包方式**总是写 ADMA**）、0x167 ← 1（K7 无唤醒脚，关 CP 睡眠）、
CCCR IEN ← 0x03、0x16 bit1（异步中断）→ DRAM@0x20200000、IRAM@0x00100000（NV CMFG 28 字节合并到
IRAM+0xcc）按 512 字节 CMD53 写 F1 0x0f → 0x160 ← 1 → 轮询 CCCR 0x05 bit1，有挂起就从 F1 0x20 读
0x600+512 字节。

```
SKW6621S: download done in 3538 ms
SKW6621S: rx 1 pac, valid 79, next 0, hdr 0x013f004f
SKW6621S: LOOPCHECK "trunk_W426 20250611-01:36:19, sl…"
SKW6621S: handshake 0 after 46 polls, 251 ms
```

写 0x160 后约 250ms 收到 LOOPCHECK 通道的版本串，CP 固件在跑（`serial-bt-init-fw-boot.txt`）。
没看到 BSPREADY：固件直接报版本，或在开始轮询之前就发了，未区分。版本串被 64 字节缓冲截断。

固件经 HTTP 传到 `/data/skw/`（不进仓库）。注意：WSL 镜像网络下默认 MSS 的 http.server 传不动大文件
（板子 recv 10 秒超时，EAGAIN），服务端 `TCP_MAXSEG=536` 后正常。

天线 NV 用的 SHARE，未确认。

## 下一步

服务启动（F0 0x1b0 bit2 = BT start，等 BTREADY）→ 蓝牙 HCI 通道 → 接 NuttX 蓝牙栈；WiFi 在其后。
