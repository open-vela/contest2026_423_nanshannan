#!/usr/bin/env bash
# 生成 kickpi_ui 用的大号数字字库 app/kickpi_ui/lv_font_k7_digits_{72,48}.c
# 只含数字和少量符号，4bpp，几十 KB。中文字库只有 20px，放大
# 变换会糊且每帧都要重新缩放，所以时钟单独出一套只有数字的大字库。
# 字体：Montserrat-Medium（OFL，LVGL 自带，与界面其余数字字形一致）。
# 依赖：node（npx 拉 lv_font_conv@1.5.3）。
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
LVGL_FONTS=${LVGL_FONTS:-$ROOT/../apps/graphics/lvgl/lvgl/scripts/built_in_font}
# 72px：首页时钟；48px：卡片里的大数字（分钟数、杯数，多了 % /）
gen() {
  local size=$1 syms=$2 out=$ROOT/app/kickpi_ui/lv_font_k7_digits_$1.c
  npx -y lv_font_conv@1.5.3 --size "$size" --bpp 4 --format lvgl --no-compress \
    --font "$LVGL_FONTS/Montserrat-Medium.ttf" --symbols "$syms" \
    --lv-font-name "lv_font_k7_digits_$size" \
    -o "$out"
  # 头部 Opts 行带本机绝对路径，换成一句说明
  sed -i "s#^ \\* Opts: .*\$# * Opts: 由 scripts/gen-clock-font.sh 生成（Montserrat-Medium ${size}px，仅 $syms）#" "$out"
  echo "生成 $out"
}
gen 72 "0123456789:-"
gen 48 "0123456789:-%/"
