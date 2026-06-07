/*
 * waveplayer.c — Additive harmonic synthesis with Attack-Sustain envelope
 *
 * Theory
 * ------
 * Any periodic waveform f(t) can be represented as a sum of sinusoids
 * (Fourier series).  For a tone at frequency f0 sampled at fs:
 *
 *   x[n] = A * SUM_k { c_k * sin(2*pi * k*f0/fs * n) }
 *
 * where c_k is the amplitude coefficient of the k-th harmonic.
 * Five presets are defined, each with a different spectral profile:
 *   SINE — only the fundamental (k=1)
 *   SQR  — odd harmonics, 1/k series (approximates square wave)
 *   SAW  — all harmonics, 1/k series (approximates sawtooth wave)
 *   CLAR — strong 3rd harmonic (clarinet-like timbre)
 *   ORGN — rich even+odd harmonics (pipe-organ-like timbre)
 *
 * Attack-Sustain envelope
 * -----------------------
 * When a note starts (or changes), the amplitude rises linearly from 0 to 1
 * over ATTACK_SAMPLES samples (~50 ms at 44.1 kHz) to avoid an audible click.
 * The amplitude then stays at 1 until the next note change.
 * Muting is handled separately at the codec level by Apply_Volume() in main.c.
 *
 * Double-buffering
 * ----------------
 * The I2S DMA operates in circular mode on a buffer of AUDIO_OUT_BUFFER_SIZE
 * bytes.  The DMA fires HAL_I2S_TxHalfCpltCallback (half done) and
 * HAL_I2S_TxCpltCallback (full done), which set BufferCtl.state.  The main
 * loop calls AUDIO_PLAYER_Process() which refills whichever half just played.
 */

#include "waveplayer.h"
#include "AUDIO.h"
#include "main.h"
#include "recorder.h"
#include <math.h>
#include <string.h>
#include <stdbool.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* -----------------------------------------------------------------------
 * Harmonic preset table
 *
 * Coefficients are pre-normalised so sum(c_k) == 1.0 per preset.
 * This guarantees the synthesised waveform stays within [-amplitude, +amplitude]
 * regardless of the chosen preset.
 * --------------------------------------------------------------------- */
const HarmonicPreset_t AUDIO_PRESETS[NUM_PRESETS] = {
    {
        /* SINE: pure fundamental only */
        "SINE", "Pure Sine",
        { 1.000f, 0.000f, 0.000f, 0.000f, 0.000f, 0.000f, 0.000f, 0.000f }
    },
    {
        /* SQR: odd harmonics 1/k, normalised (sum = 1 + 1/3 + 1/5 + 1/7 = 1.676) */
        "SQR ", "Square",
        { 0.597f, 0.000f, 0.199f, 0.000f, 0.119f, 0.000f, 0.085f, 0.000f }
    },
    {
        /* SAW: all harmonics 1/k, normalised (sum of 8 terms = 2.718) */
        "SAW ", "Sawtooth",
        { 0.368f, 0.184f, 0.123f, 0.092f, 0.074f, 0.061f, 0.053f, 0.046f }
    },
    {
        /* CLAR: strong 3rd harmonic, models clarinet-like closed-pipe resonance */
        "CLAR", "Clarinet",
        { 0.500f, 0.050f, 0.350f, 0.050f, 0.030f, 0.010f, 0.005f, 0.005f }
    },
    {
        /* ORGN: rich even+odd harmonics, models pipe-organ flue pipe */
        "ORGN", "Organ",
        { 0.350f, 0.200f, 0.180f, 0.150f, 0.070f, 0.030f, 0.015f, 0.005f }
    }
};

/* -----------------------------------------------------------------------
 * Internal state
 * --------------------------------------------------------------------- */
static AUDIO_OUT_BufferTypeDef BufferCtl;
static float currentPhase  = 0.0f;  /* phase accumulator for fundamental [0, 2*pi) */
static uint8_t currentPresetIdx = 0;

/* Envelope */
#define ATTACK_SAMPLES  2205u   /* 50 ms at 44100 Hz */
static EnvState_t envState   = ENV_IDLE;
static uint32_t   envCounter = 0;
static float      envGain    = 0.0f;

