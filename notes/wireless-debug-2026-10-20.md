# openvela 恢复与无线调试（2026-10-20 起）

## 已完成恢复

用户授权刷回 openvela 并开始调试。恢复前板上运行 Sep 30 Linux 6.1.75，
U-Boot 为 Sep 28 版本，没有 bootamp；串口 Linux=115200、U-Boot=1500000。
Linux 曾成功枚举 mmc1:0001:1，运行到 SDR104 198MHz。这是同板软件对照，
不能再把旧记录的“剩余只可能是硬件”当成当前结论。

使用现有 scripts/flash.sh --fit ... --stay 写 FIT，flash-kernel.sh 写 AMP
配套内核，flash-uboot.sh 恢复支持 bootamp 的引导器。全部先备份再回读比较。
AMP DTB 与本地原件一致，未写。没有修改 GPT、SPL 或恢复整盘镜像。

| 部件 | SHA-256 |
|---|---|
| 首次恢复 FIT | 14289e4901d01ed5ce0fde759480aa3dbeac77e8d69b2971f106e3a2c5a052cb |
| 首次恢复 nuttx.bin | d4a24534df1508a2461aa911e0c6a9ee3176f64d2497fd46d6dcd20f3e927f8e |
| AMP U-Boot | fd79b3cc1e9b6e4a36c27fa3ef6a46a8871766c67998966c2eff3f4fec54f2bc |
| AMP Linux | ad0d4d468486febb2cf9e2bf53e52ccd85056c1d0e5c4d2342aebc2588d890d1 |
| AMP DTB（仅核对） | fd2cacb73d8cc6225783dcf20a5af2fe1c421cc0a0a3077323bd3932d0cb2637 |

本地备份：
- FIT: /tmp/k7-amp-fit/flash-20261020T140724Z/lba24576-8192-before.bin
- 内核: /tmp/k7-amp-kernel/flash-20261020T140936Z/lba49152-16384-before.bin
- U-Boot: /tmp/k7-uboot/20261020T140938Z/lba16384-before.bin
- DTB与原始日志: /tmp/k7-restore-20261020/

镜像可能包含本地应用密钥，仅本地保存，不加入仓库。

## 恢复后板上结果

- NSH 正常，uname 构建 Oct 20 2026 09:42:57。
- ampctl status 握手完成，/dev/rpmsg/linux 存在。
- eth0 地址192.168.1.50，TCP2323 NSH可用。
- /dev/mmcsd1 存在；这只证明TF枚举，尚非完整读写回归。
- bt probe 有界返回 -5。CMD52/CMD5 RINTSTS=2，RESP0=0；
  稍后 settled STS=6，说明 CMD_DONE 最终到达但 RE 保留。
- DAT0高电平采样0/64，CMD高64/64。尚未进入固件或HCI阶段。

证据：[串口基线](wireless-debug-2026-10-20/openvela-probe.txt)。
串口1.5M存在丢字，后续数值回读优先用网络NSH。

## 参考核对后待验证的差异

1. Linux dw_mmc-rockchip.c 设置 CIU 源为卡时钟乘2，400kHz枚举对应
   24MHz/30。当前 probe 为24MHz/60；旧“完全一致”解释有误。
   低频未必导致失败，不能直接将此视为根因。
2. Linux sdio-pwrseq 只释放GPIO1_C6；rfkill-bt默认将GPIO1_C7置低。
   当前probe同时拉高C6/C7。Linux运行时是否被用户态unblock还未取证。
3. Linux pinctrl 为GPIO1_12..17设drive-strength=2；当前实际回读
   0x2604602c=0x6666、0x26046030=0x6666，即相关字段为6。
   原probe只配置复用，没有显式配置驱动强度。

第一次尝试通过mw写驱动强度未执行：当前配置禁用了mw，命令明确返回
command not found。该次bt probe只算基线复测，不能声称驱动强度对照失败。
记录：[无效实验](wireless-debug-2026-10-20/drive-ab.txt)。

为继续对照，本地nuttx/.config将CONFIG_NSH_DISABLE_MW改为未设置，
完整重编成功；不改板级默认defconfig、不同时修改驱动逻辑。
后续实验结果追加于下方。

