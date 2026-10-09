# 闪存字库实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 用微软雅黑为 `charactor_list.txt` 的 2500 汉字 + 中文标点生成 32×32 1bpp 位图字库，存入新增 `fonts` 闪存分区，固件 mmap 只读并按需回退查找，修复 today_plan 缺字显示为空白的问题。

**架构：** 离线工具 `gen_font.py` 把 `msyh.ttc` 子集化并光栅化为 `fonts.bin`，一次性烧录到 `fonts` 分区（`0x270000`）。固件新增 `font_store` 模块 mmap 该分区，`manual_font_cjk_rows()` 对内置 121 字集未命中的 codepoint 回退到 flash 字库二分查找。字库不可用时仅降级（缺字空白），不阻塞启动。

**技术栈：** ESP-IDF 6.1、ESP32-C3、`esp_partition`/`esp_partition_mmap`、Python 3.11、fonttools、Pillow、unittest。

**测试策略：**
- Python 工具（`font_format.py` / `gen_font.py`）用内置 `unittest`（TDD：先写失败测试）。
- C 固件侧**无 host 编译器**（系统无 gcc，`idf.py`/`gcc` 不在 PATH），采用 `idf.py build` 编译验证 + 上板验证；二分查找逻辑在 `font_format.py` 中用 Python 参考实现先行 unittest 验证（索引布局一致），C 实现按同一算法。
- 构建验证需先启用 ESP-IDF 环境（见任务 4 的构建说明）。

**环境事实（已验证）：**
- `python` = `C:\Espressif\tools\python\python.exe`（Python 3.11.15），内置 `unittest` 可用，**无 pytest、无 fonttools、无 Pillow**。
- 系统字体：`C:\Windows\Fonts\msyh.ttc`（微软雅黑，18.79MB，TrueType Collection）。
- 分区备用空间：`0x270000..0x400000`。

---

## 文件结构

**新增：**
- `tools/font/font_format.py` — 纯逻辑：字符集解析、fonts.bin 打包、参考二分查找（可独立测试，不依赖 fonttools）。
- `tools/font/test_font_format.py` — `font_format.py` 的 unittest。
- `tools/font/gen_font.py` — 实际光栅化：fonttools 子集化 + Pillow 渲染，产出 `fonts.bin`。
- `main/storage/font_store.h` — 固件字体分区接口。
- `main/storage/font_store.c` — mmap + 校验 + 二分查找实现。

**修改：**
- `partitions.csv` — 新增 `fonts` 数据分区（`0x270000`，384KB）。
- `main/display/font_bitmap.c` — `manual_font_cjk_rows()` 增加 flash 回退。
- `main/main.c` — 初始化流程加 `font_store_init()`。
- `main/CMakeLists.txt` — SRCS 加 `storage/font_store.c`。
- `readme.md` — 目录结构、构建/烧录说明（可选，最后做）。

## fonts.bin 布局（font_format.py 与 C 侧约定一致）

```
偏移 0x00  魔数 "EPF1"（4 字节）
0x04      glyph 数 n（uint32 LE）
0x08      每字形字节数（uint32 LE）= 128
0x0C      字面宽/高（uint32 LE）= 0x00200020
0x10      codepoint 表 [uint32 LE × n]，升序
0x10+4n   位图数据 [n × 128B]，32×32 1bpp，每行 4 字节，MSB 左
```

---

### 任务 1：`font_format.py` 纯逻辑（TDD）

**文件：**
- 创建：`tools/font/font_format.py`
- 创建：`tools/font/test_font_format.py`

- [ ] **步骤 1：编写失败测试**

创建 `tools/font/test_font_format.py`：

```python
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
```

- [ ] **步骤 2：运行测试验证失败**

运行（工作目录 `tools/font`）：
```
python -m unittest test_font_format -v
```
预期：FAIL / ModuleNotFoundError（`font_format` 不存在）。

- [ ] **步骤 3：实现 `font_format.py`**

创建 `tools/font/font_format.py`：

```python
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
```

- [ ] **步骤 4：运行测试验证通过**

运行（工作目录 `tools/font`）：
```
python -m unittest test_font_format -v
```
预期：PASS（4 个测试全部通过）。

- [ ] **步骤 5：Commit**

