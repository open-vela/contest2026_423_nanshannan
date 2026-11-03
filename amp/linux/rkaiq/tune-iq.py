#!/usr/bin/env python3
# 在 SDK 自带的 IMX415 IQ json 上改两处，改的是安装目录里的副本，不动 SDK：
#
#   tune-iq.py <iq.json>
#
# 1. 关 BAY3D（bayertnr，3D 时域降噪）。
#    这块模组上它对移动物体留下明显拖影（notes/camera-isp-2026-11-02 run5）。
#    关掉后暗处噪点多一些，但没有拖影。
# 2. AE 增益上限从 256 倍降到 16 倍（sw_aeT_gain_dot 的最后两档）。
#    暗处 AE 把模拟增益拉到 116（约 35dB），行噪声被放大成彩色横纹；
#    封顶后暗处会偏暗，但横纹消失（同一份笔记，shot-gain16.jpg）。
#
# json 里有三个场景，每处都要改。用文本替换而不是 json 重写，保持原格式，
# 和板上手改验证过的那一版逐字一致；每处都先数个数，数不对就报错，
# 防止 SDK 换了 IQ 之后静默不生效。

import re
import sys

path = sys.argv[1]
s = open(path, encoding="utf-8").read()

edits = [
    # （说明，正则，替换，应有个数）
    ("BAY3D 关",
     r'("bayertnr": \{\s*"opMode": "[A-Z_]+",\s*"en": )1,', r"\g<1>0,", 3),
    ("AE 增益封顶 16 倍",
     r'"sw_aeT_gain_dot": \[1,2,8,8,32,256\]',
     '"sw_aeT_gain_dot": [1,2,8,8,16,16]', 3),
]

for what, pat, rep, want in edits:
    s, n = re.subn(pat, rep, s)
    if n != want:
        sys.exit("%s：匹配到 %d 处，应为 %d 处（IQ 文件变了？）" % (what, n, want))
    print("%s：改了 %d 处" % (what, n))

open(path, "w", encoding="utf-8").write(s)
