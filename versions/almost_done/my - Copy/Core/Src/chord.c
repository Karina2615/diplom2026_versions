/*
 * chord.c — Real-time guitar chord recognition via Goertzel algorithm
 *
 * Detected chord set (12 chords):
 *   Am  A   Bm  B   Cm  C   Em  E   Fm  F   Gm  G
 *
 * These are the most common open-position guitar chords.  The algorithm
 * only considers roots A, B, C, E, F, G so all other chromatic roots are
 * ignored — this dramatically reduces false positives on guitar input.
 *
 * Temporal stability
 * ------------------
 * A new chord is displayed only after CHORD_HOLD_WINDOWS consecutive 64 ms
 * windows agree on the same label.  Once a chord is "held", it stays on
 * screen until another chord accumulates enough votes.  Silence for
 * CHORD_SILENCE_RESET consecutive windows clears the held chord.
 * This prevents the display from flickering between detections.
 *
 * CPU load: 6 roots × 2 templates × 1024 samples × (Goertzel per octave)
 * Only 12 Goertzel filters are evaluated (one per restricted note), making
 * this considerably lighter than the full-12-root version.
 *
 * CHORD_FeedSamples — ISR context (microphone DMA callback)
 * CHORD_Process     — main loop
 */

#include "chord.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

#define CHORD_WIN_SIZE      1024u     /* 64 ms @ 16 kHz                        */
#define CHORD_FS            16000.0f
#define SILENCE_THRESH      3.0e4f    /* raised: ignore ambient noise           */
#define CHORD_HOLD_WINDOWS  5u        /* ~320 ms of consistent detection        */
#define CHORD_SILENCE_RESET 4u        /* silent frames before clearing display  */

static int16_t  chord_win[CHORD_WIN_SIZE];
static uint32_t chord_idx   = 0u;
static bool     chord_ready = false;

static float        pc_mag[12];
static ChordResult_t last_result;

/* -----------------------------------------------------------------------
 * Restricted root set — only A B C E F G (pitch classes 9 11 0 4 5 7)
 * --------------------------------------------------------------------- */
/* Roots: A B C D E F G — 7 common open-position guitar chord roots */
static const uint8_t VALID_ROOTS[]    = { 9u, 11u, 0u, 2u, 4u, 5u, 7u };
static const char  * const ROOT_NAMES[] = { "A","B","C","D","E","F","G" };
#define N_VALID_ROOTS  7u

/* -----------------------------------------------------------------------
 * Chord templates — major and minor triads only
 * --------------------------------------------------------------------- */
typedef struct {
    const char *sfx;       /* name suffix: "" = major, "m" = minor */
    const char *typename_;
    uint8_t     iv[3];     /* semitone intervals from root          */
} CTemplate_t;

#define N_TEMPLATES 2u
static const CTemplate_t TEMPLATES[N_TEMPLATES] = {
    { "",  "major", {0u, 4u, 7u} },
    { "m", "minor", {0u, 3u, 7u} },
};

/* -----------------------------------------------------------------------
 * Temporal stability state
 * --------------------------------------------------------------------- */
static char    held_name[CHORD_NAME_LEN] = "--";
static char    held_type[10]             = "---";
static uint8_t held_root                 = 0u;
static bool    held_valid                = false;

static char    cand_name[CHORD_NAME_LEN] = "--";
static uint8_t cand_count                = 0u;
static uint8_t silence_count             = 0u;

/* -----------------------------------------------------------------------
 * Note frequency table — C2 base frequencies for all 12 semitones
 * --------------------------------------------------------------------- */
static const float BASE_FREQ[12] = {
     65.41f,  69.30f,  73.42f,  77.78f,  82.41f,  87.31f,
     92.50f,  98.00f, 103.83f, 110.00f, 116.54f, 123.47f
};

/* -----------------------------------------------------------------------
 * chord_lpf — 1st-order IIR low-pass filter applied in-place on chord_win
 *
 * fc ≈ 1200 Hz @ 16 kHz  →  α = exp(-2π·1200/16000) ≈ 0.621
 * Attenuates inharmonic overtones (> 1.2 kHz) that confuse low-root
 * detection (Em root E2 = 82 Hz, G Major root G2 = 98 Hz).
 * The important chord tones (fundamentals + lower harmonics up to ~800 Hz)
 * pass with minimal attenuation.
 * --------------------------------------------------------------------- */
