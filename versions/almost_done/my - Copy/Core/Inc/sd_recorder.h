#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * sd_recorder.h — SD-card based multi-slot recorder (FatFS)
 *
 * Directory: 0:/RECORDINGS
 * File format: raw 16-bit signed PCM, 16 kHz, mono (.PCM files)
 * Max slots: SD_REC_MAX (scanned at init / after save or delete)
 *
 * Live streaming API allows recording directly to SD without RAM buffer
 * limits — supports 60+ second recordings at 16 kHz.
 */

#define SD_REC_MAX       16u
#define SD_REC_DIR_PATH  "0:/RECORDINGS"

typedef struct {
    char     fname[16];    /* filename only, e.g. "REC_001.PCM"   */
    uint32_t n_samples;    /* total samples = file_size / 2       */
} SDRec_Entry_t;

/* ── Yield callback: called during long SD operations to keep audio alive ── */
typedef void (*SDRec_YieldFn_t)(void);
void SDRec_SetYield(SDRec_YieldFn_t fn);

/* Call once after FATFS init */
void     SDRec_Init(void);

/* Re-scan directory; returns false if SD not mounted */
bool     SDRec_Scan(void);

/* Number of recordings found in last scan */
uint8_t  SDRec_GetCount(void);

/* Entry pointer (NULL if idx out of range) */
const SDRec_Entry_t *SDRec_GetEntry(uint8_t idx);

/* Save current recorder RAM buffer to a new file; returns true on success */
bool     SDRec_SaveFromRecorder(void);

/* Load file idx into recorder RAM buffer (sets READY state); returns true on success */
bool     SDRec_Load(uint8_t idx);

/* Delete file idx, re-scans list; returns true on success */
bool     SDRec_Delete(uint8_t idx);

/* True if SD card was successfully mounted */
bool     SDRec_IsMounted(void);

/* ── Live streaming recording — no RAM buffer limit ──────────────────────── */

/* Open a new .PCM file for streaming.  Call when recording starts.
 * Returns true on success.  Calls SDRec_Scan to pick the next filename. */
bool     SDRec_LiveStart(void);

/* Feed a block of 16-bit PCM samples to the open file.
 * Call from the main loop (NOT from ISR).
 * Returns true if write succeeded. */
bool     SDRec_LiveFeed(const int16_t *pcm, uint16_t n);

/* Finalize and close the streaming file, then re-scan.
 * Call when recording stops. */
bool     SDRec_LiveStop(void);

/* True if a live SD recording is currently open */
bool     SDRec_IsLiveRecording(void);

/* Bytes written to the current live file (0 if not active) */
uint32_t SDRec_GetLiveBytes(void);
