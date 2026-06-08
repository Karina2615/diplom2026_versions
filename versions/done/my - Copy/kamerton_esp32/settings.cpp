#include "settings.h"

SettingsMgr g_settings;

void SettingsMgr::begin() {
    _prefs.begin("kamerton", false);
    load();
}

void SettingsMgr::load() {
    cfg.last_mode   = (AppMode)_prefs.getUChar("mode",    MODE_GENERATOR);
    cfg.volume      = _prefs.getUChar("vol",     70);
    cfg.octave      = _prefs.getUChar("octave",   3);
    cfg.timbre      = _prefs.getUChar("timbre",   0);
    cfg.warmup_seq  = _prefs.getUChar("warmup",   0);
    cfg.arpeggio_idx= _prefs.getUChar("arp",      0);
    cfg.rec_slot    = _prefs.getUChar("recslot",  1);
    _prefs.getString("btdev", cfg.bt_device, sizeof(cfg.bt_device));
}

void SettingsMgr::save() {
    _prefs.putUChar("mode",    (uint8_t)cfg.last_mode);
    _prefs.putUChar("vol",     cfg.volume);
    _prefs.putUChar("octave",  cfg.octave);
    _prefs.putUChar("timbre",  cfg.timbre);
    _prefs.putUChar("warmup",  cfg.warmup_seq);
    _prefs.putUChar("arp",     cfg.arpeggio_idx);
    _prefs.putUChar("recslot", cfg.rec_slot);
    _prefs.putString("btdev",  cfg.bt_device);
}

void SettingsMgr::saveImmediate(const Settings& s) {
    cfg = s;
    save();
}
