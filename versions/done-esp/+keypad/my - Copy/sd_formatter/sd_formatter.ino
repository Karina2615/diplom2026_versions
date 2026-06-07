/*  SD Card Formatter для ESP32
 *  ────────────────────────────────────────────────────────────────
 *  Форматує SD-карту будь-якого розміру (включно з 64 ГБ).
 *  Карти до 32 ГБ → FAT32
 *  Карти більше 32 ГБ → exFAT
 *
 *  Бібліотека: "SdFat" by Bill Greiman (встанови через Library Manager)
 *
 *  Підключення SD-модуля:
 *    CS   → пін 5
 *    MOSI → пін 23
 *    MISO → пін 19
 *    SCK  → пін 18
 *    VCC  → 3.3V або 5V
 *    GND  → GND
 *
 *  Як користуватись:
 *    1. Завантаж цей скетч на ESP32
 *    2. Відкрий Serial Monitor (швидкість 115200)
 *    3. Чекай — процес займає 1-5 хвилин
 *    4. Після "ГОТОВО" завантаж основний проєкт kamerton_esp32
 *  ────────────────────────────────────────────────────────────────
 */

#include "SdFat.h"

#define PIN_SD_CS   5
#define PIN_SD_SCK  18
#define PIN_SD_MOSI 23
#define PIN_SD_MISO 19

SdCardFactory cardFactory;
SdCard*       card = nullptr;
FatFormatter      fatFormatter;
ExFatFormatter    exFatFormatter;
uint8_t sectorBuf[512];

void setup() {
    Serial.begin(115200);
    delay(2000);

    Serial.println("╔══════════════════════════════╗");
    Serial.println("║    SD Card Formatter v1.0    ║");
    Serial.println("╚══════════════════════════════╝");
    Serial.println();

    // Ініціалізація картки
    SdSpiConfig cfg(PIN_SD_CS, DEDICATED_SPI, SD_SCK_MHZ(4));
    card = cardFactory.newCard(cfg);

    if (!card || card->errorCode()) {
        Serial.println("✗ ПОМИЛКА: SD-карта не знайдена!");
        Serial.println("  Перевір підключення і перезапусти.");
        return;
    }

    // Визначити розмір
    uint32_t sectors = card->sectorCount();
    float    gb      = (float)sectors * 512.0f / 1e9f;

    Serial.print("✓ Карта знайдена: ");
    Serial.print(gb, 1);
    Serial.println(" ГБ");
    Serial.println();

    bool ok;

    if (sectors > 67108864UL) {
        // Більше 32 ГБ → exFAT (підтримується бібліотекою SdFat у головному проєкті)
        Serial.println("Розмір > 32 ГБ → форматування як exFAT...");
        Serial.println("(орієнтовно 2-5 хвилин, не вимикай живлення!)");
        Serial.println();
        ok = exFatFormatter.format(card, sectorBuf, &Serial);
    } else {
        // До 32 ГБ → FAT32
        Serial.println("Форматування як FAT32...");
        Serial.println("(орієнтовно 1-3 хвилини, не вимикай живлення!)");
        Serial.println();
        ok = fatFormatter.format(card, sectorBuf, &Serial);
    }

    Serial.println();
    if (ok) {
        Serial.println("╔══════════════════════════════╗");
        Serial.println("║   ✓ ГОТОВО! Карта готова.   ║");
        Serial.println("╠══════════════════════════════╣");
        Serial.println("║  Тепер завантаж kamerton_    ║");
        Serial.println("║  esp32 / kamerton_esp32.ino  ║");
        Serial.println("╚══════════════════════════════╝");
    } else {
        Serial.println("╔══════════════════════════════╗");
        Serial.println("║   ✗ ПОМИЛКА форматування!   ║");
        Serial.println("║  Спробуй іншу карту або      ║");
        Serial.println("║  перевір підключення.        ║");
        Serial.println("╚══════════════════════════════╝");
    }
}

void loop() {
    // нічого не робить — результат в Serial Monitor
}
