/*
 * microphone.c — PDM microphone capture and pitch detection
 *
 * Clock arithmetic
 * ----------------
 * I2S2 AudioFreq = I2S_AUDIOFREQ_32K
 * I2S SCK  = AudioFreq × 2 × DataBits = 32 000 × 32 = 1 024 000 Hz  (PDM clock)
 * PDM filter decimation = 64
 * PCM output rate = 1 024 000 / 64 = 16 000 Hz
 *
 * Note: I2S2 and I2S3 share the PLLI2S R output.  PLLI2S is configured
 * for 44.1 kHz (I2S3/audio-out), so I2S2's actual SCK differs slightly
 * from 1.024 MHz (≈ 0.2 % error → < 4 cents at worst).  The effective
 * sample rate constant MIC_FS_HZ is tuned accordingly.
 *
 * Pitch detection
 * ---------------
 * Normalized autocorrelation over a 1024-sample window (64 ms).
 * Lag range 14–267 covers 60–1143 Hz (wider than C2–B5).
 * Parabolic interpolation around the peak gives sub-sample lag accuracy
 * (< 1 cent error for notes above A3).
 * Confidence threshold 0.45 rejects noise and non-periodic signals.
 */

#include "microphone.h"
#include "pdm2pcm.h"
#include "main.h"
#include "recorder.h"
#include "chord.h"
#include <math.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Configuration
 * --------------------------------------------------------------------- */
#define PDM_HALF_WORDS  64u      /* PDM words per half-buffer (mono, ×64 decim, 16 PCM/call) */
#define PDM_BUF_WORDS   (PDM_HALF_WORDS * 2u)
#define PCM_PER_CALL    16u      /* PDM_Filter output samples per call */
#define PCM_WIN_SIZE    1024u    /* autocorrelation window (64 ms @ 16 kHz) */

#define LAG_MIN         14       /* 16000 / 1143 Hz ≈  14 */
#define LAG_MAX         300      /* 16000 /   53 Hz ≈ 300 — safely below E2 (82 Hz, lag≈194) */

#define SILENCE_E0           8000000LL

/* IIR low-pass filter: 1st-order Butterworth, fc = 220 Hz @ 16 kHz.
 * Targets the thick-string problem: E2 (82 Hz), A2 (110 Hz), D3 (147 Hz)
 * fundamentals pass with < 0.5 dB loss; 3rd harmonic of E2 (247 Hz) is
 * attenuated ~4 dB; higher harmonics > 400 Hz are cut > 7 dB.
 * This suppresses the over-loud upper harmonics that fool the correlator
 * into reporting octave-doubled pitch on the 6th/5th/4th strings.
 * α = exp(−2π·220/16000) = 0.9172 */
#define LPF_ALPHA            0.9172f
#define LPF_BETA             0.0828f   /* 1 − LPF_ALPHA */

/* Transient/attack suppression */
#define TRANSIENT_RATIO      3.5f      /* energy-jump ratio that triggers hold  */
#define TRANSIENT_HOLD_WIN   2u        /* windows to skip after spike (~128 ms) */
#define ENERGY_EMA_ALPHA     0.85f     /* running-average decay coefficient     */

/* Dynamic confidence: base + up-to-DELTA as signal decays */
#define CONFIDENCE_THR_BASE  0.38f
#define CONFIDENCE_THR_DELTA 0.15f

/* Effective sample rate — adjust if measured pitch is consistently off */
#define MIC_FS_HZ       16000.0f

/* -----------------------------------------------------------------------
 * Peripheral handles
 * --------------------------------------------------------------------- */
static I2S_HandleTypeDef  hi2s2;
DMA_HandleTypeDef         hdma_spi2_rx;   /* extern in microphone.h and msp.c */

/* -----------------------------------------------------------------------
 * Buffers
 * --------------------------------------------------------------------- */
static uint16_t pdm_buf[PDM_BUF_WORDS];           /* DMA circular buffer      */
static int16_t  pcm_win[PCM_WIN_SIZE];             /* PCM accumulation window  */
static uint32_t pcm_idx = 0u;
static volatile bool pitch_ready = false;

/* -----------------------------------------------------------------------
 * Results (written by MIC_Process, read by application)
 * --------------------------------------------------------------------- */
static float detected_freq = 0.0f;
static bool  signal_valid  = false;

