# 上游侧改动

这些改动**不在** nuttx 核心仓，也不在本仓的板级/芯片层，而是散落在
openvela 工程的其它仓里。按《新平台适配指南》「不得修改核心代码」的精神，
它们不该跟板级适配混在一起，各自独立提 PR。

归档在这里是为了让别人能复现我们的构建。

| 补丁 | 仓 | 内容 |
|---|---|---|
| `sdio-cmd53-pio-errors.patch` | `nuttx` | CMD5 等待 READY 并校验 OCR；CMD52/CMD53 拒绝参数截断；CMD53 根据能力选择 PIO/DMA；检查锁、setup、发送失败并清理；保留原始 R5，避免后续 CMD52 覆盖错误；CMD52/CMD53 检查 CRC 和非法命令标志。配套主机回归见 `tests/sdio/`，SKW 实板枚举仍待验证。 |
| `gicv2-shared-dist.patch` | `nuttx` | 新增 `CONFIG_ARM64_GICV2_SHARED_DIST`：AMP 下 GIC distributor 归 Linux，NuttX 不复位 SPI、不写 GICD_CTLR；ICDICFR（4 个 IRQ 挤一个字节）只校验不写；ITARGETSR / IPRIORITYR 按字节写，把自己用的 SPI 的目标核改成本核（Linux 会把所有 SPI 都指向 A72，不改的话中断型驱动无声挂住）。**amp-dual defconfig 依赖它**：补丁缺失时 olddefconfig 会静默丢掉这一项，NuttX 复位共享的 distributor，Linux 侧中断全乱。 |
| `es8388-capture-reset.patch` | `nuttx` | ① `AUDIOIOC_CONFIGURE` 先保存采样率/声道/位宽、后调 `es8388_reset()`，reset 把它们恢复成编译期默认值 —— 请求的格式被静默替换。改为 reset 之后再保存。② 录音全零：reset 没写 CHIPLOPOW1/2（热重启继承上一个系统留下的低功耗位），CONTROL1 关着 ENREF、CONTROL2 用低功耗 VREF 缓冲。按厂商 Linux 驱动录音时的寄存器值（CONTROL1 0x36、CONTROL2 0x60、ADCCONTROL4 0x4c、ADCCONTROL7 0x30）改正。 |
| `bcm4343x-speed-kconfig.patch` | `nuttx` | 工作波特率写死 2000000；24MHz 时钟的 16550 只能 16·n 分频，2M 实际落在 1.5M，控制器被告知切到主机达不到的速率，链路变成噪声。加 `CONFIG_BLUETOOTH_BCM4343X_SPEED`（默认仍 2000000）；顺带 `%x` 配 `ptrdiff_t` 改 `%tx`。 |
| `ft5x06-v2-touchup-lvgl.patch` | `nuttx` | 叠在 `../nuttx-drivers.patch` 之上。① 支持 FocalTech v2 报点格式（`buf[1]` 高 4 位是协议号，原代码按裸值判 tdstatus 必判无效）。② 抬手时芯片只报"0 个点"，原代码当没数据丢掉，LVGL 永远停在 PRESSED —— 改为补一帧 TOUCH_UP。③ read 返回真实长度、支持 `TSIOC_GETMAXPOINTS`（按 `uint8_t` 写），否则 LVGL 的 NuttX 触摸驱动绑不上。④ 诊断计数器 `g_ft5x06_*`（`app/k7diag` 引用）—— 这一项不提上游。 |
| `hym8563-kconfig-rtc-external.patch` | `nuttx` | 叠在 `../nuttx-drivers.patch` 之上。`RTC_HYM8563` 原先放在 `if RTC_DSXXXX` 块里，不先选 DS 系列就看不到；移出来，改为 `depends on RTC_EXTERNAL`、`select I2C` / `RTC_DATETIME`。 |
| `ai_agent.patch` | `packages/ai_agent` | ① `feishu_http.c` 用了 `cJSON_int` —— 这个类型在上游 cJSON 里不存在，全仓只出现这一处、无任何定义，是笔误；顺带 `valueint` 是 `int` 却配 `%lld`，一并改正。② Makefile 加 `-Wno-format-truncation`：`agent_loop.c` 拼路径的 `snprintf` 会被判可能截断，该检查随优化级别变化，参考平台（goldfish，`-O3`）不触发。③ Makefile 补编 `tool_camera.c`（`CONFIG_AI_AGENT_CAMERA=y` 时）：CMakeLists.txt 有这一段、Makefile 漏了，开相机工具后链接报 `undefined reference to tool_camera_capture_execute`。④ `agent_config.h`：`AGENT_LLM_TIMEOUT_SEC` 60→180、socket 超时 120→240 —— Token Plan 的 mimo-v2.5 是推理模型，带图后第二轮实测 86~99s，60s 看门狗会把已经返回的回答当超时丢掉。 |
| `agent-monotonic-latency.patch` | `packages/ai_agent` | ① LLM 耗时改用单调时钟（TLS 握手前改墙钟会让耗时算成几十天，已返回的回答被当超时）。② `send_working_status()` 对 `local_client` 渠道不发"处理中"：velaclaw SDK（`velaclaw_client_local.c`）的异步回调只触发一次，这条过渡消息会把它用掉，真正的回答到达时已无人接收 —— kickpi_ui 的桌面助手表现为永远停在"让我查一下..."。 |
| `agent-skills-mkdir-fat.patch` | `packages/ai_agent` | `skill_loader_init()` 用 `mkdir(AGENT_SKILLS_DIR)` 建目录，而这个宏带末尾 `/`（后面要直接拼文件名）。NuttX 的 FAT 对带末尾斜杠的 `mkdir` 返回 ENOTDIR，tmpfs 却接受 —— `/data` 是 tmpfs 时从没暴露；2026-10-25 `/data` 改挂 eMMC（vfat）后，每次开机 10 条 `Cannot write skill`，内置技能一个都装不上。改成不带斜杠的路径建目录。 |
| `agent-tls-stale-close-timeout.patch` | `packages/ai_agent` | 连接池里的 TLS 连接空闲两分钟左右会被服务器关掉；下一次请求走"stale, reconnecting"，先 `tls_ctx_free()` 发 `close_notify`。没开 `CONFIG_NET_TCP_WRITE_BUFFERS` 时阻塞 `send()` 要等对端 ACK，已关掉的连接不会再 ACK，调用线程卡到 TCP 重传放弃（实测 8 分钟以上没回来，速记打标签线程因此停摆）。① 发 `close_notify` 前把 `SO_SNDTIMEO` 设成 1 秒（告警本就是尽力而为）；② 建连后总设发送超时（原来只在 `CONFIG_AI_AGENT_NET_RPMSG` 下设），往死连接写请求也会失败返回而不是阻塞。修后同样"空闲 3 分钟再请求"，重连 1.2 秒拿到应答，见 `notes/feature-memo-2026-11-02/run3/`。 |
| `frameworks-uv.patch` | `frameworks/system/utils/uv` | `uv_aes.c` / `uv_hkdf.c` / `uv_ecdh.c` 照 mbedtls 2.x 写的，直接访问 `mbedtls_cipher_context_t` 的 `cipher_info` / `add_padding` / `get_padding`。树里是 mbedtls 3.4.0，这些成员已被 `MBEDTLS_PRIVATE()` 包起来。加 `-DMBEDTLS_ALLOW_PRIVATE_ACCESS`。 |
| `0002-nuttx-include-tls-task-header.patch` | `apps/system/libuv` | libuv 的 nuttx 移植只 include 了 `nuttx/tls.h`，但用的是 `task_tls_*`，那些声明在 `nuttx/tls_task.h`。**注意这是给下载源码打的补丁**：libuv 源码是构建时从 GitHub 拉的，直接改工作副本会在干净构建时丢失，所以必须以 `000*.patch` 的形式放进 `apps/system/libuv/`（Makefile 会 `sort $(wildcard 000*.patch)` 全部应用）。 |
| `apps.patch` | `apps` | `drivertest_spidev_master.c` 一处格式符 `%d` -> `%zu`。 |
| `ft5x06-tsioc-getmaxpoints.patch` | `nuttx` | **触摸能上报、LVGL 却绑不上**：`lv_nuttx_touchscreen_create()` 打开设备后第一件事是 `ioctl(fd, TSIOC_GETMAXPOINTS, &maxpoint)`，失败就 close 并返回 NULL，于是 `lv_nuttx_init()` 的 `result->indev` 是空的 —— 界面出得来、触摸完全没反应，而且 `LV_USE_LOG` 默认关着，一行日志都没有。ft5x06 本来就按多点上报（`ft5x06_multitouch` 里有 `npoints`），只是 ioctl 里没有这一条，补上 `FT5X06_MAX_TOUCHES`。 |
| `agent-mimo-no-thinking.patch` | `packages/ai_agent` | 对小米 MiMo（地址含 xiaomimimo）的文本与视觉请求加 `"thinking": {"type": "disabled"}`。mimo-v2.5 默认开深度思考：主机实测纯文本 8.6s → 2.1s、看一张图 20s → 4.3s；板上一次"拍照 → 看图 → 作答"三轮 196s → 23s。可用 `AGENT_LLM_DISABLE_THINKING=0` 关闭此行为。 |
| `telnetd-survive-bad-connection.patch` | `apps` | **一次端口探测就让 telnet 永久下线**：客户端连上立刻断开（端口扫描、`bash /dev/tcp` 探活），`accept()` 返回 `-ENOTCONN`，`netutils/telnetd/telnetd_daemon.c` 把 EINTR 以外的任何 accept 错误都当致命，关监听 socket 并退出守护进程 —— 此后 2323 一律 Connection refused，只能串口重启 telnetd 或复位。改为只有监听 socket 本身坏了（EBADF/EINVAL/ENOTSOCK/EOPNOTSUPP）才退出，单个连接建会话失败只丢这个连接。上板：修复前 1 次裸连接即挂；修复后 10 次 FIN + 10 次 RST 断开后正常会话仍可用。已知残留：连上后**立即** RST、连发十几次时，个别 `Telnet_session` 会卡在信号量上不退出，占住 8 个预分配 TCP 连接之一。 |
| `tftpc-null-blockno.patch` | `apps` | **空指针解引用**：`netutils/tftpc/tftpc_put.c` 等 WRQ 首个 ACK 时传 `blockno = NULL`（那个块号必然为 0，调用方不关心），但 `tftp_rcvack()` 无条件写 `*blockno = rblockno`。**任何一次握手成功的 TFTP put 都会 panic**。单独成补丁而不并进 `apps.patch`，因为这是个可独立提 PR 的真实缺陷，跟那边的格式符清理不是一回事。 |
| `posixspawn-enoent-not-error.patch` | `nuttx` | **每条 NSH 命令都带一行 `nxposix_spawn_exec: ERROR: exec failed: 2`**：开了 `LIBC_EXECFUNCS` + `NSH_BUILTIN_APPS` 时，NSH 对每条命令先用 `nsh_fileapp()`（`posix_spawnp`）当程序文件试，`free`/`echo` 这类 NSH 自带命令必然 ENOENT，然后才回退。找不到是正常的查找结果，调用方有返回码；ENOENT 降为 `sinfo`，其它错误照旧 `serr`。影响 xTS 各项"日志无异常"的判读。`LIBC_EXECFUNCS` 不能关：`posix_spawn` 本身只在它打开时编译，界面拍照（v4l2cap）和启动 ai_agent 都靠它。 |
| `stdio-stream-limit-open-max.patch` | `nuttx` | **`fopen` 最多 16 个流**：`libs/libc/stdio/lib_fopen.c` 拿 `_POSIX_STREAM_MAX`（POSIX 规定的**最低**保证值 16）当上限，写死、不可配置。xTS 1.3.16 的 `nist_sts` 跑全部 15 项要同时开 32 个日志流，打开第 6 项 Rank 的 results.txt 时 `EMFILE`，报 "LOG FILES COULD NOT BE OPENED. MAX # OF OPENED FILES HAS BEEN REACHED = 11"。改用 `OPEN_MAX`（`CONFIG_LIBC_OPEN_MAX`，本配置 256）：FILE 结构按需分配，流又必然占一个 fd，上限本就受 OPEN_MAX 约束。 |
| `bmi160-spi-only-guard.patch` | `nuttx` | **只开 I²C 时 bmi160_base.c 编不过**：`bmi160_configspi()` 在 `#ifdef CONFIG_SENSORS_BMI160_SPI` 内，而调用它的 `bmi160_transferspi()` 没有同样的条件编译，得到 implicit declaration 与 unused function 两个 `-Werror`。xTS 1.3.7 的用例程序 `cmocka_driver_i2c_spi` 的编译开关写死为 `CONFIG_SENSORS_BMI160` 非空，只开 I²C 就会撞上。给 `bmi160_transferspi()` 补上同样的条件编译。 |
| `xhci-platform-hub.patch` | `nuttx` | **xHCI 只能挂 PCI、不支持 hub**，而 K7 的三个 Type-A 口全在板载 GL3510 hub 下面、控制器是总线直连的 DWC3。① 新增 `CONFIG_USBHOST_XHCI_PLATFORM` 与 `usbhost_xhci_platform_init(base, irq)`：PCI 相关代码进 `#ifdef CONFIG_USBHOST_XHCI_PCI`，探测流程抽成 `xhci_probe_common()`，64 位寄存器按低/高两次 32 位写。② 新增 `CONFIG_USBHOST_XHCI_CTX64`：DWC3 报 HCCPARAMS1.CSZ=1（64 字节上下文），原驱动直接拒绝；设备上下文按自身大小对齐，保证不跨页。③ hub 支持：去掉 `#error`，设备不再按根端口一一对应，改为按 slot 管理（`xhci_dev_get()` 从端点取设备）；slot 上下文补 route string、速度、TT hub slot/port；读到 hub 描述符后用只带 A0 的 Configure Endpoint 置 Hub 位、端口数、TTT；`xhci_connect()` 用 4 项队列把 hub 端口事件交给 waiter；hub 子设备的 EP0 在 slot 之前分配，释放时连同 slot 一起释放。④ 顺带修：Context Entries 只增不减（否则后配的低号端点会把前面的端点挤出上下文）；传输事件找不到端点时忽略而不是断言；`SET_ADDRESS` 返回真实结果；`no_erst`（uint8_t）先钳位再赋值 —— DWC3 报 ERST Max=15，`1 << 15` 截成 0，ERSTSZ 写 0，事件环被关掉（上板查出）。**上板：控制器能起、端口能检测到 hub，但 xHC 的 DMA 不通（事件环无事件），未枚举成功**。 |

