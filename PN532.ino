#include <Wire.h>
#include <Adafruit_PN532.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <BLEAdvertisedDevice.h>
#include <Preferences.h>
#include <vector>

// ==========================================
// 1. 硬體引腳與 I2C 位址定義 (ESP32-C3 SuperMini)
// ==========================================
#define I2C_SDA 8
#define I2C_SCL 9
#define PN532_IRQ 3       // 接收外部 RF 場強喚醒 ESP32
#define PN532_RSTPDN 2    // 控制 PN532 RSTO 硬體重置
#define PN532_I2C_ADDR 0x24

// 必須傳入 I2C_SDA 與 I2C_SCL，不可傳入 IRQ/RSTPDN
Adafruit_PN532 nfc(I2C_SDA, I2C_SCL);
Preferences pref;

// ==========================================
// 2. 多槽位儲存資料結構 (永久保存於 Flash)
// ==========================================
#define MAX_SLOTS 4

struct SlotConfig {
  bool enabled;           // 槽位是否啟用
  uint8_t uid0;           // UID 第 1 碼 (註：PN532 硬體模擬首碼固定發射 0x08)
  uint8_t uid1;           // UID 第 2 碼 (實際模擬發射)
  uint8_t uid2;           // UID 第 3 碼 (實際模擬發射)
  uint8_t uid3;           // UID 第 4 碼 (實際模擬發射)
  char label[24];         // 自訂備註名稱 (例如 "大門門禁", "公司電梯")
};

struct SystemStorage {
  uint8_t activeSlot;             // 目前啟用的槽位 (0 ~ 3)
  SlotConfig slots[MAX_SLOTS];    // 4 組獨立門禁卡槽位
};

SystemStorage sysData;
String rxBuffer = "";
unsigned long engModeStartTime = 0;

BLECharacteristic* pTxChar = nullptr;
bool deviceConnected = false;

// 函式原型宣告
void armPN532ForWakeup();
void enterDeepSleep();
void sendBLEMessage(String msg);

// ==========================================
// 3. 藍牙廣播特徵值暗號定義
// ==========================================
enum BroadcastTrigger {
  TRG_NONE = -1,          // 未偵測到特殊訊號 -> 沿用上一次模式
  TRG_SLOT_0 = 0,         // 切換至 Slot 0
  TRG_SLOT_1 = 1,         // 切換至 Slot 1
  TRG_SLOT_2 = 2,         // 切換至 Slot 2
  TRG_SLOT_3 = 3,         // 切換至 Slot 3
  TRG_ENG_MODE = 99       // 特定特徵值 -> 進入工程模式
};

int detectedTrigger = TRG_NONE;
BLEScan* pBLEScan = nullptr;

// 專屬服務 UUID 切換暗號
static BLEUUID UUID_SLOT_0("00001110-0000-1000-8000-00805f9b34fb");
static BLEUUID UUID_SLOT_1("00001111-0000-1000-8000-00805f9b34fb");
static BLEUUID UUID_SLOT_2("00001112-0000-1000-8000-00805f9b34fb");
static BLEUUID UUID_SLOT_3("00001113-0000-1000-8000-00805f9b34fb");
static BLEUUID UUID_ENG_MODE("00001119-0000-1000-8000-00805f9b34fb");

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    int rssi = advertisedDevice.getRSSI();

    // 限制手機必須靠近卡片 (RSSI > -65 dBm) 才觸發切換
    if (rssi > -65 && advertisedDevice.haveServiceUUID()) {
      BLEUUID devUUID = advertisedDevice.getServiceUUID();

      if (devUUID.equals(UUID_SLOT_0)) {
        detectedTrigger = TRG_SLOT_0;
        if (pBLEScan) pBLEScan->stop();
      } else if (devUUID.equals(UUID_SLOT_1)) {
        detectedTrigger = TRG_SLOT_1;
        if (pBLEScan) pBLEScan->stop();
      } else if (devUUID.equals(UUID_SLOT_2)) {
        detectedTrigger = TRG_SLOT_2;
        if (pBLEScan) pBLEScan->stop();
      } else if (devUUID.equals(UUID_SLOT_3)) {
        detectedTrigger = TRG_SLOT_3;
        if (pBLEScan) pBLEScan->stop();
      } else if (devUUID.equals(UUID_ENG_MODE)) {
        detectedTrigger = TRG_ENG_MODE;
        if (pBLEScan) pBLEScan->stop();
      }
    }
  }
};

