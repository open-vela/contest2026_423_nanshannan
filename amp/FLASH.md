# AMP 的烧写与启动

## ★ 一个必须知道的限制：rkdeveloptool 写不到 32MB 以上

`rkdeveloptool wl` 对 **LBA >= 0x10000（32MB）的写是静默丢弃的**：命令
照样报 `Write LBA from file (100%)`，回读却是 0xCC（未写）。

逐项验证过：

| 实验 | 结果 |
|---|---|
| `wl 65528` 写入后回读 | 一致 ✓ |
| `wl 70000` 写入后回读 | 全 0xCC ✗ |
| 换 rkbin 的 `rk3576_spl_loader` 用 `db` 重下再写 | 一样 ✗ |
| `wlx recovery` 走分区名路径 | 一样 ✗ |
| **同一个 LBA 70000 用 U-Boot 的 `mmc write`** | **成功 ✓** |

最后一条把责任划清了：**eMMC 没问题，限制在 USB 通道**（rkdeveloptool
发的是 32 位 LBA，设备侧在 0x10000 处截断）。

这个坑最坏的地方是**它不报错**。第一次往 boot 分区写 46MB 的 Linux
镜像，只有前 7MB 落了地，而工具说 100%。后面所有"为什么起不来"的排查
都建立在一个假前提上。**所有烧写之后都必须回读对比**，这不是谨慎，
是这条通道的必要条件。

## ★ 第二个坑：借来当中转站的分区，U-Boot 每次启动都会写

把 `amp-solo.itb`（A/B 对照用）放在 LBA 30000，写完回读校验**通过**。
后来同一段再读，尾部 393 字节变成了 `\x00AB0\x01...` —— Android A/B 的
**bootctrl BCB**。

原因：那份 FIT 跨过了 LBA 32768，也就是 `misc` 分区的起点，而 U-Boot
每次走 Android 启动路径都会往 `misc` 写 BCB（日志里的
`ANDROID: reboot reason` / `Vboot=0, AVB images, AVB verify`）。

**写入和当时的校验都是对的，是后来被覆盖的。** 所以"烧完校验一次"
不够 —— 借分区当中转站时，必须借 U-Boot **从不写**的那一段。

已知会被 U-Boot 写的：`misc`（每次启动）。实测安全的：`security`
（8192 起）、`trust`（24576 起，但**不要跨过 32768**）。

这个坑的排查路径也值得记：先在主机上验证 FIT 文件自洽（数据段哈希 ==
裸镜像哈希）→ 再用 `md.b` 看 RAM 头 16 字节（对）→ 再用 U-Boot 的
`cmp.b` 比两次 `mmc read`（一致，读是可靠的）→ 最后 `cmp.b` 比"已知好的
那份"和"这份"的数据段，差异点 0x159A00 折算回 LBA 32772，正好压在 misc
上。**每一步都换一个已知正确的对照物**，而不是反复怀疑同一个环节。

## ★ 第三个限制：openvela 自己访问 eMMC 只能到 4GB

openvela 没开 `CONFIG_FS_LARGEFILE`，`off_t` 是 32 位。对整盘
`/dev/mmcsd0` 定位到 4GB 以外会**静默回绕**：2026-10-31 实测
`dd if=/dev/mmcsd0 skip=61079518`（备份 GPT 所在处）读到的是
LBA 2359262（super 分区里）的内容，和直接 `skip=2359262` 读出的逐字节相同。
写也一样会回绕，而且不报错。

所以 openvela 只通过注册好的**分区设备**写 eMMC（偏移从分区起点算），
分区整个落在 LBA 8388608 以内（`kickpi_k7_appinit.c` 注册时检查）：

| 设备 | GPT 分区 | LBA | 用途 |
|---|---|---|---|
| `/dev/data` | cache | 1165312 + 786432（384MB） | `/data`（FAT） |
| `/dev/lxroot` | backup | 378880 + 786432（384MB） | A72 上 Linux 的根文件系统镜像，见 `app/lxroot` |
| `/dev/abslot` | recovery | 182272 + 196608（96MB） | openvela 的 A/B 两个 FIT 槽（各 32MB），见下 |
| `/dev/abctl` | trust 的末尾 | 32760 + 8 | A/B 启动控制块 |

### openvela 镜像的 A/B 与 OTA（2026-11-04）