#define CHORD_LPF_ALPHA  0.621f
#define CHORD_LPF_BETA   0.379f   /* 1 - CHORD_LPF_ALPHA */

static void chord_lpf(int16_t *win, uint32_t N)
{
    float y = 0.0f;
    for (uint32_t i = 0u; i < N; i++) {
        y = CHORD_LPF_ALPHA * y + CHORD_LPF_BETA * (float)win[i];
        win[i] = (int16_t)y;
    }
}

/*
 * Octave-magnitude weights: boost fundamentals (oct 0/1) relative to the
 * upper harmonic octaves (oct 3) where inharmonic string overtones cluster.
 * Em (E2=82 Hz) and G (G2=98 Hz) sit in oct 0 — boosting this octave
 * helps the root pitch-class dominate after normalisation.
 */
static const float OCT_WEIGHT[4] = { 1.5f, 1.2f, 1.0f, 0.7f };

/* -----------------------------------------------------------------------
 * Goertzel — magnitude at frequency f in x[N] sampled at fs
 * --------------------------------------------------------------------- */
static float goertzel(const int16_t *x, uint32_t N, float f, float fs)
{
    float omega = 2.0f * 3.14159265f * f / fs;
    float coeff = 2.0f * cosf(omega);
    float s1 = 0.0f, s2 = 0.0f;

    for (uint32_t n = 0u; n < N; n++) {
        float s0 = (float)x[n] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }

    float re = s1 - s2 * cosf(omega);
    float im = s2 * sinf(omega);
    return sqrtf(re * re + im * im);
}

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

void CHORD_Init(void)
{
    chord_idx   = 0u;
    chord_ready = false;
    memset(pc_mag, 0, sizeof(pc_mag));

    strncpy(held_name, "--", CHORD_NAME_LEN - 1u);
    strncpy(held_type, "---", sizeof(held_type) - 1u);
    held_root  = 0u;
    held_valid = false;

    strncpy(cand_name, "--", CHORD_NAME_LEN - 1u);
    cand_count    = 0u;
    silence_count = 0u;

    last_result.valid      = false;
    last_result.confidence = 0.0f;
    last_result.root_pc    = 0u;
    strncpy(last_result.name, "--", CHORD_NAME_LEN - 1u);
    strncpy(last_result.type, "---", sizeof(last_result.type) - 1u);
}

/* Called from microphone DMA ISR */
void CHORD_FeedSamples(const int16_t *pcm, uint32_t n)
{
    for (uint32_t i = 0u; i < n; i++) {
        chord_win[chord_idx++] = pcm[i];
        if (chord_idx >= CHORD_WIN_SIZE) {
            chord_idx   = 0u;
            chord_ready = true;
        }
    }
}