// ==========================================
// 4. 工程模式：讀卡功能與藍牙指令解析器
// ==========================================
void sendBLEMessage(String msg) {
  Serial.println(msg);
  if (deviceConnected && pTxChar != nullptr) {
    msg += "\n";
    pTxChar->setValue(msg.c_str());
    pTxChar->notify();
    delay(20);
  }
}

void executeEngineeringReadCard(int targetSlotToClone) {
  sendBLEMessage("[工程讀卡] 請在 10 秒內將外部實體卡片靠近感應區...");

  digitalWrite(PN532_RSTPDN, LOW);
  delay(40);
  digitalWrite(PN532_RSTPDN, HIGH);
  delay(100);

  nfc.begin();
  nfc.SAMConfig();

  std::vector<uint8_t> uid(8, 0);
  uint8_t uidLength = 0;

  bool success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid.data(), &uidLength, 10000);

  if (success) {
    String hexStr = "";
    for (uint8_t i = 0; i < uidLength; i++) {
      if (uid.at(i) < 0x10) hexStr += "0";
      hexStr += String(uid.at(i), HEX);
    }
    hexStr.toUpperCase();

    sendBLEMessage("[讀卡成功] 長度: " + String(uidLength) + " Bytes | UID: " + hexStr);

    if (uidLength == 4) {
      if (uid.at(0) != 0x08) {
        sendBLEMessage("[提示] PN532 硬體首碼固定為 08，實際將模擬為 08" + hexStr.substring(2));
      }
      if (targetSlotToClone >= 0 && targetSlotToClone < MAX_SLOTS) {
        pref.begin("sys_card", false);
        sysData.slots[targetSlotToClone].enabled = true;
        sysData.slots[targetSlotToClone].uid0 = uid.at(0);
        sysData.slots[targetSlotToClone].uid1 = uid.at(1);
        sysData.slots[targetSlotToClone].uid2 = uid.at(2);
        sysData.slots[targetSlotToClone].uid3 = uid.at(3);
        strcpy(sysData.slots[targetSlotToClone].label, "Cloned_Card");
        pref.putBytes("config", &sysData, sizeof(SystemStorage));
        pref.end();
        sendBLEMessage("[複製完成] 已將卡號寫入 Slot " + String(targetSlotToClone));
      }
    } else if (uidLength == 7) {
      sendBLEMessage("[資訊] 此為 7-Byte UID 卡片 (NTAG / 悠遊卡)");
    }
  } else {
    sendBLEMessage("[讀卡超時] 10 秒內未感應到任何卡片。");
  }
}

void processEngineeringCommand(String cmd) {
  cmd.trim();
  engModeStartTime = millis();

  // 1. 查詢所有槽位狀態: "INFO"
  if (cmd.equalsIgnoreCase("INFO")) {
    sendBLEMessage("=== 門禁卡槽位配置 (目前啟用: Slot " + String(sysData.activeSlot) + ") ===");
    for (int i = 0; i < MAX_SLOTS; i++) {
      SlotConfig* s = sysData.slots + i;
      std::vector<char> buf(16, 0);
      sprintf(buf.data(), "%02X%02X%02X%02X", s->uid0, s->uid1, s->uid2, s->uid3);
      String hexStr = String(buf.data());
      String line = "Slot " + String(i) + " [" + String(s->label) + "]: ";
      line += "發射 08" + hexStr.substring(2) + " (原碼:" + hexStr + ")";
      sendBLEMessage(line);
    }
    return;
  }

  // 2. 手動寫入指定槽位 UID: "SET_UID:0:AABBCCDD"
  if (cmd.startsWith("SET_UID:") && cmd.length() == 18) {
    int slot = cmd.substring(8, 9).toInt();
    String hexStr = cmd.substring(10);
    if (slot >= 0 && slot < MAX_SLOTS) {
      pref.begin("sys_card", false);
      sysData.slots[slot].enabled = true;
      sysData.slots[slot].uid0 = (uint8_t) strtol(hexStr.substring(0, 2).c_str(), NULL, 16);
      sysData.slots[slot].uid1 = (uint8_t) strtol(hexStr.substring(2, 4).c_str(), NULL, 16);
      sysData.slots[slot].uid2 = (uint8_t) strtol(hexStr.substring(4, 6).c_str(), NULL, 16);
      sysData.slots[slot].uid3 = (uint8_t) strtol(hexStr.substring(6, 8).c_str(), NULL, 16);
      pref.putBytes("config", &sysData, sizeof(SystemStorage));
      pref.end();
      sendBLEMessage("[設定成功] Slot " + String(slot) + " 卡號已更新為 " + hexStr);
    }
    return;
  }

  // 3. 設定槽位備註名稱: "NAME:0:HomeDoor"
  if (cmd.startsWith("NAME:") && cmd.length() > 7) {
    int slot = cmd.substring(5, 6).toInt();
    String nameStr = cmd.substring(7);
    if (slot >= 0 && slot < MAX_SLOTS) {
      pref.begin("sys_card", false);
      nameStr.toCharArray(sysData.slots[slot].label, 24);
      pref.putBytes("config", &sysData, sizeof(SystemStorage));
      pref.end();
      sendBLEMessage("[備註更新] Slot " + String(slot) + " 名稱已設為 " + nameStr);
    }
    return;
  }

  // 4. 手動切換預設啟用槽位: "USE:2"
  if (cmd.startsWith("USE:") && cmd.length() == 5) {
    int slot = cmd.substring(4, 5).toInt();
    if (slot >= 0 && slot < MAX_SLOTS) {
      pref.begin("sys_card", false);
      sysData.activeSlot = slot;
      pref.putBytes("config", &sysData, sizeof(SystemStorage));
      pref.end();
      sendBLEMessage("[切換成功] 預設啟用槽位已變更為 Slot " + String(slot));
    }
    return;
  }

  // 5. 工程讀卡: "READ" 或 "CLONE:0"
  if (cmd.equalsIgnoreCase("READ")) {
    executeEngineeringReadCard(-1);
    return;
  }
  if (cmd.startsWith("CLONE:") && cmd.length() == 7) {
    int targetSlot = cmd.substring(6, 7).toInt();
    if (targetSlot >= 0 && targetSlot < MAX_SLOTS) {
      executeEngineeringReadCard(targetSlot);
    }
    return;
  }

  // 6. 退出工程模式並進入低功耗休眠: "EXIT"
  if (cmd.equalsIgnoreCase("EXIT")) {
    sendBLEMessage("[系統] 正在退出工程模式並回歸低功耗待機...");
    delay(200);
    enterDeepSleep();
  }

  sendBLEMessage("[錯誤] 未知指令。支援: INFO, SET_UID:s:hex, NAME:s:txt, USE:s, READ, CLONE:s, EXIT");
}

