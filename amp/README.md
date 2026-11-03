# AMP：openvela 跑 A53 簇，Linux 跑 A72 簇

RK3576 是 4×A53 + 4×A72。AMP 就是让两个簇各跑一个**完整的操作系统**，
同时在跑，共享同一片 DDR 和同一个 GIC。

```
        ┌──────────────── RK3576 ────────────────┐
        │  A53 簇 (MPIDR 0x000..0x003)           │
        │     openvela / NuttX   @ 0x4a400000    │
        ├────────────────────────────────────────┤
        │  A72 簇 (MPIDR 0x100..0x103)           │
        │     Linux 6.1          @ 0x40400000    │
        └────────────────────────────────────────┘
              共享：GIC（Linux 拥有 distributor）
                    mailbox group0/3（门铃）
                    0x47800000 4MB（vring + 缓冲池）
```

## 目录

| 路径 | 是什么 |
|---|---|
| `uboot/0004-amp-boot-from-ram.patch` | U-Boot：新增 `bootamp <addr>` 命令 |
| `uboot/0005-display-without-kernel-dtb.patch` | U-Boot：用自己的完整控制 dtb 点亮 5 寸 MIPI 屏 |
| `uboot/0006-bootamp-load-from-emmc.patch` | U-Boot：`bootamp` 不带参数时自己从 eMMC 读三块料 |
| `uboot/0007-do-not-wait-for-peer-kick.patch` | U-Boot：不再在「Linux 已跑、U-Boot 未退」的窗口里干等 |
| `linux/rk3576-kickpi-k7-amp.dts` | Linux 的 DTB：只保留 A72 的四个 cpu 节点 |
| `linux/0001-gic-keep-amp-irqs-enabled.patch` | Linux：gic_dist_config() 别把对端 OS 已使能的 SPI 关掉 |
| `linux/amp.config` | Linux 的配置增量 |
| `fit/amp.its` | 哪个镜像跑在哪颗核上 —— AMP 的全部契约 |

openvela 那一侧在 `../chip/rk3576/`（mailbox + rptun）和
`../board/kickpi-k7/configs/amp-dual/`。

**谁拥有哪个外设，见 [OWNERSHIP.md](OWNERSHIP.md)** —— AMP 里没有硬件
隔离，两边都去初始化同一个控制器的后果不是报错，而是"某一侧偶发地不
工作"，现场看起来像那一侧自己的 bug。

## 启动是怎么分核的

U-Boot 跑在 MPIDR 0（A53 core0）。`bootamp` 读 FIT，按每个镜像的
`cpu` 属性分核：

1. `linux` 节点 `cpu = <0x100>` → 不是当前核，所以走
   `load_linux_for_nonboot_cpu()`：跑 `boot_fit`/`boot_android` 从 **boot
   分区**把内核和 DTB 读进来，然后 `psci_cpu_on(0x100, entry)`。
2. `loadables` 里的 openvela `cpu = <0x000>` → 正是当前核，于是只记下来。
3. 全部处理完，当前核 `armv8_switch_to_el2()` 跳进 openvela。

**顺序是 Linux 先起**，这不是巧合：GIC 的 distributor 归 Linux，它得先
配完，openvela 那边带着 `CONFIG_ARM64_GICV2_SHARED_DIST=y` 才能接着用。
U-Boot 在 PSCI 之前还会 `setup_sync_bits_for_linux()` 关掉 GICD 并清一个
优先级寄存器，那是 Rockchip 定的交接约定。

两个簇里**剩下的三颗核不在 FIT 里**，各自的 OS 起来以后自己拉
（openvela 走 `arm64_cpustart.c`，Linux 走它自己的 DTB）。

## 为什么不用官方那条路

官方 `amp_cpus_on()` 写死了从**名为 "amp" 的分区**读 FIT，而且在
`rk_board_late_init()` 里无条件执行。板上这张 GPT 是 Android 的，没有
amp 分区，要用就得改分区表 —— 而改分区表和刷 bootloader 一样只能靠
maskrom 恢复。调试阶段不值得为一个还没跑通的引导流程押上它。

`bootamp` 把 FIT 的来源换成内存，分区表一个字节不动；而且它是**手动**
命令，默认 bootcmd 不变，AMP 试炸了下一次上电仍然走原来的路。

## 怎么构建

