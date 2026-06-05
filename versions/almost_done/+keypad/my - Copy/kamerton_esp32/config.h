#pragma once
#include <Arduino.h>

// ─── Pin Definitions ──────────────────────────────────────────────────────────
// TTP229 16-key capacitive keyboard (2-wire serial)
#define PIN_TTP_SCL     16
#define PIN_TTP_SDO     17

// I2C LCD 20x4
#define PIN_LCD_SDA     21
#define PIN_LCD_SCL     22
#define LCD_I2C_ADDR    0x27

// SD Card (VSPI bus)
#define PIN_SD_MOSI     23
#define PIN_SD_MISO     19
#define PIN_SD_SCK      18
#define PIN_SD_CS        5

// Joystick (analog + digital)
#define PIN_JOY_X       34   // ADC1_CH6 (input-only capable)
#define PIN_JOY_Y       35   // ADC1_CH7
#define PIN_JOY_BTN     32

// MAX9814 microphone (analog output)
#define PIN_MIC_OUT     36   // VP / ADC1_CH0 (input-only)

// Status LED
#define PIN_LED          2

// ─── Audio Constants ──────────────────────────────────────────────────────────
#define SAMPLE_RATE         44100   // A2DP BT output rate
#define REC_SAMPLE_RATE     16000   // Recording / ADC sample rate
#define ADC_CENTER          2048    // ADC midpoint (12-bit @ 3.3V)
#define ADC_TO_PCM_SCALE      16    // 12-bit → 16-bit headroom factor

// ─── Modes ────────────────────────────────────────────────────────────────────
enum AppMode : uint8_t {
    MODE_GENERATOR = 0,
    MODE_TUNER,
    MODE_ARPEGGIO,
    MODE_WARMUP,
    MODE_RECORDER,
    MODE_SAMPLER,
    MODE_COUNT
};

// ─── Counts ───────────────────────────────────────────────────────────────────
#define NUM_NOTES           7
#define NUM_OCTAVES         5
#define NUM_TIMBRES         5
#define NUM_WARMUP_SEQS     7
#define NUM_ARPEGGIOS       7
#define MAX_REC_SLOTS      20
#define MAX_SAMPLE_PADS    15

// ─── Timing ───────────────────────────────────────────────────────────────────
#define MAX_REC_SECONDS     8
#define MAX_SAMPLE_SECONDS  4
// Max samples = 8s * 16000 Hz = 128 000 words = 256 KB  (fits in ESP32 PSRAM or IRAM)
#define MAX_REC_SAMPLES    (MAX_REC_SECONDS * REC_SAMPLE_RATE)
#define MAX_SMP_SAMPLES    (MAX_SAMPLE_SECONDS * REC_SAMPLE_RATE)

// ─── Tables ───────────────────────────────────────────────────────────────────
// Solfège note frequencies in octave 4 (C4-B4)
static const float NOTE_FREQS_OCT4[NUM_NOTES] = {
    261.63f,  // Do  (C4)
    293.66f,  // Re  (D4)
    329.63f,  // Mi  (E4)
    349.23f,  // Fa  (F4)
    392.00f,  // Sol (G4)
    440.00f,  // La  (A4)
    493.88f   // Si  (B4)
};

static const char* NOTE_NAMES[NUM_NOTES] = {
    "Do", "Re", "Mi", "Fa", "Sol", "La", "Si"
};

static const char* TIMBRE_NAMES[NUM_TIMBRES] = {
    "Sine", "Square", "Sawtooth", "Clarinet", "Organ"
};

static const char* MODE_NAMES[MODE_COUNT] = {
    "Generator", "Tuner", "Arpeggio", "Warmup", "Recorder", "Sampler"
};

static const char* WARMUP_NAMES[NUM_WARMUP_SEQS] = {
    "Scale Up", "Scale Down", "Triad Up", "Triad Down",
    "Octave Jump", "5th Jump",  "Full Scale"
};

// Diatonic C-major chords (3-note voicing, note indices 0-6)
static const char* ARPEGGIO_NAMES[NUM_ARPEGGIOS] = {
    "C major", "D minor", "E minor", "F major",
    "G major", "A minor", "B dim"
};

// Each arpeggio: 3 scale-degree offsets from root
static const uint8_t ARPEGGIO_NOTES[NUM_ARPEGGIOS][3] = {
    {0, 2, 4},  // C  E  G
    {1, 3, 5},  // D  F  A
    {2, 4, 6},  // E  G  B
    {3, 5, 0},  // F  A  C
    {4, 6, 1},  // G  B  D
    {5, 0, 2},  // A  C  E
    {6, 1, 3},  // B  D  F
};

// Warmup sequences: each row = note indices (0-6), terminated by 0xFF
// All indices must be 0-6 (notes Do..Si within octave).
static const uint8_t WARMUP_SEQ[NUM_WARMUP_SEQS][14] = {
    {0,1,2,3,4,5,6,0xFF,0,0,0,0,0,0},   // Scale Up
    {6,5,4,3,2,1,0,0xFF,0,0,0,0,0,0},   // Scale Down
    {0,2,4,0,2,4,0xFF,0,0,0,0,0,0,0},   // Triad Up
    {4,2,0,4,2,0,0xFF,0,0,0,0,0,0,0},   // Triad Down
    {0,4,0,4,0,4,0xFF,0,0,0,0,0,0,0},   // 5th Jump
    {0,2,4,2,0,2,4,0xFF,0,0,0,0,0,0},   // Arpeggio pattern
    {0,1,2,3,4,5,6,5,4,3,2,1,0,0xFF}    // Full Scale up+down
};
