#include "./PowerMeter.h"

// ---------------------------------------------------------------------------
// 本板（SuperMini NRF52840 克隆板）由 MCU 控制的板载指示灯挂在 P0.13。
// 依据：该板引脚图把 P0.13 标注为 "MCU Control"，并注明
//       "当 P0.13 设置为低时，将关闭 3.3V ~ VCC 引脚的电源"
//       （原设计用于关掉板载 RGB LED 省电）。
// 注意：Arduino 核心里的 LED_BLUE(P1.10) / LED_BUILTIN(P1.15) 在这块板上
//       对应的是排针上的其它脚位，用它们驱动板载灯是无效的。
// ---------------------------------------------------------------------------
#define PIN_LED_MCU   (13)   // P0.13

// 静态实例指针定义
PowerMeter* PowerMeter::instance = nullptr;

void PrintUnhandledANTEvent(ant_evt_t *evt)
{
  Serial.printf("Channel #%d for %s: event %s\n", evt->channel, ANTplus.getAntProfileByChNum(evt->channel)->getName(), AntEventTypeDecode(evt)); 
  if (evt->event != EVENT_CHANNEL_COLLISION 
    && evt->event != EVENT_RX_FAIL
    && evt->event != EVENT_CHANNEL_CLOSED
    )
    Serial.printf("  (%s)\n", AntEventType2LongDescription(evt));
}
void ReopenANTChannel(ant_evt_t *evt)
{
  if (evt->event == EVENT_CHANNEL_CLOSED ) {
    Serial.printf("Channel #%d closed for %s\n", evt->channel,ANTplus.getAntProfileByChNum(evt->channel)->getName()); 
    Serial.printf("Reopening...");
    uint32_t ret;
    ret = sd_ant_channel_open(evt->channel);
    if (ret == NRF_SUCCESS) Serial.println("success!");
    else Serial.printf("failed with code:%#x\n", ret);
  }
}

PowerMeter::PowerMeter(powermeter_config * cfg) : 
    meshProxyService(MESH_PROXY_SERVICE_UUID),
    powerMeasurementChar(CYCLING_POWER_MEASUREMENT_UUID),
    powerBridgeService(CYCLING_POWER_SERVICE_UUID),
    powerBridgeChar(CYCLING_POWER_MEASUREMENT_UUID)
{
    config.profileUpdateCycle = cfg->profileUpdateCycle;
    //config.p_power_profile = cfg->p_power_profile;
    pwr = new BicyclePower(TX);
    
    // 设置静态实例指针
    instance = this;
    
    // 初始化虚拟数据参数
    basePower = 100;           // 基础功率100W
    baseCadence = 70;          // 基础踏频70RPM
    virtualDataInterval = 1000; // 1秒更新一次虚拟数据
    lastVirtualDataUpdate = 0;
    lastCadenceUpdate = 0;
    
    // 初始化功率和踏频
    instPWR = basePower;
    instCAD = baseCadence;
    accPWR = 0;
    PWREventCount = 0;
    
    // 初始化蓝牙客户端状态
    isConnected = false;
    isScanning = false;
    connectionHandle = 0;
    
    // 初始化BLE服务端（桥接转发）状态
    serverDeviceConnected = false;
    serverConnHandle = 0;
    
    // 初始化错误处理和数据质量监控
    invalidDataCount = 0;
    validDataCount = 0;
    lastValidDataTime = 0;
    dataTimeoutMs = 5000;      // 5秒数据超时
    dataQualityGood = true;
    notificationsEnabled = false;  // 初始化通知状态为禁用
}

