#include "audio_synth.h"
#include <math.h>

AudioSynth g_synth;

static const float TWO_PI_F = 2.0f * (float)M_PI;
static const float PHASE_SCALE = 1.0f / 4294967296.0f;  // 1/2^32

// ─── Synthesis helpers ────────────────────────────────────────────────────────

void AudioSynth::updatePhaseInc(int v) {
    _phase_inc[v] = (uint32_t)(_freq[v] * (4294967296.0f / SAMPLE_RATE));
}

void AudioSynth::setNote(uint8_t note_idx, uint8_t octave) {
    if (note_idx >= NUM_NOTES || octave >= NUM_OCTAVES) return;
    _pb_active  = false;
    _voice_count = 1;
    _freq[0] = NOTE_FREQS_OCT4[note_idx] * powf(2.0f, (float)octave - 4.0f);
    updatePhaseInc(0);
    _envelope[0] = 0.0f;   // restart attack
    _active  = true;
}

void AudioSynth::setChord(const uint8_t* note_indices, uint8_t count, uint8_t octave) {
    _pb_active  = false;
    _voice_count = (count > MAX_VOICES) ? MAX_VOICES : count;
    for (int i = 0; i < _voice_count; i++) {
        uint8_t ni = note_indices[i] % NUM_NOTES;
        _freq[i] = NOTE_FREQS_OCT4[ni] * powf(2.0f, (float)octave - 4.0f);
        updatePhaseInc(i);
        _envelope[i] = 0.0f;
    }
    _active = true;
}

void AudioSynth::stopNote() {
    _active    = false;
    _pb_active = false;
    for (int i = 0; i < MAX_VOICES; i++) _envelope[i] = 0.0f;
}

// Synthesize one sample (sum of all active voices)
int16_t AudioSynth::synthSample() {
    if (!_active || _muted || _voice_count == 0) return 0;

    float out = 0.0f;
    const float attack_step = 1.0f / (SAMPLE_RATE * 0.05f);  // 50 ms attack

    for (int v = 0; v < _voice_count; v++) {
        if (_freq[v] == 0.0f) continue;

        // Update envelope
        if (_envelope[v] < 1.0f) {
            _envelope[v] += attack_step;
            if (_envelope[v] > 1.0f) _envelope[v] = 1.0f;
        }

        float angle = _phase_acc[v] * PHASE_SCALE * TWO_PI_F;
        _phase_acc[v] += _phase_inc[v];

        float s = 0.0f;
        switch (_timbre) {
            case 0: // Sine
                s = sinf(angle);
                break;
            case 1: // Square (band-limited: 1st + 3rd + 5th harmonics)
                s = sinf(angle) + 0.33f*sinf(3*angle) + 0.2f*sinf(5*angle);
                s *= 0.6f;
                break;
            case 2: // Sawtooth (1/k harmonics)
                s = sinf(angle) + 0.5f*sinf(2*angle) + 0.33f*sinf(3*angle)
                  + 0.25f*sinf(4*angle) + 0.2f*sinf(5*angle);
                s *= 0.5f;
                break;
            case 3: // Clarinet (odd harmonics, 3rd prominent)
                s = sinf(angle) + 0.5f*sinf(3*angle) + 0.1f*sinf(5*angle);
                s *= 0.6f;
                break;
            case 4: // Organ (rich even+odd mix)
                s = sinf(angle) + 0.6f*sinf(2*angle) + 0.4f*sinf(3*angle)
                  + 0.2f*sinf(4*angle) + 0.1f*sinf(6*angle);
                s *= 0.43f;
                break;
        }

        out += s * _envelope[v];
    }

    // Normalize by voice count and scale to amplitude
    out /= _voice_count;
    float amp = (_volume / 100.0f) * 22000.0f;
    return (int16_t)(out * amp);
}

// Resample PCM buffer from src_rate to SAMPLE_RATE on the fly
int16_t AudioSynth::playbackSample() {
    if (!_pb_active || _pb_buf == nullptr) return 0;

    uint32_t pos = (uint32_t)_pb_frac;
    if (pos + 1 >= _pb_len) {
        _pb_active = false;
        return 0;
    }

    // Linear interpolation
    float frac = _pb_frac - (float)pos;
    float s = _pb_buf[pos] * (1.0f - frac) + _pb_buf[pos + 1] * frac;
    _pb_frac += (float)_pb_rate / (float)SAMPLE_RATE;

    return (int16_t)(s * _volume / 100.0f);
}

// ─── Public API ───────────────────────────────────────────────────────────────

void AudioSynth::setPlayback(const int16_t* buf, uint32_t num_samples, uint32_t src_rate) {
    _pb_buf   = buf;
    _pb_len   = num_samples;
    _pb_rate  = src_rate;
    _pb_frac  = 0.0f;
}

void AudioSynth::startPlayback() {
    _pb_frac   = 0.0f;
    _pb_active = true;
    _active    = false;   // mute synthesis during playback
}

void AudioSynth::stopPlayback() {
    _pb_active = false;
}

bool AudioSynth::isPlaybackDone() const {
    return !_pb_active;
}

// Called from BT audio task — must be fast and lock-free
void AudioSynth::fillBuffer(AudioFrame* buf, int32_t count) {
    for (int32_t i = 0; i < count; i++) {
        int16_t s;
        if (_pb_active)       s = playbackSample();
        else if (_active)     s = synthSample();
        else                  s = 0;

        buf[i].ch1 = s;
        buf[i].ch2 = s;
    }
}