## 后续实板对照结果（UTC 2026-10-22）

本地调试镜像 FIT SHA-256：
`09703faf53446f63e30fd86fc6caa1bd3d2e6909a283a147bc01d326e7b0beba`。
`nuttx.bin` SHA-256：
`8b62fc7a41ae5e916e679137d2e6974f56651e919fd42a25670ceb02809b7300`。
写入已回读一致，复位后NSH、AMP、网络和TF枚举正常。修改范围仅本地
`.config` 启用mw；本轮未把任何未证实的参数改动写入驱动默认路径。

| 实验 | 已验证的改变 | 结果 |
|---|---|---|
| SDIO驱动强度 | 0x2604602c:6666→2222，0x26046030:6666→6622 | bt probe=-5，CMD5 RE、RESP0=0；恢复6666/6666已回读 |
| 原始命令基线 | 模组与控制器先复位，再CMD52读/复位、CMD0、CMD5 | CMD0状态4；CMD52/CMD5状态6、RESP0=0 |
| 时钟分频 | 0x272004a0低6位59→29，读回2fbb→2f9d；CLKDIV仍0 | CMD52/CMD5状态6、RESP0=0 |
| BT保持关闭 | 仅释放GPIO1_C6，GPIO1_C7保持低；GPIO实测6d0bff | CMD52/CMD5状态6、RESP0=0 |

状态6是CMD_DONE和RESP_ERR同时置位，不是成功响应。网络命令读取发生在
命令结束之后，因此与驱动先读到2、稍后读到6一致。三个单变量实验均不能
单独解释故障；组合效应、其他初始化差异和实际电气状态仍未排除。

第一次原始时钟实验在停钟时检测到UPDATE_CLOCK START超过1秒不清，
尚未改分频或复位脚，立即中止。原因表现为DAT0忙时带PRV_DAT_WAIT的
时钟更新不能完成；随后bt probe通过硬复位恢复控制器。第二版实验先将
模组保持复位、复位控制器，再停钟，所有命令受理检查通过。
不能将第一次无效实验计为时钟对照结果。

实验结束用原有bt probe恢复基线，确认：
- 0x2604602c=6666，0x26046030=6666；
- 0x272004a0=2fbb；
- GPIO1_EXT_PORT=00ed0bff，C6/C7均高；
- 网络NSH返回提示符，串口已释放。

证据：
- [驱动强度网络回读](wireless-debug-2026-10-20/drive-verified-net.txt)
- [驱动强度串口日志](wireless-debug-2026-10-20/drive-verified-uart.txt)
- [时钟/复位结果JSON](wireless-debug-2026-10-20/clock-reset-results.json)
- [时钟/复位逐条命令](wireless-debug-2026-10-20/clock-reset-verified-v2-net.txt)
- [恢复状态与TF校验](wireless-debug-2026-10-20/final-state.txt)

### 独立回归与参考核对

`ampctl exec uname -a`成功得到A72 Linux 6.1.75 #17 Sep19构建信息。
从`/dev/mmcsd1`相同起点读取64个512字节块两次到/tmp，两文件均32768字节，
cmp返回0。未写TF或eMMC数据区。这是有限的只读回归，不代表完整压力测试。

RTC I2C2地址0x51寄存器0x0d回读b0，FE=1、FD=0，表示32.768kHz配置已使能；
没有用示波器验证输出波形。SDIO输入施密特寄存器0x26046214/218均ff，
相关输入已使能，与本机Oct2保存的Linux pinconf快照一致。

Linux K7 WiFi DTS没有声明单独vmmc/vqmmc regulator；vcc_1v8_s0和vcc_3v3_s0
是boot-on/always-on的固定供电声明，没有额外GPIO开关。这个源码核对不能
替代模组端电压测量，也未证明PMIC所有运行状态相同。

下一步优先取得能枚举模组的Linux运行时完整基线（SDIO/CRU/IOC/复位GPIO/
PMIC/RTC），再与openvela快照逐项比较；不继续重复已测的单变量。BT固件、
HCI、BLE扫描和WiFi联网仍未实现，bt init仍为ENOSYS。
