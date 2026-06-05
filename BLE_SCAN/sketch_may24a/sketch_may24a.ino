/*══════════════════════════════════════════════════════════════════════════════
 *  ESP32 — Bluetooth Audio Bridge + Scanner  (один файл, два режими)
 *
 *  РЕЖИМ 1: SCAN_ONLY true
 *    Сканує BLE та Classic BT пристрої, виводить назви в Serial Monitor.
 *    Використовуй щоб дізнатись точну назву навушників.
 *
 *  РЕЖИМ 2: SCAN_ONLY false
 *    Підключається до TARGET_NAME і стрімить I2S аудіо від STM32 через BT A2DP.
 *
 *  ── Підключення I2S (STM32F407 → ESP32) ────────────────────────────────────
 *    STM32 PC10  I2S3_CK  (BCLK)  →  ESP32 GPIO 26
 *    STM32 PA4   I2S3_WS  (LRCLK) →  ESP32 GPIO 25
 *    STM32 PC12  I2S3_SD  (DATA)  →  ESP32 GPIO 22
 *    GND                          ↔  GND
 *
 *  ── Бібліотека ──────────────────────────────────────────────────────────────
 *    Arduino Library Manager → "ESP32-A2DP" by pschatzmann
 *══════════════════════════════════════════════════════════════════════════════*/

#include <Arduino.h>

// ════════════════════════════════════════════════════════════════════════════
//  КОНФІГУРАЦІЯ  ← змінюй тут
// ════════════════════════════════════════════════════════════════════════════

// true  → тільки сканування (знайди назву навушників)
// false → аудіо-міст (підключитися і стрімити)
#define SCAN_ONLY  false

// Назва навушників (отримай з результатів сканування)
#define TARGET_NAME  "MAJOR IV"

// I2S піни (від STM32F407 Discovery)
#define PIN_BCLK    26   // ← PC10 I2S3_CK
#define PIN_LRCLK   25   // ← PA4  I2S3_WS
#define PIN_DIN     22   // ← PC12 I2S3_SD

// Параметри сканування (тільки для SCAN_ONLY true)
#define BLE_SCAN_SEC      10
#define CLASSIC_SCAN_SEC  10

// Аудіо параметри (тільки для SCAN_ONLY false)
#define SAMPLE_RATE  44100

// ════════════════════════════════════════════════════════════════════════════


// ────────────────────────────────────────────────────────────────────────────
//  РЕЖИМ 1: СКАНУВАННЯ
// ────────────────────────────────────────────────────────────────────────────
#if SCAN_ONLY

#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp32-hal-bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gap_bt_api.h"

volatile bool bleScanDone    = false;
volatile bool classicScanDone = false;
volatile bool bleReady       = false;

enum Phase { PH_IDLE, PH_BLE, PH_CLASSIC, PH_DONE } phase = PH_IDLE;

esp_ble_scan_params_t bleScanParams;

// Вивід MAC-адреси
static void printMac(const uint8_t *a) {
    Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X", a[0],a[1],a[2],a[3],a[4],a[5]);
}

// Витягнути ім'я з BLE advertisement
static void bleName(uint8_t *adv, char *out, size_t sz) {
    out[0] = '\0';
    uint8_t len = 0;
    uint8_t *p = esp_ble_resolve_adv_data(adv, ESP_BLE_AD_TYPE_NAME_CMPL, &len);
    if (!p) p   = esp_ble_resolve_adv_data(adv, ESP_BLE_AD_TYPE_NAME_SHORT, &len);
    if (p && len) { size_t n = min((size_t)len, sz-1); memcpy(out,p,n); out[n]='\0'; }
}

// Витягнути ім'я з Classic BT EIR
static void classicName(uint8_t *eir, char *out, size_t sz) {
    out[0] = '\0';
    uint8_t len = 0;
    uint8_t *p = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
    if (!p) p   = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
    if (p && len) { size_t n = min((size_t)len, sz-1); memcpy(out,p,n); out[n]='\0'; }
}

