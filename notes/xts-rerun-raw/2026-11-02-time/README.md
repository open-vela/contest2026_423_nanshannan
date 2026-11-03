# xTS 1.3.14 时间一致性：关 NTP 后静置 24 h（2026-11-03 起，**进行中**）

接 [09-20 记录](../2026-09-20-time/README.md)（时间不够、中途被别的用例改过钟，作废）。
目录名按准备当天（11-02）取，实际起跑是 11-03。

- 起点：2026-11-03 05:01:11 UTC（北京 13:01:11），板子 `date -u -s` 设时。
- 预计结束：2026-11-04 约 05:11 UTC（北京 13:11）。
- 镜像：板上正在跑的 amp-dual 正常镜像（内核 #29 那天写入的 nuttx.bin，未重刷）。
- 判据（原文）：静置 24 h 以上，板子与 PC 时间差 ≤ 2 s；每 6 h 检查一次，共 4 次。

## 前提"未配网 / 无 NTP"怎么满足

和 09-20 不同，**这版镜像编进了 NTP 客户端**（`CONFIG_NETUTILS_NTPCLIENT=y`），
界面 `kickpi_ui` 开机时调 `ntpc_start()`，每 60 s 对一次时。板上没有编 `ntpcstop`。

做法：设时前用网络 NSH `kill -9 24` 结束 `NTP_daemon` 任务（`_netsh/` 里有 kill 前后两次 `ps`），
之后**每次取样都查一次 `ps`**，记 `ntp_daemon: false`。

网线仍接着：取样走网络 NSH。其它会动时钟的代码已排查：
- `ai_agent` 的 `vela_tls.c` 只在时间早于 2024-01-01 时把钟拨到 2026，设时后不会触发；
- 应用与板级代码里没有别的 `clock_settime` / `settimeofday`。

板子若重启，界面会重新起 NTP，整轮作废。因此设时后在 tmpfs 上放了标记文件
`/tmp/xts1314.mark`，每次取样确认它还在（`mark_present`）。
（`/proc/uptime` 在这版固件上读出溢出的大数，不能用。）

## "PC 时间"用什么

**不用 WSL 的墙钟。** 试跑时它在一次请求内跳了约 50 s，之后又停在比 NTP 快约 50 s 的位置。
`capture.py` 的全部计时用 `time.monotonic()`，基准 = monotonic + K，
K 每次取样由 `sntp.py` 向 ntp.aliyun.com / ntp.tencent.com / cn.pool.ntp.org 重新量
（取往返最短的一次，往返为负或 > 0.5 s 的回复丢弃）。
起跑前实测 40 s 内 K 的波动在 ±2 ms 以内。墙钟相对 NTP 的偏差另记一列 `wall_minus_ntp_s`，只作参考。

设时也按这个基准：命令在基准时间正好走到整秒时发出（`sent_ref` = `target_epoch`），
起点误差是毫秒级。

## 取样

每 10 分钟一次，6 / 12 / 18 / 24 h 标为 `CHECK-Nh`。每次连续轮询 `date -u +%s`，
捕捉板子秒数跳变的瞬间，偏差 = 板子 − NTP 基准，分辨率约 ±0.13 s（`edge_window_s` 0.256 s）。

## 与原文的差别

1. 原文用 minicom 时间戳人工比对；这里走网络 NSH 自动取样，基准是公网 NTP。
   理由：09-14 那轮串口在约 15 h 断连；而主机墙钟本身不可靠。
2. 原文要求"未配网"；这里网线接着、NTP 任务被结束，并逐次取证。
3. 每 10 分钟多采一次（原文 4 次），中途中断也有部分证据。

## 文件

- `capture.py`、`sntp.py`：取样脚本与主机侧 SNTP；
- `events.jsonl` / `capture.log`：每次取样一行；
- `session.raw`：取样时网络 NSH 的原始字节；
- `_netsh/`：起跑前的 `ps`、`kill -9 24`、`date -u` 记录。

## 结果

（24 h 后填写）

| 检查点 | 偏差（板子 − NTP） | NTP 任务 | 标记文件 |
|---|---|---|---|
| t0 | −0.096 s | 无 | 在 |
| CHECK-6h | | | |
| CHECK-12h | | | |
| CHECK-18h | | | |
| CHECK-24h | | | |
