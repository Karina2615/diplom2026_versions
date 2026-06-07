/*
 * recorder.c — Guitar loop recorder
 *
 * Buffer layout (128 KB total, 64 000 samples × 2 B ≈ 7.1 s @ 9 070 Hz):
 *   rec_buf[32000] — 64 KB in CCM-SRAM (CPU-only, not DMA-accessible)
 *   rec_ext[32000] — 64 KB in main SRAM (overflow)
 *   rec_rd / rec_wr helpers transparently span both regions.
 *
 * Sample-rate conversion (9 070 Hz → 44.1 kHz)
 * ---------------------------------------------
 * Linear interpolation with Q16.16 fixed-point position accumulator.
 * Increment = round((9070 << 16) / 44100) = 13479.
 * TIM3: Prescaler=83, Period=62 → 48 MHz / 84 / 63 ≈ 9 070 Hz ADC rate.
 */

#include "recorder.h"
#include <string.h>

#define REC_CCM_SIZE   32000u   /* int16_t count in CCM-SRAM  (64 KB) */
#define REC_EXT_SIZE   32000u   /* int16_t count in main SRAM (64 KB) */

static int16_t rec_buf[REC_CCM_SIZE] __attribute__((section(".ccmram")));
static int16_t rec_ext[REC_EXT_SIZE];   /* main SRAM overflow */

static uint32_t rec_len      = 0u;
static uint32_t play_pos_num = 0u;   /* Q16.16: integer part = sample index */

#define SRC_INC  13479u   /* round((9070 << 16) / 44100) — matches TIM3 ≈ 9070 Hz */

static RecorderState_t recState = REC_STATE_IDLE;
static volatile bool   s_autosave_pending = false; /* set when buffer fills naturally */

static inline int16_t rec_rd(uint32_t i)
{
    return (i < REC_CCM_SIZE) ? rec_buf[i] : rec_ext[i - REC_CCM_SIZE];
}
static inline void rec_wr(uint32_t i, int16_t v)
{
    if (i < REC_CCM_SIZE) rec_buf[i] = v;
    else rec_ext[i - REC_CCM_SIZE] = v;
}

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

void REC_Init(void)
{
    recState     = REC_STATE_IDLE;
    rec_len      = 0u;
    play_pos_num = 0u;
}

void REC_StartRecord(void)
{
    rec_len            = 0u;
    recState           = REC_STATE_RECORDING;
    s_autosave_pending = false;   /* clear any stale flag from a previous recording */
}

void REC_StopRecord(void)
{
    if (recState == REC_STATE_RECORDING)
        recState = (rec_len > 0u) ? REC_STATE_READY : REC_STATE_IDLE;
}

void REC_StartPlay(void)
{
    if (rec_len == 0u) return;
    play_pos_num = 0u;
    recState     = REC_STATE_PLAYING;
}

void REC_StopPlay(void)
{
    if (recState == REC_STATE_PLAYING)
        recState = REC_STATE_READY;
}

RecorderState_t REC_GetState(void)           { return recState;            }
uint32_t        REC_GetSamplesRecorded(void) { return rec_len;             }
uint32_t        REC_GetPlayPosition(void)    { return play_pos_num >> 16;  }

int16_t REC_GetSample(uint32_t idx)
{
    return rec_rd(idx);
}

void REC_LoadExternal(const int16_t *src, uint32_t n_samples)
{
    uint32_t count = (n_samples > REC_BUFFER_SIZE) ? REC_BUFFER_SIZE : n_samples;
    for (uint32_t i = 0u; i < count; i++) rec_wr(i, src[i]);
    rec_len      = count;
    play_pos_num = 0u;
    recState     = REC_STATE_READY;
}

/* -----------------------------------------------------------------------
 * Chunk-loading — stream PCM from SD card into recorder buffer
 * --------------------------------------------------------------------- */
static uint32_t s_load_pos   = 0u;
static uint32_t s_load_total = 0u;

void REC_LoadChunkBegin(uint32_t total_n)
{
    uint32_t count = (total_n > REC_BUFFER_SIZE) ? REC_BUFFER_SIZE : total_n;
    s_load_total = count;
    s_load_pos   = 0u;
    rec_len      = 0u;
    play_pos_num = 0u;
    recState     = REC_STATE_IDLE;
}

void REC_LoadChunk(const int16_t *data, uint32_t len)
{
    for (uint32_t i = 0u; i < len; i++) {
        if (s_load_pos >= s_load_total) break;
        rec_wr(s_load_pos++, data[i]);
    }
}

void REC_LoadChunkEnd(void)
{
    rec_len      = s_load_pos;
    play_pos_num = 0u;
    recState     = (rec_len > 0u) ? REC_STATE_READY : REC_STATE_IDLE;
}

/* -----------------------------------------------------------------------
 * REC_FeedSamples — called from microphone DMA ISR context
 * Stores 16 kHz PCM directly (no decimation).
 * --------------------------------------------------------------------- */
void REC_FeedSamples(const int16_t *pcm, uint32_t n)
{
    if (recState != REC_STATE_RECORDING) return;

    for (uint32_t i = 0u; i < n; i++) {
        if (rec_len >= REC_BUFFER_SIZE) {
            recState           = REC_STATE_READY;
            s_autosave_pending = true;   /* signal main loop to auto-save */
            return;
        }
        rec_wr(rec_len++, pcm[i]);
    }
}

/* -----------------------------------------------------------------------
 * REC_TakeAutoSaveFlag — returns true (and clears flag) when the buffer
 * filled naturally during recording.  Called once per main loop iteration
 * by Task_Recorder so it can save to SD without double-saving.
 * --------------------------------------------------------------------- */
bool REC_TakeAutoSaveFlag(void)
{
    if (!s_autosave_pending) return false;
    s_autosave_pending = false;
    return true;
}

/* -----------------------------------------------------------------------
 * REC_FillPlayBuffer — called from waveplayer half-transfer refill
 * Upsamples 16 kHz mono → 44.1 kHz stereo via linear interpolation.
 * --------------------------------------------------------------------- */
void REC_FillPlayBuffer(uint16_t *buf, uint32_t nWords)
{
    if (recState != REC_STATE_PLAYING || rec_len == 0u) {
        memset(buf, 0, nWords * sizeof(uint16_t));
        return;
    }

    uint32_t nSamples = nWords / 2u;   /* stereo pairs */

    for (uint32_t i = 0u; i < nSamples; i++) {
        uint32_t idx  = play_pos_num >> 16;
        uint32_t frac = play_pos_num & 0xFFFFu;

        if (idx + 1u >= rec_len) {
            memset(&buf[i * 2u], 0, (nSamples - i) * 4u);
            recState = REC_STATE_READY;
            return;
        }

        int32_t s0  = (int32_t)rec_rd(idx);
        int32_t s1  = (int32_t)rec_rd(idx + 1u);
        int16_t out = (int16_t)(s0 + ((s1 - s0) * (int32_t)frac >> 16));

        buf[i * 2u]      = (uint16_t)out;
        buf[i * 2u + 1u] = (uint16_t)out;

        play_pos_num += SRC_INC;
    }
}