// BLE callback
void bleGapCb(esp_gap_ble_cb_event_t evt, esp_ble_gap_cb_param_t *p) {
    switch (evt) {
        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
            bleReady = true;
            break;
        case ESP_GAP_BLE_SCAN_RESULT_EVT:
            if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
                char name[64]; bleName(p->scan_rst.ble_adv, name, sizeof(name));
                Serial.print("[BLE    ] ");
                printMac(p->scan_rst.bda);
                Serial.printf("  RSSI=%4d dBm  \"%s\"\n", p->scan_rst.rssi, name);
            }
            if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT)
                bleScanDone = true;
            break;
        default: break;
    }
}

// Classic BT callback
void classicGapCb(esp_bt_gap_cb_event_t evt, esp_bt_gap_cb_param_t *p) {
    if (evt == ESP_BT_GAP_DISC_RES_EVT) {
        char name[128] = ""; int8_t rssi = 0; bool hasRssi = false;
        for (int i = 0; i < p->disc_res.num_prop; i++) {
            auto *prop = &p->disc_res.prop[i];
            switch (prop->type) {
                case ESP_BT_GAP_DEV_PROP_BDNAME: {
                    size_t n = min((size_t)prop->len, sizeof(name)-1);
                    memcpy(name, prop->val, n); name[n] = '\0'; break;
                }
                case ESP_BT_GAP_DEV_PROP_RSSI:
                    rssi = *(int8_t*)prop->val; hasRssi = true; break;
                case ESP_BT_GAP_DEV_PROP_EIR:
                    if (name[0] == '\0') classicName((uint8_t*)prop->val, name, sizeof(name));
                    break;
                default: break;
            }
        }
        Serial.print("[Classic] ");
        printMac(p->disc_res.bda);
        if (hasRssi) Serial.printf("  RSSI=%4d dBm", rssi);
        else         Serial.print ("  RSSI= N/A    ");
        Serial.printf("  \"%s\"\n", name);
    }
    if (evt == ESP_BT_GAP_DISC_STATE_CHANGED_EVT) {
        if (p->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED)
            classicScanDone = true;
    }
}

void setup() {
    Serial.begin(115200); delay(800);
    Serial.println();
    Serial.println("╔══════════════════════════════════════════╗");
    Serial.println("║  ESP32 Bluetooth Scanner                 ║");
    Serial.println("║  Відкрий Serial Monitor 115200 бод       ║");
    Serial.println("╚══════════════════════════════════════════╝");
    Serial.println();

    // NVS ініціалізація
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nvs_flash_init();
    }

    // Bluetooth стек
    if (!btStarted()) btStart();
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED) esp_bluedroid_init();
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED)       esp_bluedroid_enable();

    esp_ble_gap_register_callback(bleGapCb);
    esp_bt_gap_register_callback(classicGapCb);
    esp_bt_dev_set_device_name("ESP32_Scanner");

    // BLE параметри сканування
    bleScanParams.scan_type          = BLE_SCAN_TYPE_ACTIVE;
    bleScanParams.own_addr_type      = BLE_ADDR_TYPE_PUBLIC;
    bleScanParams.scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
    bleScanParams.scan_interval      = 0x50;   // 50 мс
    bleScanParams.scan_window        = 0x30;   // 30 мс
    bleScanParams.scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE;
    esp_ble_gap_set_scan_params(&bleScanParams);
}

void loop() {
    if (phase == PH_IDLE && bleReady) {
        Serial.printf("── BLE скан (%d сек) ───────────────────────\n", BLE_SCAN_SEC);
        phase = PH_BLE;
        esp_ble_gap_start_scanning(BLE_SCAN_SEC);
    }

    if (phase == PH_BLE && bleScanDone) {
        Serial.printf("\n── Classic BT скан (%d сек) ────────────────\n", CLASSIC_SCAN_SEC);
        phase = PH_CLASSIC;
        uint8_t dur = (uint8_t)max(1, (int)(CLASSIC_SCAN_SEC / 1.28f));
        esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, dur, 0);
    }

    if (phase == PH_CLASSIC && classicScanDone) {
        Serial.println();
        Serial.println("══════════════════════════════════════════════");
        Serial.println("Скан завершено!");
        Serial.println("Скопіюй назву навушників, встав у TARGET_NAME,");
        Serial.println("постав SCAN_ONLY false і перепрошивай.");
        Serial.println("══════════════════════════════════════════════");
        phase = PH_DONE;
    }
    delay(20);
}


