/*
 * recorder.h — Guitar loop recorder (16 kHz PCM, up to 2 s, CCM-SRAM)
 *
 * Signal flow
 * -----------
 * RECORD:  microphone PDM → PCM (16 kHz) → rec_buf[]  (no decimation)
 * PLAY:    rec_buf[] → linear-interpolation SRC (16→44.1 kHz) → I2S DMA
 *
 * Buffer: 32000 samples × 2 B = 64 000 B — fits entirely in 64 KB CCM-SRAM.
 *
 * Keypad (in APP_MODE_RECORDER)
 *   '*'  — start 3-second countdown, then record (second press cancels)
 *   '0'  — toggle Playback (start / stop)
 *   '#'  — stop all (abort record or play)
 */

#ifndef RECORDER_H
#define RECORDER_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    REC_STATE_IDLE = 0,   /* nothing recorded yet    */
    REC_STATE_RECORDING,  /* actively capturing       */
    REC_STATE_READY,      /* recording stored, idle   */
    REC_STATE_PLAYING,    /* playback running         */
} RecorderState_t;

#define REC_SAMPLE_RATE   16000u
#define REC_BUFFER_SIZE   64000u   /* 4.0 s × 16 kHz × 2 B = 128 000 B — CCM 64 KB + SRAM 64 KB */

void            REC_Init(void);
void            REC_StartRecord(void);
void            REC_StopRecord(void);
void            REC_StartPlay(void);
void            REC_StopPlay(void);
RecorderState_t REC_GetState(void);
uint32_t        REC_GetSamplesRecorded(void);
uint32_t        REC_GetPlayPosition(void);   /* current read-head (sample index) */

/* Called from microphone ISR context — feeds raw PCM into rec_buf */
void REC_FeedSamples(const int16_t *pcm, uint32_t n);

/* Called from waveplayer half-transfer fill — upsamples to 44.1 kHz stereo */
void REC_FillPlayBuffer(uint16_t *buf, uint32_t nWords);

/* Return one recorded sample by index (used by flash_recorder to read the buffer) */
int16_t REC_GetSample(uint32_t idx);

/*
 * Load external PCM into the recorder buffer and transition to READY state.
 * src may be a flash memory-mapped pointer (read-only flash is valid here).
 * n_samples is clamped to REC_BUFFER_SIZE.
 */
void REC_LoadExternal(const int16_t *src, uint32_t n_samples);

#endif /* RECORDER_H */
