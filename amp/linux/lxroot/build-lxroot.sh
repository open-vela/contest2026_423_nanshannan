#!/usr/bin/env bash
# 构建 lxroot 镜像：U-Boot 读进内存、内核解在内嵌 initramfs 上面的那个 cpio。
#
#   bash amp/linux/lxroot/build-lxroot.sh
#   python3 scripts/lxroot-push.py ~/rk3576-amp/out/lxroot.cpio
#
# 先跑 amp/linux/rootfs/build-rootfs.sh（它出 Image 和 $OUT/lxroot-modules），
# 要 3A 的话再跑 amp/linux/rkaiq/build-rkaiq.sh。
# 格式与校验见 amp/include/k7_lxroot.h；这一份只负责内容：
#
#   /lib/modules/<版本>/     摄像头驱动（amp/linux/amp-camera.config）
#   /etc/k7-lxroot.d/*.sh    /init 起来后在后台逐个跑（每个限时 60 秒）
#   /usr/bin/k7cam           取图验证工具（k7cam.c）
#   /usr/bin/k7aiq           手动起停 rkaiq_3A_server
#   /usr/bin/k7npu           NPU 手动装驱动、提频和自测（amp/linux/npu/），连同
#                            k7pmic、librknnrt.so、k7npu-test、MobileNet v1 和一张猫图；
#                            要 glibc，所以和 rkaiq 一样只在有 $AIQ 时带上
#   rkaiq + glibc            有 $OUT/rkaiq-install 时带上（amp/linux/rkaiq/
#                            build-rkaiq.sh）：librkaiq.so、rkaiq_3A_server、
#                            IMX415 的 IQ，以及它们要的 glibc/libstdc++ 运行库
#   /etc/k7-lxroot           构建信息，Linux 上 cat 它就知道跑的是哪一版
#
# 镜像是不压缩的 newc cpio：内核只开了 RD_XZ，xz 压缩省空间但 U-Boot 读、
# 内核解都要多花时间，而 eMMC 上的分区有 384MB，不缺地方。

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
OUT=${OUT:-$HOME/rk3576-amp/out}
TC=${TC:-$HOME/rk3576-uboot/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-}
MODS=$OUT/lxroot-modules
AIQ=$OUT/rkaiq-install
RKNPU2=${RKNPU2:-$HOME/openvela-rk3576/docs/Rk3576-SDK/1-SDK软件源码/Linux/sdk/20260320/rk3576-linux-20260320/rk3576-linux/external/rknpu2}
STAGE=$OUT/lxroot-stage
IMG=$OUT/lxroot.cpio

[[ -d $MODS/lib/modules ]] || { echo "没有 $MODS，先跑 build-rootfs.sh" >&2; exit 1; }
KVER=$(ls "$MODS/lib/modules")

rm -rf "$STAGE"
mkdir -p "$STAGE/lib" "$STAGE/etc/k7-lxroot.d" "$STAGE/usr/bin"
cp -a "$MODS/lib/modules" "$STAGE/lib/"

# 装载顺序在主机上按 modules.dep 排好写成清单，板上的 busybox 没有 modprobe。
#
# ★ NPU 的模块（rknpu 和它要的 DRM 核心）剔出来另写 npu.order：rknpu 一
#   probe 就读写 NPU 寄存器，而 NPU 的电源域要等 openvela 打开，开机自动装
#   会把总线挂死。它依赖的 rockchip_iommu 摄像头也要，留在 load.order。
ML=$STAGE/lib/modules/$KVER
"$HERE/modorder.py" "$ML" > "$ML/all.order"
NPU_RE='/(rknpu|drm[a-z_]*)\.ko$'
grep -Ev "$NPU_RE" "$ML/all.order" > "$ML/load.order"
grep -E "$NPU_RE" "$ML/all.order" > "$ML/npu.order" || true
rm "$ML/all.order"

install -m 755 "$HERE/10-camera.sh" "$STAGE/etc/k7-lxroot.d/10-camera.sh"

