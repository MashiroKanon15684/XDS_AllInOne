# SETUP_NOTES — 环境搭建、关键补丁与踩坑记录

> 本文件记录**工程代码之外的**环境改动和踩坑经验。
> 这些内容**装不进 git 仓库**，但一旦丢失就会重现当年那些"怎么也调不通"的问题，
> 所以单独记在这里。换新板子 / 重装开发环境 / 分享给别人时，请先读这份文件。

---

## 0. 一句话总览

让"喜德盛 XDS 功率计 → 单块 nRF52840 → ANT+ 码表"跑通，除了本仓库的代码，
还额外需要 **4 处环境改动**：

| # | 改动 | 在哪里 | 丢了会怎样 |
|---|---|---|---|
| 1 | 换用 **ANT 版 BSP**（S340 软设备 + w.ANT 引导器） | Arduino 板卡包目录 | ANT+ 完全无法使用（S140 不支持 ANT） |
| 2 | 给 Bluefruit 库打 **CCCD Write Request 补丁** | Bluefruit52Lib | **收不到功率计任何数据**（本项目的核心坑） |
| 3 | 硬件上 **RST ↔ 3.3V** 短接 | 板子上 | 冷启动时芯片被按在复位里，USB/蓝牙/ANT 全都不工作 |
| 4 | 板子上刷入 **S340 软设备 + w.ANT 引导器** | 芯片 Flash | 同上，ANT 无法使用 |

---

## 1. 硬件

- **开发板**：SuperMini NRF52840（ProMicro 外形，nice!nano 兼容的克隆板）
- **功率计**：喜德盛（XDS）功率计，BLE 服务 `0x1828` / 特征 `0x2A63`，11 字节通知
- **码表**：任意 ANT+ 码表（本项目用 XOSS NAV 验证）

### 1.1 【必须】RST ↔ 3.3V 短接

**现象**：不接这根线时，冷启动（上电）后芯片**完全不动** ——
USB 不枚举（电脑无串口）、蓝牙/ANT 都不工作；只有把 `RESET` 顶到高电平才能起来。

**原因**：冷启动时 `P0.18`（该脚即复位脚，`UICR.PSELRESET = [18,18]`）被板上的电路拉低，
芯片一直处于复位状态。注意：**UICR 配置本身是正常的**（已实测 `REGOUT0=0xFFFFFFFD`
即 3.3V、`PSELRESET=[18,18]`），所以这不是固件能解决的问题。

**做法**（两种都验证可用）：

| 方案 | 说明 |
|---|---|
| **直接短接** `RST` ↔ `3.3V` | 最省事，实测可用。代价：**失去手动复位**（以后烧固件靠 USB 1200bps touch 或 DFU 脚，够用） |
| 串 **10kΩ** 电阻到 `3.3V` | 更正规，保留"接地仍可复位"。若 10k 仍不行，说明该线被拉得较狠，直接用方案一 |

> ⚠️ **绝对不要接 `RAW`**：RAW 是 5V（USB）或 4.2V（电池）的未稳压输入，
> 而 `P0.18` 绝对最大额定仅 3.6V，长期顶 RAW 可能打坏该引脚。
> 网上"顶 RAW 能修"的说法是拿引脚 ESD 二极管倒灌，属于危险操作。

### 1.2 供电

- USB 或 3.7V 锂电池均可；实测用**充电宝**供电正常工作
- 峰值电流约 15mA 量级（BLE + ANT 并发），电池余量充足
- 电池请带保护板

---

## 2. 开发环境（Arduino IDE）

### 2.1 板卡包：必须换成 ANT 版

Nordic 的 SoftDevice 命名规则决定了这一点：

| SoftDevice | 能力 |
|---|---|
| **S140** | 只有 BLE，**不能**跑 ANT |
| **S340** | ANT + BLE 二合一 ← **本项目必须用这个** |

而 Arduino 官方 BSP 只带 S140。本项目使用带 ANT 的 fork：