void PowerMeter::begin() {
    Serial.println("Starting PowerMeter Setup...");
    Serial.println("Adding PWR profile");
    pwr->setUnhandledEventListener(PrintUnhandledANTEvent);
    pwr->setAllEventListener(ReopenANTChannel);
    pwr->setName("PWR");
    ANTplus.AddProfile(pwr);

    Serial.println("Bluefruit52 BLEUART Startup");
    Serial.println("---------------------------\n");
    // 【LED 改由我们自己控制】
    // 背景1：核心自带的 autoConnLed 只看"广播/外设侧"连接状态，功率计（Central 侧）
    //        连上了它也可能一直闪，所以关掉它，改由 update() 按"是否连上功率计"驱动：
    //            连上功率计 → 熄灭（省电）；未连上 → 每秒短闪 80ms
    // 背景2：【引脚已按实测修正】本板是 SuperMini NRF52840 克隆板，
    //        板上由 MCU 控制的那颗灯挂在 **P0.13**（板子丝印/引脚图上标注为
    //        "MCU Control"，官方说明还写着"P0.13 置低可关闭 VCC 引脚供电以省电"）。
    //        而 Arduino 核心里的 LED_BLUE(P1.10)/LED_BUILTIN(P1.15) 在这块板上
    //        对应的是别的排针脚位，所以之前控制无效、灯一直闪。
    Bluefruit.autoConnLed(false);
    pinMode(PIN_LED_MCU, OUTPUT);
    digitalWrite(PIN_LED_MCU, (1 - LED_STATE_ON));   // 默认熄灭

    // 【引脚探针 / 一次性扫描】已停用：本板板载指示灯未能通过固件找到对应引脚
    // （P0.13 / P1.15 / P1.10 等候选均试过，它仍持续快闪，判断由板载独立电路驱动）。
    // 该灯不影响任何功能，故不再处理。若以后想再扫一遍，把 LED_PIN_SCAN 改回 1 重烧即可。
#define LED_PIN_SCAN 0
#if LED_PIN_SCAN
    {
        static const uint8_t kCandidates[] = { 13, 15, 10, 6, 8, 17, 20, 22, 24, 0, 11, 4, 9, 31, 29, 2 };
        Serial.println("[LED-SCAN] start: watching which candidate drives the board LED");
        for (uint8_t i = 0; i < sizeof(kCandidates); i++) {
            uint8_t const pin = kCandidates[i];
            Serial.printf("[LED-SCAN] step %u/%u -> pin %u\n",
                          (unsigned)(i + 1), (unsigned)sizeof(kCandidates), (unsigned)pin);
            pinMode(pin, OUTPUT);
            for (uint8_t k = 0; k < 4; k++) {
                digitalWrite(pin, LED_STATE_ON);
                delay(400);
                digitalWrite(pin, (1 - LED_STATE_ON));
                delay(400);
            }
            pinMode(pin, INPUT);
        }
        Serial.println("[LED-SCAN] done");
    }
#endif

    // 【一次性 LED 自检】已停用（同上）
#define LED_SELF_TEST 0
#if LED_SELF_TEST
    Serial.println("[LED] self-test: board LED on 1.5s ...");
    digitalWrite(PIN_LED_MCU, LED_STATE_ON);
    delay(1500);
    digitalWrite(PIN_LED_MCU, (1 - LED_STATE_ON));
    Serial.println("[LED] self-test done");
#endif

    // Bluefruit.configPrphBandwidth(BANDWIDTH_NORMAL);

    // 【冷启动修复 - 关键】启用 SoftDevice 之前，先给 USB 留出"上电就绪 + 上拉使能"的时间。
    // Adafruit 核心 bluefruit.cpp 里 usb_softdevice_post_enable() 的注释明确写道：
    //   "depending on how fast Bluefruit.begin() is called, Ready event may or may not
    //    be handled before we disable the nrfx_power.
    //    USBPULLUP not enabled -> Ready event not yet handled"
    // 即：Bluefruit.begin() 调用太早（冷启动时 VBUS 还在爬升）时，USB 的 READY 事件
    // 尚未处理，随后 nrfx_power 被 SoftDevice 接管，USB 上拉（USBPULLUP）就永远不会打开，
    // 电脑自然完全看不到设备；而热复位/刚刷完固件时 USB 早已枚举完成，所以表现为正常。
    // 这里先等 USB 就绪（最多 3 秒；独立供电、没接主机时就是启动慢 3 秒）。
    {
        uint32_t const tUsb = millis();
        while (!Serial && (millis() - tUsb) < 3000) {
            delay(10);
        }
        Serial.printf("USB ready wait: %lu ms (Serial open=%d)\n",
                      (unsigned long)(millis() - tUsb), (int)(bool)Serial);
    }

    // 【关键】把 Central 侧 ATT MTU 上限提到 247。
    // 默认是 23（单包最多 20 字节）；若功率计的通知包超过 20 字节，
    // 在默认 MTU 下它根本发不出来，表现就是"CCCD 订阅成功但永远收不到数据"
    // —— 这也解释了为什么原作者当年必须用 ESP32（Bluedroid 默认大 MTU）中转。
    Bluefruit.configCentralBandwidth(BANDWIDTH_MAX);

    Serial.print("Starting BLE stack as Central+Peripheral. Expecting 'true':");
    bool ret = Bluefruit.begin(1, 1);  // 1 peripheral (BLE桥接), 1 central
    Serial.println(ret);
    
    // 初始化蓝牙客户端
    initBLEClient();
    // 初始化BLE服务端（桥接转发，替代原ESP32 XDSmonitor角色）
    initBLEServer();
    Serial.print("Starting ANT stack. Expecting 'true':");
    ret = ANTplus.begin(1);
    Serial.println(ret);
    if (!ret)
    {
        // ANT 没起来时，码表一定搜不到 ANT+ 功率计；这里把结论说清楚，
        // 具体失败原因已由 SdAnt::begin() 打印（SVC 失败 / 密钥 / 通道配置）。
        Serial.println("[ANT] ANT+ 未能启动：码表将搜不到 ANT+ 功率计（设备号 1000，类型 0x0B）。");
        Serial.println("[ANT] 请对照上面的 [ANT] 诊断行处理，通常需要 ANT 版 SoftDevice。");
    }
    ANTProfile* profiles[] = {pwr};
    for (auto i: profiles)
    {
        Serial.printf("Channel number for %s became %d\n", i->getName(), i->getChannelNumber());
    }
    
    // 初始化虚拟数据时间戳
    nextProfileUpdate = millis();
    lastVirtualDataUpdate = millis();
    lastCadenceUpdate = millis();
    
    Serial.println("Virtual PowerMeter initialized successfully!");
    Serial.printf("Base Power: %dW, Base Cadence: %dRPM\n", basePower, baseCadence);
    Serial.printf("Startup is complete.\n");
    
    // 串口命令使用提示
    Serial.println("\n============================");
    Serial.println("Serial Commands Available:");
    Serial.println("Type 'help' for command list");
    Serial.println("Type 'scan' to start BLE scan");
    Serial.println("Notifications will auto-enable on connect");
    Serial.println("============================\n");
}

