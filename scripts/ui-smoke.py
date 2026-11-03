#!/usr/bin/env python3
"""kickpi_ui 交互冒烟测试：经 telnet 往板上注入点击，每步截屏，拼成一张总图。

用法：python3 scripts/ui-smoke.py [输出目录]      （默认 /tmp/k7-ui-smoke）

依赖界面里的两个调试入口（kickpi_ui_main.c 的 tick_cb）：
  /tmp/k7-tap   "x y" 点击，"x1 y1 x2 y2" 拖动；屏幕像素，720x1280 竖屏
  /tmp/k7-shot  写 -1 只截屏不切页，截图是缩到一半的 JPEG

只验证界面逻辑（按钮、开关、下拉框、滑条、页面切换），不验证触摸屏硬件。
坐标对应 2026-10-31 的布局，改了界面布局要同步改 STEPS。
结果要人看总图判断；脚本只保证每一步都截到了图。
会改动设置（久坐开关、间隔），最后一步把音量点回约 70%。重启后设置恢复默认。
"""
import os
import re
import socket
import sys
import time

HOST = os.environ.get("K7_HOST", "192.168.1.50")
PORT = int(os.environ.get("K7_TELNET_PORT", "2323"))

TAB_Y = 1224                      # 底部页签栏中线
TAB_X = [90, 270, 450, 630]       # 今日 / 家居 / 助手 / 设置

# (名字, 注入内容；None 表示只截屏)
STEPS = [
    ("今日-初始", f"{TAB_X[0]} {TAB_Y}"),   # 先回今日页，不管板子停在哪页
    ("喝了一杯", "360 852"),
    ("再喝一杯", "360 852"),
    ("切到设置", f"{TAB_X[3]} {TAB_Y}"),
    ("关久坐开关", "624 116"),
    ("回今日看提示", f"{TAB_X[0]} {TAB_Y}"),
    ("切到设置", f"{TAB_X[3]} {TAB_Y}"),
    ("打开久坐下拉", "480 112"),
    ("选 90 分钟", "480 300"),
    ("拖音量", "200 570 500 570"),
    ("开发者工具", "360 722"),
    ("返回", "60 32"),
    ("家居", f"{TAB_X[1]} {TAB_Y}"),
    ("切到设置", f"{TAB_X[3]} {TAB_Y}"),
    ("音量点回 70%", "440 570"),
]


def rd(s, t, until=None):
    b, end = b"", time.time() + t
    while time.time() < end:
        try:
            d = s.recv(65536)
            if not d:
                break
            b += d
            if until and until in b[-200:]:
                break
        except socket.timeout:
            pass
    return b


def step(tap, out):
    """注入一次（可选），再截屏取回。每步一条新连接，免得上一步残留的输出混进来。"""
    s = socket.create_connection((HOST, PORT), timeout=5)
    s.settimeout(0.3)
    try:
        rd(s, 1.5)
        if tap:
            s.sendall(f"echo {tap} > /tmp/k7-tap\r\n".encode())
            rd(s, 2)
        s.sendall(b"rm /tmp/k7-shot.jpg\r\n")
        rd(s, 1)
        s.sendall(b"echo -1 > /tmp/k7-shot\r\n")
        rd(s, 4)
        s.sendall(b"hexdump /tmp/k7-shot.jpg\r\n")
        b = rd(s, 120, b"nsh> ")
    finally:
        s.close()

    data = bytearray()
    for line in b.decode("latin1").splitlines():
        m = re.match(r"^\s*([0-9a-fA-F]{4,8}):\s+((?:[0-9a-fA-F]{2}\s){1,16})", line)
        if m:
            data += bytes(int(x, 16) for x in m.group(2).split())
    if not data.startswith(b"\xff\xd8"):
        raise RuntimeError(f"{out}: 没取回 JPEG（{len(data)} 字节）")
    with open(out, "wb") as f:
        f.write(data)


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "/tmp/k7-ui-smoke"
    os.makedirs(outdir, exist_ok=True)
    shots = []
    for i, (name, tap) in enumerate(STEPS):
        out = os.path.join(outdir, f"{i:02d}.jpg")
        step(tap, out)
        shots.append((out, name))
        print(f"{i:02d} {name:<10} {tap or '-':<18} {os.path.getsize(out)} B")

    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print("没装 Pillow，不拼总图")
        return
    cols, w, h = 8, 240, 427
    rows = (len(shots) + cols - 1) // cols
    grid = Image.new("RGB", (cols * w, rows * h), "white")
    for i, (path, name) in enumerate(shots):
        im = Image.open(path).resize((w, h))
        d = ImageDraw.Draw(im)
        d.rectangle((0, 0, 30, 18), fill="red")
        d.text((4, 3), f"{i:02d}", fill="white")
        grid.paste(im, ((i % cols) * w, (i // cols) * h))
    path = os.path.join(outdir, "grid.jpg")
    grid.save(path)
    print("总图", path)


if __name__ == "__main__":
    main()
