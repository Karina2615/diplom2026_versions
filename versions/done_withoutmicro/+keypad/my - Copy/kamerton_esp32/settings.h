#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "config.h"

struct Settings {
    AppMode   last_mode;
    uint8_t   volume;        // 0-100
    uint8_t   octave;        // 0-4
    uint8_t   timbre;        // 0-4
    uint8_t   warmup_seq;    // 0-6
    uint8_t   arpeggio_idx;  // 0-6
    uint8_t   rec_slot;      // 1-MAX_REC_SLOTS (last used recorder slot)
    char      bt_device[64]; // BT headphone name (empty = scan)
};

class SettingsMgr {
public:
    void begin();
    void load();
    void save();
    void saveImmediate(const Settings& s);

    Settings cfg;

private:
    Preferences _prefs;
};

extern SettingsMgr g_settings;