# 静态链接：内嵌 initramfs 里没有 C 库
"${TC}gcc" -Os -Wall -Wextra -Werror -static -s -I"$REPO/amp/include" \
  -o "$STAGE/usr/bin/k7cam" "$HERE/k7cam.c"

install -m 755 "$HERE/k7aiq" "$STAGE/usr/bin/k7aiq"

# rkaiq 是动态链接的 glibc 程序，内嵌 initramfs 里只有静态 busybox，
# 运行库得自己带。这套 glibc 的 ld.so 默认搜 /lib64 和 /usr/lib64，
# 解释器路径是 /lib/ld-linux-aarch64.so.1。全部 strip，libc 从 12MB 降到 1.4MB。
if [[ -d $AIQ ]]; then
  SYSROOT=$("${TC}gcc" -print-sysroot)
  TCLIB=$(cd "$SYSROOT/../lib64" && pwd)
  mkdir -p "$STAGE/lib64"
  cp -L "$SYSROOT/lib/ld-linux-aarch64.so.1" "$STAGE/lib64/"
  for l in libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1; do
    cp -L "$SYSROOT/lib64/$l" "$STAGE/lib64/$l"
  done
  for l in libstdc++.so.6 libgcc_s.so.1; do
    cp -L "$TCLIB/$l" "$STAGE/lib64/$l"
  done
  "${TC}strip" --strip-unneeded "$STAGE"/lib64/*
  ln -s ../lib64/ld-linux-aarch64.so.1 "$STAGE/lib/ld-linux-aarch64.so.1"
  cp -a "$AIQ"/. "$STAGE/"

  # NPU 运行库和自测。猫图在主机上转成 224x224 NHWC RGB，板上不要 OpenCV。
  if [[ -s $ML/npu.order && -d $RKNPU2 ]]; then
    RTI=$RKNPU2/runtime/Linux/librknn_api
    EX=$RKNPU2/examples/rknn_mobilenet_demo/model
    install -m 755 "$RTI/aarch64/librknnrt.so" "$STAGE/lib64/"
    "${TC}gcc" -O2 -Wall -Wextra -Werror -s -I"$RTI/include" \
      -o "$STAGE/usr/bin/k7npu-test" "$REPO/amp/linux/npu/k7npu-test.c" \
      -L"$RTI/aarch64" -lrknnrt
    install -m 755 "$REPO/amp/linux/npu/k7npu" "$STAGE/usr/bin/k7npu"
    # vdd_npu 升压（k7npu load 提频前用），静态链接
    "${TC}gcc" -Os -Wall -Wextra -Werror -static -s \
      -o "$STAGE/usr/bin/k7pmic" "$REPO/amp/linux/npu/k7pmic.c"
    mkdir -p "$STAGE/usr/share/k7npu"
    cp "$EX/RK3576/mobilenet_v1.rknn" "$STAGE/usr/share/k7npu/"
    python3 -c 'import sys; from PIL import Image
open(sys.argv[2], "wb").write(
    Image.open(sys.argv[1]).convert("RGB").resize((224, 224)).tobytes())' \
      "$EX/cat_224x224.jpg" "$STAGE/usr/share/k7npu/cat.rgb"
  else
    echo "没有 npu.order 或 $RKNPU2，不带 NPU 自测"
  fi
else
  echo "没有 $AIQ，不带 rkaiq（要 3A 先跑 amp/linux/rkaiq/build-rkaiq.sh）"
fi

{
  echo "k7 lxroot $(date -u +%FT%TZ)"
  echo "repo $(git -C "$REPO" describe --always --dirty 2>/dev/null || echo unknown)"
  echo "kernel $KVER"
} > "$STAGE/etc/k7-lxroot"

(cd "$STAGE" && find . | LC_ALL=C sort | cpio -o -H newc --owner=0:0 --quiet) > "$IMG"

printf 'lxroot.cpio: %d 字节，开机装 %d 个模块（另有 NPU %d 个手动装），sha256 %s\n' \
  "$(stat -c %s "$IMG")" "$(grep -c . "$ML/load.order")" \
  "$(grep -c . "$ML/npu.order" || true)" "$(sha256sum "$IMG" | cut -c1-16)"