void PowerMeter::generateVirtualData() // 未连接功率计时，生成"一眼假"的固定值
{
    uint32_t currentTime = millis();
    
    // 每1秒刷新一次（数值恒定，不随机、不跳动）
    if (currentTime - lastVirtualDataUpdate >= virtualDataInterval) 
    {
        // 【设计意图】功率计没连上时，故意发"一看就假"的固定值：
        //     功率 = 999 W        踏频 = 250 RPM
        // 码表上出现 999W / 250RPM，就说明"板子还没连上功率计"；
        // 一旦连上，真实数据会立即接管（见 update() 里的 realData 判断）。
        // 协议说明：ANT+ 第10页踏频字段是 1 字节，合法范围 0~254，255(0xFF) 才是"无效"。
        instPWR = 999;   // 恒定 999 W
        instCAD = 250;   // 恒定 250 RPM
        
        // 累积功率和事件计数
        accPWR += instPWR;
        PWREventCount++;
        
        lastVirtualDataUpdate = currentTime;
        
        // 输出调试信息（明确标注这是"未连接"时的假数据）
        Serial.printf("Virtual (NOT connected) - Power: %dW, Cadence: %dRPM\n", instPWR, instCAD);
    }
}

void PowerMeter::simulateHallInterrupt() // 模拟霍尔传感器中断，用于踏频计算
{
    uint32_t currentTime = millis();
    
    // 根据当前踏频计算中断间隔
    // 踏频 = 60 / (间隔秒数)，所以间隔 = 60000ms / 踏频
    uint32_t expectedInterval = (instCAD > 0) ? (60000 / instCAD) : 1000;
    
    // 模拟霍尔传感器中断
    if (currentTime - lastCadenceUpdate >= expectedInterval) 
    {
        lastCadenceUpdate = currentTime;
        
        // 这里可以添加一些踏频相关的处理逻辑
        // 但主要的数据生成在generateVirtualData()中完成
        Serial.printf("Simulated Hall Interrupt - Cadence: %dRPM\n", instCAD);
    }
}

void PowerMeter::update()
{   
    // 处理串口命令
    processSerialCommands();
    
    uint32_t currentTime = millis();
    static uint32_t lastStatusCheck = 0;
    static uint32_t lastDataRequest = 0;

    // 真实数据判据：已连接 且 收到过数据 且 未超时。
    // 关键：isConnected 只表示 BLE 链路建立，不代表功率计真的在推送数据。
    bool const realData = isReceivingRealData();

    // 【LED 指示 - 省电设计】
    //   连上功率计  → 熄灭（正常骑行状态，不需要灯，省电）
    //   没连上功率计 → 每秒短闪 80ms（占空比 8%，功耗极低），方便一眼确认状态
    // 用 LED_BLUE（本板 = P1.10）。LED_STATE_ON 为点亮电平。
    {
        static uint32_t lastLedMs = 0;
        static bool     ledLit    = true;   // 缓存当前状态，避免每次循环都写寄存器
        bool wantLit;
        if (isConnected) {
            wantLit = false;                                  // 连上 → 熄灭
        } else {
            uint32_t const dt = currentTime - lastLedMs;
            if (dt >= 1000) lastLedMs = currentTime;           // 每秒重新开始一次短闪
            wantLit = (dt < 80);                               // 短闪 80ms
        }
        if (wantLit != ledLit) {
            ledLit = wantLit;
            digitalWrite(PIN_LED_MCU, wantLit ? LED_STATE_ON : (1 - LED_STATE_ON));
        }
    }
    
    // 每5秒检查一次连接状态
    if (currentTime - lastStatusCheck > 5000) {
        lastStatusCheck = currentTime;
        if (!isConnected) {
            Serial.println("Status: Not connected");
        } else if (realData) {
            Serial.printf("Status: Connected + data OK (%u ms ago)\n", (unsigned)(currentTime - lastValidDataTime));
        } else if (lastValidDataTime == 0) {
            Serial.println("Status: Connected but NO data yet -> using virtual data");
        } else {
            Serial.printf("Status: Connected but data stale (%u ms) -> using virtual data\n", (unsigned)(currentTime - lastValidDataTime));
        }
    }
    
    // 数据超时提示（限流，避免每轮都刷屏）
    if (isConnected && lastValidDataTime > 0 && (currentTime - lastValidDataTime) > dataTimeoutMs) {
        static uint32_t lastTimeoutWarn = 0;
        if (currentTime - lastTimeoutWarn > dataTimeoutMs) {
            lastTimeoutWarn = currentTime;
            Serial.printf("Warning: no data for %u ms (meter connected but silent)\n",
                          (unsigned)(currentTime - lastValidDataTime));
        }
    }
    
    // 只要没有真实数据（未连接 / 从未收到 / 已超时）就继续生成虚拟数据，
    // 这样即使功率计不吐数据，ANT+ 上也有一个持续跳动的值可用于验证链路
    if (!realData) {
        generateVirtualData();
        simulateHallInterrupt();
    }
    
    // 定期发送ANT+数据
    if (millis() > nextProfileUpdate)
    {
        nextProfileUpdate += config.profileUpdateCycle;
        pwr->SetInstantPWR(instPWR);
        pwr->SetAccumulatedPWR(accPWR);
        pwr->SetPWREventCount(PWREventCount);
        
        // 数据源标记：只有"真的在收数据"才标成 XDS BLE
        // 上报真实踏频：0xFF 才是 ANT+ 的 "OFF/无效"，
        // 所以这里直接送 instCAD（真实数据时来自功率计，虚拟模式时是模拟值）。
        // 注意 ANT+ 踏频字段是 1 字节，>=250 时按无效处理，避免溢出成奇怪数值。
        // 【协议修正】ANT+ 第10页里"踏频无效"只有 0xFF(255) 这一个值，250~254 都是合法踏频。
        // 原写法 >=250 会把 250 误判为无效（码表显示 "--"），这里按协议原样发送。
        uint8_t const cadToSend = instCAD;
        pwr->SetInstantCadence(cadToSend);

        if (realData) {
            Serial.printf("ANT+ Data Sent (XDS BLE) - Power: %dW, Cadence: %d%s, AccPWR: %d, Events: %d\n",
                         instPWR, cadToSend, (cadToSend == 0xFF ? "(OFF)" : ""), accPWR, PWREventCount);
        } else {
            Serial.printf("ANT+ Data Sent (Virtual) - Power: %dW, Cadence: %d%s, AccPWR: %d, Events: %d\n",
                         instPWR, cadToSend, (cadToSend == 0xFF ? "(OFF)" : ""), accPWR, PWREventCount);
        }
        
        // 定期打印数据质量统计 (每分钟一次)
        static uint32_t lastStatsTime = 0;
        if (currentTime - lastStatsTime > 60000) {  // 60秒
            lastStatsTime = currentTime;
            if (validDataCount > 0 || invalidDataCount > 0) {
                float errorRate = (float)invalidDataCount / (validDataCount + invalidDataCount) * 100.0;
                Serial.printf("=== Data Quality Report ===\n");
                Serial.printf("Valid packets: %d, Invalid packets: %d\n", validDataCount, invalidDataCount);
                Serial.printf("Error rate: %.2f%%, Data quality: %s\n", errorRate, dataQualityGood ? "Good" : "Poor");
                Serial.printf("Last valid data: %d ms ago\n", lastValidDataTime > 0 ? currentTime - lastValidDataTime : 0);
                Serial.printf("Connection status: %s\n", isConnected ? "Connected" : "Disconnected");
                Serial.println("===========================");
            }
        }
    }
}

