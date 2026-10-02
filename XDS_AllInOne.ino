// ============================================================================
//  XDS All-In-One —— 单块 Promicro NRF52840 同时实现三个角色
//
//  1) BLE Central  : 连接喜德盛(XDS)功率计，订阅 0x1828/0x2A63 通知
//  2) BLE Peripheral: 桥接转发原始数据包（"XDS Power Bridge"，原ESP32角色）
//  3) ANT+ Master  : 4Hz 广播 ANT+ 功率计数据给 Garmin 等码表（原Feather角色）
//
//  硬件：1× Promicro NRF52840（SparkFun Pro nRF52840 Mini 或兼容克隆，S140软设备）
//        1× 3.7V 锂电池（可选 TP4056 充电模块）
//  板卡包：Adafruit nRF52 BSP（或 SparkFun nRF52840 Boards）
//  依赖库：Adafruit Bluefruit nRF52 Libraries
// ============================================================================
#define ANT_LICENSE_KEY "AUTO"
#define ANT_FS_LICENSE_KEY "AUTO"
#include "src/PowerMeter/PowerMeter.h"

// 功率计配置
powermeter_config PWRconfig =
{
  250  // profileUpdateCycle - ANT+数据发送间隔(ms)
};

PowerMeter power(&PWRconfig);

void setup(void)
{
  Serial.begin(115200);
  delay(100);
  Serial.println("=== XDS All-In-One (BLE Bridge + ANT+) ===");

  // 初始化：BLE Central + BLE Server(桥接) + ANT+ 广播
  power.begin();

  // 启动蓝牙扫描，寻找喜德盛功率计并连接
  Serial.println("Starting BLE scan for Xidesheng power meter...");
  power.startScanning();

  Serial.println("PowerMeter setup is finished!");
  Serial.println("Will use BLE data if connected, otherwise virtual data (~100W, ~70RPM)...");
  Serial.println("Phone/App: connect to \"XDS Power Bridge\" via BLE.");
  Serial.println("Head unit: search ANT+ power sensor (device #1000, 57ch).");
}

void loop(void)
{
  // 更新数据、ANT+发送、桥接转发、串口命令（help/status/scan/...）
  power.update();

  // 小延时避免过度占用CPU
  delay(10);
}