class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    engModeStartTime = millis();
  }
  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    BLEDevice::getAdvertising()->start();
  }
};

class MyCharCallbacks: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String rxValue = pCharacteristic->getValue().c_str();
    for (int i = 0; i < rxValue.length(); i++) {
      char c = rxValue[i];
      if (c == '\n' || c == '\r') {
        if (rxBuffer.length() > 0) {
          processEngineeringCommand(rxBuffer);
          rxBuffer = "";
        }
      } else {
        rxBuffer += c;
      }
    }
  }
};
// ==========================================
// 5. PN532 原生 I2C 封包收發函式
// ==========================================
void sendPN532RawCommand(uint8_t* cmd, uint8_t cmdLen) {
  uint8_t checksum = 0xD4;
  for (uint8_t i = 0; i < cmdLen; i++) {
    checksum += *(cmd + i);
  }
  uint8_t dcs = ~checksum + 1;
  uint8_t len = cmdLen + 1;
  uint8_t lcs = ~len + 1;

  Wire.beginTransmission(PN532_I2C_ADDR);
  Wire.write(0x00);
  Wire.write(0x00);
  Wire.write(0xFF);
  Wire.write(len);
  Wire.write(lcs);
  Wire.write(0xD4);
  for (uint8_t i = 0; i < cmdLen; i++) {
    Wire.write(*(cmd + i));
  }
  Wire.write(dcs);
  Wire.write(0x00);
  Wire.endTransmission();
}

void readPN532Ack() {
  delay(5);
  Wire.requestFrom((uint8_t)PN532_I2C_ADDR, (uint8_t)7);
  while (Wire.available()) Wire.read();
}

int readPN532RawResponse(uint8_t* outBuf, uint16_t timeoutMs) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    if (digitalRead(PN532_IRQ) == LOW) {
      Wire.requestFrom((uint8_t)PN532_I2C_ADDR, (uint8_t)64);
      if (Wire.available() >= 8) {
        uint8_t rdy = Wire.read();
        if (rdy == 0x01) {
          Wire.read(); Wire.read(); Wire.read(); // 略過 00 00 FF
          uint8_t len = Wire.read();
          uint8_t lcs = Wire.read();
          if (len > 1 && len < 60) {
            uint8_t tfi = Wire.read();
            uint8_t dataLen = len - 1;
            for (uint8_t i = 0; i < dataLen; i++) {
              *(outBuf + i) = Wire.available() ? Wire.read() : 0x00;
            }
            return dataLen;
          }
        }
      }
    }
    delay(2);
  }
  return -1;
}