// 蓝牙客户端方法实现
void PowerMeter::initBLEClient() {
    Serial.println("Initializing BLE Client...");
    
    // 设置设备名称
    Bluefruit.setName("PowerMeter Central");
    
    // 服务和特征值 UUID 必须在 begin() 之前设置：
    // 默认构造的 BLEUuid 为 0x0000/BLE_UUID_TYPE_UNKNOWN，否则扫描过滤条件会失效
    meshProxyService.uuid = MESH_PROXY_SERVICE_UUID;
    powerMeasurementChar.uuid = CYCLING_POWER_MEASUREMENT_UUID;

    // 初始化Mesh Proxy服务
    meshProxyService.begin();
    
    // 初始化Cycling Power Measurement特征值
    // 同时挂 notify 与 indicate 两种回调：部分功率计是 Indicate-only，
    // 只订阅 Notification 会出现"订阅成功但永远收不到数据"
    powerMeasurementChar.setNotifyCallback(staticPowerMeasurementNotify);
    powerMeasurementChar.setIndicateCallback(staticPowerMeasurementIndicate);
    powerMeasurementChar.begin();
    
    // 设置连接回调
    Bluefruit.Central.setConnectCallback(staticConnectCallback);
    Bluefruit.Central.setDisconnectCallback(staticDisconnectCallback);
    
    Serial.println("BLE Client initialized successfully!");
}

void PowerMeter::startScanning() {
    if (isScanning) {
        Serial.println("Already scanning...");
        return;
    }
    
    Serial.println("Starting BLE scan for power meters...");
    Serial.printf("Looking for service UUID: 0x%04X\n", MESH_PROXY_SERVICE_UUID);
    
    // 设置扫描回调
    Bluefruit.Scanner.setRxCallback(staticScanCallback);
    Bluefruit.Scanner.filterUuid(meshProxyService.uuid);
    
    // 设置扫描参数以确保持续扫描
    Bluefruit.Scanner.restartOnDisconnect(true);
    
    // 开始扫描
    Bluefruit.Scanner.start(0);  // 0 = 永久扫描直到找到设备
    isScanning = true;
    
    Serial.println("BLE scanning started successfully");
    Serial.println("Scanning will continue until correct device is found...");
}

