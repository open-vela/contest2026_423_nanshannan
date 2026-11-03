#!/usr/bin/env bash
# 生成 kickpi_ui 产品界面用的图标字库 app/kickpi_ui/lv_font_k7_icons_{32,64}.c
#
# 图标取自 LVGL 自带的 FontAwesome 5（OFL / CC BY 4.0），只收界面用到的
# 十几个，码位与 FontAwesome 一致，在 kickpi_ui_main.c 里用 ICON_* 宏引用。
# 32px 用在页签和卡片标题，64px 用在提醒卡片和空状态。
# 后备字体设为 lv_font_k7_cjk_20：页签文字写成 ICON "\n" "今日"，一个标签
# 里图标走本字库、汉字走后备。
# 增删图标：改下面的 ICONS，并同步 kickpi_ui_main.c 的 ICON_* 宏。
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
LVGL_FONTS=${LVGL_FONTS:-$ROOT/../apps/graphics/lvgl/lvgl/scripts/built_in_font}

# sun home microphone cog chair tint eye chevron-right plus check
# door-open sign-out-alt moon brain volume-up code bell unlink
# chevron-left lightbulb snowflake wind plug tv wifi
# video sticky-note
ICONS=0xf185,0xf015,0xf130,0xf013,0xf6c0,0xf043,0xf06e,0xf054,0xf067,0xf00c
ICONS=$ICONS,0xf52b,0xf2f5,0xf186,0xf5dc,0xf028,0xf121,0xf0f3,0xf127
ICONS=$ICONS,0xf053,0xf0eb,0xf2dc,0xf72e,0xf1e6,0xf26c,0xf1eb
ICONS=$ICONS,0xf03d,0xf249

for size in 32 64; do
  out=$ROOT/app/kickpi_ui/lv_font_k7_icons_$size.c
  npx -y lv_font_conv@1.5.3 --size $size --bpp 4 --format lvgl --no-compress \
    --font "$LVGL_FONTS/FontAwesome5-Solid+Brands+Regular.woff" -r "$ICONS" \
    --lv-font-name lv_font_k7_icons_$size --lv-fallback lv_font_k7_cjk_20 \
    -o "$out"
  sed -i "s#^ \\* Opts: .*\$# * Opts: 由 scripts/gen-icon-font.sh 生成（FontAwesome 5，${size}px）#" "$out"
  echo "生成 $out"
done
