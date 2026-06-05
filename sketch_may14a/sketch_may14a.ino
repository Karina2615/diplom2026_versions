// SdFormatter — завантаж, запусти Serial Monitor (115200), потім заміни на основний проєкт
#include "SdFat.h"

#define SD_CS   5
#define SD_SCK  18
#define SD_MOSI 23
#define SD_MISO 19

SdCardFactory cardFactory;
SdCard* card = nullptr;
FatFormatter fatFormatter;
ExFatFormatter exFatFormatter;
uint8_t buf[512];

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("=== Форматування SD ===");

  SdSpiConfig cfg(SD_CS, DEDICATED_SPI, SD_SCK_MHZ(4));
  card = cardFactory.newCard(cfg);

  if (!card || card->errorCode()) {
    Serial.println("ПОМИЛКА: SD не знайдено!");
    return;
  }

  uint32_t sectors = card->sectorCount();
  float gb = sectors / 2.0 / 1024.0 / 1024.0;
  Serial.print("Карта: "); Serial.print(gb, 1); Serial.println(" ГБ");
  Serial.println("Починаю... (1-3 хвилини)");

  bool ok;
  if (sectors > 0xFFFFFFUL) {
    // 64 ГБ — форматуємо як exFAT
    Serial.println("Формат: exFAT");
    ok = exFatFormatter.format(card, buf, &Serial);
  } else {
    Serial.println("Формат: FAT32");
    ok = fatFormatter.format(card, buf, &Serial);
  }

  if (ok) Serial.println("\n>>> ГОТОВО! Карта відформатована. <<<");
  else    Serial.println("\n>>> ПОМИЛКА форматування! <<<");
}

void loop() {}