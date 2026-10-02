#ifndef PowerMeter_h
#define PowerMeter_h

// #define USE_TINYUSB


#include "../sdant.h"
#include "BicyclePower.h"
#include <bluefruit.h>
#include "stdint-gcc.h"

//#include "list"

// 蓝牙服务和特征值UUID定义
#define MESH_PROXY_SERVICE_UUID         0x1828
#define CYCLING_POWER_SERVICE_UUID      0x1828   // 桥接服务（与原ESP32 XDSmonitor一致）
#define CYCLING_POWER_MEASUREMENT_UUID  0x2A63

typedef struct powermeter_config
{
    uint16_t profileUpdateCycle;
    BicyclePower* p_power_profile;
} powermeter_config;

// 喜德盛功率计数据结构定义
// 数据包格式：11字节（字段位置为实测校正后的结果，见 PowerMeter.cpp::parseXdsData 的注释）
// Byte 0-1:  总功率 (无符号16位，小端序)
// Byte 2-3:  左腿功率 (有符号16位，小端序)
// Byte 4-5:  右腿功率 (有符号16位，小端序)
// Byte 6-7:  踏频 RPM (无符号16位，小端序)   ← 原注释误标为"角度"
// Byte 8-9:  曲柄角度 0~359° (小端序)        ← 原注释误标为"踏频"
// Byte 10:   错误代码 (8位)
typedef struct XdsPowerMeasurementData
{
    uint16_t totalPower;    // 总功率 (瓦特)
    int16_t leftPower;      // 左腿功率 (瓦特)
    int16_t rightPower;     // 右腿功率 (瓦特)
    uint16_t angle;         // 曲柄角度 (度, 0~359)
    uint16_t cadence;       // 踏频 (RPM)
    uint8_t errorCode;      // 错误代码
    bool isValid;           // 数据有效性标志
} XdsPowerMeasurementData;

class PowerMeter
{
public:

    

    PowerMeter(powermeter_config*);
    void begin();
    void update();
    void generateVirtualData();
    void simulateHallInterrupt();
    
    // 蓝牙客户端相关方法
    void initBLEClient();
    void startScanning();
    void connectToPowerMeter();
    void onConnect(uint16_t conn_handle);
    void onDisconnect(uint16_t conn_handle, uint8_t reason);
    void onPowerMeasurementNotify(BLEClientCharacteristic* chr, uint8_t* data, uint16_t len);
    void parsePowerData(uint8_t* data, uint16_t len);
    
    // BLE 服务端（桥接转发，替代原ESP32角色）相关方法
    void initBLEServer();
    void onPeriphConnect(uint16_t conn_handle);
    void onPeriphDisconnect(uint16_t conn_handle, uint8_t reason);
    
    // 喜德盛功率计数据解析相关函数
    XdsPowerMeasurementData parseXdsData(uint8_t* data, uint16_t len);
    uint16_t getUnsignedValue(uint8_t* data, uint16_t offset);
    int16_t getSignedValue(uint8_t* data, uint16_t offset);
    bool validateXdsData(const XdsPowerMeasurementData& data);
    void printXdsDataDetails(const XdsPowerMeasurementData& data, uint8_t* rawData);
    
    // 串口命令处理相关函数
    void processSerialCommands();
    void handleSerialCommand(String command);
    void enableNotifications();
    void disableNotifications();
    void printHelp();
    void printStatus();

    void SetAccPWR(uint16_t val)        { accPWR = val; }
    void SetInstPWR(uint16_t val)       { instPWR = val; }
    void SetInstCAD(uint8_t val)        { instCAD = val; }
    void SetPWREventCount(uint8_t val)  { PWREventCount = val; }
    
    // 获取连接状态
    bool getConnectionStatus() const    { return isConnected; }
    bool getScanningStatus() const      { return isScanning; }

    // 是否"真的在收数据"：已连接 != 有数据。
    // 未连接、从未收到过数据、或数据已超时，一律返回 false（此时应继续用虚拟数据）。
    // 旧逻辑只看 isConnected，导致"连上但功率计不吐数据"时虚拟数据被停掉，
    // ANT+ 上冻结成一个固定值，既不真实也无法用于验证链路。
    bool isReceivingRealData() const {
        return isConnected && lastValidDataTime > 0 &&
               (millis() - lastValidDataTime) <= dataTimeoutMs;
    }

private:
    BicyclePower* pwr;
    powermeter_config config;
    uint16_t accPWR, instPWR;
    uint8_t instCAD, PWREventCount;

    uint32_t nextProfileUpdate;
    uint32_t lastVirtualDataUpdate;
    uint32_t lastCadenceUpdate;
    
    // 虚拟数据生成相关变量
    uint16_t basePower;      // 基础功率 (约100W)
    uint8_t baseCadence;     // 基础踏频 (约70RPM)
    uint32_t virtualDataInterval;  // 虚拟数据更新间隔
    
    // 蓝牙客户端相关变量
    BLEClientService meshProxyService;
    BLEClientCharacteristic powerMeasurementChar;
    bool isConnected;
    bool isScanning;
    uint16_t connectionHandle;
    
    // BLE 服务端（桥接转发）相关变量
    BLEService powerBridgeService;      // 0x1828 Cycling Power Service
    BLECharacteristic powerBridgeChar;  // 0x2A63 Cycling Power Measurement（UUID/属性在 initBLEServer() 中于 begin() 前设置）
    bool serverDeviceConnected;         // 是否有手机/App等外设客户端连接
    uint16_t serverConnHandle;          // 外设客户端连接句柄
    
    // 错误处理和数据质量监控
    uint16_t invalidDataCount;      // 无效数据包计数
    uint16_t validDataCount;        // 有效数据包计数
    uint32_t lastValidDataTime;     // 最后一次有效数据时间
    uint32_t dataTimeoutMs;         // 数据超时时间 (毫秒)
    bool dataQualityGood;           // 数据质量状态
    bool notificationsEnabled;      // 通知启用状态
    
    // 静态回调函数
    static void staticPowerMeasurementNotify(BLEClientCharacteristic* chr, uint8_t* data, uint16_t len);
    static void staticPowerMeasurementIndicate(BLEClientCharacteristic* chr, uint8_t* data, uint16_t len);
    static void staticConnectCallback(uint16_t conn_handle);
    static void staticDisconnectCallback(uint16_t conn_handle, uint8_t reason);
    static void staticScanCallback(ble_gap_evt_adv_report_t* report);
    static void staticPeriphConnectCallback(uint16_t conn_handle);
    static void staticPeriphDisconnectCallback(uint16_t conn_handle, uint8_t reason);
    
    // 静态实例指针，用于在静态回调中访问实例
    static PowerMeter* instance;

};


#endif