/* Silence flag — set by main when in chord/recorder modes */
static bool silenceEnabled = false;

/* External note state owned by main.c */
extern I2S_HandleTypeDef        hi2s3;
extern const uint32_t           TONE_FREQUENCIES[4][7];
extern volatile uint8_t         currentNote;
extern volatile uint8_t         currentOctave;

/* -----------------------------------------------------------------------
 * Forward declaration
 * --------------------------------------------------------------------- */
static void Generate_Wave(uint16_t *pBuffer, uint32_t samplingFreq,
                           uint32_t toneFreqCentihz, uint32_t bufSizeWords);

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

AUDIO_ErrorTypeDef AUDIO_PLAYER_Init(void)
{
    if (AUDIO_OUT_Init(OUTPUT_DEVICE_HEADPHONE, 70, 44100) != AUDIO_OK) {
        Error_Handler();
        return AUDIO_ERROR_IO;
    }
    BufferCtl.state = BUFFER_OFFSET_NONE;
    envState   = ENV_ATTACK;
    envCounter = 0;
    envGain    = 0.0f;
    return AUDIO_ERROR_NONE;
}

AUDIO_ErrorTypeDef AUDIO_PLAYER_Start(uint8_t idx)
{
    (void)idx;
    uint32_t fs   = hi2s3.Init.AudioFreq;
    uint32_t freq = TONE_FREQUENCIES[currentOctave][currentNote];

    currentPhase = 0.0f;
    /* Fill the entire buffer before handing it to DMA */
    Generate_Wave((uint16_t *)BufferCtl.buff, fs, freq, AUDIO_OUT_BUFFER_SIZE / 2u);

    if (AUDIO_OUT_Play((uint16_t *)BufferCtl.buff, AUDIO_OUT_BUFFER_SIZE) != AUDIO_OK) {
        return AUDIO_ERROR_IO;
    }
    BufferCtl.state = BUFFER_OFFSET_NONE;
    return AUDIO_ERROR_NONE;
}

AUDIO_ErrorTypeDef AUDIO_PLAYER_Process(bool isLoop)
{
    (void)isLoop;
    uint32_t fs           = hi2s3.Init.AudioFreq;
    uint32_t freq         = TONE_FREQUENCIES[currentOctave][currentNote];
    uint32_t halfBufWords = (AUDIO_OUT_BUFFER_SIZE / 2u) / 2u; /* uint16_t units */

    if (BufferCtl.state != BUFFER_OFFSET_HALF &&
        BufferCtl.state != BUFFER_OFFSET_FULL) {
        return AUDIO_ERROR_NONE;
    }

    uint16_t *dst = (BufferCtl.state == BUFFER_OFFSET_HALF)
                    ? (uint16_t *)BufferCtl.buff
                    : (uint16_t *)(BufferCtl.buff + AUDIO_OUT_BUFFER_SIZE / 2u);

    RecorderState_t rs = REC_GetState();
    if (rs == REC_STATE_PLAYING) {
        /* Recorder playback overrides synthesis */
        REC_FillPlayBuffer(dst, halfBufWords);
    } else if (silenceEnabled) {
        /* Chord/recorder mode with no active playback — output silence */
        memset(dst, 0, halfBufWords * sizeof(uint16_t));
    } else {
        /* Normal additive synthesis */
        Generate_Wave(dst, fs, freq, halfBufWords);
    }

    BufferCtl.state = BUFFER_OFFSET_NONE;
    return AUDIO_ERROR_NONE;
}

AUDIO_ErrorTypeDef AUDIO_PLAYER_Stop(void)
{
    return (AUDIO_OUT_Stop(CODEC_PDWN_HW) == AUDIO_OK)
           ? AUDIO_ERROR_NONE : AUDIO_ERROR_IO;
}

AUDIO_ErrorTypeDef AUDIO_PLAYER_SetVolume(uint8_t vol)
{
    return (AUDIO_OUT_SetVolume(vol) == AUDIO_OK)
           ? AUDIO_ERROR_NONE : AUDIO_ERROR_IO;
}