```bash
git add tools/font/font_format.py tools/font/test_font_format.py
git commit -m "feat(font): add pure font-blob format logic with tests"
```

---

### 任务 2：`gen_font.py` 字库生成工具（TDD）

**文件：**
- 创建：`tools/font/gen_font.py`
- 创建：`tools/font/test_gen_font.py`

- [ ] **步骤 1：编写失败测试**

创建 `tools/font/test_gen_font.py`（只测不依赖 fonttools/Pillow 的导出函数；实际光栅化函数在有依赖时跳过）：

```python
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))
import gen_font


class BuildTest(unittest.TestCase):
    def test_collect_glyphs_makes_rows_for_each_codepoint(self):
        cps = [0x4e00, 0x4e01]
        # _glyph_rows is injected to avoid needing fonttools/Pillow here.
        rows = [bytes([i]) * 128 for i in (0x11, 0x22)]
        data = gen_font.build_blob(cps, rows)
        self.assertEqual(data[0:4], b"EPF1")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **步骤 2：运行测试验证失败**

运行（工作目录 `tools/font`）：
```
python -m unittest test_gen_font -v
```
预期：FAIL / ModuleNotFoundError（`gen_font` 不存在）。

- [ ] **步骤 3：实现 `gen_font.py`**

创建 `tools/font/gen_font.py`：

```python
"""Generate fonts.bin from msyh.ttc + charactor_list.txt.

Requires: pip install fonttools pillow
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
    from PIL import Image, ImageFont

    big = size * oversample
    font = ImageFont.truetype(ttc_path, size=big, index=0)
    ch = chr(codepoint)
    img = Image.new("1", (big, big), 1)
    draw = ImageDraw.Draw(img)
    draw.text((0, 0), ch, font=font, fill=0)
    bbox = img.getbbox()
    if bbox is None:
        return bytes(size * (size // 8))
    crop = img.crop(bbox)
    # Scale to fit inside size box, preserving aspect, then center.
    cw, ch_h = crop.size
    scale = min(size / cw, size / ch_h)
    nw, nh = max(1, int(cw * scale)), max(1, int(ch_h * scale))
    crop = crop.resize((nw, nh), Image.NEAREST)
    out = Image.new("1", (size, size), 1)
    out.paste(crop, ((size - nw) // 2, (size - nh) // 2))
    return out.tobytes("raw", "1;R", 0, -1)  # 32 rows * 4 bytes


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
```

- [ ] **步骤 4：运行测试验证通过**

运行（工作目录 `tools/font`）：
```
python -m unittest test_gen_font -v
```
预期：PASS。

- [ ] **步骤 5：安装依赖并实跑生成（验证真实路径）**

安装依赖（一次）：
```
python -m pip install fonttools pillow
```
生成字库（工作目录 `tools/font`）：
```
python gen_font.py
```
预期：输出 `wrote .../fonts.bin: 25xx glyphs, ~33xKB bytes`（glyph 数 = 2500 + 标点 + ASCII 数字字母去重后的数量）。用 `python -c` 快速校验头部：
```
python -c "import font_format,sys; cps,off,gb=font_format.parse_font(open('fonts.bin','rb').read()); print(len(cps),off,gb)"
```
预期打印 `252? 4096 128`（数量与步骤 5 一致）。

- [ ] **步骤 6：Commit**

```bash
git add tools/font/gen_font.py tools/font/test_gen_font.py
git commit -m "feat(font): add msyh subset rasterizer to build fonts.bin"
```

---

### 任务 3：新增 `fonts` 闪存分区

**文件：**
- 修改：`partitions.csv`

- [ ] **步骤 1：编辑分区表**

当前 `partitions.csv` 内容（末尾行）：
```
# Name,   Type, SubType, Offset,   Size,   Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
factory,  app,  factory, 0x10000,  0x190000,
photos,   data, 0x40,     0x1a0000, 0xd0000,
# 0x270000..0x400000 为备用空间（1,562,560 字节，未分配）
```
在 `photos` 行后、注释行前插入：
```
fonts,    data, 0x41,    0x270000, 0x60000,
```
并把注释行改为：
```
# 0x2d0000..0x400000 为备用空间（1,179,648 字节，未分配）
```
即把 `fonts` 分区（0x60000=384KB）放进备用空间开头。改动后文件：
```
# Name,   Type, SubType, Offset,   Size,   Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
factory,  app,  factory, 0x10000,  0x190000,
photos,   data, 0x40,     0x1a0000, 0xd0000,
fonts,    data, 0x41,    0x270000, 0x60000,
# 0x2d0000..0x400000 为备用空间（1,179,648 字节，未分配）
```

- [ ] **步骤 2：Commit**

```bash
git add partitions.csv
git commit -m "feat(partition): add 384KB fonts data partition in spare space"
```

---

### 任务 4：固件 `font_store` 模块

**文件：**
- 创建：`main/storage/font_store.h`
- 创建：`main/storage/font_store.c`

（`idf.py` 不在 PATH，构建需先启用 ESP-IDF 环境，例如在 VS Code 中用 ESP-IDF 扩展终端，或运行 `C:\Espressif\v6.1\esp-idf\export.ps1` 后再 `idf.py build`。以下构建命令均以此为前提。）

- [ ] **步骤 1：编写头文件**

创建 `main/storage/font_store.h`：

```c
#ifndef FONT_STORE_H
#define FONT_STORE_H

#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
typedef int esp_err_t;
#define ESP_OK 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define FONT_STORE_MAGIC "EPF1"
#define FONT_STORE_GLYPH_BYTES 128u
#define FONT_STORE_GLYPH_WIDTH 32u
#define FONT_STORE_GLYPH_HEIGHT 32u

/* Look up a codepoint in the flash-resident font library. On hit sets *rows to
 * the 128-byte 32x32 1bpp bitmap and returns 1; otherwise returns 0. */
int font_store_lookup(uint32_t codepoint, const uint8_t **rows);

#ifdef ESP_PLATFORM
esp_err_t font_store_init(void);
#else
esp_err_t font_store_init(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* FONT_STORE_H */
```

- [ ] **步骤 2：实现 `font_store.c`**

创建 `main/storage/font_store.c`：

```c
#include "font_store.h"

#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_partition.h"
#endif

#define FONT_STORE_HEADER_SIZE 0x10u
#define FONT_STORE_CODEPOINT_SIZE 4u

static const uint32_t *s_table = NULL;
static const uint8_t *s_glyphs = NULL;
static uint32_t s_count = 0;

static int index_lookup(uint32_t codepoint, const uint32_t *table, uint32_t n)
{
    uint32_t lo = 0u, hi = n;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        if (table[mid] < codepoint) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    if (lo < n && table[lo] == codepoint) {
        return (int)lo;
    }
    return -1;
}

int font_store_lookup(uint32_t codepoint, const uint8_t **rows)
{
    if (!rows || !s_table || !s_glyphs) return 0;
    const int idx = index_lookup(codepoint, s_table, s_count);
    if (idx < 0) return 0;
    *rows = s_glyphs + (size_t)idx * FONT_STORE_GLYPH_BYTES;
    return 1;
}

#ifdef ESP_PLATFORM
esp_err_t font_store_init(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x41, "fonts");
    if (!part || part->size < FONT_STORE_HEADER_SIZE) {
        ESP_LOGW("font_store", "fonts partition not found");
        return ESP_ERR_NOT_FOUND;
    }
    const void *map = NULL;
    esp_err_t err = esp_partition_mmap(part, 0, part->size,
                                       ESP_PARTITION_MMAP_DATA, &map, NULL);
    if (err != ESP_OK) {
        ESP_LOGW("font_store", "mmap failed: %d", err);
        return err;
    }
    const uint8_t *p = (const uint8_t *)map;
    if (p[0] != 'E' || p[1] != 'P' || p[2] != 'F' || p[3] != '1') {
        ESP_LOGW("font_store", "bad font magic");
        return ESP_ERR_INVALID_MAGIC;
    }
    s_count = ((const uint32_t *)p)[1];
    if (s_count == 0 || s_count > part->size / (FONT_STORE_CODEPOINT_SIZE + FONT_STORE_GLYPH_BYTES)) {
        ESP_LOGW("font_store", "invalid glyph count %lu", (unsigned long)s_count);
        return ESP_ERR_INVALID_SIZE;
    }
    s_table = (const uint32_t *)(p + FONT_STORE_HEADER_SIZE);
    s_glyphs = p + FONT_STORE_HEADER_SIZE + FONT_STORE_CODEPOINT_SIZE * s_count;
    ESP_LOGI("font_store", "loaded %lu glyphs", (unsigned long)s_count);
    return ESP_OK;
}
#else
esp_err_t font_store_init(void)
{
    s_table = NULL;
    s_glyphs = NULL;
    s_count = 0;
    return ESP_OK;
}
#endif
```

- [ ] **步骤 3：加入构建并在 ESP32 环境编译**

编辑 `main/CMakeLists.txt`，在 `"storage/photo_store.c"` 行后加入：
```
        "storage/font_store.c"
```
启用 ESP-IDF 环境后运行：
```
idf.py build
```
预期：编译成功，`font_store.c.obj` 生成，链接无未定义引用。若无环境，此步转为 "在 VS Code ESP-IDF 终端执行"，由执行者确认无编译错误。

- [ ] **步骤 4：Commit**

```bash
git add main/storage/font_store.h main/storage/font_store.c main/CMakeLists.txt
git commit -m "feat(storage): add flash font_store with mmap and binary lookup"
```

---

### 任务 5：`manual_font_cjk_rows` 回退到 flash 字库

**文件：**
- 修改：`main/display/font_bitmap.c`

- [ ] **步骤 1：确认现有实现**

`main/display/font_bitmap.c` 末尾（约 1338 行起）：

```c
const uint8_t *manual_font_cjk_rows(uint32_t codepoint)
{
    for (unsigned i = 0; i < sizeof(CJK_GLYPHS) / sizeof(CJK_GLYPHS[0]); ++i) {
        if (CJK_GLYPHS[i].codepoint == codepoint) return CJK_GLYPHS[i].rows;
    }
    return NULL;
}
```

- [ ] **步骤 2：加入 include**

在 `main/display/font_bitmap.c` 顶部 `#include "font_bitmap.h"` 之后加入：
```c
#include "font_store.h"
```

- [ ] **步骤 3：修改回退逻辑**

将末尾 `manual_font_cjk_rows` 函数替换为：

```c
const uint8_t *manual_font_cjk_rows(uint32_t codepoint)
{
    for (unsigned i = 0; i < sizeof(CJK_GLYPHS) / sizeof(CJK_GLYPHS[0]); ++i) {
        if (CJK_GLYPHS[i].codepoint == codepoint) return CJK_GLYPHS[i].rows;
    }
    const uint8_t *rows = NULL;
    if (font_store_lookup(codepoint, &rows)) {
        return rows;
    }
    return NULL;
}
```

- [ ] **步骤 4：编译验证**

启用 ESP-IDF 环境后：
```
idf.py build
```
预期：编译成功。确认 `font_bitmap.c` 链接 `font_store` 符号无错误。

- [ ] **步骤 5：Commit**

```bash
git add main/display/font_bitmap.c
git commit -m "feat(display): fall back to flash font for unknown CJK codepoints"
```

---

### 任务 6：`main.c` 初始化调用

**文件：**
- 修改：`main/main.c`

- [ ] **步骤 1：确认初始化位置**

`main/main.c` 约 276 行有 `err = photo_store_init();`。在其后加入字体初始化。先确认 `main.c` 顶部 include 了 `photo_store.h`，添加：

```c
#include "storage/font_store.h"
```

- [ ] **步骤 2：加入初始化调用**

在 `photo_store_init()` 调用之后加入：

```c
    /* Flash font is optional: a missing/corrupt library only degrades missing
     * glyphs to blanks and never blocks boot. */
    font_store_init();
```

放在 photo_store 之后、其他需要字体的 UI 初始化之前。

- [ ] **步骤 3：编译验证**

启用 ESP-IDF 环境后：
```
idf.py build
```
预期：编译成功，无警告报错。

- [ ] **步骤 4：Commit**

```bash
git add main/main.c
git commit -m "feat(main): init flash font_store at startup"
```

---

### 任务 7：烧录与上板验证

**文件：**
- 无（烧录与验证）

- [ ] **步骤 1：烧录固件 + 字库分区**

启用 ESP-IDF 环境后烧录固件：
```
idf.py flash
```
单独烧录字库分区到 `0x270000`：
```
idf.py write-flash 0x270000 tools/font/fonts.bin
```
或使用 `esptool.py`：
```
python -m esptool write_flash 0x270000 tools/font/fonts.bin
```
预期：字库分区烧录成功，无 CRC 校验错误。

- [ ] **步骤 2：上板验证字库加载**

重启设备，观察串口日志出现：
```
font_store: loaded N glyphs
```
N 与 `gen_font.py` 生成的 glyph 数一致。

- [ ] **步骤 3：验证 today_plan 完整显示**

通过 BLE `SET_TODAY_PLAN` 下发含以下内容的计划（覆盖内置 121 字之外的常用字 + 标点 + 数字）：
```
吃火锅 去，三里屯2号
```
确认屏上 `今日计划:吃火锅 去，三里屯2号` 完整显示，无空白格、无乱码。再下发一个全为内置字集的文本（如 `今天去北京`），确认渲染外观与改动前一致（回退不影响命中内置字的字形）。

- [ ] **步骤 4：验证无字库降级路径**

仅烧固件、不烧字库分区，重启。预期：设备正常启动（`font_store: fonts partition not found` 日志），today_plan 缺字回退为空白，不崩溃、不卡死。

- [ ] **步骤 5：Commit（无代码变更时跳过）**

若验证过程中发现需修复的代码问题，修复后单独 commit；否则此步骤留空并注明无需提交。

---

### 任务 8：文档更新

**文件：**
- 修改：`readme.md`

- [ ] **步骤 1：更新目录结构说明**

在 `readme.md` 目录结构段的 `tools/` 说明后补充字体工具，并把 `partitions.csv` 行更新：

```
tools/                  照片量化、面板测量与校准脚本；字体子集生成（tools/font）
partitions.csv          nvs / phy_init / factory / photos / fonts(0xd0000+0x60000) + 备用
```

- [ ] **步骤 2：更新构建与烧录段**

在 `## 构建与烧录` 段追加字库生成与烧录说明：

```
## 字库

today_plan 等任意文本依赖 flash 内置字库。首先生成字库（需 fonttools + pillow）：

    cd tools/font
    python -m pip install fonttools pillow
    python gen_font.py

生成 `tools/font/fonts.bin` 后，烧录到 `fonts` 分区：

    idf.py write-flash 0x270000 tools/font/fonts.bin

字库覆盖 `charactor_list.txt` 的 2500 汉字及常用标点、数字、字母。未烧录时设备仍可正常启动，缺失字形以空白显示。
```

- [ ] **步骤 3：Commit**

```bash
git add readme.md
git commit -m "docs: document flash font generation and flashing"
```

---

## 自检

- **规格覆盖度：**
  - 分区表新增 `fonts` 分区 ✓（任务 3）
  - `fonts.bin` 文件格式 ✓（任务 1 `font_format.py`、任务 2 生成）
  - 固件 mmap + 二分查找 ✓（任务 4 `font_store`）
  - `manual_font_cjk_rows` 回退 ✓（任务 5）
  - `main.c` 初始化 ✓（任务 6）
  - 错误处理/降级 ✓（任务 4 `ESP_LOGW` + 返回码；任务 6 容错；任务 7 验证降级）
  - 工具链（fonttools/Pillow、msyh.ttc、烧录命令）✓（任务 2、任务 7）
  - 测试：gen_font 相关 ✓（任务 1/2 unittest）、上板验证 ✓（任务 7）

- **占位符扫描：** 无 TODO/待定。所有步骤含具体代码或精确命令。

- **类型一致性：**
  - `font_format.pack_font(codepoints, glyph_rows)`（任务 1）与 `gen_font.build_blob(codepoints, glyph_rows)`（任务 2）签名一致。
  - `font_format.index_lookup`（任务 1）与 C `index_lookup`（任务 4）算法一致（`mid = lo + (hi-lo)/2`）。
  - 头文件布局在 Python（`font_format.parse_font`）与 C（`font_store.c`）一致：魔数 `EPF1`、n @0x04、glyph_bytes=128 @0x08、wh=0x00200020 @0x0c、table @0x10、glyph @0x10+4n。
  - `font_store_lookup(uint32_t, const uint8_t **)` 在 `.h` 与 `.c` 一致，`manual_font_cjk_rows` 调用签名匹配。
  - 分区子类型 `0x41` 在 `partitions.csv`（任务 3）与 `font_store.c`（任务 4）一致。