// ────────────────────────────────────────────────────────────────────────────
//  РЕЖИМ 2: АУДІО-МІСТ  STM32 I2S → Bluetooth A2DP
// ────────────────────────────────────────────────────────────────────────────
#else

#include "BluetoothA2DPSource.h"
#include <driver/i2s.h>

BluetoothA2DPSource a2dp;

// ── I2S ініціалізація (slave, приймаємо від STM32) ──────────────────────────
static void i2s_init() {
    i2s_config_t cfg      = {};
    cfg.mode              = (i2s_mode_t)(I2S_MODE_SLAVE | I2S_MODE_RX);
    cfg.sample_rate       = SAMPLE_RATE;
    cfg.bits_per_sample   = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format    = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags  = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count     = 8;
    cfg.dma_buf_len       = 128;
    cfg.use_apll          = false;
    i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);

    i2s_pin_config_t pins = {};
    pins.mck_io_num       = I2S_PIN_NO_CHANGE;
    pins.bck_io_num       = PIN_BCLK;
    pins.ws_io_num        = PIN_LRCLK;
    pins.data_out_num     = I2S_PIN_NO_CHANGE;
    pins.data_in_num      = PIN_DIN;
    i2s_set_pin(I2S_NUM_0, &pins);
}

// ── Audio callback: читаємо I2S → віддаємо в Bluetooth стек ─────────────────
static int32_t audio_cb(Frame *frames, int32_t count) {
    size_t bytes = 0;
    i2s_read(I2S_NUM_0,
             (void*)frames,
             (size_t)(count * sizeof(Frame)),
             &bytes,
             portMAX_DELAY);
    return (int32_t)(bytes / sizeof(Frame));
}

void setup() {
    Serial.begin(115200); delay(500);
    Serial.println();
    Serial.println("╔══════════════════════════════════════════╗");
    Serial.println("║  ESP32 BT Audio Bridge — Kamerton        ║");
    Serial.println("╚══════════════════════════════════════════╝");
    Serial.printf ("  Ціль    : \"%s\"\n",  TARGET_NAME);
    Serial.printf ("  I2S піни: BCLK=%d  LRCLK=%d  DIN=%d\n", PIN_BCLK, PIN_LRCLK, PIN_DIN);
    Serial.printf ("  Частота : %d Гц\n\n", SAMPLE_RATE);

    // I2S має бути готовий до старту BT, щоб буфери не переповнились
    i2s_init();
    Serial.println("[I2S] Slave RX: OK");
    Serial.println("[I2S] Очікую тактовий сигнал від STM32...");

    // A2DP Source
    a2dp.set_data_callback_in_frames(audio_cb);
    a2dp.set_auto_reconnect(true);   // автоматичне перепідключення
    a2dp.start(TARGET_NAME);

    Serial.printf("[BT]  Пошук \"%s\"...\n", TARGET_NAME);
    Serial.println("[BT]  Увімкни навушники (режим парування при першому запуску)");
}

void loop() {
    static bool prev = false;
    bool cur = a2dp.is_connected();

    if (cur && !prev) {
        Serial.println();
        Serial.println("[BT]  ✓ ПІДКЛЮЧЕНО!");
        Serial.println("[BT]  Потік: STM32 I2S → ESP32 → Bluetooth A2DP → Навушники");
        Serial.println();
    } else if (!cur && prev) {
        Serial.println("[BT]  ✗ З'єднання втрачено — перепідключення...");
    }

    prev = cur;
    delay(1000);
}

#endif  // SCAN_ONLY