void PowerMeter::onConnect(uint16_t conn_handle) {
    Serial.printf("Connected to power meter, handle: %d\n", conn_handle);
    connectionHandle = conn_handle;
    isConnected = true;

    // 主动发起 ATT MTU 协商：默认只有 23（单包 20 字节），
    // 功率计若发大于 20 字节的通知包会被卡死在这里。
    BLEConnection* conn = Bluefruit.Connection(conn_handle);
    if (conn) {
        Serial.printf("MTU before exchange = %u\n", (unsigned)conn->getMtu());
        conn->requestMtuExchange(247);
        delay(300);   // 等协商完成（响应是异步的）
        Serial.printf("MTU after exchange  = %u  (max payload = %u)\n",
                      (unsigned)conn->getMtu(), (unsigned)(conn->getMtu() - 3));
    } else {
        Serial.println("WARN: Bluefruit.Connection() returned NULL");
    }

    // 发现服务
    if (meshProxyService.discover(conn_handle)) {
        Serial.println("Mesh Proxy Service discovered");
        
        // 发现特征值
        if (powerMeasurementChar.discover()) {
            // 诊断：特征属性决定该订阅 Notification 还是 Indication。
            // 在 Indicate-only 的特征上只写 CCCD=0x0001 可能"成功"但永远收不到数据。
            uint8_t const props = powerMeasurementChar.properties();
            Serial.printf("Cycling Power Measurement discovered: props=0x%02X valueHandle=%u (notify=%u indicate=%u)\n",
                          props, (unsigned)powerMeasurementChar.valueHandle(),
                          (unsigned)((props & 0x10) ? 1 : 0), (unsigned)((props & 0x20) ? 1 : 0));

            bool ok = false;
            if (props & 0x10) {
                ok = powerMeasurementChar.enableNotify();
                Serial.printf("enableNotify (CCCD=0x0001) -> %s\n", ok ? "OK" : "FAILED");
            }
            if (!ok && (props & 0x20)) {
                ok = powerMeasurementChar.enableIndicate();
                Serial.printf("enableIndicate (CCCD=0x0002) -> %s\n", ok ? "OK" : "FAILED");
            }
            if (!ok && !(props & 0x10) && !(props & 0x20)) {
                // 属性读不出来时两条都试
                ok = powerMeasurementChar.enableNotify();
                Serial.printf("fallback enableNotify -> %s\n", ok ? "OK" : "FAILED");
                if (!ok) {
                    ok = powerMeasurementChar.enableIndicate();
                    Serial.printf("fallback enableIndicate -> %s\n", ok ? "OK" : "FAILED");
                }
            }

            notificationsEnabled = ok;
            if (ok) {
                Serial.println("subscribed OK - waiting for meter to push data...");
            } else {
                Serial.println("subscribe FAILED entirely");
            }
            Serial.println("Type 'help' for available commands");
        } else {
            Serial.println("Failed to discover Cycling Power Measurement characteristic");
        }
    } else {
        Serial.println("Failed to discover Mesh Proxy Service");
    }
}

void PowerMeter::onDisconnect(uint16_t conn_handle, uint8_t reason) {
    Serial.printf("Disconnected from power meter, handle: %d, reason: 0x%02X\n", conn_handle, reason);
    isConnected = false;
    connectionHandle = 0;
    notificationsEnabled = false;  // 重置通知状态
    
    // 重新开始扫描
    Serial.println("Restarting scan...");
    delay(1000);  // 等待1秒后重新扫描
    startScanning();
}

void PowerMeter::onPowerMeasurementNotify(BLEClientCharacteristic* chr, uint8_t* data, uint16_t len) {
    Serial.printf("Received power data (%d bytes): ", len);
    for (int i = 0; i < len; i++) {
        Serial.printf("%02X ", data[i]);
    }
    Serial.println();
    
    // 解析功率数据
    parsePowerData(data, len);
    
    // ---- 桥接转发：把原始XDS数据包转发给BLE Server客户端（手机App等） ----
    // 替代原方案中 ESP32 (XDSmonitor.ino) 的角色
    if (serverDeviceConnected && len > 0) {
        powerBridgeChar.notify(data, len);
        Serial.println("Data forwarded to BLE Server");
    }
}

void PowerMeter::parsePowerData(uint8_t* data, uint16_t len) {
    Serial.printf("Parsing power data, length: %d bytes\n", len);
    
    // 打印原始数据用于调试
    Serial.print("Raw data: ");
    for (int i = 0; i < len; i++) {
        Serial.printf("%02X ", data[i]);
    }
    Serial.println();
    
    // 使用喜德盛数据解析
    XdsPowerMeasurementData xdsData = parseXdsData(data, len);
    
    if (xdsData.isValid) {
        // 更新功率和踏频数据
        instPWR = xdsData.totalPower;
        // 功率计用 0xFFFF 表示"踏频无效"（不转时偶发），不能当成 65535 使用。
        // ANT+ 的无效值恰好也是 0xFF，所以直接映射成 0xFF (=OFF)。
        // 【协议修正】0xFFFF 是功率计的无效标记；>254 也视为无效，
        // 但 250~254 是合法踏频，应原样保留。
        if (xdsData.cadence == 0xFFFF || xdsData.cadence > 254) {
            instCAD = 0xFF;   // ANT+ OFF / 无效
        } else {
            instCAD = (uint8_t)xdsData.cadence;
        }
        
        // 更新累积功率
        accPWR += instPWR;
        PWREventCount++;
        
        // 更新最后有效数据时间
        lastValidDataTime = millis();
        validDataCount++;
        
        // 打印解析后的数据
        Serial.printf("=== Xidesheng Power Data ===\n");
        Serial.printf("Total Power: %dW\n", xdsData.totalPower);
        Serial.printf("Left Power: %dW\n", xdsData.leftPower);
        Serial.printf("Right Power: %dW\n", xdsData.rightPower);
        Serial.printf("Cadence: %dRPM%s\n", xdsData.cadence,
                      (xdsData.cadence == 0xFFFF ? " (0xFFFF=INVALID, sent as ANT+ OFF)" : ""));
        Serial.printf("Angle: %d degrees\n", xdsData.angle);
        Serial.printf("Error Code: 0x%02X\n", xdsData.errorCode);
        Serial.println("============================");
        
        // 打印详细的数据分析
        printXdsDataDetails(xdsData, data);
        
    } else {
        invalidDataCount++;
        Serial.printf("Invalid Xidesheng data packet (count: %d)\n", invalidDataCount);
        
        // 如果数据无效，尝试基本解析作为备用
        if (len >= 4) {
            uint16_t basicPower = (data[1] << 8) | data[0];
            uint16_t basicCadence = (data[3] << 8) | data[2];
            Serial.printf("Fallback parsing - Power: %dW, Cadence: %dRPM\n", basicPower, basicCadence);
        }
    }
}

