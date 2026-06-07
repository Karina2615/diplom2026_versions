#include "sd_recorder.h"

// Глобальні об'єкти
SdFs      g_sdfs;
SDRecorder g_sdrec;

// ─── WAV-заголовок ────────────────────────────────────────────────────────────

void SDRecorder::writeWavHeader(FsFile& f, uint32_t sample_rate, uint16_t channels) {
    uint16_t bits        = 16;
    uint32_t byte_rate   = sample_rate * channels * (bits / 8);
    uint16_t block_align = channels * (bits / 8);

    f.write("RIFF", 4);
    uint32_t placeholder = 0xFFFFFFFF;
    f.write((uint8_t*)&placeholder,  4);   // розмір чанку (оновиться в кінці)
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    uint32_t sub1 = 16;  f.write((uint8_t*)&sub1,        4);
    uint16_t fmt  = 1;   f.write((uint8_t*)&fmt,          2);  // PCM
    f.write((uint8_t*)&channels,    2);
    f.write((uint8_t*)&sample_rate, 4);
    f.write((uint8_t*)&byte_rate,   4);
    f.write((uint8_t*)&block_align, 2);
    f.write((uint8_t*)&bits,        2);
    f.write("data", 4);
    f.write((uint8_t*)&placeholder, 4);   // розмір даних (оновиться в кінці)
}

void SDRecorder::finalizeWavHeader(FsFile& f, uint32_t num_samples,
                                    uint32_t sample_rate, uint16_t channels) {
    uint32_t data_bytes = num_samples * channels * 2;
    uint32_t chunk_size = 36 + data_bytes;
    f.seek(4);  f.write((uint8_t*)&chunk_size, 4);
    f.seek(40); f.write((uint8_t*)&data_bytes, 4);
}

// ─── Ініціалізація ────────────────────────────────────────────────────────────

bool SDRecorder::begin(uint8_t cs_pin) {
    _cs_pin = cs_pin;
    // SdSpiConfig: CS-пін, виділений SPI, швидкість 4 МГц
    _sd_ok = g_sdfs.begin(SdSpiConfig(cs_pin, DEDICATED_SPI, SD_SCK_MHZ(4)));
    return _sd_ok;
}

// ─── Запис ────────────────────────────────────────────────────────────────────

bool SDRecorder::startRec(const char* filename) {
    if (!_sd_ok || _recording) return false;

    // Відкрити файл: створити або перезаписати
    _rec_file = g_sdfs.open(filename, O_RDWR | O_CREAT | O_TRUNC);
    if (!_rec_file) return false;

    writeWavHeader(_rec_file, REC_SAMPLE_RATE);
    _rec_samples  = 0;
    _head = _tail = 0;
    _rec_start_ms = millis();
    _recording    = true;
    return true;
}

void SDRecorder::stopRec() {
    if (!_recording) return;
    _recording = false;

    // Дозаписати залишок буфера
    uint32_t avail = _head - _tail;
    while (avail > 0) {
        uint32_t to_write = (avail < (uint32_t)HALF) ? avail : (uint32_t)HALF;
        uint32_t base = _tail & (uint32_t)(HALF * 2 - 1);
        _rec_file.write((const uint8_t*)&_buf[base], to_write * 2);
        _rec_samples += to_write;
        _tail += to_write;
        avail -= to_write;
    }

    finalizeWavHeader(_rec_file, _rec_samples, REC_SAMPLE_RATE);
    _rec_file.close();
}

uint32_t SDRecorder::recElapsedMs() const {
    if (!_recording) return 0;
    return millis() - _rec_start_ms;
}

// ISR — викликається таймером кожні 62.5 мкс (16 кГц)
void IRAM_ATTR SDRecorder::onTimerISR() {
    if (!_recording) return;
    if (_rec_samples >= (uint32_t)(MAX_REC_SECONDS * REC_SAMPLE_RATE)) {
        _recording = false;
        return;
    }
    int raw = analogRead(PIN_MIC_OUT);
    int16_t sample = (int16_t)((raw - ADC_CENTER) * ADC_TO_PCM_SCALE);
    _buf[_head & (uint32_t)(HALF * 2 - 1)] = sample;
    _head++;
}

// Скидати буфер на SD — викликати в loop()
void SDRecorder::flushToSD() {
    if (!_recording) return;
    uint32_t avail = _head - _tail;
    while (avail >= (uint32_t)HALF) {
        uint32_t base = _tail & (uint32_t)(HALF * 2 - 1);
        _rec_file.write((const uint8_t*)&_buf[base], HALF * 2);
        _rec_samples += HALF;
        _tail += HALF;
        avail -= HALF;
    }
}

// ─── Відтворення (завантаження WAV у RAM) ─────────────────────────────────────

bool SDRecorder::loadWav(const char* filename, int16_t* play_buf,
                          uint32_t buf_size, uint32_t& out_samples, uint32_t& out_rate) {
    FsFile f = g_sdfs.open(filename, O_RDONLY);
    if (!f) return false;

    uint8_t hdr[44];
    if (f.read(hdr, 44) != 44)                        { f.close(); return false; }
    if (memcmp(hdr, "RIFF", 4) || memcmp(hdr+8, "WAVE", 4)) { f.close(); return false; }

    uint32_t sr, data_bytes;
    memcpy(&sr,         hdr + 24, 4);
    memcpy(&data_bytes, hdr + 40, 4);

    out_rate    = sr;
    uint32_t ns = data_bytes / 2;          // 16-біт моно
    if (ns > buf_size) ns = buf_size;

    f.read((uint8_t*)play_buf, ns * 2);
    f.close();
    out_samples = ns;
    return true;
}

// ─── Файловий менеджмент ──────────────────────────────────────────────────────

int SDRecorder::scanFiles(const char* prefix, RecMeta* list, int max_count) {
    if (!_sd_ok) return 0;

    FsFile root;
    if (!root.open("/")) return 0;

    int count = 0;
    FsFile entry;
    char name_buf[32];

    while (entry.openNext(&root, O_RDONLY) && count < max_count) {
        if (!entry.isDir()) {
            entry.getName(name_buf, sizeof(name_buf));
            if (strncasecmp(name_buf, prefix, strlen(prefix)) == 0) {
                strncpy(list[count].filename, name_buf, 15);
                list[count].filename[15] = '\0';
                uint32_t sz = entry.fileSize();
                list[count].num_samples = (sz > 44) ? (sz - 44) / 2 : 0;
                list[count].valid       = true;
                count++;
            }
        }
        entry.close();
    }
    root.close();
    return count;
}

bool SDRecorder::fileExists(const char* filename) {
    return g_sdfs.exists(filename);
}

bool SDRecorder::deleteFile(const char* filename) {
    return g_sdfs.remove(filename);
}

int SDRecorder::nextSlotNumber(const char* prefix) {
    for (int n = 1; n <= 99; n++) {
        char name[16];
        snprintf(name, sizeof(name), "%s%03d.WAV", prefix, n);
        if (!fileExists(name)) return n;
    }
    return -1;
}
