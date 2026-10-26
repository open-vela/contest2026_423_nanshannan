# xTS 1.3.15 Watchdog：4 项中 2 项通过，卡在"复位原因"（2026-09-20）

> **2026-10-25 更新**：复位原因已改用重启模式寄存器判定，-r 1/2/3 通过，见 [10-25 重跑](../2026-10-25-watchdog/README.md)。

镜像：ecc7cba 正常镜像。按原文顺序执行 `cmocka_driver_watchdog -r 0/1/2/3`。

| 子项 | 结果 | 说明 |
|---|---|---|
| `drivertest_watchdog_feeding`（-r 0） | OK | 停止喂狗后板子被看门狗复位（ATF 打印 `warm boot, reset status: 0x1050`，bit12=WDT_NS、bit6=看门狗复位）。原始日志 `serial-r0.raw` |
| `drivertest_watchdog_loop`（-r 2） | OK | |
| `drivertest_watchdog_interrupts`（-r 1） | **FAILED** | `drivertest_watchdog.c:377` 断言上次复位原因为 RWDT，实得 CHIPPOR（`1 != 2`） |
| `drivertest_watchdog_api`（-r 3） | **FAILED** | `drivertest_watchdog.c:457/460`，同一原因 |

## 根因（已定位，证据充分）

`board_reset_cause()` 读 CRU_GLB_RST_ST（0x27200000+0x0c04，偏移与 U-Boot
`cru_rk3576.h` 的 `RK3576_GLB_RST_ST` 一致）。**看门狗复位后在 openvela 里读到的是 0**：

    nsh> xd 0x27200c04 16
    0000: 00 00 00 00 ...        ← 紧接在一次看门狗复位之后

而同一次启动里 ATF（BL31，闭源二进制）打印 `warm boot, reset status: 0x1050`。
即：**状态锁存位在 ATF 阶段被读走并清掉**，轮到 openvela 时已经没有了；
U-Boot 源码里没有任何写 RK3576 GLB_RST_ST 的代码（已 grep 确认）。

## 可行的修法（未实施，待决定）

原文注里要求"厂商初始化 wdt 时需要在 wdt 中断里主动调用 panic"。对应做法：
WDT 恒定用两段式（RMOD=1）+ 芯片层自带 ISR，第一次超时时
① 往一块掉电才清的存储（PMU OS_REG 或已预留的 ramoops 区 0x40110000）写标志，
② `PANIC()` 打断言与栈回溯；第二次超时复位。`board_reset_cause()` 先看
GLB_RST_ST，再看该标志。这样 -r 0/1/2 的"assert+栈+重启"和 RWDT 判定都能满足。
