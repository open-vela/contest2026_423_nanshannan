#!/usr/bin/env python3
"""生成开机 logo（U-Boot 显示）并打包成 Rockchip resource 镜像。

用法：scripts/gen-boot-logo.py [输出目录]      默认 ~/rk3576-amp/out/logo
产物：logo.bmp、logo_kernel.bmp（同一张图，U-Boot → bootamp 之间不闪）、
      resource-logo.img（写 dtbo 分区 LBA 40960，见 amp/uboot/0011）。

画面是 kickpi_ui 开机动画的第一帧（见 render()），界面起来后接着动。

★ 格式照出厂 logo：8 位调色板 + RLE8 压缩（kernel-6.1/logo.bmp 就是
  654x270 bpp8 comp1）。U-Boot 的 bmpdecoder 对这种格式是验证过的；全屏
  720x1280 的 24 位图要 2.7MB，两张就放不进 4MB 的分区，而 RLE8 下
  大片纯色背景只有几十 KB。U-Boot 解码成 RGB565 显示，所以只用平涂色，
  不用渐变（565 下会出色带）。
"""

import os
import struct
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

W, H = 720, 1280                # 面板（竖屏）尺寸，BMP 按它存
LW, LH = H, W                   # 界面（横屏）尺寸，按它画
BG = (0x0e, 0x11, 0x16)         # 以下颜色与 kickpi_ui_main.c 的 UI_* 一致
TRACK = (0x24, 0x2b, 0x36)      # UI_SURFACE2
TEXT = (0xe8, 0xed, 0xf2)
DIM = (0x8a, 0x96, 0xa3)

HOME = Path.home()
CJK = HOME / 'rk3576-amp/font/pkg/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf'
LVGL_FONTS = Path(__file__).resolve().parents[1] / '../apps/graphics/lvgl/lvgl/scripts/built_in_font'
LATIN = LVGL_FONTS / 'Montserrat-Medium.ttf'
ICONS = LVGL_FONTS / 'FontAwesome5-Solid+Brands+Regular.woff'
RESOURCE_TOOL = HOME / 'rk3576-amp/u-boot/tools/resource_tool'

ICON_HOME = '\uf015'


def text_at(draw, cx, cy, text, font, fill):
    """以 (cx, cy) 为字形外框中心写字。"""
    x0, y0, x1, y1 = draw.textbbox((0, 0), text, font=font)
    draw.text((cx - (x0 + x1) / 2, cy - (y0 + y1) / 2), text, font=font,
              fill=fill)


def render():
    """画 kickpi_ui 开机动画（splash_show）的第一帧。

    ★ U-Boot 的 logo 一直留在屏上，直到 kickpi_ui 起来画第一帧。两张图
      对不上，那一刻就是一次跳变；所以这里照 splash_show() 的布局画：
      三道圆环只有暗色底轨、中间家的图标、标题。界面起来后彩色圆环从
      这里接着画满，看上去是同一段动画。改了一边要同步另一边。

    界面是横屏（UI_ROTATE 90），面板是竖屏：先按 1280x720 画，再逆时针
    转 90° —— 与 ui_to_panel() 一致：逻辑 (lx, ly) → 面板 (ly, 719-lx)，
    逻辑左上角落在面板左下角。UI_ROTATE 改成 270 时这里要改成顺时针。
    """
    img = Image.new('RGB', (LW, LH), BG)
    d = ImageDraw.Draw(img)

    cx, cy = LW // 2, LH // 2 - 70          # 圆环中心：屏幕中心上方 70
    for i in range(3):
        r = (240 - i * 52) // 2
        d.ellipse((cx - r, cy - r, cx + r - 1, cy + r - 1), outline=TRACK,
                  width=14)
    text_at(d, cx, cy, ICON_HOME, ImageFont.truetype(str(ICONS), 64), TEXT)

    # 标题。"K7" 用 Montserrat、汉字用 Droid，与 lv_font_k7_ui_28 的来源一致
    latin = ImageFont.truetype(str(LATIN), 28)
    cjk = ImageFont.truetype(str(CJK), 28)
    a, b, gap = 'K7', '桌面助手', 8
    aw, bw = d.textlength(a, font=latin), d.textlength(b, font=cjk)
    x = (LW - (aw + gap + bw)) / 2
    ty = LH // 2 + 100
    text_at(d, x + aw / 2, ty, a, latin, TEXT)
    text_at(d, x + aw + gap + bw / 2, ty, b, cjk, TEXT)

    return img.transpose(Image.Transpose.ROTATE_90)


def rle8(pal_img):
    """8 位调色板图 → BMP RLE8 数据（自下而上，逐行 EOL，最后 EOBMP）。

    ★ 最后一行后面不能再写 EOL，直接 EOBMP。U-Boot 的 libnsbmp 在 EOL 上
      先 y++，y 到了高度就返回 BMP_DATA_ERROR；显示驱动只容忍 20 万像素
      以内的"部分解码"。出厂 logo（654x270）也是多写了这个 EOL，只是
      刚好在 20 万以内；全屏 720x1280 会被整张拒掉
      （"partially decoded bmp ... can not be too large"）。
    """
    px = pal_img.load()
    out = bytearray()
    for y in range(H - 1, -1, -1):
        x = 0
        while x < W:
            v = px[x, y]
            n = 1
            while x + n < W and n < 255 and px[x + n, y] == v:
                n += 1
            out += bytes((n, v))
            x += n
        if y:
            out += b'\x00\x00'
    out += b'\x00\x01'
    return bytes(out)


def write_bmp(path, pal_img):
    palette = pal_img.getpalette()[:256 * 3]
    palette += [0] * (256 * 3 - len(palette))
    table = b''.join(bytes((palette[i * 3 + 2], palette[i * 3 + 1],
                            palette[i * 3], 0)) for i in range(256))
    data = rle8(pal_img)
    off = 14 + 40 + len(table)
    size = off + len(data)
    hdr = b'BM' + struct.pack('<IHHI', size, 0, 0, off)
    info = struct.pack('<IiiHHIIiiII', 40, W, H, 1, 8, 1, len(data),
                       2835, 2835, 256, 0)
    Path(path).write_bytes(hdr + info + table + data)
    return size


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else HOME / 'rk3576-amp/out/logo')
    out.mkdir(parents=True, exist_ok=True)
    pal = render().quantize(colors=64, method=Image.Quantize.MEDIANCUT,
                            dither=Image.Dither.NONE)
    pal.convert('RGB').save(out / 'logo-preview.png')
    for name in ('logo.bmp', 'logo_kernel.bmp'):
        n = write_bmp(out / name, pal)
        print(f'{name}: {n} 字节')
    img = out / 'resource-logo.img'
    subprocess.run([str(RESOURCE_TOOL), '--pack', f'--root={out}',
                    f'--image={img}', 'logo.bmp', 'logo_kernel.bmp'],
                   check=True, cwd=out, stdout=subprocess.DEVNULL)
    print(f'{img}: {os.path.getsize(img)} 字节')


if __name__ == '__main__':
    main()