- **fork 仓库**：`1wpc/Adafruit_nRF52_Arduino_ANT`（基于 Adafruit nRF52 **1.7.0**）
- **本机安装位置**：`C:\Users\ikun\AppData\Local\Arduino15\packages\adafruit\hardware\nrf52\1.7.0`
- **原始官方版备份**：`C:\Users\ikun\nrf52_1.7.0_stock`（需要回退时改名替换即可）

该 fork 与官方版的差异（也是它能跑 ANT 的原因）：

1. 内含 `s340_nrf52_6.1.1_API`（S340 头文件 + `sd_softdevice_enable()` 的 **3 参数**版本）
2. `nrf_sdm.h` 里预置了评估用 ANT 许可密钥（`ANT_LICENSE_KEY`），因此 `sd_softdevice_enable()`
   会带第三个参数；这也是 `src/sdant.cpp` 里对 `Bluefruit.setMultiprotocolSemaphore()` 做
   `#ifdef ANT_LICENSE_KEY` 保护的原因
3. 板型列表里有 `feather52840_s340`（**Adafruit Bluefruit Feather nRF52840 Express w.ANT**）
4. 带 `w.ANT` 版引导器

**编译用 FQBN**：`adafruit:nrf52:feather52840_s340`

### 2.2 【核心坑】Bluefruit 库的 CCCD 补丁

**这是"能用 ESP32 收到数据、用 nRF52840 收不到"的真正原因。**

Adafruit 的 `BLEClientCharacteristic::writeCCCD()` 原文用的是
**Write Command（无响应写入）**：

```c
// 文件：libraries/Bluefruit52Lib/src/BLEClientCharacteristic.cpp
.write_op = BLE_GATT_OP_WRITE_CMD,          // ← 无响应
.flags    = BLE_GATT_EXEC_WRITE_FLAG_PREPARED_WRITE,
```

而**标准 CCCD 属性通常不允许 Write Without Response**，这个写命令会被对端（功率计）
**静默丢弃**。于是：

- `enableNotify()` 返回 `true`（因为 `sd_ble_gattc_write()` 只表示"软设备把命令排进了队列"，
  并不代表对端接受）
- CCCD 实际从未生效 → **功率计一个数据包都不发**
- 而 ESP32（Bluedroid）的 CCCD 写入是**带响应**的，所以参考实现 `xds_repeater` 能收到

**补丁内容**（本机已打，原文件备份为同目录 `BLEClientCharacteristic.cpp.orig`）：

```c
.write_op = BLE_GATT_OP_WRITE_REQ,   // 改为带响应写入
.flags    = 0,
...
// WRITE_REQ 不用 Write Command 的发送缓冲池，故不再校验 getWriteCmdPacket()
BLEConnection* conn = Bluefruit.Connection(conn_handle);
VERIFY( conn );
```

打上之后立刻就能收到数据（实测 60 秒内 74 个包，0 丢包）。

> **重装/升级 BSP 会丢掉这个补丁**，务必重新打上，或把补丁提交给 fork 作者。

### 2.3 MTU

Adafruit 核默认 ATT MTU = **23**（单包最多 20 字节），且核心从不主动协商。
本项目在 `PowerMeter::onConnect()` 里主动申请 247：

```cpp
BLEConnection* conn = Bluefruit.Connection(conn_handle);
conn->requestMtuExchange(247);
```

实测协商成功（`MTU after exchange = 247`），参考实现（ESP32）也要到了 517。
虽然功率计的通知包只有 11 字节、MTU 23 也放得下，但**保持大 MTU 更稳妥**。

### 2.4 variant.h 说明（已回退，勿改）

排查冷启动问题时曾把 `variants/feather_nrf52840_express/variant.h` 的
`USE_LFXO` 改成 `USE_LFRC` 试过，**结论是无效，已还原为 `USE_LFXO`**。

原因：**ANT+ 对时钟精度要求约 50ppm，而内部 RC 是 250ppm**，
改用 LFRC 会让 ANT+ 时序不合格（甚至被 SoftDevice 拒绝）。
所以这块板**必须用外部 32.768kHz 晶振**，不要动这个宏。

（备份：同目录 `variant.h.orig`）

---

## 3. 烧录芯片：S340 软设备 + w.ANT 引导器

这一步**只在全新板子或需要升级引导器时做一次**。

