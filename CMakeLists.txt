cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(epd_3in98_c3)

# 让 `idf.py flash`（含 VS Code ESP-IDF 插件烧录）自动包含 fonts 分区字库。
# fonts.bin 是生成产物，由 tools/font/gen_font.py 生成；缺失时跳过烧录该分区
# （请先运行 tools/font/gen_font.py 生成）。
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/font/fonts.bin")
    esptool_py_flash_to_partition(flash fonts "${CMAKE_SOURCE_DIR}/tools/font/fonts.bin")
endif()
