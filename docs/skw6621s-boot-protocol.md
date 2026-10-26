# SKW6621S 固件引导协议核对（2026-10-09）

本记录依据用户提供的 20260320 Rockchip SDK 及其中真实二进制，补充并纠正
09-15 的 SKW 设计假设。原 requirements/design/tasks 未跟踪文件保留原样。
这里只确认协议和文件结构，尚未证明 NuttX 下固件能在板上运行。

## 必须纠正的区别

| 项 | 原厂代码可确认的行为 |
|---|---|
| 模块编号 | `skw_boot.h:enum skw_subsys`：BSP=1、WiFi=2、BT=3。F0 0x164 的 BT 选择应为 **3**；旧头文件注释/旧 requirements 的 2 不可信 |
| 服务编号 | WiFi service=0、BT service=1；service map 是 WiFi bit0、BT bit1。这与模块编号不同 |
| 首次固件引导 | 完整 DRAM → 合并 CMFG 后的完整 IRAM → F0 0x160=1 → 等 RX 握手 |
| 首次引导成功 | 接收 LOOPCHECK 通道的 BSPREADY，发送 RDVERSION，再收到 trunk_W 开头的版本消息；不能仅凭下载返回成功判定 |
| 服务启动成功 | RX 收到 WIFIREADY / BTREADY，驱动再更新实际服务状态 |
| F0 0x180 | 按需模块 RAM 上电通知：读旧值 → 0x164 选模块 → 0x1b0 bit6 → 等值改变；不是首次整包启动 ACK |
| 0x1b0 bit4 | 强制 CP assert；不能作为固件启动信号 |
| 0x1b0 bit0/1 | WiFi start/stop |
| 0x1b0 bit2/3 | BT start/stop |
| 主机 PIO / CP DMA 模式 | 是不同层的概念。F0 0x165 要依据 CP 协议配置，不能因为 NuttX 用 PIO 就猜它应写哪个值 |

`SKW_BOOT_MEMPOWERON` 是另一条可选生命周期。本次 platform 源码中未找到
其定义，不能混合首次整包引导和按模块加载两条分支。蓝牙与 WiFi 共用 CP、
Function 1、地址锁存和 packet FIFO；已有一个服务运行时不能为另一个服务
重新复位整个芯片。未来驱动需显式状态机、共享锁和经过 ACK 的服务位图。

## 文件格式与检查器

SDK 根目录：
`/home/dministrator/openvela-rk3576/docs/Rk3576-SDK/1-SDK软件源码/Linux/sdk/20260320/rk3576-linux-20260320/rk3576-linux`

原厂驱动相对路径：
`external/rkwifibt/drivers/skw6621s/seekwaveplatform_lite/`。
协议依据包括 `skwutil/skw_boot.c:skw_boot_init`、`skw_boot.h`，
`sdio/skw_sdio_main.c:skw_sdio_boot_cp/skw_sdio_dt_write` 和
`sdio/skw_sdio_rx.c:skw_pin_config/skw_sdio_handle_packet2`。

固件目录：`external/rkwifibt/firmware/seekwave/ea6x21qx/`。

| 文件 | 大小 | 首次下载目的地址 |
|---|---:|---:|
| SWT6621S_DRAM_SDIO.bin | 193224 | 0x20200000 |
| SWT6621S_IRAM_SDIO.bin | 356616 | 0x00100000 |
| SWT6621S_NV_SDIO_ALONE.bin | 220 | CMFG 合并进 IRAM，非独立下载区 |
| SWT6621S_NV_SDIO_SHARE.bin | 220 | 同上，天线选择必须按板级资料/实测确认 |

IRAM 前 512 字节中，4 字节对齐的 `kees0616` 开始于 H，`evaw0616`
开始于 T。H+8/H+12 是 IRAM/DRAM 基址，均 LE32；[H+16,T) 是 12 字节
模块记录：source LE32、type高8/size低24 LE32、destination LE32。
本地文件 H=0xec，T=0x150，共 BSP 2 条、WiFi 3 条、BT 2 条。

NV 容器 +8/+12 是 CMFG 偏移/长度，+16/+20 是 PNFG 偏移/长度。
本地两份 NV 均 CMFG=32/28，PNFG=0/0。IRAM 的 TSVN=0xc8，DEVN=0xe8，
CMFG 复制到 [TSVN+4,DEVN) 的 28 字节槽。PNFG 若存在，必须为完整的
8 字节引脚记录、最多20条，并在 CP ready 后执行额外引脚配置。
`NV_for_test_*.bin` 原始配置或控制器 NVDS 文件不能当作这个 NV 容器。

检查器用法（显式选择待检查的 NV 文件）：

```sh
python3 scripts/skw-firmware-inspect.py \
  --iram /path/to/SWT6621S_IRAM_SDIO.bin \
  --dram /path/to/SWT6621S_DRAM_SDIO.bin \
  --nv /path/to/SWT6621S_NV_SDIO_ALONE.bin
```

它限制文件大小、核验标记/记录/地址范围、拒绝截断和溢出、只在内存副本中
合并 CMFG，并向 stdout 输出 JSON。没有硬件写入、输入文件修改或二进制导出。
ALONE/SHARE 均已实跑，结果见 `notes/skw-firmware-*-2026-10-09.json`；
两个结果都标注天线选择尚未验证。

SHA-256 用于复现，CRC16 为初值0/poly0x1021（XMODEM）。未发现此引导
路径用外部预期 CRC 验证镜像，因此计算 hash/CRC 不等于验证固件真实性。

## 下载事务约束

每次完整 direct 操作先通过 F0 0x15c..0x15f 写目标32位地址，低字节在前；
随后 F1 0x0f 使用 incrementing CMD53，每条从 0x0f 起、通常最多512字节。
原厂在整个操作开始时锁存一次，依赖内部地址推进；这仍需 NuttX 实板验证。
F1 0x20 是另一个 fixed-address packet FIFO，不得混用。

初次完整下载需要 DRAM 378 条、IRAM 697 条 CMD53（按512字节分段）。
需统一的总线锁覆盖锁存和全部分段，任何分段失败立即停止，并保留首个错误。
最后的 0x160=1 只能在两个镜像都写完后发送。

先建立 RX/IRQ 与有界 ready 握手，再提供能报告成功的 `bt boot/init`。
原厂等待 ready 的2秒只能作为初始参考；其 RAM上电代码存在时间单位/错误返回
疑点，不能把那个轮询常量照搬为硬件时限。
