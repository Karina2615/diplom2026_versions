#include "display_mgr.h"
#include <stdio.h>
#include <string.h>

DisplayMgr g_display;

// ─── Init ─────────────────────────────────────────────────────────────────────

void DisplayMgr::begin() {
    // _lcd is already constructed (see DisplayMgr ctor) — just initialise it.
    _lcd.init();
    _lcd.backlight();
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

void DisplayMgr::printPadded(const char* s, int width) {
    int len = strlen(s);
    _lcd.print(s);
    for (int i = len; i < width; i++) _lcd.print(' ');
}

void DisplayMgr::printCentered(const char* s, int row, int width) {
    int len = strlen(s);
    int pad = (width - len) / 2;
    _lcd.setCursor(0, row);
    for (int i = 0; i < pad; i++) _lcd.print(' ');
    _lcd.print(s);
    for (int i = pad + len; i < width; i++) _lcd.print(' ');
}

void DisplayMgr::drawVolumeBar(uint8_t volume, int col, int row, int width) {
    int filled = (volume * width) / 100;
    _lcd.setCursor(col, row);
    for (int i = 0; i < width; i++)
        _lcd.print((i < filled) ? '#' : '-');
}

const char* DisplayMgr::centsArrow(float cents) {
    if      (cents >  5)  return ">>";
    else if (cents < -5)  return "<<";
    else                  return "OK";
}

// ─── Mode screens ─────────────────────────────────────────────────────────────

void DisplayMgr::showBoot() {
    _lcd.clear();
    printCentered("=== KAMERTON ===", 0);
    printCentered("ESP32 v2.0", 1);
    printCentered("Initialising...", 2);
}

void DisplayMgr::showMessage(const char* l1, const char* l2) {
    _lcd.clear();
    if (l1) printCentered(l1, 1);
    if (l2) printCentered(l2, 2);
}

void DisplayMgr::showGenerator(uint8_t note, uint8_t octave, uint8_t timbre,
                                 uint8_t volume, bool muted, bool bt_ok) {
    char buf[24];
    _lcd.clear();

    // Row 0: mode name + BT indicator
    _lcd.setCursor(0, 0);
    snprintf(buf, sizeof(buf), "%-9s %s %s",
             "Generator",
             bt_ok ? "BT" : "  ",
             muted ? "[MUTE]" : "      ");
    _lcd.print(buf);

    // Row 1: Note name + octave + frequency
    float freq = NOTE_FREQS_OCT4[note % NUM_NOTES] * powf(2.0f, (float)octave - 4.0f);
    snprintf(buf, sizeof(buf), "%s%d  %6.1f Hz      ",
             NOTE_NAMES[note % NUM_NOTES], octave, freq);
    _lcd.setCursor(0, 1);
    printPadded(buf, 20);

    // Row 2: Timbre name
    snprintf(buf, sizeof(buf), "Timbre: %-12s", TIMBRE_NAMES[timbre % NUM_TIMBRES]);
    _lcd.setCursor(0, 2);
    _lcd.print(buf);

    // Row 3: Volume bar
    _lcd.setCursor(0, 3);
    snprintf(buf, sizeof(buf), "Vol %3d%% ", volume);
    _lcd.print(buf);
    drawVolumeBar(volume, 9, 3, 11);
}

void DisplayMgr::showTuner(float freq_hz, float cents, const char* hint) {
    char buf[24];
    _lcd.clear();

    printCentered("== GUITAR TUNER ==", 0);

    // Row 1: detected frequency
    snprintf(buf, sizeof(buf), "  %.1f Hz", freq_hz);
    _lcd.setCursor(0, 1);
    printPadded(buf, 20);

    // Row 2: cents bar  -----[||]------
    _lcd.setCursor(0, 2);
    int pos = 9 + (int)(cents / 10.0f);   // 0-18 range for ±100 cents
    if (pos < 0) pos = 0;
    if (pos > 18) pos = 18;
    for (int i = 0; i < 20; i++) {
        if (i == 9)        _lcd.print('|');
        else if (i == pos) _lcd.print('^');
        else               _lcd.print('-');
    }

    // Row 3: textual hint
    printCentered(hint ? hint : "", 3);
}

void DisplayMgr::showArpeggio(uint8_t arp_idx, uint8_t step,
                               bool playing, int8_t transpose) {
    char buf[24];
    _lcd.clear();

    // Row 0
    snprintf(buf, sizeof(buf), "%-14s %s", "Arpeggio",
             playing ? "[PLAY]" : "      ");
    _lcd.setCursor(0, 0); printPadded(buf, 20);

    // Row 1: chord name
    printCentered(ARPEGGIO_NAMES[arp_idx % NUM_ARPEGGIOS], 1);

    // Row 2: step + transpose
    snprintf(buf, sizeof(buf), "Step %d/3  Tr: %+d  ", step + 1, transpose);
    _lcd.setCursor(0, 2); printPadded(buf, 20);

    // Row 3: hints
    _lcd.setCursor(0, 3);
    _lcd.print("1-7:chrd 8/9:trns  ");
}

void DisplayMgr::showWarmup(uint8_t seq_idx, uint8_t step,
                              bool playing, uint8_t note_idx, uint8_t octave) {
    char buf[24];
    _lcd.clear();

    snprintf(buf, sizeof(buf), "%-14s %s", "Warmup",
             playing ? "[PLAY]" : "      ");
    _lcd.setCursor(0, 0); printPadded(buf, 20);

    printCentered(WARMUP_NAMES[seq_idx % NUM_WARMUP_SEQS], 1);

    if (playing) {
        snprintf(buf, sizeof(buf), "%s%d  step %d",
                 NOTE_NAMES[note_idx % NUM_NOTES], octave, step + 1);
    } else {
        snprintf(buf, sizeof(buf), "Select: 1-7");
    }
    _lcd.setCursor(0, 2); printPadded(buf, 20);

    _lcd.setCursor(0, 3);
    _lcd.print("10:play 11:stop     ");
}

void DisplayMgr::showRecorder(bool recording, bool playing,
                               uint32_t elapsed_ms,
                               const RecMeta* list, int list_count, int cursor) {
    char buf[24];
    _lcd.clear();

    // Row 0: header + status
    const char* status = recording ? "[REC]" : (playing ? "[PLAY]" : "[IDLE]");
    snprintf(buf, sizeof(buf), "Recorder  %s       ", status);
    _lcd.setCursor(0, 0); printPadded(buf, 20);

    // Row 1: time counter when recording
    if (recording) {
        uint32_t sec = elapsed_ms / 1000;
        snprintf(buf, sizeof(buf), "  %lu / %d s", sec, MAX_REC_SECONDS);
        _lcd.setCursor(0, 1); printPadded(buf, 20);
    } else if (list_count == 0) {
        _lcd.setCursor(0, 1);
        _lcd.print("  No recordings     ");
    } else {
        // Show up to 2 entries around cursor
        for (int row = 0; row < 2 && (cursor + row) < list_count; row++) {
            int idx = cursor + row;
            bool sel = (row == 0);
            uint32_t dur_s = list[idx].num_samples / REC_SAMPLE_RATE;
            snprintf(buf, sizeof(buf), "%c %-12s %lus",
                     sel ? '>' : ' ', list[idx].filename, dur_s);
            _lcd.setCursor(0, 1 + row);
            printPadded(buf, 20);
        }
    }

    // Row 3: hints
    _lcd.setCursor(0, 3);
    _lcd.print("1:rec 2:play 3:stop ");
}

void DisplayMgr::showSampler(const Sampler& s) {
    _lcd.clear();

    // Row 0: header + rec indicator
    char buf[24];
    snprintf(buf, sizeof(buf), "Sampler  %s           ",
             s.isRecording() ? "[REC]" : "      ");
    _lcd.setCursor(0, 0); printPadded(buf, 20);

    // Rows 1-2: pad grid 5x3 minimap (pads 1-15)
    // Show 10 pads (5 per row) centred around cursor
    _lcd.setCursor(0, 1);
    for (int i = 0; i < 15; i++) {
        char c;
        if      (i == s.cursor())                     c = (s.pad(i).has_sample ? '*' : 'o');
        else if (s.playingPad() == (uint8_t)(i + 1)) c = '>';
        else                                          c = (s.pad(i).has_sample ? '#' : '.');
        _lcd.print(c);
        if (i == 4 || i == 9) {
            for (int sp = 5; sp < 20; sp++) _lcd.print(' ');
            if (i == 4) _lcd.setCursor(0, 2);
        }
    }

    // Row 3: selected pad info
    const SamplerPad& cp = s.pad(s.cursor());
    snprintf(buf, sizeof(buf), "Pad%2d %s Pt:%+d     ",
             s.cursor() + 1,
             cp.has_sample ? "OK" : "--",
             cp.pitch_shift);
    _lcd.setCursor(0, 3); printPadded(buf, 20);
}

void DisplayMgr::update(AppMode /*mode*/) {
    // Animated spinner tick used by individual screens
    _anim = (_anim + 1) & 0x0F;
}
