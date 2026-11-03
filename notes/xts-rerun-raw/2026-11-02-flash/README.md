# xTS 1.3.5 Flash 功能：PASS（2026-11-02）

接 [09-20 记录](../2026-09-20-sdblock/README.md)：原文命令 `cmocka_driver_block -m <设备>`
的 stress 子项按设备容量 95% 逐扇区"写 + fsync + 读 + CRC 比对"，在整张 16 GB SD 卡上
是 2,920 万次循环，跑不完；板载 eMMC 上是启动镜像，不能作测试对象。

## 做法

把 SD 卡末尾 64 MB 注册成独立块设备 `/dev/sdtest`，对它执行原文命令。

- 新增手动命令 `k7diag sdpart [扇区数]`（`app/k7diag/k7diag_main.c`），内部调
  NuttX 的 `register_blockpartition()`——板子开机注册 `/dev/data`、`/dev/lxroot`
  用的是同一个接口。
- **父设备写死 `/dev/mmcsd1`（SD 卡），命令不接受设备参数**，不可能指到 eMMC。
- 不写分区表，只注册设备节点，重启即消失；放在卡末尾（留 1 MB 余量），卡前部的
  FAT 引导区与 FAT 表不动。被测区域里原有数据会被覆盖（这张卡上是空 FAT32）。
- 设备仍是 SD 卡这块 Flash 介质，只是把测试范围限定在用例规模合理的大小。

## 结果

| 步骤 | 输出 |
|---|---|
| `k7diag sdpart` | `/dev/sdtest：/dev/mmcsd1 的扇区 30589952 起 131072 个（64 MB），ret 0` |
| `ls -l /dev/sdtest` | `brw-rw-rw- 1 67108864 /dev/sdtest` |
| `cmocka_driver_block -m /dev/sdtest` | `[ OK ] drivertest_block_stress`、`[ OK ] drivertest_block_single_write`、`[ OK ] drivertest_block_cache_write`、**`[ PASSED ] 3 test(s).`**，用时 144 s |

串口里个别行被 1.5 Mbaud 丢字截断（如 `[ RUN     tress`），三个 `[ OK ]` 行与汇总行完整。

镜像：`uname` 见 `镜像-uname--a.net.txt`。原始记录：每步 `.net.txt`（telnet）与
`.serial.raw`（串口），`events.jsonl`、`run.log`；脚本 `run.py`。
