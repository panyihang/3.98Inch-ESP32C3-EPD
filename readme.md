# 3.98Inch-ESP32C3-EPD

适用于华为 nova14/15 墨水屏手机壳的电子相册/日历

基于 **ESP32-C3 + 3.98 英寸四色墨水屏（768×552，黑/白/黄/红）** 的墨水屏手机壳固件：显示日历、照片相框或两者合成页，通过 BLE 由手机小程序配网、上传照片和修改设置。

---

## 实物照片

<table>
  <tr>
    <td align="center"><img src="docs/device-calendar.webp" width="440" alt="装进华为 nova 手机壳的日历页"><br><sub></sub></td>
    <td align="center"><img src="docs/device-stand.webp" width="220" alt="结构件支架形态的相框模式"><br><sub></sub></td>
  </tr>
</table>

## 功能

- **四种显示模式**：日历 / 相框 / 日历+相框 / 状态页
- **照片相册**：768×552 四色 2bpp 原始画面，4 个照片槽位，每槽双 bank 写入，上传中断电不破坏已显示的照片
- **BLE 控制**：私有 GATT 服务 + 帧协议（Magic `EP`、CRC-16/CCITT、序号去重），共 14 条命令，支持命令结果 Notify
- **BLE 配网**：Wi-Fi 凭据经 BLE 写入并保存到 NVS，不使用 ESPTouch/SmartConfig；启动后自动重连
- **网络功能**：SNTP 校时 + 天气拉取，屏幕显示温度与天气摘要
- **今日计划**：最多 30 字节 UTF-8 文本，由小程序下发并持久化
- **旋转与刷新策略**：0/90/180/270° 软件旋转；自动刷新最快 15 分钟一次，所有刷新走单一串行队列并自动合并
- **内置字库与画布**：固件内自带点阵字库和手写画布渲染，无外部图形库依赖
- **串口测试图样**：不接手机也能通过 USB 串口输入 `1`~`9`、`i`、`A`/`B` 等字符输出自检图样

## 硬件

| 项目 | 说明 |
| --- | --- |
| MCU | ESP32-C3，内置 4 MB Flash，控制台走 USB-Serial/JTAG |
| 屏幕 | 3.98 英寸四色墨水屏，可见区 768×552，控制器内部帧 800×600×2bpp（JD79665 系列） |
| FPC / PCB | [立创开源硬件：3.98 寸四色墨水屏日历相框](https://oshwhub.com/an_ye/project_wgvyuprv) |
| 结构件 | [MakerWorld：4 寸彩色墨水屏日历相框·华为](https://makerworld.com.cn/zh/models/3014683-4cun-cai-se-mo-shui-ping-ri-li-xiang-kuang-hua-wei#profileId-3541528) |


## 目录结构

```
main/
├── main.c              启动流程、任务编排、串口诊断
├── epd_config.h        屏幕几何、引脚、颜色码、时序参数
├── epd_panel.c/.h      JD79665 面板驱动与刷新流程
├── app/                刷新策略、刷新队列、应用状态
├── display/            驱动封装、软件旋转、手写画布、点阵字库
├── ble/                GATT 服务、协议解帧、命令分发、照片上传
├── net/                Wi-Fi、配网、SNTP、天气
├── storage/            NVS 配置存储、照片分区双 bank 存储
├── ui/                 日历 / 相框 / 状态页渲染
└── test_patterns.c     串口触发的面板自检图样
docs/                   BLE 协议规范与交接说明（中文）+ 实物照片
tools/                  照片量化、面板测量与校准脚本；字体子集生成（tools/font）
partitions.csv          nvs / phy_init / factory / photos / fonts + 备用
```

## 构建与烧录

需要 ESP-IDF **6.0 或更高版本**（`main/idf_component.yml` 依赖 `h2zero/esp-nimble-cpp`，首次构建会自动下载）。

`sdkconfig.defaults` 已固定目标芯片、4 MB Flash、自定义分区表和 NimBLE 单连接外设配置，直接构建即可。

## 字库

today_plan 等任意文本依赖 flash 内置字库，覆盖 `charactor_list.txt` 的 2500 汉字及常用标点、数字、字母。首先生成字库（需 `fonttools` 与 `pillow`）：

    cd tools/font
    python -m pip install fonttools pillow
    python gen_font.py

生成 `tools/font/fonts.bin` 后，`idf.py flash`（含 VS Code ESP-IDF 插件烧录）会自动把它烧到 `fonts` 分区（`0x270000`）——项目根 `CMakeLists.txt` 已通过 `esptool_py_flash_to_partition` 注册。因此直接正常烧录即可：

    idf.py flash

若 `tools/font/fonts.bin` 不存在（例如新克隆仓库），烧录会自动跳过 `fonts` 分区；请先运行上面的 `gen_font.py` 生成后再烧录。

未烧录字库时设备仍可正常启动，缺失字形以空白显示。

## BLE 接口

| 特征 | UUID | 用途 |
| --- | --- | --- |
| Service | `7b4d0001-6a6c-4c61-9f20-65706463616c` | 私有服务 |
| Control RX | `7b4d0002-6a6c-4c61-9f20-65706463616c` | 控制命令写入 |
| Notify TX | `7b4d0003-6a6c-4c61-9f20-65706463616c` | 命令结果通知 |
| Image RX | `7b4d0004-6a6c-4c61-9f20-65706463616c` | `PHOTO_DATA` 图像数据 |

命令一览：`GET_INFO`、`SET_ROTATION`、`SET_MODE`、`REFRESH`、`NEXT_PHOTO`、`SET_TIMEZONE`、`SET_LOCATION`、`WIFI_SET_CREDENTIALS`、`WEATHER_REFRESH`、`PHOTO_BEGIN`、`PHOTO_DATA`、`PHOTO_END`、`PHOTO_ABORT`、`SET_TODAY_PLAN`。

完整帧格式、状态码、`GET_INFO` 字段布局与照片上传流程见：

- [`docs/ble_protocol.md`](docs/ble_protocol.md) —— 小程序与固件之间的接口契约
- [`docs/ble_handover.md`](docs/ble_handover.md) —— 连接流程、上传参考实现与上板验收清单

## 注意事项

- 当前 GATT 刻意保持开放：**不配对、不绑定、不加密**。协议 CRC 只用于检测传输损坏，不提供机密性或身份认证；Wi-Fi 密码会以明文经过 BLE 空中链路，请仅在可信的近距离环境下配网。
- 设备只允许一个 BLE 连接；响应可能被拆成多条通知，客户端需要按字节流拼帧解析。
- 照片必须是完整的 105984 字节（768×552、2bpp）画面，提交前由固件做 CRC32 与 Flash 回读校验。