- 引导器工程：`C:\Users\ikun\ANT_Adafruit_bootloader`
- 预编译产物：
  - `_build\build-feather_nrf52840_express_s340\..._s340_6.1.1.zip`（**DFU 包，含 SD+BL**）
  - 同目录 `..._s340_6.1.1.hex`
- 包内已含 `lib\softdevice\s340_nrf52_6.1.1\s340_nrf52_6.1.1_softdevice.hex`

烧录后可用 UF2 盘的 `INFO_UF2.TXT` 核对（本机验证结果）：

```
UF2 Bootloader 0.3.0-403-gda70d4a-dirty
Model: Adafruit Feather nRF52840 Express w.ANT
Board-ID: nRF52840-Feather-revD
SoftDevice: S340 6.1.1
```

> ⚠️ **重要提醒**：刷这种"**SD + 引导器整体包**"（DFU type 3）时，
> MBR 更新引导器的过程会**擦除 UICR 页**。
> 对 nice!nano 这类板子，原厂引导器会顺手把 `UICR.REGOUT0` 写回 3.3V，
> 而 Feather 版引导器**没有**这个逻辑。
> 好消息：本机实测擦除后 `REGOUT0` 仍为 `0xFFFFFFFD`（3.3V），**没有出问题**；
> 但如果以后刷完整体包发现"USB 不认、必须顶 RESET"，可以先用工具读一下 UICR
> （见 `../uicr_repair/uicr_repair.ino`，它会把 `REGOUT0` / `PSELRESET` 的实测值打印出来）。
>
> **日常烧固件（DFU type 4，arduino-cli 上传）不会碰 UICR，是安全的。**

### 3.1 烧录命令

**日常烧固件**（最常用，会自动 1200bps touch 进 DFU，无需手动按键）：

```powershell
& "D:\Arduino\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe" `
  compile --fqbn adafruit:nrf52:feather52840_s340 --export-binaries --warnings none `
  "C:\Users\ikun\Desktop\XDS_AllInOne\XDS_AllInOne"

& "D:\Arduino\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe" `
  upload -p COM8 --fqbn adafruit:nrf52:feather52840_s340 `
  --input-dir "C:\Users\ikun\Desktop\XDS_AllInOne\XDS_AllInOne\build\adafruit.nrf52.feather52840_s340" `
  "C:\Users\ikun\Desktop\XDS_AllInOne\XDS_AllInOne"
```

**手动进 DFU 的三种方式**（arduino-cli 上传失败时用）：

1. **1200bps touch**：以 1200 波特打开串口再关闭；或直接在上面的 `upload` 里自动完成
2. **DFU 脚（P1.02）接地** + 复位
3. **双击 RST**（需要 `RESETREAS.RESETPIN`，本板可能不可用）

**板子在 DFU 时用 nrfutil 直刷**（DFU 口是 `239A:0029` / `239A:002A`）：

```powershell
& "C:\Users\ikun\AppData\Local\Arduino15\packages\adafruit\hardware\nrf52\1.7.0\tools\adafruit-nrfutil\win32\adafruit-nrfutil.exe" `
  dfu serial --package <build>\XDS_AllInOne.ino.zip -p COM15 -b 115200 --singlebank
```

### 3.2 串口注意

- 板子的 CDC 需要 **DTR 置位**才会输出；用脚本读串口时记得 `DtrEnable = $true`
- **上电横幅容易丢**：主机打开串口前打印的内容看不到
- 板子**插着但没打开串口**时，程序照常运行（Adafruit CDC 无主机时会丢弃输出，不会阻塞）
- **Arduino IDE 的串口监视器会占用端口**，导致外部脚本读不到（`访问被拒绝`）——
  用脚本抓日志前请先关掉监视器

---

## 4. 固件行为说明（本工程当前版本）

### 4.1 未连接功率计时显示"一眼假"的固定值

| 状态 | 码表显示 | LED（固件驱动 P1.10） |
|---|---|---|
| **没连上功率计** | **999 W / 250 rpm** | 每秒短闪 80ms（省电） |
| **连上功率计** | 真实功率 + 真实踏频 | **熄灭**（省电） |

