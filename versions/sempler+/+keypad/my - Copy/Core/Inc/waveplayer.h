#ifndef __WAVEPLAYER_H
#define __WAVEPLAYER_H

#include "main.h"
#include <stdbool.h>

/* -----------------------------------------------------------------------
 * Buffer
 * --------------------------------------------------------------------- */
#define AUDIO_OUT_BUFFER_SIZE   32768u

/* -----------------------------------------------------------------------
 * Harmonic synthesis
 * --------------------------------------------------------------------- */
#define NUM_HARMONICS   8u   /* fundamental + 7 overtones                 */
#define NUM_PRESETS     5u

/*
 * Each preset defines amplitude coefficients for harmonics 1..NUM_HARMONICS.
 * The coefficients are normalised so their sum == 1.0, preventing clipping.
 * name[] must be exactly 4 printable characters (padded with spaces).
 */
typedef struct {
    const char  name[5];                  /* 4-char compact label + '\0'  */
    const char  fullname[12];             /* up to 11-char full label      */
    float       harmonics[NUM_HARMONICS]; /* coeff[0] = fundamental       */
} HarmonicPreset_t;

extern const HarmonicPreset_t AUDIO_PRESETS[NUM_PRESETS];

/* -----------------------------------------------------------------------
 * Attack-Sustain envelope
 * --------------------------------------------------------------------- */
typedef enum {
    ENV_ATTACK  = 0,   /* gain ramps 0 -> 1 over ATTACK_SAMPLES          */
    ENV_SUSTAIN,       /* gain holds at 1.0                               */
    ENV_IDLE,          /* gain holds at 0.0 (used before first note)      */
} EnvState_t;

/* -----------------------------------------------------------------------
 * Internal buffer state (used by DMA callbacks)
 * --------------------------------------------------------------------- */
typedef enum {
    BUFFER_OFFSET_NONE = 0,
    BUFFER_OFFSET_HALF,
    BUFFER_OFFSET_FULL,
} BUFFER_StateTypeDef;

typedef struct {
    uint8_t             buff[AUDIO_OUT_BUFFER_SIZE];
    BUFFER_StateTypeDef state;
    uint32_t            fptr;
} AUDIO_OUT_BufferTypeDef;

/* -----------------------------------------------------------------------
 * Error codes (unchanged from original)
 * --------------------------------------------------------------------- */
typedef enum {
    AUDIO_ERROR_NONE = 0,
    AUDIO_ERROR_IO,
    AUDIO_ERROR_EOF,
    AUDIO_ERROR_INVALID_VALUE,
} AUDIO_ErrorTypeDef;

/* -----------------------------------------------------------------------
 * Player API
 * --------------------------------------------------------------------- */
AUDIO_ErrorTypeDef  AUDIO_PLAYER_Init(void);
AUDIO_ErrorTypeDef  AUDIO_PLAYER_Start(uint8_t idx);
AUDIO_ErrorTypeDef  AUDIO_PLAYER_Process(bool isLoop);
AUDIO_ErrorTypeDef  AUDIO_PLAYER_Stop(void);
AUDIO_ErrorTypeDef  AUDIO_PLAYER_SetVolume(uint8_t vol);
void                AUDIO_PLAYER_SetPreset(uint8_t presetIdx);
void                AUDIO_PLAYER_NoteChange(void); /* restart envelope attack */
void                AUDIO_PLAYER_SetSilence(bool on); /* suppress synthesis (chord/recorder modes) */

#endif /* __WAVEPLAYER_H */
