#!/usr/bin/env python3
"""吉祥物图片 → LVGL9 RGB565A8 C 数组。
裁掉白边 → 缩放到目标尺寸 → 圆形遮罩（圆屏）→ 输出 src/ui/assets/img_duck.c。
用法：python3 scripts/convert_duck.py [源图] [尺寸]
"""
import struct
import sys

from PIL import Image, ImageDraw, ImageOps

SRC = sys.argv[1] if len(sys.argv) > 1 else \
    "../docs/assets/icons/gaga-icon.png"
SIZE = int(sys.argv[2]) if len(sys.argv) > 2 else 466
OUT_C = "src/ui/assets/img_duck.c"
OUT_H = "src/ui/assets/img_duck.h"


def main() -> None:
    img = Image.open(SRC).convert("RGBA")
    # 裁掉白边：找非白色像素的包围盒
    gray = img.convert("L")
    mask = gray.point(lambda v: 255 if v < 245 else 0)
    bbox = mask.getbbox()
    if bbox:
        img = img.crop(bbox)
    # 方形化（短边补齐）→ 缩放
    side = min(img.size)
    img = ImageOps.fit(img, (side, side))
    img = img.resize((SIZE, SIZE), Image.LANCZOS)

    px = img.load()
    # RGB565（无 alpha：全屏铺满，不需要透明）
    argb = bytearray()
    for y in range(SIZE):
        for x in range(SIZE):
            r, g, b, a = px[x, y]
            argb += struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))

    name = "img_duck"
    with open(OUT_C, "w") as f:
        f.write("// 自动生成：scripts/convert_duck.py（品牌图标 gaga-icon，圆形遮罩，ARGB8888）\n")
        f.write("// 来源：" + SRC + "\n")
        f.write('#include "lvgl.h"\n\n')
        f.write(f"const uint8_t {name}_map[] = {{\n")
        for i in range(0, len(argb), 12):
            row = ",".join(f"0x{b:02X}" for b in argb[i:i + 12])
            f.write(f"    {row},\n")
        f.write("};\n\n")
        f.write(f"const lv_image_dsc_t {name} = {{\n")
        f.write("    .header = {\n")
        f.write("        .cf = LV_COLOR_FORMAT_ARGB8888,\n")
        f.write(f"        .w = {SIZE},\n")
        f.write(f"        .h = {SIZE},\n")
        f.write(f"        .stride = 932,\n")
        f.write("        .magic = LV_IMAGE_HEADER_MAGIC,\n")
        f.write("    },\n")
        f.write(f"    .data_size = sizeof({name}_map),\n")
        f.write(f"    .data = {name}_map,\n")
        f.write("};\n")
    with open(OUT_H, "w") as f:
        f.write("#pragma once\n\n#include \"lvgl.h\"\n\n")
        f.write(f"extern const lv_image_dsc_t {name};\n")
    print(f"完成：{SIZE}x{SIZE} ARGB8888 → {OUT_C}")


if __name__ == "__main__":
    main()
