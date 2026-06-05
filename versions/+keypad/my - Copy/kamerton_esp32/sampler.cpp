#include "sampler.h"
#include "sd_recorder.h"
#include "audio_synth.h"

Sampler  g_sampler;
int16_t  Sampler::_play_buf[MAX_SMP_SAMPLES];

// ─── Ініціалізація ────────────────────────────────────────────────────────────

void Sampler::begin() {
    memset(_pads, 0, sizeof(_pads));
    for (int i = 0; i < MAX_SAMPLE_PADS; i++) {
        snprintf(_pads[i].filename, 16, "SMP_%03d.WAV", i + 1);
        _pads[i].has_sample  = g_sdrec.fileExists(_pads[i].filename);
        _pads[i].pitch_shift = 0;
    }
    loadSettings();
}

// ─── Головний update ──────────────────────────────────────────────────────────

void Sampler::update(bool pad_pressed[MAX_SAMPLE_PADS], bool mod_held) {
    bool mod_falling = !mod_held && _mod_prev;
    _mod_prev = mod_held;

    // Зупинити запис коли modifier відпустили
    if (mod_falling && _rec_active) {
        stopRecPad();
    }

    for (int i = 0; i < MAX_SAMPLE_PADS; i++) {
        bool cur = pad_pressed[i];
        bool was = _prev_pad[i];

        if (cur && !was) {
            if (mod_held) startRecPad(i);
            else          triggerPad(i);
        }
        _prev_pad[i] = cur;
    }
}

// ─── Джойстик ─────────────────────────────────────────────────────────────────

void Sampler::onUp()    { if (_cursor > 0)                        _cursor--; }
void Sampler::onDown()  { if (_cursor < MAX_SAMPLE_PADS - 1)      _cursor++; }
void Sampler::onLeft()  { if (_pads[_cursor].pitch_shift > -7) { _pads[_cursor].pitch_shift--; saveSettings(); } }
void Sampler::onRight() { if (_pads[_cursor].pitch_shift <  7) { _pads[_cursor].pitch_shift++; saveSettings(); } }
void Sampler::onBtnClick() { _pads[_cursor].pitch_shift = 0; saveSettings(); }

// ─── Запис ────────────────────────────────────────────────────────────────────

void Sampler::startRecPad(uint8_t idx) {
    if (_rec_active || idx >= MAX_SAMPLE_PADS) return;
    g_synth.stopPlayback();
    _play_pad   = 0;
    _rec_pad    = idx;
    _rec_active = true;
    g_sdrec.startRec(_pads[idx].filename);
}

void Sampler::stopRecPad() {
    if (!_rec_active) return;
    g_sdrec.stopRec();
    _pads[_rec_pad].has_sample = true;
    _rec_active = false;
    _rec_pad    = 0;
}

// ─── Відтворення ─────────────────────────────────────────────────────────────

float Sampler::pitchRatio(int8_t semitones) {
    return powf(2.0f, semitones / 12.0f);
}

void Sampler::triggerPad(uint8_t idx) {
    if (idx >= MAX_SAMPLE_PADS || !_pads[idx].has_sample) return;

    uint32_t ns, sr;
    if (!g_sdrec.loadWav(_pads[idx].filename, _play_buf, MAX_SMP_SAMPLES, ns, sr))
        return;

    // Зміна висоти тону — коригуємо ефективну частоту дискретизації
    uint32_t effective_rate = (uint32_t)(sr * pitchRatio(_pads[idx].pitch_shift));

    g_synth.setPlayback(_play_buf, ns, effective_rate);
    g_synth.startPlayback();
    _play_pad = idx + 1;
}

void Sampler::stopAll() {
    g_synth.stopPlayback();
    if (_rec_active) stopRecPad();
    _play_pad = 0;
}

// ─── Збереження налаштувань висоти тону на SD ─────────────────────────────────

static const char PITCH_FILE[] = "SMP_PITCH.BIN";

void Sampler::saveSettings() {
    if (!g_sdrec.sdOk()) return;
    FsFile f = g_sdfs.open(PITCH_FILE, O_RDWR | O_CREAT | O_TRUNC);
    if (!f) return;
    for (int i = 0; i < MAX_SAMPLE_PADS; i++)
        f.write((uint8_t)(_pads[i].pitch_shift + 7));   // зсув: зберігаємо як 0-14
    f.close();
}

void Sampler::loadSettings() {
    if (!g_sdrec.sdOk()) return;
    FsFile f = g_sdfs.open(PITCH_FILE, O_RDONLY);
    if (!f) return;
    for (int i = 0; i < MAX_SAMPLE_PADS; i++) {
        int v = f.read();
        if (v >= 0) _pads[i].pitch_shift = (int8_t)(v - 7);
    }
    f.close();
}