/* Transient detection & dynamic confidence state */
static float   energy_avg     = 0.0f;
static uint8_t transient_hold = 0u;

/* -----------------------------------------------------------------------
 * External PDM filter state (declared in pdm2pcm.c)
 * --------------------------------------------------------------------- */
extern PDM_Filter_Handler_t PDM1_filter_handler;
extern PDM_Filter_Config_t  PDM1_filter_config;

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

void MIC_Init(void)
{
    /*
     * Reconfigure PDM filter for mono.
     * CubeMX initialises it as stereo (in_ptr_channels=2); we override
     * here so only the L channel (PB10 rising edge) is used.
     */
    PDM1_filter_handler.in_ptr_channels  = 1u;
    PDM1_filter_handler.out_ptr_channels = 1u;
    PDM_Filter_Init(&PDM1_filter_handler);

    PDM1_filter_config.decimation_factor     = PDM_FILTER_DEC_FACTOR_64;
    PDM1_filter_config.output_samples_number = PCM_PER_CALL;
    PDM1_filter_config.mic_gain              = 24;   /* +24 dB — MP45DT02 has low sensitivity */
    PDM_Filter_setConfig(&PDM1_filter_handler, &PDM1_filter_config);

    /* Configure I2S2 — HAL_I2S_Init triggers HAL_I2S_MspInit in msp.c */
    hi2s2.Instance            = SPI2;
    hi2s2.Init.Mode           = I2S_MODE_MASTER_RX;
    hi2s2.Init.Standard       = I2S_STANDARD_LSB;
    hi2s2.Init.DataFormat     = I2S_DATAFORMAT_16B;
    hi2s2.Init.MCLKOutput     = I2S_MCLKOUTPUT_DISABLE;
    hi2s2.Init.AudioFreq      = I2S_AUDIOFREQ_32K;    /* → SCK = 1.024 MHz PDM clock */
    hi2s2.Init.CPOL           = I2S_CPOL_HIGH;
    hi2s2.Init.ClockSource    = I2S_CLOCK_PLL;
    hi2s2.Init.FullDuplexMode = I2S_FULLDUPLEXMODE_DISABLE;

    if (HAL_I2S_Init(&hi2s2) != HAL_OK) {
        Error_Handler();
    }
}

void MIC_Start(void)
{
    pcm_idx       = 0u;
    pitch_ready   = false;
    detected_freq = 0.0f;
    signal_valid  = false;
    HAL_I2S_Receive_DMA(&hi2s2, pdm_buf, PDM_BUF_WORDS);
}

void MIC_Stop(void)
{
    HAL_I2S_DMAStop(&hi2s2);
}

float MIC_GetFrequency(void)  { return detected_freq; }
bool  MIC_IsSignalValid(void) { return signal_valid;  }

/* -----------------------------------------------------------------------
 * Pitch detection — normalized autocorrelation + parabolic interpolation
 * --------------------------------------------------------------------- */