## 另外两个必须记住的配置坑

**cJSON 版本**：`CONFIG_NETUTILS_CJSON_VERSION` 默认 1.7.12，缺
`cJSON_GetNumberValue` / `cJSON_SetValuestring`，ai_agent 编不过。已改 1.7.18。

**`CONFIG_ARCH_CHIP_CUSTOM` 必须原样写进 defconfig**，光靠
`ARCH_CHIP_ARM64_CUSTOM` 去 `select` 它不行 —— `tools/Config.mk` 是 make
在 kconfig 解析 select 之前读的。详见根目录 README。

## ft5x06-scan-all-slots-for-touch-up.patch

`drivers/input/ft5x06.c` —— 触摸只有按下、没有松开。

解码器按 `TD_STATUS`（触点数）决定扫几个槽位。手指抬起那一帧控制器报
`TD_STATUS = 0`，但**槽位 0 里仍留着一条带 UP 事件的记录** —— 整帧被当成
"没数据"丢掉，上层永远收不到 `TOUCH_UP`。

后果：LVGL 的 `process_single_touch()` 只有拿到 `TOUCH_UP` 才会把指针置成
`RELEASED`；收不到就一直停在 `PRESSED`，没有 click，点什么都没反应。而
**每一层都不报错** —— 中断在进、I2C 读得到、坐标也对。

原厂 `focaltech_touch/focaltech_core.c` 的 `fts_read_parse_touchdata()`
不是这么写的：

```c
for (i = 0; i < max_touch_num; i++) {        /* 上界是槽位数，不看 point_num */
    pointid = buf[FTS_TOUCH_ID_POS + base] >> 4;
    if (pointid >= FTS_MAX_ID) break;        /* 终止条件是"ID 无效" */
    events[i].flag = buf[FTS_TOUCH_EVENT_POS + base] >> 6;
}
```

本 patch 照此改写，并保留一条兜底：上一帧按下、这一帧完全没出现的触点
补一个 `TOUCH_UP`（对应原厂的 `data->touchs ^ touchs` 那段）。

上板验证：176 次落点全部按下/抬手成对，6 个色块全命中。
