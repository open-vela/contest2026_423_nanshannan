#!/usr/bin/env python3
"""按 modules.dep 给出模块的装载顺序（依赖在前），每行一个相对路径。

用法：modorder.py <lib/modules/版本 目录>

板上的 busybox 只有 insmod，没有 modprobe，所以顺序在主机上排好。
modules.dep 的格式是 "kernel/a.ko: kernel/b.ko kernel/c.ko"，冒号后面是 a 依赖的模块。
"""
import sys
from pathlib import Path

root = Path(sys.argv[1])
deps = {}
for line in (root / "modules.dep").read_text().splitlines():
    if not line.strip():
        continue
    mod, _, rest = line.partition(":")
    deps[mod] = rest.split()

done = []
seen = set()


def visit(mod, stack=()):
    if mod in seen:
        return
    if mod in stack:
        sys.exit(f"modules.dep 有环：{' -> '.join(stack + (mod,))}")
    for d in deps.get(mod, []):
        visit(d, stack + (mod,))
    seen.add(mod)
    done.append(mod)


for mod in sorted(deps):
    visit(mod)

print("\n".join(done))
