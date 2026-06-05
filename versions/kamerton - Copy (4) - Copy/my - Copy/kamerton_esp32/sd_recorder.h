#pragma once
#include <Arduino.h>
#include <SdFat.h>       // бібліотека: "SdFat" by Bill Greiman
#include "config.h"

// Глобальний об'єкт SdFat — використовується і тут, і в sampler.cpp
extern SdFs g_sdfs;

struct RecMeta {
    char     filename[16];   // "REC_003.WAV"
    uint32_t num_samples;    // кількість семплів (при REC_SAMPLE_RATE)
    bool     valid;
};

class SDRecorder {
public:
    bool begin(uint8_t cs_pin);
    bool sdOk() const { return _sd_ok; }

    // ── Запис ────────────────────────────────────────────────────────────────
    bool startRec(const char* filename);
    void stopRec();
    bool isRecording() const { return _recording; }
    uint32_t recElapsedMs() const;

    // Викликати з апаратного таймера ISR на частоті REC_SAMPLE_RATE
    void IRAM_ATTR onTimerISR();

    // Викликати в кожній ітерації loop() під час запису
    void flushToSD();

    // ── Відтворення ──────────────────────────────────────────────────────────
    bool loadWav(const char* filename, int16_t* play_buf,
                 uint32_t buf_size, uint32_t& out_samples, uint32_t& out_rate);

    // ── Файлові операції ─────────────────────────────────────────────────────
    int  scanFiles(const char* prefix, RecMeta* list, int max_count);
    bool deleteFile(const char* filename);
    bool fileExists(const char* filename);
    int  nextSlotNumber(const char* prefix);

    // WAV-заголовок (використовується також у sampler.cpp)
    static void writeWavHeader(FsFile& f, uint32_t sample_rate, uint16_t channels = 1);
    static void finalizeWavHeader(FsFile& f, uint32_t num_samples,
                                  uint32_t sample_rate, uint16_t channels = 1);

private:
    bool     _sd_ok      = false;
    bool     _recording  = false;
    uint32_t _rec_start_ms = 0;
    uint32_t _rec_samples  = 0;
    FsFile   _rec_file;

    static const int HALF = 2048;        // 128 мс при 16 кГц
    volatile int16_t  _buf[HALF * 2];
    volatile uint32_t _head = 0;         // пишеться ISR
    uint32_t          _tail = 0;         // читається main

    uint8_t _cs_pin = 0;
};

extern SDRecorder g_sdrec;
