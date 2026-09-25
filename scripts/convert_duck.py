#!/usr/bin/env python3
"""鸭子页吉祥物图转换：源 PNG → LVGL C 数组（ARGB8888，466×466 圆形遮罩）。

用法：python3 scripts/convert_duck.py [源.png]
默认源：docs/assets/icons/gaga-icon.png（换 duck-pendant-v3-1024.png 时
放回 docs/assets/ 后传参重跑即可）。

输出：esp32-idf/src/ui/assets/img_duck.h/.c（符号名 img_duck 不变）。
⚠️ 字节序：LVGL9 ARGB8888 = 每像素内存序 B,G,R,A（小端 0xAARRGGBB）。
2026-09-25 事故复盘：上一版数据是 RGB565(2B/px) 但头声明 ARGB8888
（stride 932≠1864），LVGL 按 4B 读 → 两幅错位绿图。
"""
import sys

from PIL import Image, ImageDraw

SIZE = 466
SRC = sys.argv[1] if len(sys.argv) > 1 else "docs/assets/icons/gaga-icon.png"
OUT_C = "esp32-idf/src/ui/assets/img_duck.c"
OUT_H = "esp32-idf/src/ui/assets/img_duck.h"

im = Image.open(SRC).convert("RGBA")
w, h = im.size
side = min(w, h)
im = im.crop(((w - side) // 2, (h - side) // 2,
              (w + side) // 2, (h + side) // 2)).resize((SIZE, SIZE), Image.LANCZOS)

# 圆形遮罩：圆外 alpha=0（屏幕本底黑色，视觉=圆形照片占满圆屏）
mask = Image.new("L", (SIZE, SIZE), 0)
ImageDraw.Draw(mask).ellipse((0, 0, SIZE - 1, SIZE - 1), fill=255)
im.putalpha(mask)

px = im.load()
out = []
for y in range(SIZE):
    for x in range(SIZE):
        r, g, b, a = px[x, y]
        out += [b, g, r, a]  # LVGL ARGB8888 内存序

with open(OUT_C, "w") as f:
    f.write(f"// 自动生成：scripts/convert_duck.py（源：{SRC}，居中裁方 → "
            f"{SIZE}×{SIZE} 圆形遮罩，ARGB8888，内存序 B,G,R,A）\n")
    f.write('#include "lvgl.h"\n\n')
    f.write("const uint8_t img_duck_map[] = {\n")
    for i in range(0, len(out), 12):
        row = ",".join(f"0x{b:02X}" for b in out[i:i + 12])
        f.write(f"    {row},\n")
    f.write("};\n\n")
    f.write("const lv_image_dsc_t img_duck = {\n")
    f.write("    .header = {\n")
    f.write("        .cf = LV_COLOR_FORMAT_ARGB8888,\n")
    f.write(f"        .w = {SIZE},\n        .h = {SIZE},\n")
    f.write(f"        .stride = {SIZE * 4},\n")
    f.write("        .magic = LV_IMAGE_HEADER_MAGIC,\n")
    f.write("    },\n")
    f.write("    .data_size = sizeof(img_duck_map),\n")
    f.write("    .data = img_duck_map,\n")
    f.write("};\n")

with open(OUT_H, "w") as f:
    f.write("#pragma once\n\n#include \"lvgl.h\"\n\n")
    f.write("extern const lv_image_dsc_t img_duck;\n")

print(f"[convert] {SRC} ({w}x{h}) → {OUT_C}（{SIZE}x{SIZE} ARGB8888，"
      f"{len(out) * 1 / 1024:.0f}KB）")