```
救援 FIT  trust 分区 LBA 24576，上限 8184 扇区    flash.sh 用 rkdeveloptool 写，永远保留
控制块    LBA 32760 / 32761（两份，seq 奇偶轮流写） flash.sh 写完救援 FIT 顺手清零
A/B 槽    recovery 分区，A 在 184320、B 在 249856  只能由 openvela 经网络写（≥ 65536）
```

- 格式：`amp/include/k7_bootab.h`；U-Boot：`amp/uboot/0014`；openvela：`app/k7ota`
  （`ota install` / `bootctl`）；主机：`scripts/ota-push.py [--reboot]`。
- 选槽规则照搬 `frameworks/system/ota` 的 `bootctl_boot()`：新槽先试启动，界面起来
  15 秒后自动 `bootctl` 确认；没确认就在下一次进 U-Boot 时作废、回到原来的槽；
  两个槽都不可用就启动救援 FIT。
- U-Boot 在**拉起 A72 之前**就校验槽里 FIT 的哈希：`brought_up_all_amp()` 先起 Linux
  再加载 openvela，到那时才发现哈希错就不能换槽了。
- 控制块无效（从没做过 OTA，或 flash.sh 刚清过）= 没有 A/B，和以前完全一样。
  `setenv amp_ab off` 临时关掉；`bootctl clear` 从板上清。
- 上板验证与日志摘录：`notes/ota-ab-2026-11-04/README.md`。

backup 分区 2026-10-31 全盘扫过是全零；首尾写标记后重启两次标记完好，
U-Boot 启动时不改写它。super、userdata 跨过或整个在 4GB 以上，不用。

### Linux 根文件系统镜像怎么到 A72 上

```
主机  python3 scripts/lxroot-push.py <镜像.cpio>
        └─ 板上 openvela `lxroot write`：边下边写 /dev/lxroot，回读 sha256 一致才写头部
U-Boot  bootamp（uboot/0012）：头部有效 → 读到 0x80000000 → sha256 一致
        → 内核 dtb /chosen 写 linux,initrd-start/end
Linux   先解内嵌 initramfs，再把这份 cpio 解在上面（同名覆盖）
```

镜像要是 **cpio（newc）**，不压缩或 xz（内核只开了 RD_XZ）。头部无效、
sha256 不一致都照常启动、只是不带外部镜像；U-Boot 里 `setenv amp_lxroot off`
可以临时关掉。32MB 镜像 U-Boot 读+校验 154ms，网络写入约 4MB/s。

## 布局：四段全部落在 32MB 以内

正因为上面这条，AMP 的四个部件都放在够得着的 LBA 上，而且**不使用**
boot 分区的 Android 镜像格式（那样太占地方）：

| LBA | 上限 | 内容 | 原分区 |
|---:|---:|---|---|
| 14336 | 272KB | `rk3576-kickpi-k7-amp.dtb`（只含 4 个 A72 核） | security |
| 16384 | 3MB | `uboot-amp.img`（带 AMP + bootamp + 面板） | uboot |
| **24576** | **4MB** | `amp.itb`（AMP FIT，里面是 openvela） | trust |
| 49152 | 8MB（到 65536） | `Image-amp-rootfs`（Linux Image + 内嵌 initramfs，7.4MB；bootamp 读 8MB 窗口 = 16384 扇区，uboot/0013，先烧新 U-Boot 再烧大 Image） | vbmeta + boot 头部 |

主机侧的唯一来源是 `amp/layout.sh`，U-Boot 侧是 `cmd/bootamp.c` 的
`AMP_*` 宏（`uboot/0009`），两处必须同步改。所有写盘脚本都先过
`amp_check_write`，区间越界直接拒绝。

★ 2026-09-17：FIT 从 security（8192，上限 6144 扇区）挪到 trust。
原因一是 openvela 继续变大，3MB 只剩约 560KB；原因二是 security 是
OP-TEE 安全存储（RKSS）的分区，每次启动都有 `TEEC: Reset area[0]/[1]`，
对应的正是 8192/9216。

