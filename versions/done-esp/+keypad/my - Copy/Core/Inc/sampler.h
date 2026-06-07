#pragma once
#include "main.h"

/* PO-33 K.O! style sampler
 * 15 pads (TTP229 keys 1-15), key 16 = mode/function
 *
 * Each pad stores one mono 16-bit PCM 16 kHz sample on SD card:
 *   /SAMPLES/PAD01.WAV  …  /SAMPLES/PAD15.WAV
 *
 * Record: hold key 16 + tap a pad → records mic input until released
 * Play  : tap pad  → plays back the stored sample once
 * Erase : hold key 16 for 1 s, then tap pad → deletes sample
 *
 * Audio output: same I2S3 path as all other modes (CS43L22 DAC).
 */

#define SAMPLER_PADS  15

typedef enum {
    SPAD_EMPTY = 0,
    SPAD_READY,
    SPAD_RECORDING,
    SPAD_PLAYING
} SpadState_t;

typedef struct {
    SpadState_t state[SAMPLER_PADS];   /* per-pad status */
    uint8_t     active_pad;            /* 1-15 or 0 = none */
    uint8_t     fn_held;               /* key 16 held flag */
} Sampler_t;

void Sampler_Init(Sampler_t *s);
void Sampler_Update(Sampler_t *s);     /* call every main-loop iteration     */
void Sampler_FillAudio(int16_t *buf, uint16_t frames); /* called from I2S DMA ISR */
