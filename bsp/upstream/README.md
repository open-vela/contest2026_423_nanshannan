# 上游侧改动

这些改动**不在** nuttx 核心仓，也不在本仓的板级/芯片层，而是散落在
openvela 工程的其它仓里。按《新平台适配指南》「不得修改核心代码」的精神，
它们不该跟板级适配混在一起，各自独立提 PR。

归档在这里是为了让别人能复现我们的构建。

| 补丁 | 仓 | 内容 |
|---|---|---|
| `ai_agent.patch` | `packages/ai_agent` | ① `feishu_http.c` 用了 `cJSON_int` —— 这个类型在上游 cJSON 里不存在，全仓只出现这一处、无任何定义，是笔误；顺带 `valueint` 是 `int` 却配 `%lld`，一并改正。② Makefile 加 `-Wno-format-truncation`：`agent_loop.c` 拼路径的 `snprintf` 会被判可能截断，该检查随优化级别变化，参考平台（goldfish，`-O3`）不触发。③ Makefile 补编 `tool_camera.c`（`CONFIG_AI_AGENT_CAMERA=y` 时）：CMakeLists.txt 有这一段、Makefile 漏了，开相机工具后链接报 `undefined reference to tool_camera_capture_execute`。④ `agent_config.h`：`AGENT_LLM_TIMEOUT_SEC` 60→180、socket 超时 120→240 —— Token Plan 的 mimo-v2.5 是推理模型，带图后第二轮实测 86~99s，60s 看门狗会把已经返回的回答当超时丢掉。 |
| `agent-monotonic-latency.patch` | `packages/ai_agent` | ① LLM 耗时改用单调时钟（TLS 握手前改墙钟会让耗时算成几十天，已返回的回答被当超时）。② `send_working_status()` 对 `local_client` 渠道不发"处理中"：velaclaw SDK（`velaclaw_client_local.c`）的异步回调只触发一次，这条过渡消息会把它用掉，真正的回答到达时已无人接收 —— kickpi_ui 的桌面助手表现为永远停在"让我查一下..."。 |
| `agent-skills-mkdir-fat.patch` | `packages/ai_agent` | `skill_loader_init()` 用 `mkdir(AGENT_SKILLS_DIR)` 建目录，而这个宏带末尾 `/`（后面要直接拼文件名）。NuttX 的 FAT 对带末尾斜杠的 `mkdir` 返回 ENOTDIR，tmpfs 却接受 —— `/data` 是 tmpfs 时从没暴露；2026-10-25 `/data` 改挂 eMMC（vfat）后，每次开机 10 条 `Cannot write skill`，内置技能一个都装不上。改成不带斜杠的路径建目录。 |
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