void CHORD_Process(void)
{
    if (!chord_ready) return;
    chord_ready = false;

    /* Pre-filter: attenuate inharmonic overtones before spectral analysis.
     * Specifically improves Em and G Major root detection by suppressing
     * high-frequency string inharmonics that can dominate over fundamentals. */
    chord_lpf(chord_win, CHORD_WIN_SIZE);

    /* --- Step 1: Goertzel magnitudes for 12 notes across C2-B5 --- */
    float raw[12] = {0};
    for (uint8_t oct = 0u; oct < 4u; oct++) {
        float mult = (float)(1u << oct);
        for (uint8_t n = 0u; n < 12u; n++) {
            float f = BASE_FREQ[n] * mult;
            if (f >= CHORD_FS * 0.45f) continue;
            /* Octave weight: boosts low-register fundamentals, dampens
             * high-register overtones — improves Em/G recognition */
            raw[n] += goertzel(chord_win, CHORD_WIN_SIZE, f, CHORD_FS)
                      * OCT_WEIGHT[oct];
        }
    }

    /* --- Step 2: Silence check --- */
    float total = 0.0f;
    for (uint8_t n = 0u; n < 12u; n++) total += raw[n];

    if (total < SILENCE_THRESH) {
        silence_count++;
        if (silence_count >= CHORD_SILENCE_RESET) {
            strncpy(held_name, "--", CHORD_NAME_LEN - 1u);
            strncpy(held_type, "---", sizeof(held_type) - 1u);
            held_root  = 0u;
            held_valid = false;
            strncpy(cand_name, "--", CHORD_NAME_LEN - 1u);
            cand_count = 0u;
        }
        last_result.valid      = false;
        last_result.confidence = 0.0f;
        memset(pc_mag, 0, sizeof(pc_mag));
        strncpy(last_result.name, held_name, CHORD_NAME_LEN - 1u);
        strncpy(last_result.type, held_type, sizeof(last_result.type) - 1u);
        return;
    }
    silence_count = 0u;

    /* --- Step 3: Normalise pitch-class magnitudes --- */
    for (uint8_t n = 0u; n < 12u; n++) pc_mag[n] = raw[n] / total;

    /* --- Step 4: Match restricted roots × {major, minor} --- */
    float   best_score = 0.0f;
    uint8_t best_ri    = 0u;   /* index into VALID_ROOTS */
    uint8_t best_tmpl  = 0u;

    for (uint8_t ri = 0u; ri < N_VALID_ROOTS; ri++) {
        uint8_t root = VALID_ROOTS[ri];
        for (uint8_t t = 0u; t < N_TEMPLATES; t++) {
            float score = (pc_mag[root % 12u]
                         + pc_mag[(root + TEMPLATES[t].iv[1]) % 12u]
                         + pc_mag[(root + TEMPLATES[t].iv[2]) % 12u]) / 3.0f;
            if (score > best_score) {
                best_score = score;
                best_ri    = ri;
                best_tmpl  = t;
            }
        }
    }

    /* Root-dominance guard — reject match when the root pitch class has
     * negligible energy.  This prevents false Em / G Major detections caused
     * by upper-harmonic energy accidentally scoring higher than the actual
     * fundamental.  Threshold 0.07 is relative to the normalised total. */
    if (pc_mag[VALID_ROOTS[best_ri]] < 0.07f) {
        silence_count++;
        if (silence_count >= CHORD_SILENCE_RESET) {
            strncpy(held_name, "--", CHORD_NAME_LEN - 1u);
            strncpy(held_type, "---", sizeof(held_type) - 1u);
            held_root  = 0u;
            held_valid = false;
            strncpy(cand_name, "--", CHORD_NAME_LEN - 1u);
            cand_count = 0u;
        }
        last_result.valid      = held_valid;
        last_result.confidence = 0.0f;
        strncpy(last_result.name, held_name, CHORD_NAME_LEN - 1u);
        strncpy(last_result.type, held_type, sizeof(last_result.type) - 1u);
        return;
    }

    /* Detected chord for this frame */
    char frame_name[CHORD_NAME_LEN];
    snprintf(frame_name, CHORD_NAME_LEN, "%s%s",
             ROOT_NAMES[best_ri], TEMPLATES[best_tmpl].sfx);

    /* --- Step 5: Temporal stability gate --- */
    if (strncmp(frame_name, held_name, CHORD_NAME_LEN) == 0) {
        /* Same as currently held chord — reset candidate */
        cand_count = 0u;
        strncpy(cand_name, "--", CHORD_NAME_LEN - 1u);
    } else if (strncmp(frame_name, cand_name, CHORD_NAME_LEN) == 0) {
        /* Same as running candidate — accumulate votes */
        cand_count++;
        if (cand_count >= CHORD_HOLD_WINDOWS) {
            /* Candidate promoted to held chord */
            strncpy(held_name, cand_name, CHORD_NAME_LEN - 1u);
            strncpy(held_type, TEMPLATES[best_tmpl].typename_,
                    sizeof(held_type) - 1u);
            held_root  = VALID_ROOTS[best_ri];
            held_valid = true;
            cand_count = 0u;
            strncpy(cand_name, "--", CHORD_NAME_LEN - 1u);
        }
    } else {
        /* New candidate — start fresh vote */
        strncpy(cand_name, frame_name, CHORD_NAME_LEN - 1u);
        cand_count = 1u;
    }

    /* --- Step 6: Build result from held state --- */
    last_result.root_pc    = held_root;
    last_result.confidence = best_score;
    last_result.valid      = held_valid;
    strncpy(last_result.name, held_name, CHORD_NAME_LEN - 1u);
    strncpy(last_result.type, held_type, sizeof(last_result.type) - 1u);
}

ChordResult_t CHORD_GetResult(void) { return last_result; }

void CHORD_GetPCMagnitudes(float *out_12)
{
    for (uint8_t i = 0u; i < 12u; i++) out_12[i] = pc_mag[i];
}
