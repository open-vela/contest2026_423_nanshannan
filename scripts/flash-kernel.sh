#!/usr/bin/env bash
# 写 A72 簇 Linux 的内核（LBA 49152，amp/layout.sh 的 kernel 区）。
#
# 用法（板子须已在下载模式，通常先跑 flash.sh --stay）：
#   ./scripts/flash-kernel.sh ~/rk3576-amp/out/Image-amp-rootfs
#   ./scripts/flash-kernel.sh ~/rk3576-amp/out/Image-amp         # 换回无用户态的旧内核
#
# 流程与 FIT 一样：先整区备份（到 65536 为止）→ 写 → 回读比对 → 不一致就
# 用备份回滚。写完不复位，要复位用 `rkdeveloptool rd`。
#
# ★ 只能写 kernel 区。2026-09-17 就是往这片区域误写 NuttX 把内核覆盖掉，
#   双系统全挂、单系统正常，查了一整天。所以区间检查不许绕过。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=../amp/layout.sh
source "$ROOT/amp/layout.sh"

RKDEV="${RKDEV:-$HOME/rkdeveloptool/rkdeveloptool}"
img=${1:?用法: flash-kernel.sh <Image>}

# bootamp 固定读 16384 扇区（amp/uboot/0013 的 AMP_KERNEL_CNT；原来 14848）。
# ★ 旧 U-Boot 只读 14848 扇区：Image 超过 7.25MB 时必须先烧新 U-Boot，
#   否则内核尾巴读不进内存，跳进去在随机的地方死。
read_max=16384

size=$(stat -c %s "$img")
sectors=$(( (size + 511) / 512 ))
(( sectors <= read_max )) || {
  echo "Image $sectors 扇区，超出 bootamp 读取窗口 $read_max 扇区" >&2
  exit 1
}
amp_check_write "$AMP_KERNEL_LBA" "$sectors" kernel

timeout 10 "$RKDEV" ld 2>/dev/null | grep -qE 'Loader|Maskrom' || {
  echo "板子不在下载模式（先跑 scripts/flash.sh --stay）" >&2
  exit 1
}

run_dir=/tmp/k7-amp-kernel/flash-$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$run_dir"
region=$(( AMP_KERNEL_END - AMP_KERNEL_LBA ))
backup=$run_dir/lba${AMP_KERNEL_LBA}-${region}-before.bin
padded=$run_dir/image-padded.bin
readback=$run_dir/lba${AMP_KERNEL_LBA}-new-readback.bin

echo "备份内核区 LBA $AMP_KERNEL_LBA..$(( AMP_KERNEL_END - 1 )) → $backup"
"$RKDEV" rl "$AMP_KERNEL_LBA" "$region" "$backup" >/dev/null
[[ $(stat -c %s "$backup") -eq $(( region * 512 )) ]] || {
  echo "备份长度不对，不写" >&2
  exit 1
}
sha256sum "$backup"

# 补到整扇区，回读才能逐字节比
cp "$img" "$padded"
truncate -s $(( sectors * 512 )) "$padded"

rollback() {
  echo "回读不一致，用备份回滚" >&2
  "$RKDEV" wl "$AMP_KERNEL_LBA" "$backup" >/dev/null
  "$RKDEV" rl "$AMP_KERNEL_LBA" "$region" "$run_dir/rollback-readback.bin" >/dev/null
  cmp "$backup" "$run_dir/rollback-readback.bin" && echo "已回滚" >&2
  exit 1
}

echo "写 $img（$sectors 扇区）→ LBA $AMP_KERNEL_LBA"
"$RKDEV" wl "$AMP_KERNEL_LBA" "$padded" >/dev/null || rollback
"$RKDEV" rl "$AMP_KERNEL_LBA" "$sectors" "$readback" >/dev/null || rollback
cmp "$padded" "$readback" || rollback

sha256sum "$img"
echo "内核写入并回读一致。备份：$backup"
