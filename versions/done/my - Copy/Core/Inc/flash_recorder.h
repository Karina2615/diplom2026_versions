/*
 * flash_recorder.h — Single-slot audio recorder using STM32F407 internal flash
 *
 * Uses Sector 5 (0x08020000, 128 KB) exclusively.
 * Each save erases the sector and writes a fresh recording — no slot tracking.
 *
 * Sector 5 layout:
 *   Bytes  0-3 : uint32_t magic    — 0xDEADCAFE = recording present
 *   Bytes  4-7 : uint32_t n_samp   — number of int16_t samples stored
 *   Bytes  8+  : int16_t  pcm[]    — raw PCM @ 16 kHz mono
 *
 * Capacity: 48 000 samples × 2 B + 8 B header = 96 008 B < 128 KB ✓
 */

#ifndef FLASH_RECORDER_H
#define FLASH_RECORDER_H

#include <stdint.h>
#include <stdbool.h>

/* Scan flash header and set internal flag — call once at startup */
void    FLASH_REC_Init(void);

/* Returns 1 if a recording is present, 0 otherwise */
uint8_t FLASH_REC_GetUsedSlots(void);

/* Always returns false — single slot, overwrite is always allowed */
bool    FLASH_REC_IsFull(void);

/* Flash-based check (reads magic word) */
bool    FLASH_REC_IsSlotUsed(uint8_t idx);

/*
 * Erase Sector 5 and write the current recorder RAM buffer.
 * Returns true on success.  BLOCKING (~1-2 s) — mute audio before calling.
 */
bool    FLASH_REC_SaveFromRecorder(void);

/*
 * Returns 0 if a recording is present, 0xFF if none.
 * Used by playback code: slot != 0xFF → load from flash.
 */
uint8_t FLASH_REC_GetLastSavedSlot(void);

/* Direct-read helpers (flash is memory-mapped).  idx is ignored. */
const int16_t *FLASH_REC_GetSlotDataPtr(uint8_t idx);     /* NULL if empty */
uint32_t       FLASH_REC_GetSlotSampleCount(uint8_t idx); /* 0    if empty */

/* Erase the recording sector and clear the saved flag.  BLOCKING (~1-2 s). */
void    FLASH_REC_EraseAll(void);

#endif /* FLASH_RECORDER_H */
