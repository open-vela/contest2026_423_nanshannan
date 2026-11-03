# xTS 1.3.11 UART 文件传输：PASS（2026-11-02）

接 [09-20 记录](../2026-09-20-ymodem/README.md)：主机 → 板方向在 1.5 Mbaud 下已通过；
板 → 主机方向主机侧 CH340 在 1.5 Mbaud 丢字 7~16%，ymodem 收不全。115200 下零丢失，
但编译期改 `CONFIG_16550_UART0_BAUD` 不生效。本次在运行时把控制台临时降到 115200，
两个方向都按原文命令（板上 `rb` / `sb`，主机 lrzsz）传完，再改回 1.5M。

## 做法

- 新增手动命令 `k7diag baud 115200|1500000`（`app/k7diag/k7diag_main.c`）：打开
  `/dev/ttyS0`，`tcdrain` 后 `cfsetspeed` + `tcsetattr(TCSANOW)`，并读回。只接受这两个值。
- defconfig 加 `CONFIG_16550_WAIT_LCR=y`（见下方第 1 次尝试）。
- 脚本 `run.py`：
  1. 串口 1.5M 下敲 `k7diag baud 115200`。
  2. 换到 115200，对每个文件：
     - 敲 `rb -f /tmp`，主机 `sb --ymodem`；
     - 敲 `md5_test -f /tmp/<文件> -c 1`，与主机 md5 比；
     - 敲 `sb /tmp/<文件>`，主机 `rb --ymodem`，收回的与原文件逐字节比；
     - 敲 `rm`。
  3. 敲 `k7diag baud 1500000`，回到 1.5M 确认提示符。不通时 telnet 兜底。

## 结果

| 文件 | 大小 | 主机 sb | 板上 md5 | 主机 rb | 收回比对 |
|---|---|---|---|---|---|
| `xts-uart-pattern.bin`（`bytes(range(256))*16`，与 09-19 相同） | 4096 | rc 0，`Transfer complete` | `2bcd3c4d…270d` = 主机 | rc 0，`Transfer complete` | 4096 B，一致 |
| `xts-uart-random.bin`（64 KB `os.urandom`） | 65536 | rc 0，`Transfer complete` | `507dc7bf…948e` = 主机 | rc 0，`Transfer complete` | 65536 B，一致 |

- 镜像：`NuttX 0.0.0 ed8f69ad-dirty Nov  2 2026 01:13:44 arm64 kickpi-k7`（带 WAIT_LCR，见 `after-wait-lcr/` 的 uname）。
- 115200 下有提示符；改回 1.5M 后有提示符，没走 telnet 兜底。整轮 33 s。
- sha256：pattern `c8f5d034…f193`，random `848617fa…d6b5`（全文见 `events.json`）。
  测试文件每次由 `run.py` 生成，传完即删，不入库。
- 主机收回文件的 mtime 是 2446 年。这是板上 ymodem 头里填的时间字段，与内容无关。

原始记录：
- `console-1500000-before.raw` / `console-115200.raw` / `console-1500000-after.raw`：三段串口原文；
- `<文件>.host-sb.log` / `.host-rb.log`：lrzsz 的 stderr；
- `events.json`、`run.log`。

## 前三次失败（原始记录各自归档）

**1. `attempt1-no-wait-lcr/`：改完波特率控制台在任何波特率下都哑。**
当时从 telnet 发 `k7diag baud 115200`，命令返回成功，但串口在 115200 和 1.5M 下都是 0 字节，
只能 telnet `reboot`。
- 原因：RK3576 的 UART 是 DesignWare 16550。UART 忙时写 LCR 会被忽略，DLAB 没置上，
  随后的两字节除数就写进了 THR/IER。IER 变 0，中断全关，控制台哑到重启。
- 处理：NuttX 的 `CONFIG_16550_WAIT_LCR` 正是为这种 UART 准备的（写 LCR 前等 USR.BUSY 清零），
  在 defconfig 打开它。
- 这次的 `run.log` 末尾那个"exclusive lock"报错是脚本自身的毛病：兜底分支重开串口前没关旧句柄。
  已修。

**2. `attempt2-telnet-baud/`：开了 WAIT_LCR，115200 下仍无提示符。**
- 用 `mw` 读 UART0（基址 `0x2ad40000`）：TCSETS 之后 DLL 确实是 13（115200），
  但串口收到下一个回车后又变回了 1。
- 原因：串口上的 nsh 读每一行前保存整份 termios，读完写回
  （`apps/system/readline/readline_fd.c`）。从 telnet 改的波特率，在串口读完下一行时被写回原值。
- 佐证：手工 `mw 0x2ad4000c=0x83; mw 0x2ad40000=0x0d; mw 0x2ad4000c=0x03` 写除数后，
  115200 下立即能对话，硬件本身没问题。
- 处理：改波特率的命令在串口控制台上敲。上一行已经读完，下一行开始时保存下来的就是新波特率。
  telnet 只留作兜底。

**3. `attempt3-nonblock-fd/`：波特率切换正常，ymodem 两个方向都瞬间失败。**
现象：
- 板上 `rb` 立刻回 `ACK CAN CAN`，生成空文件（md5 `d41d8cd9…`）；
- 主机 `sb` 一直 `Timeout on sector ACK`，最后 `Retry Count Exceeded`；
- 板上 `sb` 发出一串 0x00 后退出，主机 `rb` `Transfer incomplete`。

原因在主机侧脚本：
- pyserial 以 `O_NONBLOCK` 打开串口。直接把这个描述符交给 lrzsz，它每次 `read` 都立刻
  `EAGAIN`，当成超时，于是发错包、取消。
- 板上看到的是主机发来的错乱序列。

处理：交给 lrzsz 之前清掉 `O_NONBLOCK`，用完恢复（`run.py` 中的 `Console.lrzsz`）。
板上什么都没改，第 4 次即本次结果。

## 改动清单

- `app/k7diag/k7diag_main.c`：`k7diag baud`（以及 1.3.5 用的 `k7diag sdpart`）。手动命令，不在启动路径上。
- `board/kickpi-k7/configs/amp-dual/defconfig`：`CONFIG_16550_WAIT_LCR=y`。
  这是上游已有选项，nuttx 树没改。开它以后在本次镜像上重跑了 1.3.10：
  `cmocka_driver_uart -d /dev/ttyS0` 结果 `[  PASSED  ] 1 test(s).`，1.5M 控制台照常。
  记录在 `after-wait-lcr/`。回归那轮（`../2026-11-02-regress/`）用的镜像还没有这个选项。
