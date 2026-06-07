/*
 * chord.h — Real-time guitar chord recognition via Goertzel algorithm
 *
 * Method
 * ------
 * The Goertzel algorithm computes the magnitude at a specific frequency in
 * O(N) time without a full FFT.  We evaluate all 48 chromatic notes across
 * 4 octaves (C2-B5, 65-988 Hz) and fold the magnitudes into 12 pitch-class
 * bins (C, C#, D, …, B).  The pitch-class vector is then matched against
 * 9 chord templates (major, minor, dom7, maj7, min7, sus2, sus4, dim, aug)
 * for all 12 possible roots — the root+type combination with the highest
 * average pitch-class score is reported as the detected chord.
 *
 * Window: 1024 samples @ 16 kHz = 64 ms → frequency resolution ≈ 15.6 Hz.
 * CPU load: 48 × 1024 multiply-adds ≈ 0.5 ms per window (Cortex-M4 FPU).
 *
 * Hardware: MP45DT02 microphone via I2S2 (existing microphone.c pipeline).
 */

#ifndef CHORD_H
#define CHORD_H

#include <stdint.h>
#include <stdbool.h>

#define CHORD_NAME_LEN  12u

typedef struct {
    char    name[CHORD_NAME_LEN]; /* e.g. "Amin7", "C", "Gsus4"  */
    char    type[10];             /* e.g. "minor7", "major"        */
    float   confidence;           /* average pitch-class score, 0..~0.33 */
    uint8_t root_pc;              /* 0-11 (C=0, C#=1, …, B=11)    */
    bool    valid;                /* false when signal below silence threshold */
} ChordResult_t;

void          CHORD_Init(void);

/* Feed 16 kHz PCM from microphone ISR — accumulates CHORD_WIN_SIZE samples */
void          CHORD_FeedSamples(const int16_t *pcm, uint32_t n);

/* Run chord detection (call from main loop — returns immediately if no new window) */
void          CHORD_Process(void);

/* Query last result */
ChordResult_t CHORD_GetResult(void);

/* Normalised [0..1] magnitude for each of the 12 pitch classes (C..B) */
void          CHORD_GetPCMagnitudes(float *out_12);

#endif /* CHORD_H */
