import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))
import gen_font


class BuildTest(unittest.TestCase):
    def test_collect_glyphs_makes_rows_for_each_codepoint(self):
        cps = [0x4e00, 0x4e01]
        # Manually crafted glyph rows avoid real rasterization (no Pillow needed).
        rows = [bytes([i]) * 128 for i in (0x11, 0x22)]
        data = gen_font.build_blob(cps, rows)
        self.assertEqual(data[0:4], b"EPF1")


class RasterizeTest(unittest.TestCase):
    def test_rasterize_one_returns_128_bytes(self):
        data = gen_font._rasterize_one(gen_font.DEFAULT_TTC, 0x4e00)
        self.assertEqual(len(data), 128)
        self.assertNotEqual(data, b"\x00" * 128)

    def test_rasterize_one_row_order_is_top_to_bottom(self):
        # '丁' (0x4e01) draws ink toward the bottom of its 32px box, so its top
        # row (y=0) is blank background. Row order is guaranteed top-to-bottom
        # by tobytes("raw", "1") default stride=0.
        data = gen_font._rasterize_one(gen_font.DEFAULT_TTC, 0x4e01)
        self.assertEqual(len(data), 128)
        self.assertEqual(data[0:4], b"\x00\x00\x00\x00")  # top row blank (bg=0)
        # Some row below the top margin carries ink.
        rows = [data[r*4:r*4+4] for r in range(1, 32)]
        self.assertNotEqual(rows, [b"\x00\x00\x00\x00"] * 31)
        self.assertIn(b"\xff\xff\xff\xff", rows)  # '丁' top bar is full-width ink

    def test_rasterize_blank_char_is_all_zero(self):
        # '\u3000' (ideographic space) has no outline -> 128 bytes all 0x00.
        # Verifies polarity: blank background must be 0, not 0xff (negated).
        data = gen_font._rasterize_one(gen_font.DEFAULT_TTC, 0x3000)
        self.assertEqual(len(data), 128)
        self.assertEqual(data, b"\x00" * 128)

    def test_rasterize_ink_is_ones_not_negated(self):
        # '日' (0x65e5) is a hollow rectangle: its outline is a small fraction
        # of the 32x32 box. Under the old code (background=1/text=0) the
        # polarity was inverted, so "ink" (1-bits) covered most of the box
        # (measured ~79%). The fixed code yields ~41% (ink=1, bg=0).
        data = gen_font._rasterize_one(gen_font.DEFAULT_TTC, 0x65e5)
        self.assertEqual(len(data), 128)
        self.assertNotEqual(data, b"\x00" * 128)  # there is ink (ones)
        ink = sum(bin(b).count("1") for b in data)
        total = 32 * 32
        ratio = ink / total
        # A hollow glyph outline is well under half coverage; a negated glyph
        # (bg=1) is well over half. 60% cleanly separates 41% (fixed) from 79%.
        self.assertLess(ratio, 0.60,
                        "glyph looks negated (background counted as ink): %.0f%%" % (ratio * 100))
        # MSB-first bit order is guaranteed by tobytes("raw", "1"): bit7 of each
        # byte is the leftmost pixel, matching manual_canvas.c draw_bitmap's
        # `1u << (7 - (col & 7))` read convention.


if __name__ == "__main__":
    unittest.main()
