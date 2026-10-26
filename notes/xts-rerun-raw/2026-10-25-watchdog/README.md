# xTS 1.3.15 Watchdog：复位原因修复后重跑（2026-10-25）

接 [09-20 记录](../2026-09-20-watchdog/README.md)：当时 -r 1 / -r 3 断言上次复位原因
为 RWDT 失败，根因是 CRU_GLB_RST_ST 在 ATF 里就被清零。

## 修法

不再依赖 CRU_GLB_RST_ST，改用重启模式寄存器 PMU0 GRF OS_REG16（0x26024040）：

- 看门狗使能（`rk3576_wdt_start`）时写 `BOOT_WATCHDOG` = 0x5242c308。
- 软件重启（`rk3576_do_reset`）照旧写 NORMAL / LOADER，把它盖掉。
- U-Boot `boot_mode.c` 对 BOOT_WATCHDOG / BOOT_PANIC **不清寄存器**（代表重启原因的
  值留给下一级），对 NORMAL 等写回 NORMAL，对 0（上电）不动。
- openvela 在 `board_late_initialize`（早于看门狗注册）锁存原值：
  BOOT_WATCHDOG → RWDT，0 → CHIPPOR，其他 → CORE_SOFT。CRU_GLB_RST_ST 若非 0 仍优先。

不需要复位前软件还能动：-r 1 / -r 2 是关中断死循环，"在看门狗中断里记一笔"行不通。

另：-r 3 要求 GETSTATUS 的 timeout 等于设定值 2000ms，硬件档位只能 2796ms
（2^(16+i) 个 24MHz 周期）。驱动改为报请求值、timeleft 按请求的截止时刻算；档位总
取不小于请求值的一档，所以报出的是"保证不早于此刻咬狗"的下界（用户 10-25 决定）。

## 板上验证

| 步骤 | 串口 / `resetcause` |
|---|---|
| 手动写 0x5242c308 后由看门狗复位（`serial-bootmode-probe.raw`） | ATF `soc warm boot, reset status: 0x1050`；U-Boot `boot mode: watchdog`；openvela 读回 0x5242c308 |
| 刷写后 / `reboot` 后 | `core_soft_reset(1380107008)`（0x5242c300） |
| `mw 10000000 4` 制造 data abort → panic 停住 → 看门狗复位（`serial-crash.txt`） | `rtc_watchdog(1380107016)`（0x5242c308） |
| `cmocka_driver_watchdog -r 1`（`serial-r1.txt`） | 过了 RWDT 断言，关中断死循环，被看门狗复位 → `rtc_watchdog` |
| `cmocka_driver_watchdog -r 2`（`serial-r2.txt`） | 过了 RWDT 断言，定时器回调里死循环，被看门狗复位 → `rtc_watchdog` |
| `cmocka_driver_watchdog -r 3`（`serial-r3.txt`） | **`[  PASSED  ] 4 test(s).`** |
| 10-26 断电上电 → `resetcause` | `power_on(2093176550)`（锁存值 0x7cc356e6） |
| 紧接着 `cmocka_driver_watchdog -r 0`（`serial-r0.txt`） | 过了 CHIPPOR 断言（之后才初始化看门狗，串口有 WDT 初始化日志），停止喂狗后被看门狗复位 → `rtc_watchdog(1380107016)` |

## 未完成 / 不满足的

- **-r 0（10-26 补验）**：原设计假设上电后重启模式寄存器为 0，实测不是 —— 两次断电上电分别读到
  0x7ccb56e6、0x7cc356e6（不定值），第一次因此被判成 CORE_SOFT。改为"不是软件写的
  0x5242c3xx 就判 CHIPPOR"后，第二次断电上电判为 power_on，-r 0 通过。
  软件重启或刷写后直接跑 -r 0 仍会失败（判 CORE_SOFT，这是如实的）。
  串口抓取只录到用例开头，结果行没录到；通过与否由"断言之后的看门狗初始化日志 +
  复位后 rtc_watchdog"推断。
- 原文预期"-r 0/1/2 主动触发 assert 并打印堆栈后重启"：驱动是 RMOD=0 直接复位，
  不打印堆栈。-r 1 关中断下要进看门狗中断只能走 FIQ（GIC Group 0），openvela 在
  非安全态配不了中断分组，做不到。
- 已知盲区：Linux 侧 PSCI SYSTEM_RESET、不掉电的硬件复位，若看门狗已上弦会被判成看门狗。

镜像：nuttx.bin sha 5b146784…，FIT 写入回读一致（flash-20261025T113950Z）。
该次的"刷写前备份"不干净（前一次尝试在备份后 USB 掉线，可能已部分写入）；
要回滚用 flash-20261025T113843Z 的备份（sha 69b2026c…）。
