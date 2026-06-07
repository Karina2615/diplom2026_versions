/**
 * ESP32-WROOM Bluetooth BLE Scanner
 * Сумісно з ESP32 Arduino Core 3.x (вбудована BLE бібліотека)
 *
 * Board:            ESP32 Dev Module
 * Partition Scheme: Huge APP (3MB No OTA)  ← обов'язково!
 * Baud Rate:        115200
 */

#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <map>

// ──────────────────────────────────────────────
//  Налаштування
// ──────────────────────────────────────────────
static const int  SCAN_TIME_SEC   = 10;      // тривалість сканування (секунди)
static const int  PAUSE_MS        = 3000;    // пауза між сесіями (мс)
static const bool ACTIVE_SCAN     = true;    // активне сканування (більше даних)
static const int  SERIAL_BAUD     = 115200;

// ──────────────────────────────────────────────
//  Глобальні змінні
// ──────────────────────────────────────────────
BLEScan* pBLEScan  = nullptr;
int      scanCount = 0;
std::map<String, int> knownDevices;  // MAC → RSSI (для відстеження нових)

// ──────────────────────────────────────────────
//  Допоміжні функції
// ──────────────────────────────────────────────

// Конвертує масив байт у рядок HEX
String bytesToHex(const uint8_t* data, size_t len) {
  String out;
  out.reserve(len * 3);
  for (size_t i = 0; i < len; i++) {
    if (data[i] < 0x10) out += '0';
    out += String(data[i], HEX);
    if (i < len - 1) out += ':';
  }
  out.toUpperCase();
  return out;
}

// Якість сигналу за RSSI
const char* rssiQuality(int rssi) {
  if (rssi >= -50) return "Відмінний";
  if (rssi >= -65) return "Добрий";
  if (rssi >= -75) return "Середній";
  if (rssi >= -85) return "Слабкий";
  return "Дуже слабкий";
}

// Груба оцінка відстані (модель log-distance path loss)
float estimateDistance(int rssi, int txPower = -59) {
  if (rssi == 0) return -1.0f;
  float ratio = (float)rssi / (float)txPower;
  if (ratio < 1.0f) return powf(ratio, 10.0f);
  return 0.89976f * powf(ratio, 7.7095f) + 0.111f;
}

// Назва типу BLE appearance
const char* appearanceName(uint16_t app) {
  switch (app >> 6) {
    case 0x00: return "Generic";
    case 0x01: return "Phone";
    case 0x02: return "Computer";
    case 0x03: return "Watch";
    case 0x04: return "Clock";
    case 0x05: return "Display";
    case 0x06: return "Remote Control";
    case 0x07: return "Eyeglasses";
    case 0x08: return "Tag";
    case 0x09: return "Keyring";
    case 0x0A: return "Media Player";
    case 0x0B: return "Barcode Scanner";
    case 0x0C: return "Thermometer";
    case 0x0D: return "Heart Rate Sensor";
    case 0x0E: return "Blood Pressure";
    case 0x0F: return "HID Device";
    case 0x10: return "Glucose Meter";
    case 0x11: return "Running/Walking Sensor";
    case 0x12: return "Cycling";
    case 0x1F: return "Outdoor Sports";
    default:   return "Unknown";
  }
}

// Назва відомого виробника за Company ID
const char* companyName(uint16_t id) {
  switch (id) {
    case 0x004C: return "Apple";
    case 0x0006: return "Microsoft";
    case 0x00E0: return "Google";
    case 0x0075: return "Samsung";
    case 0x0059: return "Nordic Semiconductor";
    case 0x02FF: return "Xiaomi";
    case 0x01D8: return "Garmin";
    case 0x0087: return "Garmin";
    case 0x000F: return "Broadcom";
    case 0x0000: return "Ericsson";
    default:     return nullptr;
  }
}