// 静态回调函数实现
void PowerMeter::staticPowerMeasurementNotify(BLEClientCharacteristic* chr, uint8_t* data, uint16_t len) {
    Serial.println("Received power measurement notify");
    if (instance) {
        instance->onPowerMeasurementNotify(chr, data, len);
    }
}

// Indication 与 Notification 走同一套解析/转发逻辑，只把日志标签区分开，
// 便于判断功率计到底是用哪种方式推数据
void PowerMeter::staticPowerMeasurementIndicate(BLEClientCharacteristic* chr, uint8_t* data, uint16_t len) {
    Serial.println("Received power measurement indicate");
    if (instance) {
        instance->onPowerMeasurementNotify(chr, data, len);
    }
}

void PowerMeter::staticConnectCallback(uint16_t conn_handle) {
    Serial.printf("staticConnectCallback called with handle: %d\n", conn_handle);
    if (instance) {
        instance->onConnect(conn_handle);
    } else {
        Serial.println("ERROR: instance is null in staticConnectCallback");
    }
}

void PowerMeter::staticDisconnectCallback(uint16_t conn_handle, uint8_t reason) {
    if (instance) {
        instance->onDisconnect(conn_handle, reason);
    }
}

void PowerMeter::staticScanCallback(ble_gap_evt_adv_report_t* report) {
    if (instance) {
        Serial.print("Scan found device: ");
        Serial.printBufferReverse(report->peer_addr.addr, 6, ':');
        Serial.print(", RSSI: ");
        Serial.println(report->rssi);
        
        // 检查是否是喜德盛功率计
        if (Bluefruit.Scanner.checkReportForService(report, instance->meshProxyService)) {
            Serial.print("Found power meter with correct service: ");
            Serial.printBufferReverse(report->peer_addr.addr, 6, ':');
            Serial.println();
            
            // 停止扫描并连接
            Bluefruit.Scanner.stop();
            instance->isScanning = false;
            
            Serial.println("Attempting to connect...");
            // 连接到设备
            Bluefruit.Central.connect(report);
        } else {
            Serial.println("Device does not have the required service, continuing scan...");
            // 明确地恢复扫描以确保继续
            Bluefruit.Scanner.resume();
        }
    }
}

// 喜德盛功率计数据解析函数实现
XdsPowerMeasurementData PowerMeter::parseXdsData(uint8_t* data, uint16_t len) {
    XdsPowerMeasurementData result = {0};
    
    // 直接解析数据，不检查长度
    // 【已修正】原来把 Byte 6-7 当角度、Byte 8-9 当踏频，实测反了：
    //   静止 67 秒期间 Byte 8-9 = 44~51（踏频不可能非零），转动时扫过 0~359（这是角度）；
    //   而 Byte 6-7 静止时正好为 0，转动时升到 44（这才是踏频）。
    //   原作者固件里 ANT+ 踏频恒为 0xFF(OFF)，所以这个错位一直没暴露。
    if (len >= 2) result.totalPower = getUnsignedValue(data, 0);      // Byte 0-1: 总功率
    if (len >= 4) result.leftPower = getSignedValue(data, 2);         // Byte 2-3: 左腿功率
    if (len >= 6) result.rightPower = getSignedValue(data, 4);        // Byte 4-5: 右腿功率
    if (len >= 8) result.cadence = getUnsignedValue(data, 6);         // Byte 6-7: 踏频 (RPM)
    if (len >= 10) result.angle = getUnsignedValue(data, 8);          // Byte 8-9: 曲柄角度 (0~359°)
    if (len >= 11) result.errorCode = data[10];                       // Byte 10: 错误代码
    
    // 直接标记为有效，不进行验证
    result.isValid = true;
    
    return result;
}

// 读取无符号16位整数 (小端序)
uint16_t PowerMeter::getUnsignedValue(uint8_t* data, uint16_t offset) {
    uint16_t low = data[offset] & 0xFF;
    uint16_t high = data[offset + 1] & 0xFF;
    return (high << 8) | low;
}

// 读取有符号16位整数 (小端序)
int16_t PowerMeter::getSignedValue(uint8_t* data, uint16_t offset) {
    uint16_t value = getUnsignedValue(data, offset);
    // 如果最高位为1，则为负数
    return (value & 0x8000) ? (int16_t)(value - 0x10000) : (int16_t)value;
}

