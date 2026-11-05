# shellcheck shell=bash
# AMP 的 eMMC 布局：主机侧脚本的唯一来源。
#
# 用法：source amp/layout.sh
#
# U-Boot 那边写死在 cmd/bootamp.c 的 AMP_*_LBA / AMP_*_CNT 里，
# 改这里必须同步改那里（补丁见 amp/uboot/0008）并重编 U-Boot。
#
# GPT（2026-09-17 从板上读出）：
#   security   8192 ..  16383   4MB   dtb@14336（OP-TEE 安全存储也用这个分区）
#   uboot     16384 ..  24575   4MB   U-Boot
#   trust     24576 ..  32767   4MB   救援 FIT（openvela）+ 末尾 8 扇区 A/B 控制块
#   misc      32768 ..  40959   4MB   ✗ U-Boot 每次启动写 BCB，不能借
#   dtbo      40960 ..  49151   4MB   开机 logo 的 resource 镜像（amp/uboot/0011）
#   vbmeta    49152 ..  51199   1MB   ┐ Linux Image-amp（7.2MB）
#   boot      51200 .. 182271  64MB   ┘ 从 49152 连续放
#   recovery 182272 .. 378879  96MB   openvela A/B 槽（各 32MB），只能由 openvela 经网络写
#
# ★ rkdeveloptool 对 LBA >= 65536 的写静默丢弃（amp/FLASH.md），
#   所以全部部件都必须在 32MB 以内。
#
# ★ 2026-09-17 事故：旧 flash.sh 按单系统布局往 LBA 51200 写 NuttX，
#   把 49152 起的 AMP 内核从 1MB 处覆盖了。双系统从此每次卡死在 NSH
#   横幅附近，而单系统完全正常，连 09-13 验证过的 FIT 都一样挂 ——
#   git 回退救不了，因为坏的是 eMMC 上 FIT 之外的东西。
#   所有写盘脚本都必须先过 amp_check_write。

AMP_FIT_LBA=24576
AMP_FIT_MAX=8184          # 扇区；32760 起 8 扇区是 A/B 控制块（amp/include/k7_bootab.h）
AMP_ABCTL_LBA=32760
AMP_ABCTL_CNT=8           # 到 misc 起点 32768 为止
AMP_DTB_LBA=14336
AMP_DTB_MAX=544
AMP_UBOOT_LBA=16384
AMP_LOGO_LBA=40960        # dtbo 分区，到 vbmeta 起点 49152 为止
AMP_KERNEL_LBA=49152
AMP_KERNEL_END=65536      # 不含；rkdeveloptool 写得到的上限

# amp_check_write <lba> <扇区数> <允许的用途>
#   用途：fit | abctl | dtb | uboot | logo | kernel
#   区间必须完整落在该用途自己的区域里，否则拒绝。
amp_check_write() {
  local lba=$1 cnt=$2 what=$3 lo hi
  case "$what" in
    fit)    lo=$AMP_FIT_LBA;    hi=$((AMP_FIT_LBA + AMP_FIT_MAX)) ;;
    abctl)  lo=$AMP_ABCTL_LBA;  hi=$((AMP_ABCTL_LBA + AMP_ABCTL_CNT)) ;;
    dtb)    lo=$AMP_DTB_LBA;    hi=$((AMP_DTB_LBA + AMP_DTB_MAX)) ;;
    uboot)  lo=$AMP_UBOOT_LBA;  hi=24576 ;;
    logo)   lo=$AMP_LOGO_LBA;   hi=$AMP_KERNEL_LBA ;;
    kernel) lo=$AMP_KERNEL_LBA; hi=$AMP_KERNEL_END ;;
    *) echo "amp_check_write: 未知用途 $what" >&2; return 1 ;;
  esac
  if (( cnt <= 0 || lba < lo || lba + cnt > hi )); then
    echo "拒绝写入：LBA $lba..$((lba + cnt - 1)) 不在 $what 区 $lo..$((hi - 1)) 内" >&2
    return 1
  fi
}
