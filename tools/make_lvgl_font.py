#!/usr/bin/env python3
"""
Convert a TTF into an LVGL 9 bitmap font (.c, lv_font_fmt_txt, 4 bpp, no kerning),
the same format lv_font_conv produces - so no Node.js is needed. Uses Pillow/FreeType.

    python make_lvgl_font.py FONT.ttf SIZE NAME OUT.c [--chars vi|digits|thai] [--mono-digits] [--fallback NAME]

--chars vi      ASCII + Vietnamese letters + a few symbols (default)
--chars digits  0-9 : % . - space and a few symbols (for big clocks / numbers)
--chars thai    the Thai block U+0E01-U+0E5B (used as a fallback font behind a Latin one)
--mono-digits   give 0-9 the same advance width so clocks don't jitter
--fallback      font LVGL tries for characters this one lacks (default lv_font_montserrat_14,
                which has the LV_SYMBOL_* icons)

--extra-top N   add N px above the ascent (room for Thai marks drawn by a fallback font; LVGL clips
                label text to its line box)

Thai has no shaping on the CYD: vowel and tone marks are zero-width glyphs drawn over the previous letter.
A tone mark over an upper vowel (ที่, น้ำ) would land on the vowel, so --chars thai also emits raised
copies of the tone marks at U+F748-U+F74C; the PC server swaps them in where needed (thai_marks()).
"""
import argparse
from pathlib import Path

from PIL import ImageFont

VI = (
    "ÀÁÂÃÈÉÊÌÍÒÓÔÕÙÚÝàáâãèéêìíòóôõùúýĂăĐđĨĩŨũƠơƯư"
    + "".join(chr(c) for c in range(0x1EA0, 0x1EFA))
)
EXTRA = "°·–—…•→←↑↓✓"


def charset(kind: str) -> list[int]:
    if kind == "digits":
        chars = set("0123456789:%.-+ /°CF")
    elif kind == "thai":
        chars = set(chr(c) for c in range(0x0E01, 0x0E5C) if not 0x0E3B <= c <= 0x0E3E)
    else:
        chars = set(chr(c) for c in range(0x20, 0x7F)) | set(VI) | set(EXTRA)
    return sorted(ord(c) for c in chars)


def render(font, ch):
    """Return (w, h, ofs_x, ofs_y, adv_px, pixels[0..255]) with a tight box."""
    adv = font.getlength(ch)
    mask, (ox, oy) = font.getmask2(ch, mode="L", anchor="ls")
    w, h = mask.size
    px = list(mask) if w and h else []
    if not px or max(px) == 0:
        return 0, 0, 0, 0, adv, []
    rows = [px[y * w:(y + 1) * w] for y in range(h)]
    top = next(y for y in range(h) if any(rows[y]))
    bot = max(y for y in range(h) if any(rows[y]))
    left = min(x for x in range(w) if any(rows[y][x] for y in range(h)))
    right = max(x for x in range(w) if any(rows[y][x] for y in range(h)))
    rows = [r[left:right + 1] for r in rows[top:bot + 1]]
    bw, bh = right - left + 1, bot - top + 1
    # anchor 'ls': (ox, oy) is the box top-left relative to (left side, baseline), y down
    box_top = oy + top
    return bw, bh, ox + left, -(box_top + bh), adv, [v for r in rows for v in r]


