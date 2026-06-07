#pragma once
#include <Arduino.h>
#include <LiquidCrystal_I2C.h>
#include "config.h"
#include "sampler.h"
#include "sd_recorder.h"   // для RecMeta

// 20×4 LCD display manager.
// Renders a different screen for each app mode.
// Call update() every 100–200 ms from the main loop.
class DisplayMgr {
public:
    // LiquidCrystal_I2C has no default constructor, so _lcd must be built here
    // with the I2C address + geometry (otherwise DisplayMgr is not constructible).
    DisplayMgr() : _lcd(LCD_I2C_ADDR, 20, 4) {}

    void begin();

    // Draw the appropriate screen for the given mode
    void update(AppMode mode);

    // Specialised screens
    void showGenerator(uint8_t note, uint8_t octave, uint8_t timbre,
                       uint8_t volume, bool muted, bool bt_ok);
    void showTuner(float freq_hz, float cents, const char* hint);
    void showArpeggio(uint8_t arp_idx, uint8_t step, bool playing,
                      int8_t transpose);
    void showWarmup(uint8_t seq_idx, uint8_t step, bool playing,
                    uint8_t note_idx, uint8_t octave);
    void showRecorder(bool recording, bool playing, uint32_t elapsed_ms,
                      const RecMeta* list, int list_count, int cursor);
    void showSampler(const Sampler& s);
    void showBoot();
    void showMessage(const char* line1, const char* line2 = nullptr);

private:
    LiquidCrystal_I2C _lcd;
    uint8_t _anim = 0;   // frame counter for animated elements

    void printPadded(const char* s, int width);
    void drawVolumeBar(uint8_t volume, int col, int row, int width);
    const char* centsArrow(float cents);
    void printCentered(const char* s, int row, int width = 20);
};

extern DisplayMgr g_display;
