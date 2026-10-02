# XDS All-In-One

**用一块 nRF52840 让喜德盛（XDS）BLE 功率计直接接入 ANT+ 码表**

把原本需要 **ESP32 + Feather nRF52840 两块板**才能完成的事，合并到**一块 SuperMini NRF52840** 上：
同时充当 **BLE 主机**（连功率计读数据）与 **ANT+ 主机**（向码表广播功率/踏频）。

[![Board](https://img.shields.io/badge/board-SuperMini%20nRF52840-blue)]()
[![SoftDevice](https://img.shields.io/badge/SoftDevice-S340%206.1.1-green)]()
[![License](https://img.shields.io/badge/license-MIT%20(with%20exceptions)-lightgrey)](LICENSE)

---

## 特性

- 🔗 **单板双角色**：BLE Central（连功率计）+ BLE Peripheral（手机桥接）+ ANT+ Master（码表）并发运行
- 📊 **真实数据**：功率、踏频、曲柄角度、左右腿功率、错误码
- 🎯 **未连接时一眼可辨**：功率计没连上时，码表显示固定的 **999 W / 250 rpm**（真实骑行不可能持续 999W）
- 🔌 **断连自动重连**：功率计断开后自动重新扫描
- 🛠️ **可调试**：115200 串口命令（`status` / `scan` / `disconnect` 等），状态含固件版本标记
- 📄 **完整踩坑记录**：[`SETUP_NOTES.md`](SETUP_NOTES.md) 记录了全部环境改动与 6 个关键 bug 的定位过程

## 适用场景

喜德盛（XDS）等**只有 BLE、没有 ANT+** 的国产功率计，想要在 **Garmin / XOSS / 迈金等 ANT+ 码表**上直接看功率。
本项目相当于一个"BLE → ANT+ 协议转换器"。

## 硬件需求

| 项目 | 说明 |
|---|---|
| 开发板 | **SuperMini NRF52840**（ProMicro 外形，nice!nano 兼容克隆板） |
| 软设备 | 必须 **S340**（ANT + BLE 二合一）；官方 BSP 自带的 S140 **不支持 ANT** |
| 引导器 | **w.ANT** 版（Adafruit Feather nRF52840 Express w.ANT） |
| 功率计 | 喜德盛（XDS）功率计（BLE 服务 `0x1828` / 特征 `0x2A63`） |
| 码表 | 任意 ANT+ 码表（本项目用 XOSS NAV 验证） |
| 供电 | USB 或 3.7V 锂电池（峰值约 15mA） |

> 部分克隆板需要把 **`RST` 与 `3.3V` 短接**（冷启动时复位脚被拉低）。
> 正常板子无需此操作，详见 [`SETUP_NOTES.md`](SETUP_NOTES.md) 第 1 节。

## 快速开始

```bash
# 1. 安装 ANT 版 Arduino BSP（基于 Adafruit nRF52 1.7.0）
#    https://github.com/1wpc/Adafruit_nRF52_Arduino_ANT

# 2. 给 Bluefruit52Lib 打 CCCD 补丁 —— 最关键的步骤！
#    不打卡片会「连接成功但永远收不到数据」，详见 SETUP_NOTES.md 第 2.2 节

# 3. 用 Arduino IDE 打开本文件夹，开发板选：
#    Adafruit Bluefruit Feather nRF52840 Express w.ANT
#    （FQBN: adafruit:nrf52:feather52840_s340）

# 4. 编译并上传（会自动 1200bps touch 进 DFU，无需手动按键）
arduino-cli compile --fqbn adafruit:nrf52:feather52840_s340 --export-binaries .
arduino-cli upload  --fqbn adafruit:nrf52:feather52840_s340 -p COM8 .
```

上电后：打开功率计 → 板子自动连接 → ANT+ 码表搜索功率传感器（**设备号 1000**，通道 57）。

## 数据流

```
喜德盛功率计 (BLE 0x1828/0x2A63，11 字节通知)
   │  BLE Central
   ▼
SuperMini NRF52840  ──  BLE Peripheral("XDS Power Bridge")  ──►  手机 App
   │
   └──  ANT+ Master (4Hz, 2457MHz, 设备号 1000)  ──►  ANT+ 码表
```

## 文档

| 文件 | 内容 |
|---|---|
| [`SETUP_NOTES.md`](SETUP_NOTES.md) | ★ **环境改动、关键补丁、踩坑记录、换新板复现步骤** |
| [`LICENSE`](LICENSE) | MIT（附第三方文件例外说明） |

> ⚠️ **只 clone 本仓库无法直接编译** —— 还需要 ANT 版 BSP，以及给 Bluefruit52Lib
> 打上 CCCD 补丁（这是"连上却收不到数据"的根因）。请务必先读 [`SETUP_NOTES.md`](SETUP_NOTES.md)。

---

## 1. 原方案：两块板子各干了什么

```
喜德盛功率计 (XDS Power Meter, BLE 0x1828/0x2A63 通知)
   │  ① BLE 连接
   ├──────────────► ESP32 DevKit C V4 ──BLE Server("XDS Power Bridge")──► 手机App
   │                (XDSmonitor.ino：BLE 双向桥)
   │
   └──────────────► Feather nRF52840 ──ANT+ Master(4Hz, 57ch)──► Garmin等ANT+码表
                    (PowerMeter_v1.ino：BLE Central + ANT+ 广播)
```

- **ESP32（XDSmonitor.ino）**：作为 BLE 主机连上喜德盛功率计，把收到的 11 字节数据包原样转发给自建的 BLE 服务（0x1828/0x2A63，广播名 "XDS Power Bridge"）。作用：功率计通常只允许**一个** BLE 连接，桥接后手机 App 也能读到数据。
- **Feather nRF52840（PowerMeter_v1.ino）**：作为 BLE 主机直接连功率计读数据，同时用 nRF52840 内置 ANT 协议栈（S140 软设备）以 ANT+ 功率计协议（4Hz，2457MHz，设备号 1000）广播，让只有 ANT+ 的码表能搜到并显示。断连时自动生成约 100W/70RPM 虚拟数据兜底。

## 2. 为什么一块 Promicro NRF52840 就够了

nRF52840 的 **S140 SoftDevice 原生支持多协议并发**：BLE 外设 + BLE 主机 + ANT 可同时运行——Feather 单板能同时做 BLE Central 和 ANT Master 已经证明了这一点。它只缺 ESP32 的"第二个 BLE 服务"角色，而 Bluefruit 库只需把外设连接数从 0 改成 1 并多建一个服务即可。

| 原硬件 | 职责 | 合并后（单块 Promicro NRF52840） |
|---|---|---|
| ESP32 DevKit C V4 | BLE Central → 功率计；BLE Server → 手机 | 外设角色 1 路（BLE Server 桥接） |
| Feather nRF52840 | BLE Central → 功率计；ANT+ Master → 码表 | 主机角色 1 路（接功率计）+ ANT+ Master |
| ESP32 电源扩展板 | 5V 降压供电 | 删除 |
| 5V 锂电池 | 给 ESP32 供电 | 删除 |
| 3.7V 锂电池 | 给 Feather 供电 | 保留，改给单板供电 |

## 3. 精简后的硬件清单与接线

**新清单：**
- 1× Promicro NRF52840（SparkFun Pro nRF52840 Mini，或兼容克隆；必须是带 S140 软设备的 Adafruit/SparkFun 兼容板）
- 1× 3.7V 锂电池（带保护板）
- 可选：TP4056 充电模块（板子无内置充电时用）

**删除：** ESP32 DevKit C V4、ESP32 电源扩展板、5V 锂电池、Feather nRF52840 Express。

**供电接线：**
- 若板子带 JST 电池座 + 充电 IC（部分克隆板）：直接插 3.7V 锂电池即可。
- SparkFun Pro nRF52840 Mini（无充电管理）：电池正极接 **VIN**（板上 3.3V LDO，输入约 3.3–6V，锂电池 3.0–4.2V 在范围内），负极接 **GND**；充电用 TP4056 模块串联在电池与 VIN 之间。

## 4. 合并后的数据流

```
喜德盛功率计 ──BLE──► Promicro NRF52840 ──BLE Server("XDS Power Bridge")──► 手机App
                          │
                          └────────ANT+ Master────────► Garmin等ANT+码表
```

## 5. 固件改动说明（本工程已改好）

所有改动集中在 `src/PowerMeter/PowerMeter.h/.cpp`，其余文件（sdant、ANTProfile、BicyclePower、ant_channel_config 等）原样保留：

1. `PowerMeter::begin()`：`Bluefruit.begin(0, 1)` → **`Bluefruit.begin(1, 1)`**（打开 1 路外设角色）。
2. 新增 `initBLEServer()`：建立 0x1828 服务 + 0x2A63 特征值（READ/WRITE/NOTIFY/INDICATE，自动带 CCCD），广播名 **"XDS Power Bridge"**（与原 ESP32 一致，App 无需改动）。
3. `onPowerMeasurementNotify()` 末尾新增转发：`powerBridgeChar.setValue(data, len)` + `notify(serverConnHandle)`——即原 ESP32 的全部功能。
4. 新增外设角色连接/断开回调，记录 `serverDeviceConnected`/`serverConnHandle`；断连后 `restartOnDisconnect(true)` 自动恢复广播。
5. ANT 广播、虚拟数据、扫描重连、串口命令（`help`/`status`/`scan`/`enable`/`disable`/`disconnect`）逻辑不变。

## 6. 编译与烧录

1. Arduino IDE 安装板卡包：
   - Adafruit nRF52 BSP（追加到 开发板管理器地址）：`https://adafruit.github.io/arduino-board-index/package_adafruit_index.json`
   - SparkFun Pro nRF52840 Mini 用 SparkFun 包：`https://raw.githubusercontent.com/sparkfun/Arduino_Boards/master/package_sparkfun_index.json`，选 **SparkFun nRF52840 Boards / Pro nRF52840 Mini**；兼容克隆板通常也选它（或选 Adafruit Feather nRF52840 Express）。
2. 库管理器安装：**Adafruit Bluefruit nRF52 Libraries**。
3. 打开整个 `XDS_AllInOne` 文件夹作为一个 sketch，选择对应板子后烧录。
4. 上电后 115200 波特率串口可查看日志；功率计连接成功后，码表搜索 ANT+ 功率传感器（设备号 1000，通道 57），手机 App 搜索 "XDS Power Bridge"。

## 7. 注意事项

- **ANT 许可**：ANT 协议仅允许非商业个人用途免费，商用需向 thisisant.com 购买许可（源码注释中已注明）。
- **BLE 连接数**：喜德盛功率计若只允许 1 个 BLE 连接，本单板方案没有竞争；若手机 App 直接连了功率计，本板会连接失败，请让 App 连接本板的 "XDS Power Bridge" 转发。
- **电流**：BLE+ANT 并发峰值电流约 15mA 量级，3.7V 锂电池余量充足；电池务必带保护板。
- **可选：开启 ANT+ 踏频字段**：将 `PowerMeter::update()` 中的 `pwr->SetInstantCadence(0xFF)` 改为 `pwr->SetInstantCadence(instCAD)`（0xFF 表示关闭）。
- 桥接服务转发的是**原始 11 字节 XDS 数据包**（总功率/左腿/右腿/角度/踏频/错误码），与 ESP32 版完全一致，兼容现有 App。

## 8. 来源与许可（必读）

本项目**整合**了多份第三方代码，版权归各自作者所有：

| 文件 / 目录 | 来源 | 说明 |
|---|---|---|
| `src/ant_interface.h`、`src/ant_parameters.h`、`src/ant_event.h`、`src/ant_channel_config.*` | Nordic Semiconductor ANT SDK | Nordic 5-Clause / ANT 许可 |
| `src/ANTProfile.*`、`src/sdant.*`、`src/PowerMeter/BicyclePower.*`、`src/util.h` | 第三方 ANT+ Arduino 项目（基于 Nordic SDK 改写） | 见原项目许可 |
| `src/PowerMeter/PowerMeter.*`、`XDS_AllInOne.ino`、本文档 | 本项目作者 | **MIT**（见 [LICENSE](LICENSE)） |
| Bluefruit52Lib 的 CCCD 补丁 | **不在本仓库**，仅在 [`SETUP_NOTES.md`](SETUP_NOTES.md) 中说明做法 | Adafruit 原许可 |

> **ANT 协议许可**：ANT / ANT+ 协议仅允许**非商业个人用途**免费使用；
> 任何商业用途均需向 <https://www.thisisant.com> 购买许可。

## 9. 依赖的外部项目

- **ANT 版 Arduino BSP**（提供 S340 软设备 + w.ANT 支持）：
  [`1wpc/Adafruit_nRF52_Arduino_ANT`](https://github.com/1wpc/Adafruit_nRF52_Arduino_ANT)
- **ESP32 参考实现（本项目得以定位关键 bug 的重要参照）**：
  [`1wpc/xds_repeater`](https://github.com/1wpc/xds_repeater)
- 底层库：Adafruit nRF52 Arduino BSP 1.7.0 / Bluefruit52Lib / TinyUSB

> ⚠️ 仅 clone 本仓库**无法直接编译**：还需要按 [`SETUP_NOTES.md`](SETUP_NOTES.md)
> 安装 ANT 版 BSP，并给 Bluefruit52Lib 打上 CCCD 补丁（这是"连上却收不到数据"的根因）。
> 请优先给1wpc的项目star，谢谢！！！！！！！