★ 2026-09-17 事故：**别再往 LBA 51200 写任何东西。** 旧 `scripts/flash.sh`
按单系统布局把 NuttX 写到 51200（boot 分区起点），而那里是 49152 起的
AMP 内核的第 1MB 处。结果是双系统每次都卡死在 NSH 横幅附近，单系统却
完全正常，连 09-13 验证过的 FIT 都一样挂 —— 回退代码没有用，坏的是
eMMC 上 FIT 之外的东西。排查口诀：**双系统挂、单系统好，先把 dtb /
内核 / U-Boot 从 eMMC 读回来和原件比对。**

`Image` 之所以能压到 7.6MB（原来 43MB），见 `linux/amp-minimal.config`
——削掉的都是 AMP 下归 openvela 的外设，既省地方也是资源划分本身。

★ dtb 原来在 LBA 12800，后来往后挪到 14336：openvela 加上 LVGL 界面之后
`amp.itb` 从 2.17MB 涨到 2.54MB（4961 扇区，8192..13153），正好压过去。
**每次 openvela 变大都要重算这条边界** —— 这种越界不会报错，只会让 dtb
读出来是 FIT 的尾巴，然后 Linux 在一个看不出所以然的地方停住。
（这一段是 FIT 还在 security 时的记录；FIT 挪到 trust 之后 dtb 前面已经空出来了。）

## 烧

```bash
RK=~/rkdeveloptool/rkdeveloptool
$RK wl 24576 out/amp.itb        # 日常用 scripts/flash.sh，它带备份/回读/回滚
$RK wl 14336 out/rk3576-kickpi-k7-amp.dtb
$RK wl 16384 out/uboot-amp.img
$RK wl 49152 out/Image-amp-rootfs   # 日常用 scripts/flash-kernel.sh（备份/回读/回滚）
# 每一段都回读对比，见上
```

## 启

串口 **1500000**（U-Boot、Linux、以及 amp-dual 配置下的 openvela 都是
这个速率 —— 故意统一，否则一条启动流要在两个波特率上分两次抓，
AMP 调试期这一点很要命）。

上电后一直敲 **Ctrl-C** 打断 autoboot（U-Boot 明说了认这个键：
`Hit key to stop autoboot('CTRL+C')`），然后：

```
=> bootamp
```

就一条。`bootamp` 不带参数时自己按 `cmd/bootamp.c` 里写死的布局把内核、
dtb、FIT 三块料从 eMMC 读进来，再启动。

**为什么不再手敲六条命令**：1.5M 波特率没有硬件流控，板子忙的时候
UART 会静默丢字节。实测有一轮 `setenv amp_linux_cmd 'booti 0x40400000 -
0x4f000000'` 到板子那边成了 `booti 0x4f000000`，U-Boot 拿 dtb 当内核启动
→ data abort → 串口和 USB 一起消失，只能按 RESET。详见
`uboot/0006-bootamp-load-from-emmc.patch`。

老的手动形式仍然可用（FIT 已经在内存里时）：

```
=> mmc dev 0
=> mmc read 0x40400000 0xC000 0x39A5    # Linux Image ← LBA 49152
=> mmc read 0x4f000000 0x3800 0x212     # Linux DTB   ← LBA 14336
=> mmc read 0x60000000 0x6000 0x2000    # amp.itb     ← LBA 24576
=> setenv amp_linux_cmd 'booti 0x40400000 - 0x4f000000'
=> bootamp 0x60000000
```

`bootamp` 成功不会返回：它最后把当前这颗核送进 openvela。

## ★ 第三个坑：Linux 的 earlycon 会抢 UART0

双 OS 跑起来后 openvela 的输出整段消失，看起来像 openvela 没起来 ——
实际是它的字被 Linux 的 earlycon 逐个冲掉了。

`earlycon` 不是我加的，是**内核 DTB 自己带的**：`rk3576-linux.dtsi` 的
`/chosen/bootargs` 里有

    earlycon=uart8250,mmio32,0x2ad40000 console=ttyFIQ0 root=PARTUUID=...

而 U-Boot 的 `bootargs_add_dtb_dtbo()`（`arch/arm/mach-rockchip/board.c`）
会把内核 DTB 里的 `/chosen/bootargs` **追加**到 env 的 bootargs 上。所以
在 U-Boot 提示符下 `setenv bootargs 'panic=0'` 是**没用的** —— 内核实际
拿到的仍然带着那个 earlycon。

解法是在 AMP 的 DTS 里覆盖 `/chosen`：

```dts
&chosen {
        bootargs = "panic=0";
};
```

