#!/usr/bin/env bash
# 生成 kickpi_ui 标题/按钮用的 28px 中文字库 app/kickpi_ui/lv_font_k7_ui_28.c
#
# FIT 窗口只有 4MB，固件本体已 3.7MB，放不下整套 GB2312 一级汉字的 28px
# 字库（约 850KB）。所以这一套只收 kickpi_ui_main.c 字符串字面量里出现过
# 的字（几百个，约 150KB），正文和模型回答仍用全集的 lv_font_k7_cjk_20。
# 字库后备设为 lv_font_k7_cjk_20：漏收的字退回 20px 显示，不会变方框。
# 改了界面文字要重跑本脚本（check-ui-glyphs.py 会提示）。
#
# 字体来源同 gen-cjk-font.sh。依赖：node（npx 拉 lv_font_conv@1.5.3）。
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
LVGL_FONTS=${LVGL_FONTS:-$ROOT/../apps/graphics/lvgl/lvgl/scripts/built_in_font}
WORK=${WORK:-$HOME/rk3576-amp/font}
DROID=$WORK/pkg/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf
OUT=$ROOT/app/kickpi_ui/lv_font_k7_ui_28.c

[[ -f $DROID ]] || { echo "先跑 scripts/gen-cjk-font.sh 取 Droid 字体"; exit 1; }

# 界面字面量里的非 ASCII 字符；DejaVu 那几个符号单独给
PUNCT="“”‘’…—·×÷°→←↑↓✓✗℃"
CJK=$(python3 - "$ROOT/app/kickpi_ui/kickpi_ui_main.c" "$PUNCT" <<'PY'
import re, sys
code = re.sub(r"/\*.*?\*/", "", open(sys.argv[1], encoding="utf-8").read(), flags=re.S)
s = set()
for lit in re.findall(r'"((?:[^"\\\n]|\\.)*)"', code):
    s |= {c for c in lit if ord(c) >= 0x80 and c not in sys.argv[2]}
sys.stdout.write("".join(sorted(s)))
PY
)

npx -y lv_font_conv@1.5.3 --size 28 --bpp 4 --format lvgl --no-compress \
  --font "$LVGL_FONTS/Montserrat-Medium.ttf" -r 0x20-0x7E \
  --font "$LVGL_FONTS/DejaVuSans.ttf" --symbols "$PUNCT" \
  --font "$DROID" --symbols "$CJK" \
  --lv-font-name lv_font_k7_ui_28 --lv-fallback lv_font_k7_cjk_20 \
  -o "$OUT"
python3 - "$OUT" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
s = re.sub(r' \* Opts: .*\n',
           ' * Opts: 由 scripts/gen-ui-font.sh 生成（ASCII + 界面字面量用到的汉字）\n'
           ' * Fonts: Montserrat (OFL), DejaVu Sans, Droid Sans Fallback (Apache-2.0)\n',
           s, count=1)
open(p, 'w', encoding='utf-8').write(s)
PY
echo "生成 $OUT"
