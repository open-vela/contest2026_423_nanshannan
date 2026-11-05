#!/usr/bin/env bash
# 把本队伍仓接进 openvela 工作树。repo sync 之后、编译之前跑一次。
#
# ★ 为什么需要这个脚本
#
#   大赛 manifest 只为每个队伍映射了三个**模板**目录：
#
#     app/hello_app          -> packages/demos/contest2026_423_hello_app
#     quickapp/hello_quickapp-> packages/apps/contest2026_423_hello_quickapp
#     board/contest_board    -> vendor/openvela/boards/contest2026_423_board
#
#   而本作品真正的代码在 board/kickpi-k7/、chip/rk3576/ 和 app/{cam,...}
#   下。manifest 在组委会的 manifests 仓里，我们改不了，所以这些映射
#   必须由本脚本建立 —— 否则 repo sync 之后代码在仓里、却接不进构建树，
#   表现为"配置里找不到板子"，而原因离现象很远。
#
# 幂等：可重复执行。
set -e

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # 队伍仓根
WS="$(cd "$ROOT/.." && pwd)"                              # openvela 工作区
TEAM="$(basename "$ROOT")"

say() { printf '  %-52s %s\n' "$1" "$2"; }

echo "工作区: $WS"
echo

# ── 1) 板级：把模板链接改指到真正的板子 ─────────────────────────────
BOARD_LINK="$WS/vendor/openvela/boards/contest2026_423_board"
mkdir -p "$(dirname "$BOARD_LINK")"
ln -sfn "../../../$TEAM/board/kickpi-k7" "$BOARD_LINK"
say "vendor/openvela/boards/contest2026_423_board" "-> board/kickpi-k7"

# 芯片层不需要链接：defconfig 里用的是相对路径
#   CONFIG_ARCH_CHIP_CUSTOM_DIR="../$TEAM/chip/rk3576"
say "chip/rk3576" "由 ARCH_CHIP_CUSTOM_DIR 相对路径引用"

# ── 2) 应用：链接进 packages/demos 并登记 Kconfig ────────────────────
DEMOS="$WS/packages/demos"
KCFG="$WS/apps/packages/demos/Kconfig"
for app in cam v4l2cap hdmi spi_selftest mic bt kickpi_ui ampctl k7diag ir lxroot mqttd k7ota; do
  [ -d "$ROOT/app/$app" ] || continue
  ln -sfn "../../$TEAM/app/$app" "$DEMOS/contest2026_423_$app"
  say "packages/demos/contest2026_423_$app" "-> app/$app"

  # Kconfig 需要一条绝对路径的 source 行；缺了它 defconfig 里的
  # CONFIG_LVX_USE_DEMO_* 会被 olddefconfig 静默丢掉，编译不报错、
  # 只是命令不存在 —— 这类"配置被丢弃"的失败最难查。
  LINE="source \"$WS/packages/demos/contest2026_423_$app/Kconfig\""
  grep -qF "contest2026_423_$app/Kconfig" "$KCFG" 2>/dev/null || echo "$LINE" >> "$KCFG"
done

# ── 3) 上游补丁 ─────────────────────────────────────────────────────
#
# 这些是提给公共仓的缺陷修复与适配，按大赛规则各自独立提 PR
# （见 bsp/upstream/README）。在 PR 合入之前，本地构建需要先打上。
apply() {   # apply <补丁> <目标仓相对路径>
  local p="$ROOT/bsp/$1" t="$WS/$2"
  [ -f "$p" ] || { say "$1" "缺失，跳过"; return 0; }
  [ -d "$t" ] || { say "$1" "找不到 $2，跳过"; return 0; }
  if git -C "$t" apply --check "$p" 2>/dev/null; then
    git -C "$t" apply "$p" && say "$1" "已应用 -> $2"
  else
    say "$1" "已在树中或冲突，跳过"
  fi
}

apply nuttx-drivers.patch                    nuttx
apply upstream/tftpc-null-blockno.patch      apps
apply upstream/apps.patch                    apps
apply upstream/ai_agent.patch                packages/ai_agent
apply upstream/agent-monotonic-latency.patch packages/ai_agent
apply upstream/agent-skills-mkdir-fat.patch  packages/ai_agent
apply upstream/agent-tls-stale-close-timeout.patch packages/ai_agent
apply upstream/frameworks-uv.patch           frameworks/system/utils/uv
apply upstream/bt-framework-aarch64-build.patch frameworks/connectivity/bluetooth
apply upstream/bt-ipc-client-new-codes.patch frameworks/connectivity/bluetooth
apply upstream/zblue-lp64-stack-align.patch   apps/external/zblue/zblue
apply upstream/zblue-nuttx-smp-headers.patch  apps/external/zblue/zblue
apply upstream/posixspawn-enoent-not-error.patch nuttx
apply upstream/stdio-stream-limit-open-max.patch nuttx
apply upstream/bmi160-spi-only-guard.patch    nuttx
apply upstream/sdio-cmd53-pio-errors.patch   nuttx
apply upstream/gicv2-shared-dist.patch       nuttx
apply upstream/es8388-capture-reset.patch    nuttx
apply upstream/bcm4343x-speed-kconfig.patch  nuttx
apply upstream/xhci-platform-hub.patch      nuttx
# 下面两个叠在 nuttx-drivers.patch 之上（同一文件），必须排在它后面
apply upstream/ft5x06-v2-touchup-lvgl.patch  nuttx
apply upstream/hym8563-kconfig-rtc-external.patch nuttx

# libuv 的补丁不是打进工作副本的：libuv 源码是构建时从 GitHub 拉的，
# 直接改会在干净构建时丢失，必须以 000*.patch 形式放进 apps/system/libuv/
LIBUV="$WS/apps/system/libuv"
SRC="$ROOT/bsp/upstream/0002-nuttx-include-tls-task-header.patch"
if [ -f "$SRC" ] && [ -d "$LIBUV" ]; then
  cp -n "$SRC" "$LIBUV/" 2>/dev/null || true
  say "0002-nuttx-include-tls-task-header.patch" "已放入 apps/system/libuv/"
fi

echo
echo "完成。接下来："
echo "  source $ROOT/scripts/env.sh"
echo "  cd $WS/nuttx && cp $ROOT/board/kickpi-k7/configs/nsh/defconfig .config"
echo "  make olddefconfig && make olddefconfig && make -j\$(nproc)"