// ==========================================
// 6. 門禁卡 UID 模擬引擎 (實測最穩定 Mode = 0x01)
// ==========================================
void runCardEmulation(SlotConfig slot, bool modeJustChanged) {
  std::vector<char> uidStr(24, 0);
  sprintf(uidStr.data(), "08 %02X %02X %02X", slot.uid1, slot.uid2, slot.uid3);
  Serial.print("啟動門禁卡模擬 [");
  Serial.print(slot.label);
  Serial.print("] -> 實際發射 UID: ");
  Serial.println(uidStr.data());

  std::vector<uint8_t> rxBuf(64, 0);

  if (!modeJustChanged) {
    if (readPN532RawResponse(rxBuf.data(), 400) > 0) {
      Serial.println("【感應成功】已承接喚醒訊號並完成讀卡機驗證！");
      return;
    }
  }

  armPN532ForWakeup();
  if (readPN532RawResponse(rxBuf.data(), 3000) > 0) {
    Serial.println("【感應成功】讀卡機已完成讀取！");
  } else {
    Serial.println("【待機超時】3 秒內無讀卡機靠近，返回休眠。");
  }
}

// 啟動工程模式藍牙雙向伺服器 (Nordic UART Service 標準)
void startEngineeringBluetoothServer() {
  BLEDevice::deinit(false);
  delay(20);
  BLEDevice::init("MultiCard_EngMode");
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
  
  BLECharacteristic *pRxChar = pService->createCharacteristic(
                                 "6E400002-B5A3-F393-E0A9-E50E24DCCA9E",
                                 BLECharacteristic::PROPERTY_WRITE
                               );
  pRxChar->setCallbacks(new MyCharCallbacks());

  pTxChar = pService->createCharacteristic(
              "6E400003-B5A3-F393-E0A9-E50E24DCCA9E",
              BLECharacteristic::PROPERTY_NOTIFY
            );
  pTxChar->addDescriptor(new BLE2902());

  pService->start();
  BLEDevice::getAdvertising()->start();
  
  engModeStartTime = millis();
  Serial.println("==========================================");
  Serial.println("【工程模式已啟動】藍牙名稱: MultiCard_EngMode");
  Serial.println("支援指令: INFO / SET_UID / NAME / USE / READ / CLONE / EXIT");
  Serial.println("==========================================");
}

// 睡前布署 PN532 卡模擬待機 (採用 ELECHOUSE V3 實測最穩定之 Mode = 0x01)
void armPN532ForWakeup() {
  std::vector<uint8_t> dummy(16, 0);

  uint8_t setParamCmd[] = { 0x12, 0x14 };
  sendPN532RawCommand(setParamCmd, sizeof(setParamCmd));
  readPN532Ack();
  readPN532RawResponse(dummy.data(), 100);
  delay(10);

  SlotConfig current = sysData.slots[sysData.activeSlot];

  std::vector<uint8_t> initCmd = {
    0x8C,                               // TGINITASTARGET
    0x01,                               // Mode: Passive Only (ELECHOUSE V3 實測唯一能穩定喚醒之模式)
    0x04, 0x00,                         // ATQA (Mifare 標準)
    current.uid1, current.uid2, current.uid3, // UID 後三碼 (首碼硬體固定 0x08)
    0x08,                               // SAK: 0x08 (標準 Mifare Classic 1K 門禁卡)
    // 18 bytes FeliCa 參數
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // 10 bytes NFCID3t
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00,                               // General bytes 長度 = 0
    0x00                                // Historical bytes 長度 = 0
  };

  sendPN532RawCommand(initCmd.data(), initCmd.size());
  readPN532Ack();
  delay(20);
}

