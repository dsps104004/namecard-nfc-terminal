# 名片型 NFC 讀寫模擬終端 V1
## 使用元件
| 元件名稱 | 數量 | 備註 |
| --- | --- | --- |
| ESP32-C3 Super Mini | 1 |  |
| PN532 NFC RFID V3 | 1 |  |
| TP4056 | 1 |  |
| Type-C 公頭 | 1 | 帶小PCB版 |
| 200mAh 聚合物離電池 | 1 |  |
| SMD 0402 10kΩ | 1 | 6.2kΩ ~ 10KΩ |
| 單芯線/多芯線 | 若干長度 | 建議 0.5 mmΦ 左右 |
### 元件圖片
#### ESP32-C3 Super Mini
<table>
  <img src="https://michiel.vanderwulp.be/domotica/Modules/ESP32-C3-SuperMini/ESP32-C3-SuperMini.jpg" alt="ESP32-C3 Super Mini" width="200">
</table>

#### PN532 NFC RFID V3
<table>
  <img src="https://easyelecmodule.com/wp-content/uploads/nfc_04.jpg" alt="PN532 NFC RFID V3" width="270">
</table>

#### TP4056
<table>
  <img src="https://encrypted-tbn0.gstatic.com/images?q=tbn:ANd9GcT4uhXMvRz9wu1NOUzWIRsSd2w30YCk3So4CHfJIP75gDqFT4nOua6oa69-&s=10" alt="TP4056" width="300">
</table>

### Type-C 公頭
<table>
  <img src="https://down-tw.img.susercontent.com/file/39d3b62954eeaba6b0fc198fe3d5de49" alt="TP4056" width="300">
</table>

# PN532 硬體說明

本專案基於 **NXP PN532 (13.56 MHz)** NFC/RFID 控制器開發。以下詳細列出本硬體的通訊規格、各類卡片支援狀態，以及開發與使用上的限制。

---

## 一、 硬體與通訊基礎規格

| 規格項目 | 詳細參數與支援範圍 | 開發備註 |
| :--- | :--- | :--- |
| **核心晶片** | NXP PN532 | 整合 80C51 微控制器核心與非接觸式類比前端 |
| **工作頻率** | **13.56 MHz**（高頻 HF） | 僅支援高頻，**完全不支援** 125 kHz / 134.2 kHz 低頻 |
| **支援通訊協定** | ISO/IEC 14443 Type A / Type B<br>FeliCa (JIS X 6319-4)<br>ISO/IEC 18092 (NFCIP-1 P2P) | **不支援** ISO/IEC 15693（如圖書館 I-CODE SLI 標籤） |
| **傳輸速率** | 106 kbps / 212 kbps / 424 kbps | 視卡片協定與感應距離而定 |
| **主控通訊介面** | **HSU**（高速串口 / UART，預設 115200 bps）<br>**I2C**（預設 7-bit 位址 `0x24`）<br>**SPI**（最高 5 MHz，LSB First） | 市售模組需透過實體指撥開關（DIP Switch）切換介面 |
| **工作電壓** | 2.7V – 5.5V（核心邏輯電平 3.3V） | 連接 5V 單晶片時請確認模組是否具備電平轉換電路 |
| **有效感應距離** | 約 **3 – 5 cm**（視天線設計與卡片尺寸而定） | 貼有防金屬屏蔽層或小型滴膠扣時距離會縮短 |

---

## 二、 卡片晶片讀寫與模擬能力對照表

