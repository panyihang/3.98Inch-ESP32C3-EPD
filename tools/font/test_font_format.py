import struct
import unittest

import font_format as f


class CharSetTest(unittest.TestCase):
    def test_parse_char_list_deduplicates_and_keeps_order(self):
        chars = f.parse_char_list("一乙\n二 十\n一")
        self.assertEqual(chars, [0x4e00, 0x4e59, 0x4e8c, 0x5341])

    def test_merge_punctuation_adds_ascii_digits_and_letters(self):
        chars = [0x4e00]
        merged = f.add_punctuation(chars, "，。")
        self.assertEqual(set(merged) & {0xff0c, 0x3002}, {0xff0c, 0x3002})
        self.assertIn(ord('0'), merged)
        self.assertIn(ord('A'), merged)
        self.assertIn(ord('z'), merged)
        self.assertEqual(len(set(merged)), len(merged))


class FontPackTest(unittest.TestCase):
    def test_pack_roundtrip_layout(self):
        cps = [0x4e00, 0x4e01]
        glyphs = [bytes([0x11]) * 128, bytes([0x22]) * 128]
        data = f.pack_font(cps, glyphs)
        self.assertEqual(data[0:4], b"EPF1")
        self.assertEqual(struct.unpack_from("<I", data, 4)[0], 2)
        self.assertEqual(struct.unpack_from("<I", data, 8)[0], 128)
        self.assertEqual(struct.unpack_from("<I", data, 0x0c)[0], 0x00200020)
        table_off = 0x10
        self.assertEqual(struct.unpack_from("<I", data, table_off)[0], 0x4e00)
        self.assertEqual(struct.unpack_from("<I", data, table_off + 4)[0], 0x4e01)
        glyph_off = table_off + 8
        self.assertEqual(data[glyph_off], 0x11)
        self.assertEqual(data[glyph_off + 128], 0x22)

    def test_index_lookup_found_and_missing(self):
        table = [0x4e00, 0x4e01, 0x4e8c]
        self.assertEqual(f.index_lookup(0x4e01, table), 1)
        self.assertEqual(f.index_lookup(0x4e02, table), -1)
        self.assertEqual(f.index_lookup(0x4e00, table), 0)
        self.assertEqual(f.index_lookup(0x4e8c, table), 2)
        self.assertEqual(f.index_lookup(0x9999, table), -1)


if __name__ == "__main__":
    unittest.main()