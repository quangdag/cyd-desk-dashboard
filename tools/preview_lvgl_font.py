#!/usr/bin/env python3
"""Render text with a generated LVGL .c font the way LVGL draws it - checks offsets/packing.

    python preview_lvgl_font.py FONT.c "text" out.png
"""
import re
import sys
from PIL import Image


def load(path):
    src = open(path, encoding="utf-8").read()
    bm = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", src.split("glyph_dsc[]")[0].split("glyph_bitmap[] = {")[1]))
    dsc = [tuple(map(int, m)) for m in re.findall(
        r"bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}, /\* U\+([0-9A-F]+)",
        src.replace("U+", "U+"))] if False else []
    for m in re.finditer(r"bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), "
                         r"\.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}, /\* U\+([0-9A-F]+)", src):
        bi, adv, w, h, ox, oy, cp = m.groups()
        dsc.append((int(cp, 16), int(bi), int(adv), int(w), int(h), int(ox), int(oy)))
    line_h = int(re.search(r"\.line_height = (\d+)", src).group(1))
    base = int(re.search(r"\.base_line = (\d+)", src).group(1))
    return bm, {d[0]: d[1:] for d in dsc}, line_h, base


def main():
    path, text, out = sys.argv[1:4]
    bm, glyphs, line_h, base = load(path)
    width = sum(glyphs.get(ord(c), (0, 160, 0, 0, 0, 0))[1] for c in text) // 16 + 8
    img = Image.new("L", (width, line_h + 4), 255)
    x = 4 * 16
    baseline_y = 2 + line_h - base
    for c in text:
        bi, adv, w, h, ox, oy = glyphs.get(ord(c), (0, 160, 0, 0, 0, 0))
        n = w * h
        for i in range(n):
            byte = bm[bi + i // 2]
            v = (byte >> 4) if i % 2 == 0 else (byte & 0xF)
            px, py = x // 16 + ox + i % w, baseline_y - oy - h + i // w
            if 0 <= px < img.width and 0 <= py < img.height:
                img.putpixel((px, py), 255 - v * 17)
        x += adv
    img = img.resize((img.width * 2, img.height * 2), Image.NEAREST)
    img.save(out)


if __name__ == "__main__":
    main()
