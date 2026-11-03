#!/usr/bin/env bash
# 构建 A72 簇 Linux 的 initramfs 并编进 Image。
#
#   bash amp/linux/rootfs/build-rootfs.sh
#
# 产物：$OUT/Image-amp-rootfs（不覆盖现役的 Image-amp，刷写时显式指定）
#       $OUT/lxroot-modules/   摄像头等模块（amp/linux/amp-camera.config），
#                              由 amp/linux/lxroot/build-lxroot.sh 打进 lxroot 镜像
#
# 可用环境变量覆盖：
#   KERNEL  内核树            默认 ~/rk3576-amp/kernel-6.1
#   WORK    构建目录          默认 ~/rk3576-amp/rootfs
#   OUT     产物目录          默认 ~/rk3576-amp/out
#   BB_TAR  busybox 源码包    默认 buildroot 下载目录里的 1.36.1
#   TC      Linux 交叉前缀    默认 gcc-arm-10.3 aarch64-none-linux-gnu-（和内核同一套）

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
KERNEL=${KERNEL:-$HOME/rk3576-amp/kernel-6.1}
WORK=${WORK:-$HOME/rk3576-amp/rootfs}
OUT=${OUT:-$HOME/rk3576-amp/out}
BB_TAR=${BB_TAR:-$HOME/embedded-lab/buildroot/dl/busybox/busybox-1.36.1.tar.bz2}
TC=${TC:-$HOME/rk3576-uboot/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-}

# U-Boot bootamp 从 LBA 49152 读 16384 扇区（amp/uboot/0013 的 AMP_KERNEL_CNT，
# 原来是 14848）。Image 超过这个数，尾巴读不进内存 —— 内核照样跳进去，然后在
# 随机的地方死。65536 是 rkdeveloptool 写得到的上限，不能再大了。
KERNEL_MAX=$((16384 * 512))

mkdir -p "$WORK" "$OUT"
BB=$WORK/busybox-1.36.1

# ---- 1. busybox：allnoconfig + 白名单 ----
if [[ ! -d $BB ]]; then
  tar xjf "$BB_TAR" -C "$WORK"
fi
(
  cd "$BB"
  make allnoconfig >/dev/null
  grep -v '^#' "$HERE/busybox.applets" | while read -r s; do
    [[ -n $s ]] && sed -i "s/^# CONFIG_$s is not set/CONFIG_$s=y/" .config
  done
  yes "" | make oldconfig >/dev/null 2>&1 || true
  miss=0
  for s in $(grep -v '^#' "$HERE/busybox.applets"); do
    grep -q "^CONFIG_$s=y" .config || { echo "busybox: $s 没打开（名字错或依赖没满足）" >&2; miss=1; }
  done
  (( miss == 0 ))
  make -j"$(nproc)" CROSS_COMPILE="$TC" >/dev/null
  make busybox.links >/dev/null
)

# ---- 2. k7d ----
"${TC}gcc" -Os -Wall -Wextra -Werror -static -s -I"$REPO/amp/include" \
  -o "$WORK/k7d" "$HERE/k7d.c"

# ---- 3. initramfs 清单（gen_init_cpio 格式，不需要 root 权限建设备节点）----
LIST=$WORK/initramfs.list
{
  echo "dir /bin 755 0 0"
  echo "dir /sbin 755 0 0"
  echo "dir /usr 755 0 0"
  echo "dir /usr/bin 755 0 0"
  echo "dir /usr/sbin 755 0 0"
  echo "dir /proc 755 0 0"
  echo "dir /sys 755 0 0"
  echo "dir /dev 755 0 0"
  echo "dir /tmp 1777 0 0"
  echo "dir /root 700 0 0"
  # devtmpfs 挂上之前 /init 就要用 /dev/null
  echo "nod /dev/console 600 0 0 c 5 1"
  echo "nod /dev/null 666 0 0 c 1 3"
  echo "file /init $HERE/init 755 0 0"
  echo "file /bin/busybox $BB/busybox 755 0 0"
  echo "file /sbin/k7d $WORK/k7d 755 0 0"
  while read -r l; do
    [[ $l == /bin/busybox ]] && continue
    echo "slink $l /bin/busybox 777 0 0"
  done < "$BB/busybox.links"
} > "$LIST"

# ---- 4. 内核 ----
make_k() { make -C "$KERNEL" ARCH=arm64 CROSS_COMPILE="$TC" "$@" </dev/null; }

cp "$KERNEL/.config" "$WORK/kernel.config.before"
"$KERNEL/scripts/kconfig/merge_config.sh" -m -O "$KERNEL" "$KERNEL/.config" \
  "$REPO/amp/linux/amp-rootfs.config" "$REPO/amp/linux/amp-camera.config" \
  "$REPO/amp/linux/amp-npu.config" >/dev/null
"$KERNEL/scripts/config" --file "$KERNEL/.config" \
  --set-str INITRAMFS_SOURCE "$LIST" \
  --enable INITRAMFS_COMPRESSION_XZ
make_k olddefconfig >/dev/null
grep -q '^CONFIG_INITRAMFS_COMPRESSION_XZ=y' "$KERNEL/.config"
grep -q '^CONFIG_RPMSG_CHAR=y' "$KERNEL/.config"
grep -q '^CONFIG_MODULES=y' "$KERNEL/.config"
# 摄像头以外的 IOMMU 必须在 dts 里关掉（vop_mmu 是 openvela 的屏幕），
# 这里只能提醒：配置里打开了 IOMMU，dts 就得跟上。
grep -q '^CONFIG_ROCKCHIP_IOMMU=m' "$KERNEL/.config"
# rknpu 必须是模块（开机不能自动 probe），DRM 显示驱动必须不开
grep -q '^CONFIG_ROCKCHIP_RKNPU=m' "$KERNEL/.config"
# （set -e 不管 "! 命令"，所以写成 if）
if grep -q '^CONFIG_DRM_ROCKCHIP=' "$KERNEL/.config"; then
  echo "DRM_ROCKCHIP 被打开了：显示归 openvela，Linux 不能碰 vop" >&2
  exit 1
fi

make_k -j"$(nproc)" Image modules >/dev/null

MODS=$OUT/lxroot-modules
rm -rf "$MODS"
make_k INSTALL_MOD_PATH="$MODS" INSTALL_MOD_STRIP=1 modules_install >/dev/null

IMG=$KERNEL/arch/arm64/boot/Image
size=$(stat -c %s "$IMG")
if (( size > KERNEL_MAX )); then
  echo "Image $size 字节，超出 bootamp 窗口 $KERNEL_MAX 字节" >&2
  exit 1
fi

cp "$IMG" "$OUT/Image-amp-rootfs"
cp "$KERNEL/.config" "$OUT/kernel-amp-rootfs.config"
printf 'Image-amp-rootfs: %d 字节（窗口 %d，余 %d）sha256 %s\n' \
  "$size" "$KERNEL_MAX" $((KERNEL_MAX - size)) \
  "$(sha256sum "$IMG" | cut -c1-16)"
printf 'lxroot-modules: %d 个 .ko\n' "$(find "$MODS" -name '*.ko' | wc -l)"