| 卡片 / 晶片類型 | 通訊標準 | 讀取能力 (Read) | 寫入能力 (Write) | 卡模擬能力 (Emulation) | 關鍵限制與開發備註 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Mifare Classic 1K / 4K**<br>（S50 / S70 原廠正版卡） | ISO 14443-A | **✅ 完整讀取**<br>（UID + 全部扇區） | **⚠️ 僅扇區寫入**<br>（Block 0 唯讀鎖死） | **⚠️ 僅限純 UID**<br>（無法模擬 Crypto1 加密） | 需搭配已知金鑰或 `mfoc` / `mfcuk` 破解密碼後存取 |
| **UID 卡（一代卡 / Gen1a）** | ISO 14443-A | **✅ 完整讀取** | **✅ 完整寫入**<br>（含 Block 0 卡號） | ➖ 不適用（此為實體空白卡） | 需發送特殊後門指令寫入 Block 0；易被門禁防火牆攔截 |
| **CUID 卡（二代卡 / Gen2）** | ISO 14443-A | **✅ 完整讀取** | **✅ 完整寫入**<br>（含 Block 0 卡號） | ➖ 不適用（此為實體空白卡） | 使用標準寫入指令修改 Block 0；可繞過多數基礎防複製偵測 |
| **FUID / UFUID 卡** | ISO 14443-A | **✅ 完整讀取** | **⚠️ 有限寫入**<br>（封卡鎖死後轉唯讀） | ➖ 不適用（此為實體空白卡） | FUID 寫入一次即鎖死；UFUID 需發送封卡指令，防屏蔽最強 |
| **NTAG213 / 215 / 216**<br>**Mifare Ultralight** | ISO 14443-A<br>NFC Type 2 | **✅ 完整讀取**<br>（UID + 全部頁面） | **✅ 完整寫入**<br>（NDEF 與原始頁面） | **✅ 支援模擬**<br>（需主控端回應指令） | NTAG215 支援寫入 Amiibo；支援密碼保護（PWD）設定 |
| **Mifare DESFire**<br>（EV1 / EV2 / EV3） | ISO 14443-4 Type A | **⚠️ 僅限公開資訊**<br>（UID / ATQA / SAK） | **❌ 不支援**<br>（硬體加密保護） | **⚠️ 僅限模擬卡號**<br>（無法模擬加密應用） | 內部採用 AES / 3DES 硬體加密，無金鑰無法存取檔案系統 |
| **CPU 卡 / 金融卡 / 悠遊卡**<br>（SuperCard / 健保卡等） | ISO 14443-4 Type A/B | **⚠️ 僅限公開資訊**<br>（UID 與基礎 APDU） | **❌ 不支援**<br>（動態加密運算） | **❌ 不支援完整模擬** | 具備獨立微處理器與動態金鑰驗證，無法直接複製 |
| **FeliCa 卡片**<br>（日本 Suica / 交通卡） | JIS X 6319-4 | **⚠️ 支援公開區**<br>（IDm / PMm） | **⚠️ 僅限非加密區** | **✅ 支援底層協定模擬**<br>（可自訂 IDm / PMm） | 加密區域需具備系統金鑰（System Key）才能讀寫 |
| **ISO 15693 標籤**<br>（I-CODE SLI / 圖書館標籤） | ISO/IEC 15693 | **❌ 完全不支援** | **❌ 完全不支援** | **❌ 完全不支援** | 調變方式與副載波頻率不同，PN532 硬體無法識別 |
| **EM4100 / TK4100 / T5577**<br>**HID Prox 傳統門禁卡** | 125 kHz 低頻 LF | **❌ 完全不支援** | **❌ 完全不支援** | **❌ 完全不支援** | 工作頻率完全不同，需改用 125 kHz 讀寫器或 Proxmark3 |

---

## 三、 使用限制與注意事項

1. **卡模擬模式（Card Emulation）無法替代實體複製卡**：
   * PN532 在卡模擬模式下（`TgInitAsTarget`）**不支援硬體層級的 Mifare Classic (Crypto1) 加密驗證**。
   * 若目標門禁機僅驗證 **4-byte UID 卡號**，可直接透過 PN532 模擬開門；若門禁系統會驗證扇區密碼或資料內容，則必須將資料寫入實體的 **UID / CUID / FUID** 空白卡中。
2. **無被動感應供電能力**：
   * 執行卡模擬時，PN532 模組必須保持外部獨立供電，無法像被動式實體感應卡一樣依靠讀卡機的射頻磁場啟動。
3. **通訊介面切換**：
   * 使用前請務必確認 PN532 模組上的實體指撥開關設定與程式碼（HSU / I2C / SPI）一致，否則會出現 `Didn't find PN53x board` 錯誤。

---

## 功能列表 (Features)

### 1. 核心感應與多卡管理
* **卡片讀取 (Read)**：支援讀取 13.56 MHz 高頻卡片（ISO 14443 Type A）之 UID 卡號與基礎通訊參數（ATQA / SAK）。
* **本機儲存 (Storage)**：將讀取到的卡片資料儲存於 ESP32-C3 內部 Flash（NVS / LittleFS），斷電後資料不遺失。
* **多卡快速切換 (Quick Switch)**：支援儲存多組卡片設定檔，可透過實體按鍵（或網頁介面）快速切換目前啟用的卡槽。
* **卡片模擬 (Emulation)**：透過 PN532 卡模擬模式，模擬已選取之 4-byte UID 卡片供外部門禁讀卡機感應。
<!-- * **卡片寫入功能 (Write)**：支援將本機儲存的卡號與扇區資料寫入實體空白複製卡（UID / CUID / FUID）。 -->
<!-- * **Mifare Classic 完整扇區讀寫**：加入金鑰管理與完整 Dump 匯入／匯出功能。 -->

### 2. 硬體與低功耗設計
* **獨立隨身供電**：內建鋰電池與 Type-C 充放電管理模組，無需外接電腦即可獨立運作。
* **低功耗待機 (Low Power)**：閒置時自動進入 Deep Sleep 深度睡眠模式，支援按鍵喚醒以延長電池續航力。
* **加厚卡片型外殼**：名片／標準卡片長寬尺寸，厚度可完整容納電池、Type-C 接口與天線模組，方便隨身攜帶。

# 免責聲明 (Disclaimer)

* 本專案與相關技術文件僅供**教育研究、硬體開發測試與個人合法門禁卡備份**使用。
* 請勿將本專案用於未經授權的系統存取、非法複製他人卡片或任何違反當地法律法規之行為，開發者不對任何濫用行為承擔法律責任。

