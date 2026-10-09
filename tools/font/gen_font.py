"""Generate fonts.bin from msyh.ttc + charactor_list.txt.

Requires: pip install pillow
Usage:
    python gen_font.py [ttc_path] [char_list] [out_bin]
Defaults:
    ttc_path    = C:\\Windows\\Fonts\\msyh.ttc
    char_list   = <repo>/charactor_list.txt
    out_bin     = <repo>/tools/font/fonts.bin
"""
import os
import struct
import sys

import font_format as fmt

PUNCTUATION = "，。、？！：；""''…—·《》（）"

DEFAULT_TTC = r"C:\Windows\Fonts\msyh.ttc"
DEFAULT_LIST = os.path.join(os.path.dirname(__file__), "..", "..", "charactor_list.txt")
DEFAULT_OUT = os.path.join(os.path.dirname(__file__), "fonts.bin")


def build_blob(codepoints, glyph_rows):
    return fmt.pack_font(codepoints, glyph_rows)


def rasterize(ttc_path, codepoints):
    """Rasterize each codepoint to a 32x32 1bpp row (128 bytes)."""
    from PIL import Image, ImageDraw, ImageFont

    rows = []
    for cp in codepoints:
        rows.append(_rasterize_one(ttc_path, cp))
    return rows


def _rasterize_one(ttc_path, codepoint, size=32, oversample=4):
    from PIL import Image, ImageDraw, ImageFont

    big = size * oversample
    font = ImageFont.truetype(ttc_path, size=big, index=0)
    ch = chr(codepoint)
    img = Image.new("1", (big, big), 0)          # 背景=0(空白)
    draw = ImageDraw.Draw(img)
    draw.text((0, 0), ch, font=font, fill=1)     # 文字=1(墨迹)
    bbox = img.getbbox()
    if bbox is None:
        return bytes(size * (size // 8))
    crop = img.crop(bbox)
    # Scale to fit inside size box, preserving aspect, then center.
    cw, ch_h = crop.size
    scale = min(size / cw, size / ch_h)
    nw, nh = max(1, int(cw * scale)), max(1, int(ch_h * scale))
    crop = crop.resize((nw, nh), Image.NEAREST)
    out = Image.new("1", (size, size), 0)        # 背景=0
    out.paste(crop, ((size - nw) // 2, (size - nh) // 2))
    return out.tobytes("raw", "1")               # MSB-first, 每行4字节, 行序从上到下


def generate(ttc_path, char_list_path, out_bin):
    with open(char_list_path, "r", encoding="utf-8") as fh:
        text = fh.read()
    codepoints = fmt.parse_char_list(text)
    codepoints = fmt.add_punctuation(codepoints, PUNCTUATION)
    if not os.path.exists(ttc_path):
        raise SystemExit("font not found: " + ttc_path)
    rows = rasterize(ttc_path, codepoints)
    data = build_blob(codepoints, rows)
    with open(out_bin, "wb") as fh:
        fh.write(data)
    n = len(codepoints)
    print("wrote %s: %d glyphs, %d bytes" % (out_bin, n, len(data)))


if __name__ == "__main__":
    args = sys.argv[1:]
    ttc = args[0] if len(args) > 0 else DEFAULT_TTC
    lst = args[1] if len(args) > 1 else DEFAULT_LIST
    out = args[2] if len(args) > 2 else DEFAULT_OUT
    generate(ttc, lst, out)
