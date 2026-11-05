# openvela 蓝牙框架（bluetoothd + zblue）在 SKW6621S 上跑通（2026-11-04）

单独的配置片段：`board/kickpi-k7/configs/bt-framework.config`（叠在 amp-dual 上）。
**不是**默认镜像：默认的 amp-dual 仍用 NuttX 自带协议栈（`bt` 命令，10-26 已能扫描）。

## 结构

```
bttool ──PF_LOCAL "bt:bluetooth"──> bluetoothd（SOCKET_IPC 服务端）
                                      └─ zblue（双模，BT_CLASSIC）
                                          └─ H4: /dev/ttyHCI0（CONFIG_UART_BTH4）
                                               └─ skw6621s_bt.c（bt_driver_s，SDIO）
```

`bt init` 加载固件、注册 `/dev/ttyHCI0`；厂商初始化（NV 下载、RESET、BD_ADDR）在第一次
open 时由驱动做完，之后才交给 zblue。

## 结果

`bttool-session.log`（第三方地址已遮掉）：

| 步骤 | 结果 |
|---|---|
| `enable` | 状态 1→2→3→4（ON），名字 `XIAOMI VELA-xxx`，Class 0x280704 |
| `scan start` 20 s | 16 条广播，4 个设备，RSSI −95…−101 dBm |
| `inquiry start 8` | Discovery Started → Stopped；另一次运行扫到 1 台手机（CoD 0x5a020c，−98 dBm） |
| 全程 | 无 assert，bluetoothd / bttool 都正常退出 |

信号偏弱（−95 dBm 上下），原因没查：NV 用的是 `SWT6621S_NV_SDIO_SHARE.bin`，
K7 的天线是独立还是共用没有确认（见 `kickpi_k7_skw6621s.c` 的注释）。

## 路上的问题

按出现顺序。前几个是 64 位/栈尺寸问题，后三个才是跑不通的真正原因。

1. **栈**：bluetoothd 主线程实际用约 23 KB（默认 8 KB，溢出后改坏了 wdog 链表、lpwork）；
   服务 loop 线程默认 4096，64 位上一次 syslog 就压穿。都改成 Kconfig 可配，片段里给 16/32 KB。
2. **`ARCH_STACK_PTR_ALIGN`**：zblue 只看 `CONFIG_64BIT`，aarch64 上取了 8，
   `k_work_queue_start` 对齐断言失败 → `zblue-lp64-stack-align.patch`。
3. **★ zblue 把 NuttX 的 SMP 关掉了**（`zblue-nuttx-smp-headers.patch`）。
   zblue 的 `arch_interface.h`/`irq.h`/`spinlock.h` 会 `#undef CONFIG_SMP`，本意是让 zephyr 自己
   的代码走单核路径。但这之后才第一次被包含的 NuttX 头都按单核展开，而内核是 4 核 SMP：
   - `this_task()` → `g_readytorun.head`（就绪队列头，不是当前任务）
   - `sched_lock()` 改的是**别的任务**的 lockcount；`gettid()` 读错 TCB
   - `enter_critical_section()` → 只关本核中断

   现象：`arch_irq_lock()` 里 `sched_lock` 的 `DEBUGASSERT(tcb_ && …)` —— tcb 为 NULL
   （就绪队列空的时候）。反汇编 `atomic_ptr_set` 看得最清楚：取当前任务是一条对全局变量
   `g_readytorun` 的 `ldr`，没有按 MPIDR 取下标。
   修法：在三处 `#undef` 前面先按 SMP 包含一遍这些 NuttX 头（新文件 `nuttx_smp.h`），
   include guard 保证之后不再重新展开。修完 `atomic_ptr_set` 改成按 MPIDR 取、临界区是
   `rspin_lock_irqsave(&g_schedlock)`。
   之前看到的"lpwork/wdog 被写坏"有一部分可能也来自这里（别的任务被莫名 sched_lock），没有单独验证。
4. **★ 没有 `/dev/urandom`**：zblue `prng_init()` = `psa_crypto_init()`，熵来自
   `getrandom(flags=0)`，NuttX 的实现读 `/dev/urandom`。节点不存在 → `bt_init` 返回 −5，
   适配器停在 TURNING_ON，**没有任何报错**（zblue 的 LOG_ERR 没编进来）。
   片段里开 `CONFIG_DEV_URANDOM_ARCH`：`rk3576_rng.c` 自检通过后把同一个硬件 RNG 注册成
   `/dev/urandom`。
5. **★ 客户端不认新式 IPC 码**（`bt-ipc-client-new-codes.patch`）：LE 扫描结果按批发送，
   用的是 `BT_LE_ON_BATCH_SCAN_RESULT = BT_IPC_CODE(CALLBACK, BLE_SCAN, 1)`。客户端的回调
   分发表本来就按新式区间查，但 `bt_socket_client_receive()` 只放行老枚举区间，
   收到第一批扫描结果就 `assert(0)`。按 group 非 0 区分新老码后放行。

其余编译问题（LP64 格式串、KVDB 未开、`uv_loop_is_close`、`bt_sal_cm_*` 未编译等）
见 `bsp/upstream/bt-framework-aarch64-build.patch` 和 `frameworks-uv.patch`。

## 调试时踩的坑

- **OTA 没装上而我在读旧日志**：有四轮调试镜像其实没进板（重启那一步失败），
  串口日志文件还是上一轮的，几轮"结果一模一样"。后来的脚本先比对 `uname` 的编译时间，
  对不上就退出。
- **串口日志在 panic 时丢字**：多核同时打印 + assert 的轮询输出会吃掉缓冲里的行。
  关键的一行打完后 `up_mdelay(100)` 再继续，才看清 `tcb=0`。
- **一个线程"卡在两次 syslog 之间"**：其实是镜像没换（见第一条），不是 syslog 锁死。
- `dumpstack <pid>`（片段里开了 `CONFIG_SYSTEM_DUMPSTACK`）一次只打一个 pid，
  一次打多个时串口输出会乱。

## 没做的 / 留下的

- libuv 用的是 poll 后端（片段里关了 `CONFIG_LIBUV_BACKEND_EPOLL`）：epoll 后端下
  bt_client 在 `file_poll` 里崩过（inode=0x8），原因没查。是否也是第 3 条的后果，没有复测。
- `CONFIG_SYSLOG_BUFFER` 为调试打开，留在片段里。
- 只验证了 enable / LE 扫描 / BR 查询；配对、连接、GATT、A2DP 都没试。
- 这些补丁还没有提交到上游。
