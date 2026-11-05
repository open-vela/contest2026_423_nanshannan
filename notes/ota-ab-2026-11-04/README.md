# openvela 镜像 FIT A/B 与 OTA：上板验证（2026-11-04）

设计见 `amp/include/k7_bootab.h` 和 `amp/FLASH.md`。本页只记上板做了什么、看到了什么。

## 为什么做

- trust 分区的 FIT 窗口只有 4MB，镜像 3.55MB，余量约 0.4MB：蓝牙协议栈、media 框架放不下。
- OTA 需要两个槽和"失败回退"，否则升级坏了只能接线重刷。
- rkdeveloptool 写不到 LBA ≥ 65536，所以大槽只能放在 32MB 以外、由 openvela 经网络写 ——
  这正好就是 OTA 的路径。

## 布局

| 区域 | 位置 | 写入方 |
|---|---|---|
| 救援 FIT | trust 分区 LBA 24576，上限 8184 扇区 | flash.sh（rkdeveloptool） |
| 控制块 | LBA 32760 / 32761 | openvela `bootctl` / `ota`，U-Boot `bootamp`；flash.sh 清零 |
| 槽 A / B | recovery 分区 LBA 184320 / 249856，各 32MB | openvela `ota install` |

recovery 分区：AMP 下没有 Android recovery。分区开头是早期调试留下的 `AMPTEST7` 图案，没有人读。
LBA 32760 原来是更早一版较大 FIT 的尾巴（NuttX 的格式串），已备份到
`~/rk3576-amp/backup/lba32760-before-ab-20261104.bin`。

## 与上游 bootctl 的关系

`frameworks/system/ota/bootctl` 的状态是 KVDB 里的 7 个属性（每槽 active / bootable /
successful + 全局 try），bootloader 本身也是 NuttX。本板的 bootloader 是 U-Boot，读不了
KVDB，所以把同样的标志放进一个扇区的二进制结构（两份、seq、crc32），选槽规则照搬
`bootctl_boot()`；openvela 侧 `bootctl` 的子命令（无参数 / update / done / slot）也与上游一致。

读上游代码时发现一处：**试启动失败、回退之后 `try` 一直不清**（只有 `bootctl_success()`
在槽尚未 successful 时才清），而 `bootctl_done()` 也不清。于是下一次升级写完 done，
bootloader 一进来就看到 `try` 为真，把新槽直接判为"试启动失败"，根本没有试。
本实现在两处都清（U-Boot 作废槽时、openvela `done` 时）。下面第 4 步就是这个场景：
B 刚因未确认被作废，紧接着重装 B，新的 B 正常进入了试启动。

## 上板步骤与结果

镜像：当天 09:30 编的 amp-dual（含 k7ota），U-Boot 打 0014 后 09:22 编。

1. **flash.sh --stay + flash-uboot.sh**（备份：`~/rk3576-amp/backup/uboot-lba16384-before-ab-20261104.bin`）。
   复位后控制块已被清零：
   ```
   AMP: 没有 A/B 控制块（-2），启动救援 FIT
   ```
   `bootctl info` → "没有控制块：启动的是救援 FIT"。行为与改动前一致。

2. **负向：错误的 sha256**（`ota-push.py --sha256 000…`）
   ```
   收完 3549696 字节，974 ms（3559 KB/s）
   收到的数据 sha256 不对: 041f…，槽 A 保持不可启动
   ```
   第一次尝试在 1.25MB 处 webclient 返回 -11（EAGAIN）中断，重试即正常；
   两次槽 A 都停在 bootable=0。这次网络中断没有复现，原因没有查。

3. **安装到 A → 重启 → 试启动 → 自动确认**
   ```
   ota:   收完 3549696 字节，910 ms（3809 KB/s） / 回读 sha256 一致 / 完成：槽 A 已写入并校验
   U-Boot AMP: A/B seq 4：A[ab-] B[---] try=0
          AMP: 槽 A 尚未确认，试启动（第 1 次）
          AMP: 槽 A 的 FIT 3548184 字节，哈希一致
   openvela OTA: 本次启动已确认（界面运行 15 s）
   bootctl info: 本次启动：槽 A；槽 A active=1 bootable=1 successful=1
   ```

4. **安装到 B → 重启 → 确认之前再次重启（模拟新镜像没跑起来）→ 回退到 A**
   在板子重启后 12.2 s（界面确认之前）经网络 NSH 发 `reboot`：
   ```
   AMP: A/B seq 8：A[-bs] B[ab-] try=0      第一次：试启动 B
   AMP: A/B seq 9：A[-bs] B[ab-] try=1      第二次：try 还在
   AMP: 槽 B 上次试启动没有被确认，作废
   AMP: 槽 A 的 FIT 3548184 字节，哈希一致
   bootctl info: 本次启动：槽 A；槽 B active=0 bootable=0
   ```

5. **重装 B，再把 B 的一个扇区改坏 → U-Boot 在拉起 A72 之前发现，回到 A**
   `dd if=/tmp/junk.bin of=/dev/abslot bs=512 count=1 seek=69584`，`ota verify B` 报不一致；重启：
   ```
   AMP: A/B seq 12：A[-bs] B[ab-] try=0
   AMP: 槽 B 尚未确认，试启动（第 1 次）
      Hash(es) for Image 0 (openvela): sha256 Bad hash: becf6dfb…
   AMP: 槽 B 作废（-74）
   AMP: 槽 A 的 FIT 3548184 字节，哈希一致
   AMP: Brought up cpu[100] …          ← A72 在选定槽之后才启动
   ```

## 没做的

- 试启动挂死时没有看门狗兜底：需要按一次 RESET，U-Boot 才会回退。
  U-Boot 可以在试启动前开 WDT、openvela 接手喂狗，留作后续。
- 槽里只放 openvela；Linux 内核仍在 LBA 49152 的固定位置，不随 A/B 切换。
- 救援 FIT 仍受 4MB 窗口限制。超过 4MB 的镜像只能装进槽里，救援 FIT 要保持一个
  能联网、能跑 `ota` 的精简版本。
