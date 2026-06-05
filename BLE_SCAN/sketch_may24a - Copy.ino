#include <Arduino.h>

#include "nvs_flash.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"

#include "esp32-hal-bt.h"
#include "esp_err.h"
#include "esp_gap_ble_api.h"
#include "esp_gap_bt_api.h"

#define UART_BAUD              115200

#define BLE_SCAN_TIME_SEC      10      // час BLE-сканування
#define CLASSIC_SCAN_TIME_SEC  10      // час Classic-сканування приблизно, у секундах
#define PAUSE_BETWEEN_SCANS_MS 3000

volatile bool bleScanDone = false;
volatile bool classicScanDone = false;
volatile bool bleParamsReady = false;

enum ScanPhase {
  PHASE_IDLE,
  PHASE_BLE,
  PHASE_CLASSIC,
  PHASE_PAUSE
};

ScanPhase phase = PHASE_IDLE;
unsigned long pauseStartMs = 0;

esp_ble_scan_params_t bleScanParams;

static void printBtAddress(const uint8_t *addr) {
  Serial.printf(
    "%02X:%02X:%02X:%02X:%02X:%02X",
    addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]
  );
}

static void copyBleName(uint8_t *advData, char *outName, size_t outSize) {
  if (!outName || outSize == 0) return;

  outName[0] = '\0';

  uint8_t nameLen = 0;
  uint8_t *name = esp_ble_resolve_adv_data(
    advData,
    ESP_BLE_AD_TYPE_NAME_CMPL,
    &nameLen
  );

  if (name == nullptr) {
    name = esp_ble_resolve_adv_data(
      advData,
      ESP_BLE_AD_TYPE_NAME_SHORT,
      &nameLen
    );
  }

  if (name && nameLen > 0) {
    size_t copyLen = min((size_t)nameLen, outSize - 1);
    memcpy(outName, name, copyLen);
    outName[copyLen] = '\0';
  }
}

static void copyClassicNameFromEir(uint8_t *eir, char *outName, size_t outSize) {
  if (!eir || !outName || outSize == 0) return;

  uint8_t nameLen = 0;
  uint8_t *name = esp_bt_gap_resolve_eir_data(
    eir,
    ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME,
    &nameLen
  );

  if (name == nullptr) {
    name = esp_bt_gap_resolve_eir_data(
      eir,
      ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME,
      &nameLen
    );
  }

  if (name && nameLen > 0) {
    size_t copyLen = min((size_t)nameLen, outSize - 1);
    memcpy(outName, name, copyLen);
    outName[copyLen] = '\0';
  }
}

void bleGapCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
      bleParamsReady = true;
      break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
      esp_ble_gap_cb_param_t::ble_scan_result_evt_param *scan = &param->scan_rst;

      if (scan->search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
        char name[64];
        copyBleName(scan->ble_adv, name, sizeof(name));

        Serial.print("[BLE] MAC=");
        printBtAddress(scan->bda);

        Serial.printf(
          " RSSI=%d AddrType=%d EventType=%d AdvLen=%d ScanRspLen=%d Name=\"%s\"\n",
          scan->rssi,
          scan->ble_addr_type,
          scan->ble_evt_type,
          scan->adv_data_len,
          scan->scan_rsp_len,
          name
        );
      }

      if (scan->search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
        Serial.println("[BLE] Scan complete");
        bleScanDone = true;
      }

      break;
    }

    default:
      break;
  }
}

void classicGapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
      char name[128] = "";
      int8_t rssi = 0;
      bool hasRssi = false;

      uint32_t cod = 0;
      bool hasCod = false;

      for (int i = 0; i < param->disc_res.num_prop; i++) {
        esp_bt_gap_dev_prop_t *prop = &param->disc_res.prop[i];

        switch (prop->type) {
          case ESP_BT_GAP_DEV_PROP_BDNAME: {
            size_t copyLen = min((size_t)prop->len, sizeof(name) - 1);
            memcpy(name, prop->val, copyLen);
            name[copyLen] = '\0';
            break;
          }

          case ESP_BT_GAP_DEV_PROP_RSSI:
            rssi = *(int8_t *)(prop->val);
            hasRssi = true;
            break;

          case ESP_BT_GAP_DEV_PROP_COD:
            cod = *(uint32_t *)(prop->val);
            hasCod = true;
            break;

          case ESP_BT_GAP_DEV_PROP_EIR:
            if (name[0] == '\0') {
              copyClassicNameFromEir((uint8_t *)prop->val, name, sizeof(name));
            }
            break;

          default:
            break;
        }
      }

      Serial.print("[Classic] MAC=");
      printBtAddress(param->disc_res.bda);

      if (hasRssi) {
        Serial.printf(" RSSI=%d", rssi);
      } else {
        Serial.print(" RSSI=N/A");
      }

      if (hasCod) {
        Serial.printf(" CoD=0x%06lX", (unsigned long)cod);
      } else {
        Serial.print(" CoD=N/A");
      }

      Serial.printf(" Name=\"%s\"\n", name);
      break;
    }

    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
      if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
        Serial.println("[Classic] Discovery started");
      }

      if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
        Serial.println("[Classic] Discovery complete");
        classicScanDone = true;
      }
      break;

    default:
      break;
  }
}

