/*  ESP32 — Bluetooth Audio Bridge для Kamerton
 *  ══════════════════════════════════════════════════════════════
 *  Отримує аудіо від STM32F407 через I2S (slave)
 *  Передає на BT навушники через A2DP (Bluetooth Classic)
 *
 *  Бібліотека: "ESP32-A2DP" by pschatzmann
 *
 *  Підключення до STM32F407 Discovery:
 *    ESP32 GPIO 26  ←──  STM32 PC10  (I2S3_SCK  / BCLK)
 *    ESP32 GPIO 25  ←──  STM32 PA4   (I2S3_WS   / LRCLK)
 *    ESP32 GPIO 22  ←──  STM32 PC12  (I2S3_SD   / DATA)
 *    ESP32 GND      ←──  STM32 GND   (обов'язково!)
 *
 *  Перше підключення навушників:
 *    1. Постав навушники в режим парування
 *    2. Увімкни ESP32 — він автоматично знайде і підключиться
 *    3. Наступного разу — автопідключення без дій
 *  ══════════════════════════════════════════════════════════════
 */

#include "BluetoothA2DPSource.h"
#include <driver/i2s.h>

// ─── Піни I2S ─────────────────────────────────────────────────────────────────
#define PIN_BCLK   26   // ← PC10 STM32
#define PIN_LRCLK  25   // ← PA4  STM32
#define PIN_DIN    22   // ← PC12 STM32

// ─── Налаштування ─────────────────────────────────────────────────────────────
#define SAMPLE_RATE   44100
#define DMA_BUF_COUNT 8
#define DMA_BUF_LEN   64

BluetoothA2DPSource a2dp;

// ─── Ініціалізація I2S як slave-приймача ──────────────────────────────────────
void setupI2S() {
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_SLAVE | I2S_MODE_RX),
        .sample_rate          = SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = DMA_BUF_COUNT,
        .dma_buf_len          = DMA_BUF_LEN,
        .use_apll             = false,
        .tx_desc_auto_clear   = false,
        .fixed_mclk           = 0,
    };
    i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);

    i2s_pin_config_t pins = {
        .mck_io_num   = I2S_PIN_NO_CHANGE,
        .bck_io_num   = PIN_BCLK,
        .ws_io_num    = PIN_LRCLK,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = PIN_DIN,
    };
    i2s_set_pin(I2S_NUM_0, &pins);
}

// ─── Аудіо-колбек: читає I2S від STM32, віддає в BT стек ─────────────────────
int32_t audioCallback(Frame* frame, int32_t frameCount) {
    size_t bytesRead = 0;
    esp_err_t err = i2s_read(
        I2S_NUM_0,
        (void*)frame,
        frameCount * sizeof(Frame),
        &bytesRead,
        portMAX_DELAY          // чекати поки прийдуть дані
    );
    if (err != ESP_OK) return 0;
    return (int32_t)(bytesRead / sizeof(Frame));
}

// ─── setup() ──────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("=== ESP32 BT Audio Bridge ===");

    setupI2S();
    Serial.println("I2S slave: OK");

    // Автопідключення до останніх спарених навушників.
    // Перший раз: постав навушники в режим парування — ESP32 знайде сам.
    a2dp.set_auto_reconnect(true);
    a2dp.start(audioCallback);   // scan and connect to any A2DP sink

    Serial.println("Bluetooth: пошук навушників...");
}

// ─── loop() ───────────────────────────────────────────────────────────────────
void loop() {
    delay(2000);
    if (a2dp.is_connected()) {
        Serial.println("BT: ✓ підключено");
    } else {
        Serial.println("BT: пошук...");
    }
}
