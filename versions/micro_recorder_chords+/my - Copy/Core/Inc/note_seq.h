#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * note_seq.h — sampler note-event sequencer backed by FatFS
 *
 * Records sequences of NOTE_ON / NOTE_OFF events with millisecond
 * timestamps into 0:/NOTES/NOTE_NNN.NOT files, then plays them back
 * through the audio synthesiser.
 *
 * Each event is 8 bytes (NoteEvent_t).  Up to NSEQ_EVENT_MAX events
 * can be buffered in RAM (caller-allocated buffer).
 *
 * This module only handles FatFS I/O.  Playback timing and audio-player
 * calls are managed by main.c.
 *
 * Assumes the SD card is already mounted by sd_recorder (SDRec_Init).
 */

#define NSEQ_DIR_PATH   "0:/NOTES"     /* separate from recorder /RECORDINGS */
#define NSEQ_MAX        16u            /* maximum files listed */
#define NSEQ_EVENT_MAX  512u           /* maximum events in RAM buffer */

typedef struct {
    uint32_t t_ms;   /* ms from sequence start (0 at first event)  */
    uint8_t  note;   /* 0-6  (Do … Si)                             */
    uint8_t  octave; /* octave index used by AUDIO_PLAYER           */
    uint8_t  on;     /* 1 = NOTE ON,  0 = NOTE OFF                 */
    uint8_t  _pad;   /* reserved, write as 0                        */
} NoteEvent_t;       /* 8 bytes — no padding on any ARM alignment   */

typedef struct {
    char     fname[16];    /* e.g. "NOTE_001.NOT"            */
    uint32_t duration_ms;  /* total duration = last event ms */
    uint32_t n_events;     /* number of events in file       */
} NSeq_Entry_t;

/* One-time init: ensure directory exists + rescan */
bool   NSeq_Init(void);

/* Rescan 0:/NOTES/ and rebuild internal entry list */
bool   NSeq_Scan(void);

uint8_t             NSeq_GetCount(void);
const NSeq_Entry_t *NSeq_GetEntry(uint8_t idx);

/* Write n events to a new NOTE_NNN.NOT file; returns true on success */
bool   NSeq_Save(const NoteEvent_t *evts, uint16_t n);

/* Load file idx into caller-supplied buffer (cap = buffer capacity).
 * Sets *n_out to the number of events actually loaded.
 * Returns true on success. */
bool   NSeq_Load(uint8_t idx, NoteEvent_t *evts, uint16_t cap, uint16_t *n_out);

/* Delete file idx and rescan */
bool   NSeq_Delete(uint8_t idx);