```bash
# 1. Linux（A72 簇）
K=~/rk3576-amp/kernel-6.1
cp amp/linux/rk3576-kickpi-k7-amp.dts $K/arch/arm64/boot/dts/rockchip/
# 并在该目录的 Makefile 里加一行 dtb-$(CONFIG_ARCH_ROCKCHIP) += rk3576-kickpi-k7-amp.dtb
cd $K && make ARCH=arm64 CROSS_COMPILE=$TC rockchip_linux_defconfig rk3576.config
./scripts/kconfig/merge_config.sh -m -O . .config <本仓>/amp/linux/amp.config
make ARCH=arm64 CROSS_COMPILE=$TC olddefconfig
make ARCH=arm64 CROSS_COMPILE=$TC Image rockchip/rk3576-kickpi-k7-amp.dtb -j

# 2. 打成 boot 分区能认的 Android 镜像
python3 scripts/repack-bootimg.py ~/boot-orig.img $K/arch/arm64/boot/Image out/boot-linux-amp.img \
    --dtb $K/arch/arm64/boot/dts/rockchip/rk3576-kickpi-k7-amp.dtb \
    --cmdline "panic=0 rootwait ro printk.devkmsg=on kvm-arm.mode=none"

# 3. openvela（A53 簇）
cd nuttx
./tools/configure.sh -e ../vendor/openvela/boards/contest2026_423_board/configs/amp-dual
make -j

# 4. 带 AMP 的 U-Boot
cd ~/rk3576-amp/u-boot
patch -p1 < <本仓>/amp/uboot/0004-amp-boot-from-ram.patch
patch -p1 < <本仓>/amp/uboot/0005-display-without-kernel-dtb.patch
patch -p1 < <本仓>/amp/uboot/0006-bootamp-load-from-emmc.patch
patch -p1 < <本仓>/amp/uboot/0007-do-not-wait-for-peer-kick.patch
./make.sh rk3576-kickpi-k7 CROSS_COMPILE=$TC   # 这个配置自带 BOOTDELAY=3

# 主机需要 dtc（内核树里有一份：kernel-6.1/scripts/dtc/dtc），
# make.sh 还会检查 `python2` 是否存在 —— 它其实没被用到，
# 给一个转发到 python3 的壳脚本就行。

# 5. AMP FIT
cp nuttx.bin amp/fit/openvela-amp.bin
cd amp/fit && mkimage -f amp.its -E -p 0xe00 amp.itb
```

## 怎么烧、怎么跑

> ★ 下面这段是最早一版（boot.img 装 46MB Linux）的烧法，**已经作废**，
> 按它写会把现在 LBA 49152 起的 AMP 内核覆盖掉（2026-09-17 真出过事）。
> 现行布局和烧法见 [FLASH.md](FLASH.md) 与 `layout.sh`，日常只用
> `scripts/flash.sh`。

```bash
# 作废，勿用
rkdeveloptool wl  16384 uboot-amp.img        # uboot 分区，4MB
rkdeveloptool wl  51200 boot-linux-amp.img   # boot 分区 ← Linux
rkdeveloptool wl 182272 amp.itb              # 借 recovery 分区放 ← openvela
```

`misc` 分区（LBA 32768）里如果留着 Android 的 BCB，U-Boot 会按它去启动
recovery。清掉它再跑。

上电后串口 **1500000**（U-Boot 与 Android 都是这个速率，NuttX 是 115200），
一直敲 Ctrl-C 打断 autoboot，然后：

```
=> mmc dev 0
=> mmc read 0x60000000 0x2C800 0x1100
=> bootamp 0x60000000
```

成功的判据不是"看到 Linux 的日志"——这一版 Linux 没有控制台（UART0 归
openvela）。判据是 openvela 的 NSH 里：

```
nsh> ampctl status
  握手完成  : 是
  收到门铃  : 1
```

那个门铃只有在 Linux 跑到 `rockchip_rpmsg` 的 probe、建好两个 virtqueue、
填好接收缓冲之后才会发出来 —— 是一个相当靠后、相当可信的存活判据。

## 状态：✅ 跑通（2026-09-13）

**openvela 在 A53 簇、Linux 在 A72 簇，同时运行，rpmsg 传输已建立。**

openvela 侧（`nsh>` 提示符下）：

```
nsh> ampctl status
== rptun ==
  已注册    : 是
  握手完成  : 是
  收到门铃  : 1
  最后收到  : cmd=00000003 data=524d5347
== 共享内存 ==
  vring0(发) : 47800000   vring1(收) : 47808000   缓冲池 : 47a00000 + 2048K
== rpmsg 设备 ==
  /dev/rpmsg/linux 存在

nsh> ls /dev/rpmsg
/dev/rpmsg:
 linux

nsh> ps
  PID  PRI POLICY  TYPE     STATE     COMMAND
    0    0 FIFO    Kthread  Assigned  CPU0 IDLE
    1    0 FIFO    Kthread  Running   CPU1 IDLE
    2    0 FIFO    Kthread  Running   CPU2 IDLE
    3    0 FIFO    Kthread  Running   CPU3 IDLE      ← 四个 A53
    9  224 RR      Kthread  Waiting   rpmsg-linux-0  ← rpmsg 工作线程
```

引导器侧：

```
AMP: linux fdt at 0x4f000000
AMP: Brought up cpu[100] with state 0x12, entry 0x40400000 ...OK
AMP: waiting for peer kick on mailbox3 (max 8000ms, INTEN=0x00000101) ...
     OK, cmd=0x00000003 data=0x524d5347 (kept pending)
AMP: loadables done, bootcpu entry 0x4a400000 boot_on 1
AMP: Brought up primary cpu[0, self] with state 0x12, entry 0x4a400000 ...OK
[CPU0] mailbox: group3 @2ae53000, 收 IRQ 174，已有对端门铃在等（引导器留下的）
```