bool initNvs() {
  esp_err_t ret = nvs_flash_init();

  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    ret = nvs_flash_init();
  }

  return ret == ESP_OK;
}

bool initBluetoothDualMode() {
  // Arduino-ESP32 wrapper: коректно ініціалізує та вмикає BT controller.
  if (!btStarted()) {
    if (!btStart()) {
      Serial.println("btStart failed");
      return false;
    }
  }

  esp_err_t ret;

  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
      Serial.printf("esp_bluedroid_init failed: %s (%d)\n", esp_err_to_name(ret), ret);
      return false;
    }
  }

  if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
      Serial.printf("esp_bluedroid_enable failed: %s (%d)\n", esp_err_to_name(ret), ret);
      return false;
    }
  }

  return true;
}

void setupBleScanParams() {
  bleScanParams.scan_type = BLE_SCAN_TYPE_ACTIVE;
  bleScanParams.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
  bleScanParams.scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;

  // Значення у кроках по 0.625 мс.
  // 0x50 = 50 мс, 0x30 = 30 мс.
  bleScanParams.scan_interval = 0x50;
  bleScanParams.scan_window = 0x30;

  // DISABLE = показувати кожен пристрій один раз за скан.
  // ENABLE = показувати повторні рекламні пакети, буде багато виводу.
  bleScanParams.scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE;
}

void startBleScan() {
  Serial.println();
  Serial.println("========== BLE scan ==========");

  bleScanDone = false;
  phase = PHASE_BLE;

  esp_err_t ret = esp_ble_gap_start_scanning(BLE_SCAN_TIME_SEC);
  if (ret != ESP_OK) {
    Serial.printf("esp_ble_gap_start_scanning failed: %d\n", ret);
    bleScanDone = true;
  }
}

void startClassicScan() {
  Serial.println();
  Serial.println("========== Classic Bluetooth scan ==========");

  classicScanDone = false;
  phase = PHASE_CLASSIC;

  // Другий параметр — тривалість inquiry у блоках по 1.28 с.
  // Для приблизно 10 секунд використовуємо 10 / 1.28 ≈ 8.
  uint8_t inquiryLen = max(1, (int)(CLASSIC_SCAN_TIME_SEC / 1.28));

  esp_err_t ret = esp_bt_gap_start_discovery(
    ESP_BT_INQ_MODE_GENERAL_INQUIRY,
    inquiryLen,
    0
  );

  if (ret != ESP_OK) {
    Serial.printf("esp_bt_gap_start_discovery failed: %d\n", ret);
    classicScanDone = true;
  }
}

void setup() {
  Serial.begin(UART_BAUD);
  delay(1000);

  Serial.println();
  Serial.println("ESP32-WROOM Bluetooth scanner");
  Serial.println("UART: 115200 8N1");
  Serial.println();

  if (!initNvs()) {
    Serial.println("NVS init failed");
    while (true) delay(1000);
  }

  if (!initBluetoothDualMode()) {
    Serial.println("Bluetooth init failed");
    while (true) delay(1000);
  }

  esp_ble_gap_register_callback(bleGapCallback);
  esp_bt_gap_register_callback(classicGapCallback);

  esp_bt_dev_set_device_name("ESP32_BT_Scanner");

  setupBleScanParams();

  esp_err_t ret = esp_ble_gap_set_scan_params(&bleScanParams);
  if (ret != ESP_OK) {
    Serial.printf("esp_ble_gap_set_scan_params failed: %d\n", ret);
    while (true) delay(1000);
  }

  Serial.println("Bluetooth initialized");
}

void loop() {
  if (phase == PHASE_IDLE && bleParamsReady) {
    startBleScan();
  }

  if (phase == PHASE_BLE && bleScanDone) {
    startClassicScan();
  }

  if (phase == PHASE_CLASSIC && classicScanDone) {
    phase = PHASE_PAUSE;
    pauseStartMs = millis();

    Serial.println();
    Serial.println("========== Full scan cycle complete ==========");
  }

  if (phase == PHASE_PAUSE) {
    if (millis() - pauseStartMs >= PAUSE_BETWEEN_SCANS_MS) {
      startBleScan();
    }
  }

  delay(20);
}