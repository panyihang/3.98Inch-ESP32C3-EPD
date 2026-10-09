"""Pure logic for building and indexing the flash font blob.

Independent of fonttools/Pillow so it can be unit-tested with stdlib only.
Mirrors the C side (font_store.c) byte-for-byte layout.
"""
import struct

MAGIC = b"EPF1"
GLYPH_BYTES = 128          # 32x32 1bpp, 4 bytes/row
HEADER_SIZE = 0x10
CODEPOINT_SIZE = 4


def parse_char_list(text):
    """Return sorted-unique list of codepoints from whitespace-separated text."""
    cps = set()
    for ch in text:
        if ch.isspace():
            continue
        cps.add(ord(ch))
    return sorted(cps)


def add_punctuation(chars, punctuation):
    """Merge punctuation plus ASCII digits/letters, dedupe, keep sorted."""
    extra = {ord(c) for c in punctuation}
    extra.update(range(ord('0'), ord('9') + 1))
    extra.update(range(ord('A'), ord('Z') + 1))
    extra.update(range(ord('a'), ord('z') + 1))
    return sorted(set(chars) | extra)


def pack_font(codepoints, glyph_rows):
    """Pack header + codepoint table + glyph bitmap area into a bytes blob.

    codepoints: list of int (must be sorted ascending).
    glyph_rows: list of bytes, each exactly GLYPH_BYTES, parallel to codepoints.
    """
    n = len(codepoints)
    header = struct.pack("<4sIII", MAGIC, n, GLYPH_BYTES, 0x00200020)
    table = b"".join(struct.pack("<I", cp) for cp in codepoints)
    glyphs = b"".join(glyph_rows)
    return header + table + glyphs


def index_lookup(codepoint, table):
    """Binary search a sorted codepoint table; return index or -1.

    Reference for the C implementation (font_store.c).
    """
    lo, hi = 0, len(table)
    while lo < hi:
        mid = (lo + hi) // 2
        if table[mid] < codepoint:
            lo = mid + 1
        else:
            hi = mid
    if lo < len(table) and table[lo] == codepoint:
        return lo
    return -1


def parse_font(data):
    """Parse a blob into (codepoints, glyph_offset, glyph_bytes) for tests/verify."""
    assert data[0:4] == MAGIC
    n = struct.unpack_from("<I", data, 4)[0]
    glyph_bytes = struct.unpack_from("<I", data, 8)[0]
    table_off = HEADER_SIZE
    cps = [struct.unpack_from("<I", data, table_off + 4 * i)[0] for i in range(n)]
    glyph_off = table_off + CODEPOINT_SIZE * n
    return cps, glyph_off, glyph_bytes