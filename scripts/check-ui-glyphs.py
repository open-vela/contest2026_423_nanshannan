#!/usr/bin/env python3
"""检查 kickpi_ui 界面字符串里有没有字库 lv_font_k7_cjk_20 缺的字形。

缺的字在屏上显示成方框（例：等待提示里的"→"、唤醒提示里的"「」"）。
改界面文字后跑一下：python3 scripts/check-ui-glyphs.py
缺字时退出码为 1；补字形改 scripts/gen-cjk-font.sh 后重新生成字库。
只查源码里的字符串字面量（跳过注释）；模型回答里的生僻字查不到。
"""
import pathlib
import re
import sys

ui = pathlib.Path(__file__).resolve().parent.parent / "app" / "kickpi_ui"
font = (ui / "lv_font_k7_cjk_20.c").read_text(encoding="utf-8")
have = {int(x, 16) for x in re.findall(r"/\* U\+([0-9A-F]+)", font)}

# 会上屏的字符串所在的文件：界面本身，以及错误提示会作为回答气泡显示的
# k7_vision.c。k7_wake.c 里是唤醒词匹配表、k7_tts.c 只打日志，不查。
SHOWN = ["kickpi_ui_main.c", "k7_vision.c", "k7_llm.c", "k7_studio.c", "k7_memo.c"]

missing = {}
for src in (ui / name for name in SHOWN):
    code = re.sub(r"/\*.*?\*/", "", src.read_text(encoding="utf-8"), flags=re.S)
    for lit in re.findall(r'"((?:[^"\\\n]|\\.)*)"', code):
        for ch in lit:
            if ord(ch) >= 0x80 and ord(ch) not in have:
                missing.setdefault(ch, set()).add(f"{src.name}: {lit[:40]}")

# LV_SYMBOL_* 是 FontAwesome 图标（私有区码位），只在 LVGL 自带的
# Montserrat 里有。设了中文字库的标签上用它会显示成方框（DEV 页的 ✓
# 就这样坏过）—— 宏展开后才是字符，上面的字面量扫描看不到。
# 按变量名对：设过中文字库的标签变量，不能再 set_text 成 LV_SYMBOL_*。
def base(expr):
    return re.sub(r"\[.*?\]", "", expr).strip()

# 全局变量（g_ 开头）整个文件对；局部变量只在同一个函数里对 —— 不同
# 函数里同名的 lbl 各是各的。
FONT_RE = r"lv_obj_set_style_text_font\(\s*([^,]+),\s*&lv_font_k7_cjk_20"
SYM_RE = r"lv_label_set_text(?:_fmt)?\(\s*([^,]+),([^;]*LV_SYMBOL_[^;]*);"

# 字库设了 Montserrat 后备（gen-cjk-font.sh 的 --lv-fallback）时，图标会在
# 后备里找到，这条检查就不需要了。
HAS_FALLBACK = ".fallback = &lv_font_montserrat" in font

for name in ([] if HAS_FALLBACK else SHOWN):
    code = re.sub(r"/\*.*?\*/", "", (ui / name).read_text(encoding="utf-8"),
                  flags=re.S)
    funcs = re.split(r"\n(?=[a-z][^\n;]*\([^\n;]*\)\s*\n\{)", code)
    glob = {base(v) for v in re.findall(FONT_RE, code)
            if base(v).startswith("g_")}
    for fn in funcs:
        local = {base(v) for v in re.findall(FONT_RE, fn)}
        for var, line in re.findall(SYM_RE, fn):
            v = base(var)
            if v in glob or (not v.startswith("g_") and v in local):
                missing.setdefault("LV_SYMBOL_*", set()).add(
                    f"{name}: {v} ←{line.strip()[:40]}")

for ch, where in sorted(missing.items()):
    tag = ch if len(ch) > 1 else f"U+{ord(ch):04X} {ch}"
    print(f"缺 {tag}  <- " + " | ".join(sorted(where)))
print(f"字库 {len(have)} 个字形，界面缺字 {len(missing)} 个")
sys.exit(1 if missing else 0)
