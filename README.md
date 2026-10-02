# XDS All-In-One：用一块 Promicro NRF52840 精简 xds-tutorial 硬件

> ⚠️ **重要**：本工程依赖**若干工程代码之外的环境改动**（ANT 版 BSP、Bluefruit 库的
> CCCD 补丁、RST↔3.3V 硬件短接等）。换新板子、重装开发环境或想复现本项目时，
> **请先读 [`SETUP_NOTES.md`](SETUP_NOTES.md)** —— 那些改动丢了会重现"收不到数据"、
> "冷启动不工作"等疑难问题。

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