Linux 侧（隔离实验时把 UART0 让给它抓到的）：

```
[   18.818583] Booting Linux on physical CPU 0x0000000100 [0x411fd080]
[   19.043478] CPU1: Booted secondary processor 0x0000000101
[   19.044502] CPU2: Booted secondary processor 0x0000000102
[   19.045474] CPU3: Booted secondary processor 0x0000000103
[   19.050400] SMP: Total of 4 processors activated.        ← 只拉 A72，没碰 A53
rockchip-rpmsg 47800000.rpmsg: rpdev vdev0: vring0 0x47800000, vring1 0x47808000
virtio_rpmsg_bus virtio0: rpmsg host is online
```

| 步骤 | 状态 |
|---|---|
| 带 `bootamp` 的 U-Boot | ✅ |
| Linux 内核 + 只含 A72 的 DTB | ✅ 7.6MB（原 43MB） |
| openvela @ 0x4a400000 | ✅ |
| U-Boot 把两个簇分给两个 OS | ✅ |
| openvela 在 Linux 旁边活到 NSH | ✅ |
| **rpmsg 握手 + /dev/rpmsg/linux** | ✅ |
| 端到端收发一帧（需要 Linux 侧用户态） | ⬜ 下一步 |

烧写与启动步骤、以及踩过的坑，见 [FLASH.md](FLASH.md)；
谁拥有哪个外设见 [OWNERSHIP.md](OWNERSHIP.md)。

## Linux 用户态（2026-09-18 上板验证）

Linux 侧不再是「只有内核、挂根失败就停」：Image 里内嵌 initramfs
（静态 busybox + 守护进程 `k7d`），开机约 3.1s 进用户态。openvela 的 nsh 里：

```
nsh> ampctl exec uname -a
[Linux 6.1.75 aarch64 on k7-linux, up 71s, 53 procs]
Linux k7-linux 6.1.75 #15 SMP Fri Sep 18 21:42:37 +08 2026 aarch64
nsh> ampctl exec "grep 'CPU part' /proc/cpuinfo"     # 4 × 0xd08 = Cortex-A72
```

链路：`ampctl exec` 建名为 `rpmsg-raw` 的端点 → Linux 名字服务建 channel →
`rpmsg_char` 生成 `/dev/rpmsgN` → `k7d` 打开并先发 HELLO（openvela 这才知道
对端地址）→ 执行 `sh -c`，输出逐帧回传，最后一帧带退出码。协议见
`linux/rootfs/k7d.c` 开头。实测往返约 50ms，16KB 的 dmesg 全量不丢，连跑 20 次无失败。

| 文件 | 作用 |
|---|---|
| `linux/amp-rootfs.config` | 内核增量：INITRD(XZ)、NET 核心（不要协议栈）、RPMSG_CHAR、`-Os` |
| `linux/rootfs/busybox.applets` | busybox 白名单（allnoconfig 起，78 个 applet） |
| `linux/rootfs/k7d.c` / `init` | 守护进程和 /init |
| `linux/rootfs/build-rootfs.sh` | 一键构建，产物 `~/rk3576-amp/out/Image-amp-rootfs` |
| `../scripts/flash-kernel.sh` | 写 LBA 49152（整区备份、回读、失败回滚） |
| `linux/amp-camera.config` / `linux/0003-camera-as-modules.patch` | 摄像头链（IMX415/D-PHY/CIF/ISP/VPSS/IOMMU）编成模块 |
| `linux/amp-npu.config` / `linux/0004-rknpu-as-module.patch` | NPU：DRM 核心 + rknpu 编成模块，**开机不装**（见 `../notes/npu-2026-11-02/`） |
| `linux/lxroot/build-lxroot.sh` | lxroot 镜像（U-Boot 当 initrd 带进来）：模块 + 10-camera.sh 钩子 + k7cam/k7aiq + rkaiq 与 glibc 运行库 + NPU 自测（k7npu、librknnrt、MobileNet v1） |
| `linux/npu/k7npu` / `k7npu-test.c` | NPU 手动装驱动（先读 PMU 确认电源域）和一次静态图推理 |
| `linux/rkaiq/build-rkaiq.sh` | 用 glibc 工具链从 SDK 源码编 librkaiq.so 和 rkaiq_3A_server（ISP v39） |

**体积是唯一硬约束**：bootamp 固定读 16384 扇区（8MB，uboot/0013；原来
14848 扇区 7.25MB）。打开 MODULES+IOMMU 后 Image 7754240 字节，余 634KB；
摄像头等大块驱动编成模块放进 lxroot，不进 Image。为什么不用 eMMC 上的根分区、不用 SD 卡：Linux 这边没有也
不该有 MMC 驱动（eMMC 归 openvela），见 `linux/amp-rootfs.config` 开头。