- 999W/250RPM 是**故意**设成"一眼就知道没连上"的值：真实骑行不可能持续 999W
- 一旦连上，`isReceivingRealData()` 判定为真，真实数据**自动接管**
- ANT+ 踏频字段是 **1 字节**，合法范围 0~254，**255(0xFF) 才是"无效"**；
  发送 255 时码表显示 `--` 或不显示踏频
- 核心自带的 `autoConnLed` 只看广播/外设侧状态，功率计（Central 侧）连上它也可能一直闪，
  所以本工程**关掉了它**，改由固件按"是否连上功率计"自己驱动 LED

> 注：这块 SuperMini 板上那颗**一直在快闪的指示灯**，实测**无法由固件控制** ——
> 依次试过 `P0.13`（板子引脚图标注为 MCU Control）、`P1.15`、`P1.10`
> 以及一轮 16 个候选引脚的自动扫描，该灯始终按自己的节奏快闪，
> 判断它由**板载独立电路**驱动（很可能是充电/电源指示）。
> 它不影响任何功能，平均电流也很小，**不需要处理**。
> （固件里保留了 `LED_PIN_SCAN` / `LED_SELF_TEST` 两个开关，默认关；想再扫一遍改成 1 重烧即可。）

### 4.2 串口命令（115200）

| 命令 | 作用 |
|---|---|
| `help` / `h` | 帮助 |
| `status` / `s` | 状态（含 `Build:` 版本标记，可用来确认板子上跑的是哪一版） |
| `enable` / `en` | 启用通知 |
| `disable` / `dis` | 禁用通知 |
| `scan` | 重新扫描功率计 |
| `disconnect` / `disc` | 断开并重连（诊断用） |

---

## 5. 喜德盛功率计 11 字节报文格式（实测校正）

```
Byte 0-1 : 总功率      uint16 小端 (W)
Byte 2-3 : 左腿功率    int16  小端 (W)
Byte 4-5 : 右腿功率    int16  小端 (W)
Byte 6-7 : 踏频        uint16 小端 (RPM)      ← 原注释误标为"角度"
Byte 8-9 : 曲柄角度    uint16 小端 (0~359°)   ← 原注释误标为"踏频"
Byte 10  : 错误码      uint8
```

**这是实测校正的结果，不是推测**（原作者的代码把 6-7 和 8-9 读反了）：

- 曲柄**静止 67 秒**期间：`Byte 8-9 = 44~51`（**踏频不可能非零**），
  而 `Byte 6-7` 正好为 `0`
- 转动曲柄时：`Byte 6-7` 升到 `44`（这是踏频），
  `Byte 8-9` 在 `0~359` 之间扫动（这是角度）

**为什么原作者没发现**：他的固件里 ANT+ 踏频恒为 `0xFF`(OFF)，
正好绕过了这个错位；一旦启用踏频上报，"角度冒充踏频"就会显示成离谱数值。

另外：静止不踩时字节值**不更新**（保持最后一次的值），属正常行为；
`0xFFFF` 表示"踏频无效"，需映射为 ANT+ 的 `0xFF`(OFF)。

---

## 6. 相关仓库与工具

| 项目 | 位置 / 来源 | 用途 |
|---|---|---|
| **`xds_repeater`**（ESP32 参考实现） | `https://github.com/1wpc/xds_repeater` | **能收到数据的参考实现**，CCCD 补丁就是从它与 Adafruit 的差异里找到的 |
| ANT 版 BSP fork | `https://github.com/1wpc/Adafruit_nRF52_Arduino_ANT` | 提供 S340 + w.ANT 支持 |
| 引导器工程 | 上游 `ANT_Adafruit_bootloader` | 烧 S340 软设备 + w.ANT 引导器 |
| UICR 诊断/修复工具 | 本机 `uicr_repair/`（独立小工程，未随本仓库发布） | 打印/修复 `REGOUT0`、`PSELRESET` |
| 最小冷启动测试固件 | 本机 `minimal_test/`（独立小工程，未随本仓库发布） | 不含 SoftDevice，用来区分"硬件问题"还是"固件问题" |
| 假数据测试副本 | 本机 `XDS_AllInOne_fake/`（其行为已并入正式版，可删） | 固定假值版，用来验证链路 |