// Декодування iBeacon (Apple, company ID = 0x004C, type = 0x02, len = 0x15)
void printIBeacon(const uint8_t* d, size_t len) {
  if (len < 25) return;
  // Bytes: [0]=4C [1]=00 [2]=02 [3]=15 [4..19]=UUID [20..21]=Major [22..23]=Minor [24]=TxPwr
  Serial.println(F("  ┌── iBeacon ─────────────────────────────┐"));

  // UUID (16 байт у форматі 8-4-4-4-12)
  Serial.print(F("  │ UUID:  "));
  const uint8_t* u = d + 4;
  for (int i = 0; i < 16; i++) {
    if (u[i] < 0x10) Serial.print('0');
    Serial.print(u[i], HEX);
    if (i == 3 || i == 5 || i == 7 || i == 9) Serial.print('-');
  }
  Serial.println();

  uint16_t major = ((uint16_t)d[20] << 8) | d[21];
  uint16_t minor = ((uint16_t)d[22] << 8) | d[23];
  int8_t   power = (int8_t)d[24];

  Serial.print(F("  │ Major: ")); Serial.println(major);
  Serial.print(F("  │ Minor: ")); Serial.println(minor);
  Serial.print(F("  │ TxPwr: ")); Serial.print(power); Serial.println(F(" dBm"));
  Serial.println(F("  └────────────────────────────────────────┘"));
}

// ──────────────────────────────────────────────
//  Callback — викликається для кожного пристрою
// ──────────────────────────────────────────────
class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
public:
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    String mac = advertisedDevice.getAddress().toString();
    bool isNew = knownDevices.find(mac) == knownDevices.end();
    knownDevices[mac] = advertisedDevice.getRSSI();

    Serial.println(F("─────────────────────────────────────────────────────"));
    Serial.println(isNew ? F("  ★ НОВИЙ ПРИСТРІЙ") : F("  ↻ Повторне виявлення"));

    // ── MAC ───────────────────────────────────────────
    Serial.print(F("  MAC:     "));
    Serial.println(mac);

    // ── Ім'я ──────────────────────────────────────────
    Serial.print(F("  Ім'я:    "));
    if (advertisedDevice.haveName()) {
      Serial.println(advertisedDevice.getName());
    } else {
      Serial.println(F("(без імені)"));
    }

    // ── RSSI ──────────────────────────────────────────
    int rssi = advertisedDevice.getRSSI();
    Serial.print(F("  RSSI:    "));
    Serial.print(rssi);
    Serial.print(F(" dBm  → "));
    Serial.println(rssiQuality(rssi));

    // ── TX Power та відстань ──────────────────────────
    int txPwr = -59;
    if (advertisedDevice.haveTXPower()) {
      txPwr = advertisedDevice.getTXPower();
      Serial.print(F("  TX Pwr:  "));
      Serial.print(txPwr);
      Serial.println(F(" dBm"));
    }
    Serial.print(F("  Відст.~: "));
    Serial.print(estimateDistance(rssi, txPwr), 2);
    Serial.println(F(" м (орієнтовно)"));

    // ── Appearance ────────────────────────────────────
    if (advertisedDevice.haveAppearance()) {
      uint16_t app = advertisedDevice.getAppearance();
      Serial.print(F("  Тип:     "));
      Serial.print(appearanceName(app));
      Serial.print(F("  (0x"));
      Serial.print(app, HEX);
      Serial.println(F(")"));
    }

    // ── Service UUID ──────────────────────────────────
    if (advertisedDevice.haveServiceUUID()) {
      Serial.print(F("  SvcUUID: "));
      Serial.println(advertisedDevice.getServiceUUID().toString());
    }

    // ── Manufacturer Data ─────────────────────────────
    if (advertisedDevice.haveManufacturerData()) {
      String mfr = advertisedDevice.getManufacturerData();
      const uint8_t* d = (const uint8_t*)mfr.c_str();
      size_t len = (size_t)mfr.length();

      Serial.print(F("  MFR hex: "));
      Serial.println(bytesToHex(d, len));

      if (len >= 2) {
        uint16_t cid = (uint16_t)d[0] | ((uint16_t)d[1] << 8);  // little-endian
        Serial.print(F("  Company: 0x"));
        if (cid < 0x1000) Serial.print('0');
        Serial.print(cid, HEX);
        const char* cname = companyName(cid);
        if (cname) {
          Serial.print(F("  ["));
          Serial.print(cname);
          Serial.print(F("]"));
        }
        Serial.println();

        // iBeacon: Apple (0x004C) + type 0x02 + length 0x15
        if (cid == 0x004C && len >= 4 && d[2] == 0x02 && d[3] == 0x15) {
          printIBeacon(d, len);
        }
      }
    }

    // ── Service Data ──────────────────────────────────
    if (advertisedDevice.haveServiceData()) {
      String sd = advertisedDevice.getServiceData();
      Serial.print(F("  SvcData: "));
      Serial.println(bytesToHex((const uint8_t*)sd.c_str(), (size_t)sd.length()));
    }

    Serial.println();
  }
};

