# 相机页改走 ISP（2026-11-02）

目标：相机页从 RAW 直出（k7cam 软件取色、拉伸）改成经 ISP 出 NV12，由 rkaiq 管曝光和白平衡。

> 截图 `run4/shot-isp-3a.jpg`、`run5/shot-stripes.jpg`、`run5/shot-gain16.jpg` 拍到了人，仓库公开，只留在本地，没有提交。

## 结论（截至目前）

| 环节 | 状态 |
|---|---|
| ISP 出流 | ✓ `k7cam live-isp`：1280x720 NV12，**30.0fps**，首帧在起流后 80ms |
| 显示 | ✓ 相机页 3 秒 86 帧（≈29fps），与 RAW 路径一样 |
| rkaiq 启动 | ✓ 修好（见下），起流时 `rk_aiq_uapi2_sysctl_start success` |
| 3A 生效 | ✓ 修好（见"3A 统计无效"）：Y 均值 10 → 112~117，U/V 128~132，偏绿消失（`run4/shot-isp-3a.jpg`） |

## rkaiq 一启动就段错误（已修）

- 现象：`rkaiq_3A_server` 退出码 139，日志停在 `rk_aiq_uapi2_sysctl_init success`。
- 定位：系统里没有 gdb，busybox 也没有 base64/gzip。把 libSegFault.so 和自写的栈扫描
  小库用 `printf` 八进制转义经 `ampctl exec` 推到 Linux（每条命令约 40 字节）。
  - 栈上落在 librkaiq 里的返回地址，用 addr2line 解出来是
    `socket_client_start → ClientThreadFunc → xcam_print_log → vsnprintf`；
  - 故障地址是 0。
- 原因有两层：
  - 内核没开 `CONFIG_UNIX`（`amp-rootfs.config` 为省空间关了），所以
    rkaiq 初始化时无条件起的调试工具 socket（`socket(AF_LOCAL)`）失败；
  - 厂商这条报错的参数顺序写反了
    （`socket_client.c:144`，格式 `"%s[%d]...%s"` 传的是 `strerror, __func__, cid`），
    vsnprintf 把整数当字符串读。
- 修法：`amp-rootfs.config` 开 `CONFIG_UNIX=y`。Image 从 7754240 涨到 7821824（窗口余 566784）。
  模块没有一个依赖 CONFIG_UNIX（查了各 .o.cmd），不用重编。

## 3A 统计无效（已修）

- 现象：
  - 画面暗而且偏绿（U≈124、V≈122，都低于 128）；
  - rkaiq 每帧报 `aec_stats_valid=false, meas_type=0x0` 和 `AWB stats invalid`；
  - 内核每次起流报 `rkisp-vir2: id:0 no first iq setting cfg_upd:c000dfecc7fe473b en_upd:0 ens:5ffcc7fe473b`。
- 看 `/proc/rkisp-vir2`（`run3/_netsh`）：起流时只有 BLS、DEBAYER、CSM、OB 开着。
  AWBGAIN、CCM、GAMMA、LSC、YNR，连 RAWAE0/3、RAWAWB、RAWHIST 统计块全是 OFF。
- 开 `video_rkisp` 的 debug=4 一秒：此后每帧参数都是 `seq:0 module_en_update:0x0 module_ens:0x3400000000`。
  rkaiq 卡在初始状态：统计块没开就没有统计，没有统计算法就不出新参数。
- 根因（rkaiq，`hwi_c/aiq_CamHwBase.c` 的 setIspParams）：
  - 它把已下发的模块使能缓存在 `_isp_module_ens`，本帧 ens 与缓存相同就把
    `module_en_update` 清 0；
  - 而内核每次 stream on 都复位模块使能，只认新一路流第一个参数里的使能；
  - 3A server 模式下 prepare 在起流前就下发过一次（14.8s 那次 "first params buf queue"
    没有告警），缓存已满，所以每一路流的首个参数 en_update 都是 0。
  - 缓存从不清零（全仓库只有读和按位更新）。
- 修法：`amp/linux/rkaiq/0001-reset-module-ens-on-start.patch`。`AiqManager_start` 开头把
  `mCamHw->_isp_module_ens` 清 0：
  - 重启路径在 `AiqCamHw_start` 之前就 apply 初始参数；
  - 首次启动则在 start 之后出参数；
  - 所以清零要放在 manager 层最前面。
  - `build-rkaiq.sh` 改为在 SDK 源码的硬链接副本（`$OUT/rkaiq-src`）上打补丁，并检查打过的文件已断开硬链接；
    构建前后核对过 SDK 里的 RkAiqManager_c.c md5 不变。
- 修好后（`run4/`）：
  - 内核不再告警；
  - AWBGAIN/LSC/CCM/GAMMA/YNR/RAWAWB/RAWAE0 全 ON；
  - rkaiq 日志里 stats invalid 计数 0；
  - Y 均值第 1 帧 10，第 301 帧起稳定在 112~117（AE 收敛到中灰）；
  - AWB 的 R/B 增益在动；
  - ISP 30fps，屏上 3 秒 86 帧。

## 改动

- `amp/linux/amp-rootfs.config`：`CONFIG_UNIX=y`。
- `amp/linux/lxroot/k7cam.c`：新增 `k7cam live-isp [秒]`。只在一帧都没拿到时返回非 0。
- `app/ampctl/ampctl_main.c`：`cam start` 先 `k7aiq start`，再 `live-isp`；rkaiq 没起来或
  live-isp 失败时退回 RAW 的 `live`。
- `app/kickpi_ui/kickpi_ui_main.c`：没新帧时重发 start 的间隔从 4 秒改为 8 秒（冷启动要先起 rkaiq）。
- `amp/linux/rkaiq/0001-reset-module-ens-on-start.patch`、`build-rkaiq.sh`：见上。

镜像：openvela 10:32:33，Linux 内核 #28 `Mon Nov 2 10:30:51`，lxroot `2026-11-02T15:51:02Z`（带 rkaiq 补丁）。
原始输出在 `run1/` ~ `run4/` 的 `_netsh`。

## 拖影与彩色横纹（run5），固化进镜像

两个问题都在板上直接改 IQ json 验证过（Linux 的 `/etc/iqfiles`，在内存里，重启就没）：

1. **运动物体有拖影**：BAY3D（bayertnr，3D 时域降噪）造成。把 `en` 改成 0 就没了。
2. **暗处有闪烁的彩色横纹**：暗处 AE 把模拟增益拉到 116（约 35dB），行噪声被放大。
   读数是 exp 2025 行、again 116。把 `sw_aeT_gain_dot` 从 `[1,2,8,8,32,256]`
   改成 `[1,2,8,8,16,16]`（增益封顶 16 倍）后，again 停在 80（24dB），横纹消失。
   截图：`run5/shot-stripes.jpg`（改前）、`run5/shot-gain16.jpg`（改后）。

**固化**（2026-11-02）：`amp/linux/rkaiq/tune-iq.py`，由 `build-rkaiq.sh` 在安装 IQ
时调用。json 的三个场景都改（板上手改只改了第一个），每处先数个数，数不对就报错。

板上验证（内核 #29，lxroot `2026-11-03T02:23:41Z`）：
- `/proc/rkisp-vir2`：`BAY3D OFF`；
- 30fps（间隔 33ms）；
- 当时对着的场景偏暗：exp 2025 行（满）、again 80（封顶），均值 Y 17~25。

**代价**：暗处画面更暗、噪点多一些。如果嫌暗，可以考虑：
- 封顶放宽到 32 倍，取折中，要再看横纹；
- 或者降帧率换曝光：15fps 时最长曝光翻倍，不加增益。
