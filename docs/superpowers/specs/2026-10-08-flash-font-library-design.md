# 闪存字库设计：完整 CJK 字体修复 today_plan 缺字

## 背景与问题

固件内置点阵字库 `main/display/font_bitmap.c` 仅硬编码了 **121 个精选 CJK 字形**
（32×32、1bpp、每字 128 字节）。`manual_font_cjk_rows()` 对未收录的字返回
`NULL`，导致 today_plan 等任意用户文本中不在该子集内的汉字被渲染为**空白格**。

`today_plan` 通过 BLE `SET_TODAY_PLAN` 下发，最多 30 字节 UTF-8，可为任意汉字。
现有字库覆盖不足，无法完整显示。

## 目标

- 用系统字体（微软雅黑 `C:\Windows\Fonts\msyh.ttc`）为以下字符集生成位图字库：
  - `charactor_list.txt` 中的 **2500 个汉字**
  - 中文标点符号（约 20 个）
  - ASCII 数字/字母（复用现有内置字库，不重复存储）
- 字库体积尽可能小。
- 字库存入分区备用空间（`0x270000..0x400000`，约 1.56MB）的独立分区。
- 固件运行时只读映射，按需加载字形，修复 today_plan（及其他所有文本渲染）缺字。

## 约束

- 目标芯片 ESP32-C3，4MB Flash，内部无 PSRAM。
- 需尽量降低 RAM / CPU 开销，避免引入重型渲染依赖。
- 渲染质量应与现有日历字体一致（现从 32×32 源缩放出 20px medium、16px compact）。
- 字库不可用时不得阻塞启动，需回退到现有行为（缺字空白）而非崩溃。

## 架构总览

```
msyh.ttc + charactor_list.txt(2500字) + 标点表
      │  离线脚本 tools/font/gen_font.py (fonttools + Pillow)
      ▼
   fonts.bin (~333KB) ──esptool 一次性烧录──► 分区 fonts @ 0x270000 (384KB)
                                                    │
                   固件运行: esp_partition_mmap ◄────┘
                                                    │
  manual_font_cjk_rows(cp): 内置121字命中? → 返回内置指针
                           否则 → 查 flash 索引(二分) → 返回 mmap 位图
                           否则 → NULL（维持现有 blank 行为）
```

## 方案选择

对比三种存储/渲染方案后采用**方案 A：离线光栅化 + 分区 mmap + 回退查找**。

| 方案 | 说明 | 体积 | 权衡 |
|---|---|---|---|
| **A（采用）** | 离线把 TTF 子集光栅化为 1bpp 位图，存字体分区，mmap 按需读 + 回退 | ~333KB | 零 RAM、无运行时依赖、质量与现状一致、符合 photo_store 分区模式；需离线工具+一次性烧录 |
| B | 把位图直接编译进固件（扩充 `CJK_GLYPHS`） | ~320KB | 实现最简但固件膨胀、违背"写入分区"要求、更新字库需重编译 |
| C | 存子集 TTF + 运行时 FreeType 光栅化 | ~1-2MB | 任意尺寸清晰但 ESP32-C3 RAM/CPU 开销大、依赖重、大材小用 |

## 详细设计

### 1. 分区表（partitions.csv）

新增一行，占用备用空间开头：

```
fonts,   data, 0x41,     0x270000, 0x60000,
```

- 384KB（`0x60000`）字体分区，从 `0x270000` 开始。
- 子类型 `0x41` 为自定义 data 子类型，避开 ESP-IDF 保留 data 子类型，避免被 ESP-IDF
  误当默认分区处理。
- 分区为**只读工厂数据**：烧录时一次性写入，运行时只读，从不写。
- 剩余备用空间（约 1.17MB）继续留给未来。

### 2. 字库文件格式（fonts.bin）

```
偏移        内容
0x00        魔数 "EPF1" (4B)
0x04        glyph 数 n (uint32 LE)
0x08        每字形字节数 (uint32 LE) = 128
0x0C        字面宽/高 (uint32 LE) = 0x00200020
0x10        codepoint 表 [uint32 LE × n]，升序（供二分查找）
0x10+4n     位图数据 [n × 128B]，32×32 1bpp，每行 4 字节
```

位图布局与现有 `manual_font_cjk_rows` 返回的格式一致：行序从上到下，
每行 4 字节，MSB 在左，即 `rows[y*4 + (x>>3)]` 的 bit `(7 - (x&7))`。

### 3. 固件侧改动

**`main/storage/font_store.c` + `font_store.h`（新增）**

封装字体分区的只读访问：