// ──────────────────────────────────────────────
//  setup()
// ──────────────────────────────────────────────
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(500);

  Serial.println(F("\n╔══════════════════════════════════════════════════════╗"));
  Serial.println(F("║      ESP32-WROOM  BLE Scanner  (core 3.x)           ║"));
  Serial.println(F("╚══════════════════════════════════════════════════════╝"));
  Serial.print(F("  Chip:     ")); Serial.println(ESP.getChipModel());
  Serial.print(F("  Flash:    ")); Serial.print(ESP.getFlashChipSize() >> 20); Serial.println(F(" MB"));
  Serial.print(F("  Free RAM: ")); Serial.print(ESP.getFreeHeap() >> 10);      Serial.println(F(" KB\n"));

  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new ScanCallbacks(), true);
  pBLEScan->setActiveScan(ACTIVE_SCAN);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);

  Serial.println(F("BLE ініціалізовано. Починаємо сканування...\n"));
}

// ──────────────────────────────────────────────
//  loop()
// ──────────────────────────────────────────────
void loop() {
  ++scanCount;

  Serial.println(F("══════════════════════════════════════════════════════"));
  Serial.print(F("  ▶ Сесія #")); Serial.print(scanCount);
  Serial.print(F("  |  тривалість: ")); Serial.print(SCAN_TIME_SEC);
  Serial.println(F(" с"));
  Serial.println(F("══════════════════════════════════════════════════════\n"));

  BLEScanResults* results = pBLEScan->start(SCAN_TIME_SEC, false);

  // ── Підсумок ──────────────────────────────────────
  int found = results ? results->getCount() : 0;
  Serial.println(F("\n──────────────────── ПІДСУМОК ──────────────────────"));
  Serial.print(F("  Знайдено у сесії:  ")); Serial.println(found);
  Serial.print(F("  Всього унікальних: ")); Serial.println((int)knownDevices.size());

  if (found > 0) {
    Serial.println(F("\n  MAC               | RSSI      | Ім'я"));
    Serial.println(F("  ──────────────────────────────────────────────────"));
    for (int i = 0; i < found; i++) {
      BLEAdvertisedDevice dev = results->getDevice(i);
      Serial.print(F("  "));
      Serial.print(dev.getAddress().toString());
      Serial.print(F(" | "));
      int r = dev.getRSSI();
      if (r > -100) Serial.print(' ');
      Serial.print(r);
      Serial.print(F(" dBm | "));
      if (dev.haveName()) Serial.println(dev.getName());
      else                Serial.println(F("(no name)"));
    }
  }
  Serial.println(F("────────────────────────────────────────────────────\n"));

  pBLEScan->clearResults();

  Serial.print(F("Наступне сканування через "));
  Serial.print(PAUSE_MS / 1000);
  Serial.println(F(" с...\n"));
  delay(PAUSE_MS);
}
