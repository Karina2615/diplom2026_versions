#pragma once
#include <Arduino.h>
#include "config.h"

// PO-33 K.O! style sampler.
// 15 sample pads (buttons 1-15).  Button 16 = RECORD modifier.
//   • Hold btn 16 + press pad N  → start recording to pad N
//   • Release btn 16             → stop recording, save to SD
//   • Short press pad N alone    → play sample N
// Joystick UP/DOWN: scroll pad list on display.
// Joystick LEFT/RIGHT: adjust playback pitch (±1 semitone) of selected pad.
// Joystick BTN: select pad for detail view / reset pitch.

struct SamplerPad {
    bool     has_sample;
    char     filename[16];  // "SMP_001.WAV"
    int8_t   pitch_shift;   // semitones, -7..+7; changes playback speed
};

class Sampler {
public:
    void begin();

    // Called every loop()
    void update(bool pad_pressed[MAX_SAMPLE_PADS], bool mod_held);

    // Joystick events
    void onUp();
    void onDown();
    void onLeft();
    void onRight();
    void onBtnClick();

    // Returns true while a pad is recording
    bool isRecording() const { return _rec_active; }
    uint8_t recPad()   const { return _rec_pad; }

    // Returns currently-playing pad (0 = none)
    uint8_t playingPad() const { return _play_pad; }

    // Returns highlighted pad for display
    uint8_t cursor() const { return _cursor; }

    const SamplerPad& pad(uint8_t n) const { return _pads[n]; }

    void stopAll();

private:
    SamplerPad _pads[MAX_SAMPLE_PADS];
    uint8_t    _cursor    = 0;   // currently highlighted pad (0-14)
    uint8_t    _rec_pad   = 0;   // which pad is being recorded
    bool       _rec_active = false;
    uint8_t    _play_pad  = 0;
    bool       _prev_pad[MAX_SAMPLE_PADS] = {};
    bool       _mod_prev  = false;

    // Large static RAM buffers for sample storage (shared, one-at-a-time load)
    static int16_t _play_buf[MAX_SMP_SAMPLES];
    uint32_t       _play_samples = 0;
    uint32_t       _play_rate    = REC_SAMPLE_RATE;

    void startRecPad(uint8_t pad_idx);
    void stopRecPad();
    void triggerPad(uint8_t pad_idx);
    float pitchRatio(int8_t semitones);

    void loadSettings();
    void saveSettings();
};

extern Sampler g_sampler;