void AUDIO_PLAYER_SetPreset(uint8_t presetIdx)
{
    if (presetIdx < NUM_PRESETS) {
        currentPresetIdx = presetIdx;
    }
}

/*
 * Call this whenever the note, octave, or preset changes.
 * Resets the phase accumulator (avoids a discontinuity pop) and
 * restarts the attack envelope so the new tone fades in cleanly.
 */
void AUDIO_PLAYER_NoteChange(void)
{
    currentPhase = 0.0f;
    envCounter   = 0;
    envGain      = 0.0f;
    envState     = ENV_ATTACK;
}

void AUDIO_PLAYER_SetSilence(bool on)
{
    silenceEnabled = on;
}

/* -----------------------------------------------------------------------
 * DMA callbacks (called from HAL_I2S_TxHalfCpltCallback / TxCpltCallback)
 * --------------------------------------------------------------------- */
void AUDIO_OUT_TransferComplete_CallBack(void)
{
    BufferCtl.state = BUFFER_OFFSET_FULL;
}

void AUDIO_OUT_HalfTransfer_CallBack(void)
{
    BufferCtl.state = BUFFER_OFFSET_HALF;
}

/* -----------------------------------------------------------------------
 * Core synthesis function
 *
 * Parameters
 *   pBuffer         — destination (uint16_t, stereo interleaved L/R)
 *   samplingFreq    — audio sample rate in Hz (e.g. 44100)
 *   toneFreqCentihz — fundamental frequency × 100  (e.g. 44000 = 440.00 Hz)
 *   bufSizeWords    — number of uint16_t words to fill
 *                     (numSamples = bufSizeWords / 2 because stereo)
 * --------------------------------------------------------------------- */
static void Generate_Wave(uint16_t *pBuffer, uint32_t samplingFreq,
                           uint32_t toneFreqCentihz, uint32_t bufSizeWords)
{
    static const float AMPLITUDE = 32767.0f * 0.55f; /* headroom below full scale */
    const float TWO_PI = 2.0f * (float)M_PI;

    float toneFreq      = (float)toneFreqCentihz / 100.0f;
    float phaseInc      = TWO_PI * toneFreq / (float)samplingFreq;
    const float *coeff  = AUDIO_PRESETS[currentPresetIdx].harmonics;

    uint32_t numSamples = bufSizeWords / 2u; /* stereo pairs */

    for (uint32_t i = 0; i < numSamples; i++)
    {
        /* --- Envelope update (runs once per sample) --- */
        switch (envState)
        {
            case ENV_ATTACK:
                envGain = (float)envCounter / (float)ATTACK_SAMPLES;
                envCounter++;
                if (envCounter >= ATTACK_SAMPLES) {
                    envState = ENV_SUSTAIN;
                    envGain  = 1.0f;
                }
                break;

            case ENV_SUSTAIN:
                envGain = 1.0f;
                break;

            case ENV_IDLE:
            default:
                envGain = 0.0f;
                break;
        }

        /* --- Additive harmonic synthesis ---
         * x[n] = SUM_{k=1}^{NUM_HARMONICS} coeff[k-1] * sin(k * phi[n])
         * phi[n] = currentPhase (fundamental phase accumulator)
         */
        float sample = 0.0f;
        for (uint8_t h = 0; h < NUM_HARMONICS; h++)
        {
            if (coeff[h] < 1e-4f) continue; /* skip negligible harmonics */
            float harmPhase = fmodf(currentPhase * (float)(h + 1u), TWO_PI);
            sample += coeff[h] * sinf(harmPhase);
        }

        /* Scale by amplitude and envelope gain */
        sample *= AMPLITUDE * envGain;

        /* Hard-limit (should never trigger if coefficients are normalised) */
        if (sample >  32767.0f) sample =  32767.0f;
        if (sample < -32768.0f) sample = -32768.0f;

        int16_t out = (int16_t)sample;

        /* Write identical value to both stereo channels */
        pBuffer[i * 2u]      = (uint16_t)out;
        pBuffer[i * 2u + 1u] = (uint16_t)out;

        /* Advance and wrap phase accumulator */
        currentPhase += phaseInc;
        if (currentPhase >= TWO_PI) {
            currentPhase -= TWO_PI;
        }
    }
}