void MIC_Process(void)
{
    if (!pitch_ready) return;
    pitch_ready = false;

    /* DC removal and energy */
    int64_t dc_sum = 0;
    for (int i = 0; i < (int)PCM_WIN_SIZE; i++) dc_sum += pcm_win[i];
    int16_t dc = (int16_t)(dc_sum / (int64_t)PCM_WIN_SIZE);

    int64_t e0 = 0;
    for (int i = 0; i < (int)PCM_WIN_SIZE; i++) {
        int32_t s = (int32_t)pcm_win[i] - dc;
        e0 += s * s;
    }

    /* Silence check */
    if (e0 < SILENCE_E0) {
        signal_valid   = false;
        detected_freq  = 0.0f;
        transient_hold = 0u;
        energy_avg    *= 0.9f;   /* slow decay so next pluck can be detected */
        return;
    }

    float energy_norm = (float)e0 / (float)PCM_WIN_SIZE;

    /* Transient/attack suppression: ignore pitch during first ~128 ms of a pluck */
    if (energy_avg > 1e5f && energy_norm > energy_avg * TRANSIENT_RATIO) {
        transient_hold = TRANSIENT_HOLD_WIN;
    }
    energy_avg = ENERGY_EMA_ALPHA * energy_avg
               + (1.0f - ENERGY_EMA_ALPHA) * energy_norm;

    if (transient_hold > 0u) {
        transient_hold--;
        signal_valid  = false;
        detected_freq = 0.0f;
        return;
    }

    /* Dynamic confidence: rise up to +DELTA as signal energy decays */
    float conf_thr = CONFIDENCE_THR_BASE;
    if (energy_avg > 1.0f) {
        float ratio = energy_norm / energy_avg;
        if (ratio < 0.5f) {
            conf_thr += CONFIDENCE_THR_DELTA * (1.0f - ratio * 2.0f);
        }
    }

    /* Find the lag with maximum normalized correlation */
    int   best_lag  = 0;
    float best_corr = -2.0f;

    for (int lag = LAG_MIN; lag <= LAG_MAX; lag++)
    {
        int64_t r = 0, e = 0;
        int N = (int)PCM_WIN_SIZE - lag;
        for (int i = 0; i < N; i++) {
            int32_t xi  = (int32_t)pcm_win[i]       - dc;
            int32_t xil = (int32_t)pcm_win[i + lag] - dc;
            r += xi * xil;
            e += xil * xil;
        }
        if (e == 0) continue;
        float corr = (float)r / sqrtf((float)e0 * (float)e / (float)N);
        if (corr > best_corr) {
            best_corr = corr;
            best_lag  = lag;
        }
    }

    if (best_corr < conf_thr || best_lag == 0) {
        signal_valid  = false;
        detected_freq = 0.0f;
        return;
    }

    /* Parabolic interpolation for sub-sample lag accuracy */
    float precise_lag = (float)best_lag;
    if (best_lag > LAG_MIN && best_lag < LAG_MAX)
    {
        /* Recompute normalized correlation at lag-1 and lag+1 */
        float r_adj[2];
        for (int k = 0; k < 2; k++)
        {
            int lag = best_lag - 1 + k * 2;
            int64_t r = 0, e = 0;
            int N = (int)PCM_WIN_SIZE - lag;
            for (int i = 0; i < N; i++) {
                int32_t xi  = (int32_t)pcm_win[i]       - dc;
                int32_t xil = (int32_t)pcm_win[i + lag] - dc;
                r += xi * xil;
                e += xil * xil;
            }
            r_adj[k] = (e > 0) ? (float)r / sqrtf((float)e0 * (float)e / (float)N)
                                : best_corr;
        }
        float ym1   = r_adj[0];
        float y0    = best_corr;
        float yp1   = r_adj[1];
        float denom = ym1 - 2.0f * y0 + yp1;
        if (fabsf(denom) > 1e-6f) {
            precise_lag += 0.5f * (ym1 - yp1) / denom;
        }
    }

    detected_freq = MIC_FS_HZ / precise_lag;
    signal_valid  = true;
}

/* -----------------------------------------------------------------------
 * Internal: process one half of the DMA buffer
 * --------------------------------------------------------------------- */
static void Process_Half(uint16_t *pdm_half)
{
    static int16_t pcm_frame[PCM_PER_CALL];
    static float   lpf_y = 0.0f;   /* IIR LPF state */

    PDM_Filter(pdm_half, pcm_frame, &PDM1_filter_handler);

    for (uint32_t i = 0u; i < PCM_PER_CALL; i++) {
        /* 1st-order IIR LPF: fc ≈ 400 Hz — strips inharmonic overtones */
        lpf_y = LPF_ALPHA * lpf_y + LPF_BETA * (float)pcm_frame[i];
        pcm_win[pcm_idx++] = (int16_t)lpf_y;
        if (pcm_idx >= PCM_WIN_SIZE) {
            pcm_idx     = 0u;
            pitch_ready = true;
        }
    }

    /* Feed chord/recorder with raw (unfiltered) PCM — overtones needed */
    REC_FeedSamples(pcm_frame, PCM_PER_CALL);
    CHORD_FeedSamples(pcm_frame, PCM_PER_CALL);
}

/* -----------------------------------------------------------------------
 * HAL DMA callbacks (override weak symbols in HAL)
 * --------------------------------------------------------------------- */
void HAL_I2S_RxHalfCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s->Instance == SPI2) {
        Process_Half(pdm_buf);
    }
}

void HAL_I2S_RxCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s->Instance == SPI2) {
        Process_Half(pdm_buf + PDM_HALF_WORDS);
    }
}