- `esp_err_t font_store_init(void)`
  - `esp_partition_find_first(DATA, 0x41, "fonts")` 找分区。
  - 校验分区足够大，`esp_partition_mmap` 映射到只读地址。
  - 校验魔数 `"EPF1"`；失败返回错误码。
  - 静态保存 n、位图数据起始指针、codepoint 表指针。
- `int font_store_lookup(uint32_t codepoint, const uint8_t **rows)`
  - 在升序 codepoint 表上二分查找；命中置 `*rows` 为对应位图指针并返回 1，否则返回 0。

非 ESP32（无 `ESP_PLATFORM`）环境提供空实现，保持与现有代码一致的
`#ifdef ESP_PLATFORM` 模式。

**`main/display/font_bitmap.c`**

修改 `manual_font_cjk_rows(uint32_t codepoint)`：

1. 先在内置 `CJK_GLYPHS` 中查找；命中返回内置指针（行为不变）。
2. 未命中则调用 `font_store_lookup(codepoint, &rows)`；命中返回 flash 位图指针。
3. 否则返回 `NULL`（维持现有 blank 行为）。

函数签名不变，调用方零改动。需在 `font_bitmap.c` 中包含 `font_store.h`。

**`main/main.c`**

初始化流程（`photo_store_init()` 附近）加入 `font_store_init()`。
容错：失败仅记录日志（`ESP_LOGW`），不阻塞启动。

**`main/CMakeLists.txt`**

在 `SRCS` 中加入 `"storage/font_store.c"`。

**渲染路径**

`manual_canvas.c` 的 `text_utf8 / text_compact / text_medium` 均通过
`manual_font_cjk_rows()` 取源位图后做缩放，**无需改动**，自动获得完整字形。

### 4. 字符集

- 2500 汉字：读 `charactor_list.txt`（UTF-8），逐行去空白后收集全部 codepoint。
- 中文标点（约 20 个）：`,。、？！：；""''…—·《》（）` 及可能用到的小数点/连接符。
- ASCII 数字/字母：复用现有 `manual_font_ascii_rows`，不重复存储。

### 5. 工具链

**`tools/font/gen_font.py`（新增）**

- 参数：源字体路径（默认 `C:\Windows\Fonts\msyh.ttc`）、`charactor_list.txt` 路径、
  输出 `fonts.bin` 路径。
- 流程：
  1. 用 `fontTools.TTCollection` 打开 `msyh.ttc`，取实例 `[0]`。
  2. 构造目标字符集（2500 汉字 + 标点）。
  3. 子集化字体（`fontTools.subset`）仅保留目标字符，减小光栅化前体积。
  4. 对每个字符用 Pillow 渲染为 32×32 1bpp 位图（黑体常规字重，居中，按 bbox 裁剪）。
  5. 按 codepoint 升序写入 `fonts.bin`（头部 + codepoint 表 + 位图区）。
- 依赖：`python -m pip install fonttools pillow`。
- 输出：`tools/font/fonts.bin`（约 333KB）。

**烧录**

```
esptool.py write_flash 0x270000 tools/font/fonts.bin
```

或通过 `idf.py` 对指定偏移烧录。字库与固件解耦，可独立更新。

### 6. 错误处理 / 健壮性

- `font_store_init` 任何失败（分区缺失、魔数错、mmap 失败）返回错误码；
  调用方仅记录日志，系统继续运行，today_plan 缺字回到现状，不崩溃。
- 索引二分查找 O(log n)，一次字形读取 O(1)（mmap 直接指针访问，无拷贝）。
- mmap 只读映射，位图区不可变，无并发写风险。

## 测试

- **单元/静态验证**：`gen_font.py` 生成的 `fonts.bin` 头部魔数、n、位图大小自检；
  可选抽样校验若干字符的位图非全空。
- **上板验证**：
  - 烧录字库分区后，通过 BLE 下发包含 `charactor_list.txt` 中代表字（及内置 121 字之外
    的常用字）的 today_plan，确认完整显示、无空白格。
  - 内置 121 字集的日历 UI 渲染外观不变（回退路径不影响命中内置字的字形）。
  - 不烧录字库分区时设备正常启动，today_plan 缺字回退为空白，无崩溃。

## 非目标（YAGNI）

- 不做运行时 TTF 光栅化（FreeType）。
- 不做多字重 / 多字号字库（仅 32×32 单字重，缩放复用现有逻辑）。
- 不通过 BLE 热更新字库（工厂烧录即可）。
- 不为 `charactor_list.txt` 之外的字保证显示（超出仍为空白，但覆盖了常见 2500 字）。