// 验证喜德盛数据有效性
bool PowerMeter::validateXdsData(const XdsPowerMeasurementData& data) {
    // 检查错误代码
    if (data.errorCode != 0) {
        Serial.printf("XDS Error Code: %d\n", data.errorCode);
        // 根据错误代码决定是否继续处理数据
        if (data.errorCode > 10) {  // 严重错误
            return false;
        }
        // 轻微错误，继续验证其他数据
    }
    
    // 基本范围检查 - 总功率
    if (data.totalPower > 2000) {  // 功率不应超过2000W
        Serial.printf("Invalid total power: %dW (max 2000W)\n", data.totalPower);
        return false;
    }
    
    // 踏频范围检查
    if (data.cadence > 200) {  // 踏频不应超过200RPM
        Serial.printf("Invalid cadence: %dRPM (max 200RPM)\n", data.cadence);
        return false;
    }
    
    // 角度范围检查 (-180° 到 +180°)
    if (data.angle < -180 || data.angle > 180) {
        Serial.printf("Invalid angle: %d° (range: -180° to +180°)\n", data.angle);
        return false;
    }
    
    // 左右功率范围检查
    if (data.leftPower < -100 || data.leftPower > 1500) {
        Serial.printf("Invalid left power: %dW (range: -100W to 1500W)\n", data.leftPower);
        return false;
    }
    
    if (data.rightPower < -100 || data.rightPower > 1500) {
        Serial.printf("Invalid right power: %dW (range: -100W to 1500W)\n", data.rightPower);
        return false;
    }
    
    // 检查左右功率之和是否接近总功率 (允许15%误差)
    int16_t calculatedTotal = data.leftPower + data.rightPower;
    int16_t powerDiff = abs(calculatedTotal - (int16_t)data.totalPower);
    if (data.totalPower > 10) {  // 只在有显著功率时检查
        float errorPercent = (float)powerDiff / data.totalPower * 100.0;
        if (errorPercent > 15.0) {
            Serial.printf("Power mismatch: Total=%dW, L+R=%dW, Diff=%dW (%.1f%% error)\n", 
                         data.totalPower, calculatedTotal, powerDiff, errorPercent);
            // 不返回false，只是警告，因为可能是正常的测量误差
        }
    }
    
    // 检查功率和踏频的合理性组合
    if (data.totalPower > 0 && data.cadence == 0) {
        Serial.println("Warning: Power > 0 but cadence = 0");
    }
    
    if (data.totalPower == 0 && data.cadence > 0) {
        Serial.println("Warning: Cadence > 0 but power = 0");
    }
    
    // 检查极端功率值
    if (data.totalPower > 1000) {
        Serial.printf("Warning: Very high power detected: %dW\n", data.totalPower);
    }
    
    return true;
}

// 打印喜德盛数据详细信息
void PowerMeter::printXdsDataDetails(const XdsPowerMeasurementData& data, uint8_t* rawData) {
    Serial.println("=== XDS Power Meter Data ===");
    
    // 打印原始数据
    Serial.print("Raw data: ");
    for (int i = 0; i < 11; i++) {
        Serial.printf("%02X", rawData[i]);
        if (i < 10) Serial.print("-");
    }
    Serial.println();
    
    // 打印解析结果（字段位置已修正：6-7 是踏频，8-9 是角度）
    Serial.printf("Total Power:  0x%02X%02X = %dW\n", 
                 rawData[1], rawData[0], data.totalPower);
    Serial.printf("Left Power:   0x%02X%02X = %dW\n", 
                 rawData[3], rawData[2], data.leftPower);
    Serial.printf("Right Power:  0x%02X%02X = %dW\n", 
                 rawData[5], rawData[4], data.rightPower);
    Serial.printf("Cadence:      0x%02X%02X = %d RPM\n", 
                 rawData[7], rawData[6], data.cadence);
    Serial.printf("Angle:        0x%02X%02X = %d deg\n", 
                 rawData[9], rawData[8], data.angle);
    Serial.printf("Error Code:   0x%02X = %d\n", 
                 rawData[10], data.errorCode);
    Serial.printf("Data Valid:   %s\n", data.isValid ? "YES" : "NO");
    Serial.println("============================");
}

// ==================== 串口命令处理功能 ====================

void PowerMeter::processSerialCommands() {
    if (Serial.available()) {
        String command = Serial.readStringUntil('\n');
        command.trim(); // 移除前后空格和换行符
        if (command.length() > 0) {
            handleSerialCommand(command);
        }
    }
}

void PowerMeter::handleSerialCommand(String command) {
    command.toLowerCase(); // 转换为小写以便比较
    
    Serial.println("============================");
    Serial.printf("Received command: %s\n", command.c_str());
    Serial.println("============================");
    
    if (command == "help" || command == "h") {
        printHelp();
    }
    else if (command == "status" || command == "s") {
        printStatus();
    }
    else if (command == "enable" || command == "en") {
        enableNotifications();
    }
    else if (command == "disable" || command == "dis") {
        disableNotifications();
    }
    else if (command == "scan") {
        if (!isScanning && !isConnected) {
            Serial.println("Starting BLE scan...");
            startScanning();
        } else if (isScanning) {
            Serial.println("Already scanning...");
        } else {
            Serial.println("Already connected to a device");
        }
    }
    else if (command == "disconnect" || command == "disc") {
        if (isConnected) {
            Serial.println("Disconnecting from device...");
            Bluefruit.disconnect(connectionHandle);
        } else {
            Serial.println("Not connected to any device");
        }
    }
    else {
        Serial.printf("Unknown command: %s\n", command.c_str());
        Serial.println("Type 'help' for available commands");
    }
    Serial.println();
}

