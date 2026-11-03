# xTS 1.3.15 Watchdog：断电上电后按顺序跑 -r 0 → 3，全程录串口（2026-11-02）

接 [10-25 记录](../2026-10-25-watchdog/README.md)。那次 -r 0 的串口只录到用例开头，结论是推断出来的。
本次断电上电后，在同一次不断开的串口录制里按原文顺序跑完四个 case。

镜像：openvela nuttx.bin md5 `48d9d76af5e89a22…`（FIT 当天 21:31 写入并回读一致），Linux 内核 #29。

## 文件

- `serial.raw`：串口原始字节，1.5 Mbaud，从上电前开始录，到 -r 3 结束；
- `serial.txt`：同一份数据按行切开，带主机时间戳；
- `_netsh/`：telnet 下发的命令与 `resetcause` 读数。用例本身的输出走 syslog 上串口，
  telnet 会话里只有命令回显。

★ 主机时间戳不可靠：WSL 时钟会回跳（例如 1604 行 21:53:57，之后 1698 行 21:53:11）。
看先后用**行号**。串口在 1.5M 下偶尔丢字节，有几行被截断或粘连，但每个 case 的关键行都在。

## 过程与结果

| 步骤 | 依据（serial.txt 行号 / _netsh） | 结果 |
|---|---|---|
| 断电上电 | 120 行 `boot mode: None` | |
| `resetcause` | _netsh：`power_on(2093684454)` | 判为 CHIPPOR |
| `cmocka_driver_watchdog -r 0` | 531 行 `Running 4 test(s)`，随后是 WDT 初始化日志（`wdg_init` 在 CHIPPOR 断言**之后**才调用，说明断言已过）；**没有 FAILED**；约 8 s 后 619 行 `reset status: 0x1050`，653 行 `boot mode: watchdog` | 被看门狗复位 ✔ |
| `resetcause` | `rtc_watchdog(1380107016)` | RWDT |
| `cmocka_driver_watchdog -r 1` | 1056 行起：feeding 跳过 OK，进入 interrupts（关中断死循环）；1145 行 `soc warm boot, reset status: 0x1050`，1181 行 `boot mode: watchdog` | 被看门狗复位 ✔ |
| `resetcause` | `rtc_watchdog(1380107016)` | RWDT |
| `cmocka_driver_watchdog -r 2` | 1604 行起：前两个 OK，进入 loop（定时器回调死循环）；1698 行 `reset status: 0x1050`，1732 行 `boot mode: watchdog` | 被看门狗复位 ✔ |
| `resetcause` | `rtc_watchdog(1380107016)` | RWDT |
| `cmocka_driver_watchdog -r 3` | 2143 行起：feeding / interrupts / loop OK，api 检查 GETSTATUS；**2155 行 `[  PASSED  ] 4 test(s).`** | PASS ✔ |

-r 0 按用例源码（`drivertest_watchdog_feeding`）的设计：先喂狗 pingtime，再停喂、忙等 2 倍 timeout，
最后 `assert_true(false)`。也就是说，**通过的标志是被复位**，复位后不会再打印结果行。10-25 记录里说的
"结果行没录到"其实不存在。失败的话应该看到断言失败的 `[  FAILED  ]`，本次全程没有。

## 与原文预期的差距（不变）

原文："-r 0/1/2 ……能否主动触发 assert 和打印堆栈信息并重启，并且重启原因是 RWDT"。

- 重启 + 重启原因 RWDT：满足（三次 `rtc_watchdog`）。
- **主动 assert、打印堆栈：不满足**。驱动是 RMOD=0，直接复位，不走中断。
  -r 1 是关中断死循环，要在关中断时进入看门狗中断，只能走 FIQ（GIC Group 0），
  而 openvela 跑在非安全态，配不了中断分组。理由见 10-25 记录。

## 分档建议

- 10-25 的 B 档有两条依据：-r 0 证据不完整；不打印堆栈。
- 第一条本次补齐：完整录制，顺序与原文一致，四个 case 都符合预期。
- 第二条是平台限制，仍然存在。按 11-02 总表里"录到即升 A"的约定可以升 A，
  但"不打印堆栈"一条要在总表里保留说明，由评审判断。