def pack4(pixels):
    out, nib = bytearray(), None
    for v in pixels:
        q = (v * 15 + 127) // 255
        if nib is None:
            nib = q
        else:
            out.append((nib << 4) | q)
            nib = None
    if nib is not None:
        out.append(nib << 4)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ttf")
    ap.add_argument("size", type=int)
    ap.add_argument("name")
    ap.add_argument("out")
    ap.add_argument("--chars", default="vi", choices=["vi", "digits", "thai"])
    ap.add_argument("--mono-digits", action="store_true")
    ap.add_argument("--fallback", default="lv_font_montserrat_14")
    ap.add_argument("--extra-top", type=int, default=0)
    ap.add_argument("--axes", help="variable font axes in font order, e.g. 72,500,0,0 (opsz,wght,SOFT,WONK)")
    a = ap.parse_args()

    font = ImageFont.truetype(a.ttf, a.size)
    if a.axes:
        font.set_variation_by_axes([float(v) for v in a.axes.split(",")])
    ascent, descent = font.getmetrics()
    codes = [c for c in charset(a.chars) if c == 0x20 or font.getmask2(chr(c), mode="L")[0].size != (0, 0)]

    glyphs = [render(font, chr(c)) for c in codes]
    if a.chars == "thai":  # raised tone marks, 1 px above the tallest upper vowel
        g = dict(zip(codes, glyphs))
        upper_top = max(g[c][3] + g[c][1] for c in (0x0E31, 0x0E34, 0x0E35, 0x0E36, 0x0E37))
        for c in range(0x0E48, 0x0E4D):
            w, h, ox, oy, adv, px = g[c]
            codes.append(0xF700 + c - 0x0E00)
            glyphs.append((w, h, ox, max(oy, upper_top + 1), adv, px))
    if a.mono_digits:
        dig = [i for i, c in enumerate(codes) if chr(c).isdigit()]
        wmax = max(glyphs[i][4] for i in dig)
        for i in dig:  # widest advance, glyph centred in it
            w, h, ox, oy, adv, px = glyphs[i]
            glyphs[i] = (w, h, ox + round((wmax - adv) / 2), oy, wmax, px)

    bitmap, dsc = bytearray(), []
    for (w, h, ox, oy, adv, px), c in zip(glyphs, codes):
        dsc.append((len(bitmap), round(adv * 16), w, h, ox, oy, c))
        bitmap += pack4(px)

    # cmaps: contiguous ASCII run as FORMAT0_TINY, everything else as SPARSE_TINY
    ascii_codes = [c for c in codes if 0x20 <= c <= 0x7E]
    rest = [c for c in codes if c > 0x7E]
    cmaps = []
    if ascii_codes and ascii_codes == list(range(ascii_codes[0], ascii_codes[-1] + 1)):
        cmaps.append(("fmt0", ascii_codes[0], len(ascii_codes), 1, None))
        sparse_codes, gid = rest, 1 + len(ascii_codes)
    else:
        sparse_codes, gid = codes, 1
    if sparse_codes:
        cmaps.append(("sparse", sparse_codes[0], sparse_codes[-1] - sparse_codes[0] + 1, gid,
                      [c - sparse_codes[0] for c in sparse_codes]))

    L = []
    L.append(f"/* {a.name}: {Path(a.ttf).name} {a.size}px, 4 bpp, {len(codes)} glyphs."
             f" Generated by tools/make_lvgl_font.py - do not edit. Font license: SIL OFL 1.1 */")
    L.append('#include "lvgl.h"\n')
    if not a.fallback.startswith("lv_font_"):
        L.append(f"LV_FONT_DECLARE({a.fallback})\n")
    L.append("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {")
    for i in range(0, len(bitmap), 16):
        L.append("    " + ", ".join(f"0x{b:02x}" for b in bitmap[i:i + 16]) + ",")
    if not bitmap:
        L.append("    0x00,")
    L.append("};\n")
    L.append("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
    L.append("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},")
    for bi, adv, w, h, ox, oy, c in dsc:
        L.append(f"    {{.bitmap_index = {bi}, .adv_w = {adv}, .box_w = {w}, .box_h = {h}, "
                 f".ofs_x = {ox}, .ofs_y = {oy}}}, /* U+{c:04X} */")
    L.append("};\n")
    for i, cm in enumerate(cmaps):
        if cm[0] == "sparse":
            L.append(f"static const uint16_t unicode_list_{i}[] = {{")
            ofs = cm[4]
            for j in range(0, len(ofs), 12):
                L.append("    " + ", ".join(f"0x{o:x}" for o in ofs[j:j + 12]) + ",")
            L.append("};\n")
    L.append("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
    for i, (kind, start, length, gstart, ofs) in enumerate(cmaps):
        if kind == "fmt0":
            L.append(f"    {{.range_start = {start}, .range_length = {length}, .glyph_id_start = {gstart}, "
                     ".unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, "
                     ".type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY},")
        else:
            L.append(f"    {{.range_start = {start}, .range_length = {length}, .glyph_id_start = {gstart}, "
                     f".unicode_list = unicode_list_{i}, .glyph_id_ofs_list = NULL, .list_length = {len(ofs)}, "
                     ".type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY},")
    L.append("};\n")
    L.append("static const lv_font_fmt_txt_dsc_t font_dsc = {")
    L.append("    .glyph_bitmap = glyph_bitmap, .glyph_dsc = glyph_dsc, .cmaps = cmaps,")
    L.append(f"    .kern_dsc = NULL, .kern_scale = 0, .cmap_num = {len(cmaps)}, .bpp = 4,")
    L.append("    .kern_classes = 0, .bitmap_format = 0,")
    L.append("};\n")
    L.append(f"const lv_font_t {a.name} = {{")
    L.append("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
    L.append("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
    L.append(f"    .line_height = {ascent + descent + a.extra_top}, .base_line = {descent},")
    L.append("    .subpx = LV_FONT_SUBPX_NONE, .underline_position = -1, .underline_thickness = 1,")
    L.append("    .dsc = &font_dsc,")
    L.append(f"    .fallback = &{a.fallback},")
    L.append("};")
    Path(a.out).write_text("\n".join(L) + "\n", encoding="utf-8")
    print(f"{a.out}: {len(codes)} glyphs, {len(bitmap)} bytes bitmap, line {ascent + descent}px")


if __name__ == "__main__":
    main()