void PowerMeter::enableNotifications() {
    if (!isConnected) {
        Serial.println("Error: Not connected to any device");
        return;
    }
    
    if (notificationsEnabled) {
        Serial.println("Notifications are already enabled");
        return;
    }
    
    Serial.println("Enabling notifications...");
    
    if (powerMeasurementChar.enableNotify()) {
        notificationsEnabled = true;
        Serial.println("✓ Notifications enabled successfully!");
    } else {
        Serial.println("✗ Failed to enable notifications");
    }
}

void PowerMeter::disableNotifications() {
    if (!isConnected) {
        Serial.println("Error: Not connected to any device");
        return;
    }
    
    if (!notificationsEnabled) {
        Serial.println("Notifications are already disabled");
        return;
    }
    
    Serial.println("Disabling notifications...");
    
    if (powerMeasurementChar.disableNotify()) {
        notificationsEnabled = false;
        Serial.println("✓ Notifications disabled successfully!");
    } else {
        Serial.println("✗ Failed to disable notifications");
    }
}

void PowerMeter::printHelp() {
    Serial.println("Available Commands:");
    Serial.println("==================");
    Serial.println("help, h        - Show this help message");
    Serial.println("status, s      - Show current status");
    Serial.println("enable, en     - Enable notifications");
    Serial.println("disable, dis   - Disable notifications");
    Serial.println("scan           - Start BLE scanning");
    Serial.println("disconnect, disc - Disconnect from device");
    Serial.println("==================");
}

void PowerMeter::printStatus() {
    Serial.println("Current Status:");
    Serial.println("===============");
    // 固件版本标记：查这一行即可确认板子上跑的是哪一版
    Serial.println("Build:              v10-final (未连功率计: 999W/250RPM; 连上: 真实数据)");
    Serial.printf("Real data:           %s\n", isReceivingRealData() ? "YES" : "NO");
    Serial.printf("Connected:           %s\n", isConnected ? "YES" : "NO");
    Serial.printf("Scanning:            %s\n", isScanning ? "YES" : "NO");
    Serial.printf("Notifications:       %s\n", notificationsEnabled ? "ENABLED" : "DISABLED");
    Serial.printf("Connection Handle:   %d\n", connectionHandle);
    Serial.printf("Valid Data Count:    %d\n", validDataCount);
    Serial.printf("Invalid Data Count:  %d\n", invalidDataCount);
    Serial.printf("Data Quality:        %s\n", dataQualityGood ? "GOOD" : "POOR");
    Serial.printf("Last Valid Data:     %lu ms ago\n", 
                 lastValidDataTime > 0 ? (millis() - lastValidDataTime) : 0);
    Serial.printf("Current Power:       %d W\n", instPWR);
    Serial.printf("Current Cadence:     %d RPM\n", instCAD);
    Serial.println("===============");
}

// ==================== BLE 服务端（桥接转发）方法实现 ====================
// 单块 nRF52840 同时承担：BLE Central(接功率计) + BLE Server(转发给手机) + ANT+ Master(广播给码表)

void PowerMeter::initBLEServer() {
    Serial.println("Initializing BLE Server (bridge)...");
    
    // UUID 与属性必须在 begin() 之前配置（Adafruit Bluefruit52Lib 的 API 约定）
    powerBridgeService.setUuid(BLEUuid(CYCLING_POWER_SERVICE_UUID));
    powerBridgeChar.setUuid(BLEUuid(CYCLING_POWER_MEASUREMENT_UUID));
    powerBridgeChar.setProperties(CHR_PROPS_READ | CHR_PROPS_NOTIFY);

    // 启动服务与特征值（NOTIFY 特性会自动添加 CCCD 描述符）
    powerBridgeService.begin();
    powerBridgeChar.begin();
    
    // 外设角色连接/断开回调
    Bluefruit.Periph.setConnectCallback(staticPeriphConnectCallback);
    Bluefruit.Periph.setDisconnectCallback(staticPeriphDisconnectCallback);
    
    // 开始广播（设备名与原ESP32桥保持一致，App无需改动）
    Bluefruit.setName("XDS Power Bridge");
    Bluefruit.Advertising.addService(powerBridgeService);
    Bluefruit.Advertising.restartOnDisconnect(true);
    Bluefruit.Advertising.start();
    
    Serial.println("BLE Server initialized and advertising started");
}

void PowerMeter::onPeriphConnect(uint16_t conn_handle) {
    Serial.printf("BLE Server: Device connected, handle: %d\n", conn_handle);
    serverDeviceConnected = true;
    serverConnHandle = conn_handle;
}

void PowerMeter::onPeriphDisconnect(uint16_t conn_handle, uint8_t reason) {
    Serial.printf("BLE Server: Device disconnected, handle: %d, reason: 0x%02X\n", conn_handle, reason);
    serverDeviceConnected = false;
    serverConnHandle = 0;
    // restartOnDisconnect(true) 会自动恢复广播
}

void PowerMeter::staticPeriphConnectCallback(uint16_t conn_handle) {
    if (instance) {
        instance->onPeriphConnect(conn_handle);
    }
}

void PowerMeter::staticPeriphDisconnectCallback(uint16_t conn_handle, uint8_t reason) {
    if (instance) {
        instance->onPeriphDisconnect(conn_handle, reason);
    }
}