`panic=0` 在 AMP 下有额外的分量：Linux 没有 rootfs，挂载根必然 panic。
panic=0 让它停住而不是重启 —— **重启走 PSCI SYSTEM_RESET，会把整片 SoC
连 openvela 一起复位**。一侧的"正常错误处理"就是另一侧的灾难。

要看 Linux 日志时用 `linux/rk3576-kickpi-k7-amp-dbg.dts`，它把 UART0 让给
Linux，但**只能在不启 openvela 的隔离实验里用**。

## ★ 第四个坑：使能位关着时，mailbox 的状态位不锁存

引导器要等对端的第一次门铃（理由见 `../amp/OWNERSHIP.md` 的 GIC 一节）。
第一版只轮询 `A2B_STATUS`，超时。加上寄存器 dump 才看清：

```
AMP Error: no kick from peer on mailbox3 @0x2ae53000
AMP Error:   A2B INTEN=0x00000100 STATUS=0x00000000
             CMD=0x00000003 DATA=0x524d5347      ← "RMSG"，门铃确实按了
```

`CMD`/`DATA` 都在，`STATUS` 是 0。`INTEN=0x100` 的含义是 bit8（触发模式）
是复位值，而 **bit0（中断使能）是 0** —— TRM 17.3 写的是中断 "Enabled
when MAILBOX_A2B_INTEN is set to 1"，**使能位关着时状态位根本不锁存**。

所以引导器要做的不只是"等"，而是**先替对侧把接收使能打开**，等到门铃，
然后原样留着那一位不清（openvela 关闭再重新使能时不会清 STATUS）。

**这个坑的教训不是 mailbox 的细节，是"没等到"本身信息量为零。** 超时
消息里必须带上寄存器值，否则分不清三件事：对端没发、我们读错了地方、
还是 pclk 没开（pclk 关着时读回来全 0，和"没发"长得一模一样）。

## 运行：已验证的完整命令序列（2026-09-13 跑通）

烧好四样之后（见上面的布局表），串口 **1500000**，一直敲 Ctrl-C 打断
autoboot，然后：

```
=> mmc dev 0
=> mmc read 0x40400000 0xC000 0x39A5    # Linux Image ← LBA 49152
=> mmc read 0x4f000000 0x3800 0x213     # Linux DTB   ← LBA 14336
=> mmc read 0x60000000 0x2000 0x1089    # amp.itb     ← LBA 8192
=> setenv amp_kick_timeout 8000
=> setenv amp_linux_cmd 'booti 0x40400000 - 0x4f000000'
=> bootamp 0x60000000
```

★ `mmc read` 的扇区数每次重新打包都会变，**必须按当前文件大小重算**
（`(size + 511) / 512`）。少读几个扇区的表现是 FIT 的 `Bad Data Hash`，
不是"少了一点数据"。

成功的输出：

```
AMP: linux fdt at 0x4f000000
AMP: Brought up cpu[100] with state 0x12, entry 0x40400000 ...OK
AMP: waiting for peer kick on mailbox3 (max 8000ms, INTEN=0x00000101) ...
     OK, cmd=0x00000003 data=0x524d5347 (kept pending)
AMP: loadables done, bootcpu entry 0x4a400000 boot_on 1
AMP: Brought up primary cpu[0, self] with state 0x12, entry 0x4a400000 ...OK
[CPU0] mailbox: group3 @2ae53000, 收 IRQ 174，已有对端门铃在等（引导器留下的）
...
NuttShell (NSH)
nsh> ampctl status
  握手完成  : 是
  收到门铃  : 1
  /dev/rpmsg/linux 存在
```

## 卡住之后怎么恢复

openvela 卡死时板子既没有串口回显也没有 USB 设备，**只能按 RESET**。
复位后默认 bootcmd 会去启动 boot 分区里的 Android 镜像（已被 Image
覆盖），失败后停在 `=>`，正好可以继续调试 —— 这也是保留默认 bootcmd
的好处之一。

要回到"只跑 NuttX"的状态：把 `nsh` 配置编出来，用
`scripts/flash.sh --raw`（nuttx.bin 2.2MB + stub dtb，都在 32MB 以内），
并把 uboot 分区写回 `~/rk3576-amp/backup/uboot-current-working.img`
（那是带 raw-nuttx bootcmd 的那一份）。