void enterDeepSleep() {
  Serial.println("重置 PN532 並布署 RF 場強監聽...");
  
  // ELECHOUSE V3 硬體重置：拉低 40ms，拉高後等足 100ms 讓內部石英震盪器穩定
  digitalWrite(PN532_RSTPDN, LOW);
  delay(40);
  digitalWrite(PN532_RSTPDN, HIGH);
  delay(100);

  Wire.begin(I2C_SDA, I2C_SCL);
  nfc.SAMConfig();
  delay(10);

  armPN532ForWakeup();

  // 清空任何殘留中斷，確保睡前 IRQ 為 HIGH
  unsigned long waitHigh = millis();
  while (digitalRead(PN532_IRQ) == LOW && millis() - waitHigh < 100) {
    std::vector<uint8_t> clearBuf(32, 0);
    readPN532RawResponse(clearBuf.data(), 20);
  }

  Serial.print("睡前 IRQ 電位狀態 (1為正常待機): ");
  Serial.println(digitalRead(PN532_IRQ));
  Serial.println("進入超低功耗 Deep Sleep（等待手機或讀卡機靠近）...");
  Serial.flush();

  gpio_pullup_en((gpio_num_t)PN532_IRQ);
  gpio_pulldown_dis((gpio_num_t)PN532_IRQ);
  esp_deep_sleep_enable_gpio_wakeup(1ULL << PN532_IRQ, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
}

// ==========================================
// 7. 主程式入口
// ==========================================
void setup() {
  Serial.begin(115200);
  
  pinMode(PN532_IRQ, INPUT_PULLUP);
  pinMode(PN532_RSTPDN, OUTPUT);
  digitalWrite(PN532_RSTPDN, HIGH);
  delay(10);

  // 1. 讀取永久儲存的 4 組門禁槽位配置 (若首次使用則建立預設值)
  pref.begin("sys_card", false);
  if (pref.getBytes("config", &sysData, sizeof(SystemStorage)) == 0) {
    sysData.activeSlot = 0;
    
    SlotConfig* s0 = sysData.slots + 0;
    s0->enabled = true; s0->uid0 = 0x08; s0->uid1 = 0x11; s0->uid2 = 0x22; s0->uid3 = 0x33;
    strcpy(s0->label, "Slot0_MainDoor");

    SlotConfig* s1 = sysData.slots + 1;
    s1->enabled = true; s1->uid0 = 0x08; s1->uid1 = 0x44; s1->uid2 = 0x55; s1->uid3 = 0x66;
    strcpy(s1->label, "Slot1_Office");

    SlotConfig* s2 = sysData.slots + 2;
    s2->enabled = true; s2->uid0 = 0x08; s2->uid1 = 0x77; s2->uid2 = 0x88; s2->uid3 = 0x99;
    strcpy(s2->label, "Slot2_Elevator");

    SlotConfig* s3 = sysData.slots + 3;
    s3->enabled = true; s3->uid0 = 0x08; s3->uid1 = 0xAA; s3->uid2 = 0xBB; s3->uid3 = 0xCC;
    strcpy(s3->label, "Slot3_Backup");

    pref.putBytes("config", &sysData, sizeof(SystemStorage));
  }

  // 2. 初始化 I2C 連線 (感應喚醒時僅接管匯流排，避免重新初始化造成 PN532 卡死)
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  if (wakeup_reason == ESP_SLEEP_WAKEUP_GPIO) {
    Wire.begin(I2C_SDA, I2C_SCL);
  } else {
    delay(100);
    nfc.begin();
    if (!nfc.getFirmwareVersion()) {
      Serial.println("警告：找不到 ELECHOUSE PN532 模組，請檢查 I2C 接線！");
    } else {
      nfc.SAMConfig();
    }
  }

  // 3. 手機或讀卡機靠近喚醒時，優先執行 1 秒極速藍牙廣播掃描
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(40);
  pBLEScan->setWindow(30);
  pBLEScan->start(1, false);
  pBLEScan->clearResults();

  // 4. 判斷是否掃到「進入工程模式」的特定特徵值
  if (detectedTrigger == TRG_ENG_MODE) {
    pref.end();
    startEngineeringBluetoothServer();
    return;
  }

  // 關閉藍牙掃描節省電力
  BLEDevice::deinit(false);

  // 5. 比對廣播特徵值是否發生變化
  bool modeJustChanged = false;
  if (detectedTrigger >= TRG_SLOT_0 && detectedTrigger <= TRG_SLOT_3) {
    if (detectedTrigger != sysData.activeSlot) {
      Serial.print("【特徵值變化】切換至 Slot ");
      Serial.println(detectedTrigger);
      sysData.activeSlot = detectedTrigger;
      pref.putBytes("config", &sysData, sizeof(SystemStorage));
      modeJustChanged = true;
    } else {
      Serial.print("【特徵值相同】維持目前 Slot ");
      Serial.println(sysData.activeSlot);
    }
  } else {
    Serial.print("【未掃到廣播】沿用上一次模式 Slot ");
    Serial.println(sysData.activeSlot);
  }
  pref.end();

  // 6. 執行目前啟用槽位的門禁卡模擬
  runCardEmulation(sysData.slots[sysData.activeSlot], modeJustChanged);

  // 7. 任務完成，回歸低功耗休眠
  enterDeepSleep();
}

void loop() {
  // 工程模式下若超過 2 分鐘無任何操作，自動回歸休眠保護電池
  if (millis() - engModeStartTime > 120000) {
    Serial.println("工程模式閒置超時，自動回歸休眠...");
    enterDeepSleep();
  }
  delay(100);
}
