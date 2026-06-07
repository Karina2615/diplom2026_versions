#pragma once
#include <Arduino.h>
#include "config.h"

// Stereo frame as used by BluetoothA2DPSource
struct AudioFrame {
    int16_t ch1;
    int16_t ch2;
};

class AudioSynth {
public:
    // Synthesis control (safe to call from main loop)
    void setNote(uint8_t note_idx, uint8_t octave);  // 0-6, 0-4
    void setChord(const uint8_t* note_indices, uint8_t count, uint8_t octave);
    void stopNote();

    void setTimbre(uint8_t t)   { _timbre = t % NUM_TIMBRES; }
    void setVolume(uint8_t v)   { _volume = (v > 100) ? 100 : v; }
    void setMuted(bool m)       { _muted = m; }
    void setActive(bool a)      { _active = a; }

    // Accessors
    uint8_t getTimbre()  const { return _timbre; }
    uint8_t getVolume()  const { return _volume; }
    bool    isMuted()    const { return _muted; }
    bool    isActive()   const { return _active; }
    float   getFreq(int voice=0) const { return _freq[voice]; }

    // Fill output buffer (called from BT audio task)
    void fillBuffer(AudioFrame* buf, int32_t count);

    // Stream a PCM buffer instead of synthesizing (playback)
    void setPlayback(const int16_t* buf, uint32_t num_samples, uint32_t src_rate);
    void startPlayback();
    void stopPlayback();
    bool isPlaybackDone() const;

private:
    static const int MAX_VOICES = 3;

    float    _freq[MAX_VOICES] = {};
    int      _voice_count = 0;
    uint8_t  _timbre  = 0;
    uint8_t  _volume  = 70;
    bool     _muted   = false;
    bool     _active  = false;

    // DDS per voice
    uint32_t _phase_acc[MAX_VOICES]  = {};
    uint32_t _phase_inc[MAX_VOICES]  = {};
    float    _envelope[MAX_VOICES]   = {};

    // Playback
    const int16_t* _pb_buf   = nullptr;
    uint32_t       _pb_len   = 0;
    float          _pb_frac  = 0.0f;
    uint32_t       _pb_rate  = REC_SAMPLE_RATE;
    bool           _pb_active = false;

    int16_t synthSample();
    int16_t playbackSample();
    void    updatePhaseInc(int voice);
};

extern AudioSynth g_synth;
