#!/usr/bin/env bash
# 编 rkaiq（librkaiq.so，装在 /usr/lib64：这套 glibc 的 ld.so 默认只搜
# /lib64 和 /usr/lib64）和 rkaiq_3A_server，装到 $OUT/rkaiq-install，
# 由 amp/linux/lxroot/build-lxroot.sh 连同 glibc 运行库一起放进 lxroot。
#
#   bash amp/linux/rkaiq/build-rkaiq.sh
#
# 源码用厂商 SDK 里 external/camera_engine_rkaiq（RKAIQ_SRC 可覆盖），不拷进
# 仓库。本目录的 *.patch 打在 $OUT/rkaiq-src 上：那是 SDK 源码的硬链接副本
# （576MB，硬链接不占空间），patch 改文件时是写新文件再改名，不会穿过硬链接
# 改到 SDK。构建在 $OUT/rkaiq-build，SDK 目录不落任何文件。
#
#   0001  每次起流清掉 module_ens 缓存，否则新一路流的首个参数不带模块使能，
#         3A 统计全无效（notes/camera-isp-2026-11-02/）
# 选项照 buildroot 的 camera-engine-rkaiq.mk 里 RK3576 那一段：ISP v39、
# MinSizeRel、CAMGROUP。另外两处：
#   RKAIQ_J2S4B_DEV=OFF   不编"在板上把 IQ json 转 bin"的工具（它用交叉
#                         编译器编出来再在主机上跑，必然失败；我们直接用 json）
#   _LARGEFILE64_SOURCE   见 toolchain.cmake

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HOME/rk3576-amp/out}
SDK=${SDK:-$HOME/openvela-rk3576/docs/Rk3576-SDK/1-SDK软件源码/Linux/sdk/20260320/rk3576-linux-20260320/rk3576-linux}
RKAIQ_SRC=${RKAIQ_SRC:-$SDK/external/camera_engine_rkaiq}
export TC=${TC:-$HOME/rk3576-uboot/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-}
B=$OUT/rkaiq-build
I=$OUT/rkaiq-install

[[ -f $RKAIQ_SRC/rkaiq/CMakeLists.txt ]] || { echo "没有 $RKAIQ_SRC" >&2; exit 1; }

S=$OUT/rkaiq-src
rm -rf "$S"
cp -al "$RKAIQ_SRC" "$S"
for p in "$HERE"/*.patch; do
  patch -d "$S" -p1 --no-backup-if-mismatch -s < "$p"
  # 硬链接要已断开：打过补丁的文件和 SDK 里那份不能是同一个 inode
  for f in $(sed -n 's|^+++ b/||p' "$p"); do
    [[ $(stat -c %i "$S/$f") != $(stat -c %i "$RKAIQ_SRC/$f") ]] ||
      { echo "$f 还和 SDK 共用 inode，停" >&2; exit 1; }
  done
  echo "已打 $(basename "$p")"
done
RKAIQ_SRC=$S

cmake -S "$HERE" -B "$B" \
  -DCMAKE_TOOLCHAIN_FILE="$HERE/toolchain.cmake" \
  -DRKAIQ_SRC="$RKAIQ_SRC" \
  -DBUILDROOT_BUILD_PROJECT=TRUE -DARCH=aarch64 \
  -DISP_HW_VERSION=-DISP_HW_V39 -DRKAIQ_TARGET_SOC=rk3576 \
  -DCMAKE_BUILD_TYPE=MinSizeRel -DRKAIQ_ENABLE_CAMGROUP=TRUE \
  -DRKAIQ_J2S4B_DEV=OFF >"$B.cmake.log" 2>&1 ||
  { tail -30 "$B.cmake.log" >&2; exit 1; }
make -C "$B" -j"$(nproc)" >"$B.make.log" 2>&1 ||
  { grep -a -E "error:|\*\*\*" "$B.make.log" | head -20 >&2; exit 1; }

rm -rf "$I"
mkdir -p "$I/usr/lib64" "$I/usr/bin" "$I/etc/iqfiles"
install -m 755 "$B/rkaiq/all_lib/MinSizeRel/librkaiq.so" "$I/usr/lib64/"
install -m 755 "$B/rkaiq_3A_server/rkaiq_3A_server" "$I/usr/bin/"
"${TC}strip" --strip-unneeded "$I/usr/lib64/librkaiq.so" "$I/usr/bin/rkaiq_3A_server"

# IQ：只放 K7 上这一颗模组的（文件名 = 传感器_模组名_镜头名，与 DT 里
# rockchip,camera-module-name / -lens-name 一致，rkaiq 按这个找）
install -m 644 "$RKAIQ_SRC/rkaiq/iqfiles/isp39/imx415_CMK-OT2022-PX1_IR0147-50IRC-8M-F20.json" \
  "$I/etc/iqfiles/"
# 关 BAY3D、AE 增益封顶 16 倍（理由见 tune-iq.py）
python3 "$HERE/tune-iq.py" "$I/etc/iqfiles/imx415_CMK-OT2022-PX1_IR0147-50IRC-8M-F20.json"

du -sh "$I"/usr/lib64/* "$I"/usr/bin/* "$I"/etc/iqfiles/*