---

## 7. 换一块新板子：从零复现步骤

1. **装 ANT 版 BSP**
   - 用 `1wpc/Adafruit_nRF52_Arduino_ANT`（基于 Adafruit nRF52 1.7.0）
   - 或：先装官方 1.7.0，再按第 8 节打补丁
2. **刷 S340 软设备 + w.ANT 引导器**（第 3 节，DFU 包一次刷完）
   - 用 `INFO_UF2.TXT` 核对 `SoftDevice: S340 6.1.1` / `w.ANT`
3. **打 CCCD 补丁**（第 2.2 节）—— **最容易漏、也是"收不到数据"的根因**
4. **确认 `variant.h` 是 `USE_LFXO`**（第 2.4 节，ANT+ 需要晶振精度）
5. **焊接 `RST` ↔ `3.3V`**（第 1.1 节）—— 若这块板的复位脚正常，**可以跳过**
6. 编译烧录（FQBN：`adafruit:nrf52:feather52840_s340`）
7. **验证**
   - 串口 `status` 应显示 `Build: v8-...`
   - 未连功率计时：码表应显示 **999W / 250RPM**（说明 ANT+ 链路通了）
   - 打开功率计 → 串口出现 `Received power measurement notify`
     → 码表切换为**真实功率/踏频**（说明 BLE 接收 + 解析都对了）
   - 拔插 USB 冷启动 → 板子应自动出现（若不然，见第 1.1 节）

---

## 8. 已修问题清单（速查）

| 问题现象 | 根因 | 修复 |
|---|---|---|
| 编译报 `ANT_MESSAGE does not name a type` 等 | 缺 `ant_parameters.h` 头文件 | 补 include |
| ANT+ 通道完全不发射 | 事件泵在等一个永不 `give` 的信号量 | 改为"超时 + 轮询排空"（`CFG_ANT_EVENT_POLL_MS = 2`） |
| **连接成功但永远收不到功率数据** | **CCCD 用 Write Command 被对端丢弃** | **改 `BLE_GATT_OP_WRITE_REQ`**（第 2.2 节）★ |
| 冷启动 USB 不认、必须顶 RESET | 复位脚被拉低 | 硬件 `RST` ↔ `3.3V`（第 1.1 节） |
| 踏频数值离谱（如 105、65522） | 报文 6-7/8-9 字段读反 + `0xFFFF` 未处理 | 按第 5 节校正 + 无效值映射为 `0xFF` |
| 踏频 250 显示为 `--` | 代码把 `>=250` 误判为无效 | 按协议只把 255 视为无效 |
| 连上功率计后 LED 仍一直闪 | 核心 `autoConnLed` 只看外设/广播侧 | 关掉它并改由固件驱动（但见第 4.1 节：本板那颗灯实测固件控制不到） |
| 板载灯一直快闪，改引脚无效 | 该灯由板载独立电路驱动，不接 MCU | 不影响功能，忽略即可（第 4.1 节） |

---

## 9. 已知遗留项（不影响使用）

1. `pwr->SetInstantPWR()/SetAccumulatedPWR()` 目前是**逐包累加瞬时功率**，
   并非严格的能量积分；长期看 `AccPWR` 会快速增长并在 16 位处回绕
2. `printXdsDataDetails()` 无条件读 11 字节，若收到短包存在越界读风险
   （当前功率计固定发 11 字节，暂未触发）
3. 左/右腿功率（Byte 2-3 / 4-5）字段含义沿用原作者假设，**未做单腿踩踏验证**
4. ANT 许可：ANT 协议仅**非商业个人用途**免费，商用需向 thisisant.com 购买许可

---

## 10. 致谢

- 原始思路与 ESP32 参考实现：[`1wpc/xds_repeater`](https://github.com/1wpc/xds_repeater)
- ANT 版 Arduino BSP：[`1wpc/Adafruit_nRF52_Arduino_ANT`](https://github.com/1wpc/Adafruit_nRF52_Arduino_ANT)
- 底层库：Adafruit nRF52 Arduino BSP 1.7.0 / Bluefruit52Lib / TinyUSB
