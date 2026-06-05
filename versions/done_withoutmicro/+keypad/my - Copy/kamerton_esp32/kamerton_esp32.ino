/*  Kamerton ESP32
 *  ──────────────────────────────────────────────────────────────────────────
 *  Hardware:
 *    • ESP-32 DevKit V1 (ESP-WROOM-32)
 *    • TTP229-BSF 16-key capacitive keyboard (2-wire serial, 16-key mode)
 *    • Joystick module (2× ADC + 1× digital)
 *    • I2C LCD 20×4
 *    • SD card module (SPI)
 *    • MAX9814 microphone (analog)
 *    • Bluetooth A2DP → wireless headphones  (built into ESP32)
 *
 *  Required libraries (Arduino Library Manager):
 *    • "ESP32-A2DP"         by pschatzmann
 *    • "LiquidCrystal I2C"  by Frank de Brabander
 *    • "SdFat"              by Bill Greiman  (підтримує FAT32 і exFAT / 64 ГБ)
 *    • "Preferences" (built-in / ESP32 Arduino core)
 *
 *  Pin summary:  see config.h
 *  ──────────────────────────────────────────────────────────────────────────
 */

#include "config.h"
#include "settings.h"
#include "ttp229.h"
#include "joystick.h"
#include "audio_synth.h"
#include "bt_audio.h"
#include "sd_recorder.h"
#include "sampler.h"
#include "display_mgr.h"
#include <math.h>

// ─── Global objects ──────────────────────────────────────────────────────────
static TTP229   kbd(PIN_TTP_SCL, PIN_TTP_SDO);
static Joystick joy(PIN_JOY_X, PIN_JOY_Y, PIN_JOY_BTN);

// ─── ADC recording timer ─────────────────────────────────────────────────────
static hw_timer_t* g_rec_timer = nullptr;

void IRAM_ATTR onRecTimer() {
    g_sdrec.onTimerISR();
}

// ─── App state ───────────────────────────────────────────────────────────────
static AppMode g_mode;
static uint32_t g_last_disp_ms = 0;
static uint32_t g_last_save_ms = 0;

// ─── Per-mode state ──────────────────────────────────────────────────────────

// Generator
static uint8_t  g_note   = 0;   // 0-6
static uint8_t  g_octave = 3;
static uint8_t  g_timbre = 0;
static uint8_t  g_volume = 70;
static bool     g_muted  = false;

// Arpeggio
static uint8_t  g_arp_idx    = 0;
static int8_t   g_arp_trans  = 0;
static bool     g_arp_playing = false;
static uint8_t  g_arp_step   = 0;
static uint32_t g_arp_next_ms = 0;
static const uint32_t ARP_STEP_MS = 800;

// Warmup
static uint8_t  g_wu_seq   = 0;
static uint8_t  g_wu_step  = 0;
static bool     g_wu_playing = false;
static uint32_t g_wu_next_ms = 0;
static const uint32_t WU_NOTE_MS = 600;
static const uint32_t WU_GAP_MS  = 100;
static bool     g_wu_gap = false;

// Tuner
static float    g_tuner_freq  = 0.0f;
static float    g_tuner_cents = 0.0f;
static const char* g_tuner_hint = "";
// Autocorrelation buffer
static int16_t  g_mic_buf[1024];
static uint16_t g_mic_head = 0;
static bool     g_mic_ready = false;
// Timer for tuner/chord ADC sampling
static hw_timer_t* g_mic_timer = nullptr;

void IRAM_ATTR onMicTimer() {
    int raw = analogRead(PIN_MIC_OUT);
    int16_t s = (int16_t)((raw - ADC_CENTER) * ADC_TO_PCM_SCALE);
    g_mic_buf[g_mic_head++] = s;
    if (g_mic_head >= 1024) { g_mic_head = 0; g_mic_ready = true; }
}

// Recorder
static RecMeta  g_rec_list[MAX_REC_SLOTS];
static int      g_rec_count  = 0;
static int      g_rec_cursor = 0;
static int16_t  g_play_buf[MAX_REC_SAMPLES];

// ─── Helper: pitch detection via autocorrelation ─────────────────────────────
static float detectPitch(const int16_t* buf, int len, int sample_rate) {
    int min_lag = sample_rate / 1000;    // 1000 Hz max
    int max_lag = sample_rate / 50;      // 50 Hz min
    if (max_lag > len / 2) max_lag = len / 2;

    long best_lag = -1;
    long best_corr = LONG_MIN;
    for (int lag = min_lag; lag <= max_lag; lag++) {
        long corr = 0;
        for (int i = 0; i < len - lag; i++)
            corr += (long)buf[i] * buf[i + lag];
        if (corr > best_corr) { best_corr = corr; best_lag = lag; }
    }
    if (best_lag <= 0 || best_corr < 50000000L) return 0.0f;
    return (float)sample_rate / (float)best_lag;
}

// Find nearest standard guitar string frequency and return cents offset
static float freqToCents(float freq, const char** hint_out) {
    static const float STRINGS[] = {82.41f,110.0f,146.83f,196.0f,246.94f,329.63f};
    static const char* STR_NAMES[] = {"E2","A2","D3","G3","B3","E4"};
    if (freq <= 0.0f) { *hint_out = ""; return 0.0f; }
    float best_cents = 9999.0f;
    int best_i = 0;
    for (int i = 0; i < 6; i++) {
        float cents = 1200.0f * log2f(freq / STRINGS[i]);
        if (fabsf(cents) < fabsf(best_cents)) { best_cents = cents; best_i = i; }
    }
    (void)STR_NAMES[best_i];
    if      (best_cents >  5)  *hint_out = "LOWER dn!";
    else if (best_cents < -5)  *hint_out = "RAISE up!";
    else                       *hint_out = "IN TUNE!";
    return best_cents;
}

// ─── Mode: Generator ─────────────────────────────────────────────────────────
static void handleGenerator(bool keys[17]) {
    for (int n = 1; n <= 7; n++) {
        if (keys[n]) {
            g_note = n - 1;
            g_synth.setTimbre(g_timbre);
            g_synth.setVolume(g_volume);
            g_synth.setMuted(g_muted);
            g_synth.setNote(g_note, g_octave);
        }
    }
    if (keys[8] && g_octave < NUM_OCTAVES - 1) { g_octave++; g_synth.setNote(g_note, g_octave); }
    if (keys[9] && g_octave > 0)               { g_octave--; g_synth.setNote(g_note, g_octave); }
    if (keys[10] && g_volume < 100) { g_volume += 5; g_synth.setVolume(g_volume); }
    if (keys[11] && g_volume > 0)   { g_volume -= 5; g_synth.setVolume(g_volume); }
    if (keys[12]) {
        g_timbre = (g_timbre + 1) % NUM_TIMBRES;
        g_synth.setTimbre(g_timbre);
    }
    if (keys[13]) {
        g_muted = !g_muted;
        g_synth.setMuted(g_muted);
    }
}

// ─── Mode: Arpeggio ───────────────────────────────────────────────────────────
static void handleArpeggio(bool keys[17]) {
    for (int i = 1; i <= 7; i++) {
        if (keys[i]) {
            g_arp_idx = i - 1;
            g_arp_playing = true;
            g_arp_step    = 0;
            g_arp_next_ms = millis();
        }
    }
    if (keys[8])  g_arp_trans = (g_arp_trans < 6)  ? g_arp_trans + 1 : g_arp_trans;
    if (keys[9])  g_arp_trans = (g_arp_trans > -6) ? g_arp_trans - 1 : g_arp_trans;
    if (keys[10] && g_volume < 100) { g_volume += 5; g_synth.setVolume(g_volume); }
    if (keys[11] && g_volume > 0)   { g_volume -= 5; g_synth.setVolume(g_volume); }
    if (keys[12]) { g_arp_playing = !g_arp_playing; if (!g_arp_playing) g_synth.stopNote(); }
}

static void taskArpeggio() {
    if (!g_arp_playing) return;
    if (millis() < g_arp_next_ms) return;

    if (g_arp_step >= 3) { g_arp_step = 0; }

    uint8_t base_note = ARPEGGIO_NOTES[g_arp_idx][g_arp_step];
    int note = ((int)base_note + g_arp_trans + 70) % NUM_NOTES;
    g_synth.setVolume(g_volume);
    g_synth.setTimbre(g_timbre);
    g_synth.setNote((uint8_t)note, g_octave);

    g_arp_step++;
    g_arp_next_ms = millis() + ARP_STEP_MS;
}

// ─── Mode: Warmup ─────────────────────────────────────────────────────────────
static void handleWarmup(bool keys[17]) {
    for (int i = 1; i <= 7; i++) {
        if (keys[i]) { g_wu_seq = i - 1; }
    }
    if (keys[8] && g_octave < NUM_OCTAVES - 1) g_octave++;
    if (keys[9] && g_octave > 0)               g_octave--;
    if (keys[10]) {
        g_wu_playing = true;
        g_wu_step    = 0;
        g_wu_next_ms = millis();
        g_wu_gap     = false;
    }
    if (keys[11]) {
        g_wu_playing = false;
        g_synth.stopNote();
    }
    // Joystick UP/DOWN handled in main loop for sequence selection
}

static void taskWarmup() {
    if (!g_wu_playing) return;
    if (millis() < g_wu_next_ms) return;

    const uint8_t* seq = WARMUP_SEQ[g_wu_seq % NUM_WARMUP_SEQS];

    if (g_wu_gap) {
        g_synth.stopNote();
        g_wu_gap      = false;
        g_wu_next_ms  = millis() + WU_GAP_MS;
        return;
    }

    uint8_t ni = seq[g_wu_step];
    if (ni == 0xFF) {
        g_wu_playing = false;
        g_synth.stopNote();
        return;
    }

    uint8_t note_idx = ni % NUM_NOTES;
    g_synth.setVolume(g_volume);
    g_synth.setTimbre(g_timbre);
    g_synth.setNote(note_idx, g_octave);

    g_wu_step++;
    g_wu_gap     = true;
    g_wu_next_ms = millis() + WU_NOTE_MS;
}

// ─── Mode: Recorder ──────────────────────────────────────────────────────────
static void refreshRecList() {
    g_rec_count = g_sdrec.scanFiles("REC_", g_rec_list, MAX_REC_SLOTS);
    if (g_rec_cursor >= g_rec_count) g_rec_cursor = (g_rec_count > 0) ? g_rec_count - 1 : 0;
}

static void handleRecorder(bool keys[17]) {
    if (keys[1]) {
        if (!g_sdrec.isRecording()) {
            // Start recording to a new slot
            int slot = g_sdrec.nextSlotNumber("REC_");
            if (slot > 0) {
                char fname[16];
                snprintf(fname, sizeof(fname), "REC_%03d.WAV", slot);
                // Timer-driven ADC recording
                timerAlarmEnable(g_rec_timer);
                g_sdrec.startRec(fname);
            }
        } else {
            // Stop recording
            timerAlarmDisable(g_rec_timer);
            g_sdrec.stopRec();
            refreshRecList();
        }
    }

    if (keys[2] && !g_sdrec.isRecording()) {
        // Play selected recording
        if (g_rec_count > 0 && g_rec_cursor < g_rec_count) {
            uint32_t ns, sr;
            bool ok = g_sdrec.loadWav(g_rec_list[g_rec_cursor].filename,
                                       g_play_buf, MAX_REC_SAMPLES, ns, sr);
            if (ok) {
                g_synth.setPlayback(g_play_buf, ns, sr);
                g_synth.startPlayback();
            }
        }
    }

    if (keys[3]) {
        timerAlarmDisable(g_rec_timer);
        g_sdrec.stopRec();
        g_synth.stopPlayback();
        refreshRecList();
    }

    // Quick select slots 4-7
    for (int i = 4; i <= 7; i++) {
        if (keys[i] && (i - 4) < g_rec_count) {
            g_rec_cursor = i - 4;
        }
    }
}

// ─── Mode: Sampler ───────────────────────────────────────────────────────────
static void handleSampler(bool keys[17], bool keys_just_pressed[17]) {
    bool pads[MAX_SAMPLE_PADS];
    for (int i = 0; i < MAX_SAMPLE_PADS; i++) pads[i] = keys_just_pressed[i + 1];
    g_sampler.update(pads, keys[16]);
    g_sdrec.flushToSD();   // flush if sampler is recording
}

// ─── Mode: Tuner ─────────────────────────────────────────────────────────────
static void taskTuner() {
    if (!g_mic_ready) return;
    g_mic_ready = false;

    float freq = detectPitch(g_mic_buf, 1024, REC_SAMPLE_RATE);
    if (freq > 50.0f) {
        g_tuner_freq  = freq;
        g_tuner_cents = freqToCents(freq, &g_tuner_hint);
    }
}

// ─── Mode switching ───────────────────────────────────────────────────────────
static void onModeSwitch() {
    g_arp_playing = false;
    g_wu_playing  = false;
    g_synth.stopNote();
    g_synth.stopPlayback();
    g_sdrec.stopRec();
    g_sampler.stopAll();

    g_mode = (AppMode)((g_mode + 1) % MODE_COUNT);

    if (g_mode == MODE_RECORDER) refreshRecList();
    if (g_mode == MODE_SAMPLER)  g_sampler.begin();
    if (g_mode == MODE_TUNER)    g_tuner_freq = 0.0f;

    // Save mode to NVS immediately on switch
    g_settings.cfg.last_mode = g_mode;
    g_settings.save();
}

// ─── Joystick dispatch ────────────────────────────────────────────────────────
static void handleJoystick() {
    JoyDir d = joy.dir();
    bool   btn = joy.btnPressed();

    switch (g_mode) {
        case MODE_WARMUP:
            if (d == JOY_UP   && g_wu_seq > 0)                        g_wu_seq--;
            if (d == JOY_DOWN && g_wu_seq < NUM_WARMUP_SEQS - 1)      g_wu_seq++;
            if (d == JOY_LEFT  && g_octave > 0)                        g_octave--;
            if (d == JOY_RIGHT && g_octave < NUM_OCTAVES - 1)          g_octave++;
            if (btn) {
                g_wu_playing = true; g_wu_step = 0;
                g_wu_next_ms = millis(); g_wu_gap = false;
            }
            break;

        case MODE_RECORDER:
            if (d == JOY_UP   && g_rec_cursor > 0)              g_rec_cursor--;
            if (d == JOY_DOWN && g_rec_cursor < g_rec_count-1)  g_rec_cursor++;
            if (btn && g_rec_count > 0) {
                // Play highlighted recording
                uint32_t ns, sr;
                bool ok = g_sdrec.loadWav(g_rec_list[g_rec_cursor].filename,
                                           g_play_buf, MAX_REC_SAMPLES, ns, sr);
                if (ok) { g_synth.setPlayback(g_play_buf, ns, sr); g_synth.startPlayback(); }
            }
            break;

        case MODE_SAMPLER:
            if (d == JOY_UP)    g_sampler.onUp();
            if (d == JOY_DOWN)  g_sampler.onDown();
            if (d == JOY_LEFT)  g_sampler.onLeft();
            if (d == JOY_RIGHT) g_sampler.onRight();
            if (btn)            g_sampler.onBtnClick();
            break;

        case MODE_ARPEGGIO:
            if (d == JOY_UP   && g_arp_idx > 0)               g_arp_idx--;
            if (d == JOY_DOWN && g_arp_idx < NUM_ARPEGGIOS-1) g_arp_idx++;
            break;

        case MODE_GENERATOR:
            if (d == JOY_UP   && g_octave < NUM_OCTAVES - 1) { g_octave++; g_synth.setNote(g_note, g_octave); }
            if (d == JOY_DOWN && g_octave > 0)               { g_octave--; g_synth.setNote(g_note, g_octave); }
            if (d == JOY_LEFT  && g_volume > 0)   { g_volume -= 5; g_synth.setVolume(g_volume); }
            if (d == JOY_RIGHT && g_volume < 100) { g_volume += 5; g_synth.setVolume(g_volume); }
            break;

        default:
            break;
    }
}

// ─── Display update ───────────────────────────────────────────────────────────
static void updateDisplay() {
    g_display.update(g_mode);

    switch (g_mode) {
        case MODE_GENERATOR:
            g_display.showGenerator(g_note, g_octave, g_timbre,
                                    g_volume, g_muted, g_bt.isConnected());
            break;
        case MODE_TUNER:
            g_display.showTuner(g_tuner_freq, g_tuner_cents, g_tuner_hint);
            break;
        case MODE_ARPEGGIO:
            g_display.showArpeggio(g_arp_idx, g_arp_step, g_arp_playing, g_arp_trans);
            break;
        case MODE_WARMUP:
            {
                uint8_t cur_note = 0;
                if (g_wu_playing && g_wu_step > 0) {
                    uint8_t ni = WARMUP_SEQ[g_wu_seq][g_wu_step > 0 ? g_wu_step-1 : 0];
                    if (ni != 0xFF) cur_note = ni % NUM_NOTES;
                }
                g_display.showWarmup(g_wu_seq, g_wu_step, g_wu_playing, cur_note, g_octave);
            }
            break;
        case MODE_RECORDER:
            g_display.showRecorder(g_sdrec.isRecording(), !g_synth.isPlaybackDone(),
                                   g_sdrec.recElapsedMs(),
                                   g_rec_list, g_rec_count, g_rec_cursor);
            break;
        case MODE_SAMPLER:
            g_display.showSampler(g_sampler);
            break;
        default:
            break;
    }
}

// ─── Periodic settings save ───────────────────────────────────────────────────
static void periodicSave() {
    if (millis() - g_last_save_ms < 10000) return;   // save every 10 s
    g_last_save_ms = millis();

    g_settings.cfg.last_mode    = g_mode;
    g_settings.cfg.volume       = g_volume;
    g_settings.cfg.octave       = g_octave;
    g_settings.cfg.timbre       = g_timbre;
    g_settings.cfg.warmup_seq   = g_wu_seq;
    g_settings.cfg.arpeggio_idx = g_arp_idx;
    g_settings.save();
}

// ─── setup() ─────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, HIGH);

    // Display on first so user sees boot screen
    Wire.begin(PIN_LCD_SDA, PIN_LCD_SCL);
    g_display.begin();
    g_display.showBoot();

    // Settings (NVS)
    g_settings.begin();
    g_mode   = g_settings.cfg.last_mode;
    g_volume = g_settings.cfg.volume;
    g_octave = g_settings.cfg.octave;
    g_timbre = g_settings.cfg.timbre;
    g_wu_seq = g_settings.cfg.warmup_seq;
    g_arp_idx= g_settings.cfg.arpeggio_idx;

    // SD card (SdFat керує SPI сам — SPI.begin тут не потрібен)
    if (!g_sdrec.begin(PIN_SD_CS)) {
        g_display.showMessage("SD card FAILED!", "Insert SD & reset");
        delay(3000);
    }

    // Keyboard + joystick
    kbd.begin();
    joy.begin();

    // Synth defaults
    g_synth.setVolume(g_volume);
    g_synth.setTimbre(g_timbre);

    // ADC recording timer (16 kHz).
    // Runs always; onTimerISR() exits immediately when not recording.
    g_rec_timer = timerBegin(0, 80, true);   // 80 prescaler → 1 µs ticks
    timerAttachInterrupt(g_rec_timer, &onRecTimer, true);
    timerAlarmWrite(g_rec_timer, 1000000 / REC_SAMPLE_RATE, true);
    timerAlarmEnable(g_rec_timer);

    // Microphone timer for tuner (same rate, separate timer — also always on).
    g_mic_timer = timerBegin(1, 80, true);
    timerAttachInterrupt(g_mic_timer, &onMicTimer, true);
    timerAlarmWrite(g_mic_timer, 1000000 / REC_SAMPLE_RATE, true);
    timerAlarmEnable(g_mic_timer);

    // Bluetooth A2DP
    g_bt.begin(g_settings.cfg.bt_device);

    g_display.showMessage("Starting BT...", g_settings.cfg.bt_device);
    delay(1500);

    // Sampler: load pad info from SD
    if (g_mode == MODE_SAMPLER) g_sampler.begin();

    // Seed initial note
    g_synth.setNote(g_note, g_octave);
    g_synth.setActive(true);

    digitalWrite(PIN_LED, LOW);
}

// ─── loop() ──────────────────────────────────────────────────────────────────
void loop() {
    // ── Read hardware ────────────────────────────────────────────────────────
    kbd.update();
    joy.update();

    // Build edge-triggered key arrays
    bool keys_pressed[17]       = {};   // justPressed this frame
    bool keys_held[17]          = {};   // currently held

    for (int k = 1; k <= 16; k++) {
        keys_pressed[k] = kbd.justPressed(k);
        keys_held[k]    = kbd.isPressed(k);
    }

    // Button 16 = mode switch on short tap; long hold = modifier (sampler REC).
    // Track press time to distinguish tap (<500 ms) from hold.
    static uint32_t btn16_press_ms = 0;
    if (kbd.justPressed(16))  btn16_press_ms = millis();
    if (kbd.justReleased(16) && !g_sampler.isRecording()) {
        if ((millis() - btn16_press_ms) < 500) {
            onModeSwitch();
        }
    }

    // ── Mode dispatch ────────────────────────────────────────────────────────
    switch (g_mode) {
        case MODE_GENERATOR: handleGenerator(keys_pressed); break;
        case MODE_ARPEGGIO:  handleArpeggio(keys_pressed);  break;
        case MODE_WARMUP:    handleWarmup(keys_pressed);     break;
        case MODE_RECORDER:  handleRecorder(keys_pressed);   break;
        case MODE_SAMPLER:   handleSampler(keys_held, keys_pressed); break;
        case MODE_TUNER:     /* passive — timer does the work */  break;
        default: break;
    }

    // ── Background tasks ────────────────────────────────────────────────────
    taskArpeggio();
    taskWarmup();
    taskTuner();

    if (g_sdrec.isRecording()) g_sdrec.flushToSD();

    // ── Joystick events ──────────────────────────────────────────────────────
    if (joy.anyEvent()) handleJoystick();

    // ── Display (refreshed at ~10 Hz) ────────────────────────────────────────
    if (millis() - g_last_disp_ms >= 100) {
        g_last_disp_ms = millis();
        updateDisplay();
    }

    // ── Status LED ──────────────────────────────────────────────────────────
    //   ON = BT connected; fast blink = recording; slow blink = playing
    uint32_t now = millis();
    if (g_sdrec.isRecording() || g_sampler.isRecording()) {
        digitalWrite(PIN_LED, (now / 125) & 1);   // 4 Hz blink
    } else if (!g_synth.isPlaybackDone()) {
        digitalWrite(PIN_LED, (now / 500) & 1);   // 1 Hz blink
    } else {
        digitalWrite(PIN_LED, g_bt.isConnected() ? HIGH : LOW);
    }

    // ── Periodic NVS save ────────────────────────────────────────────────────
    periodicSave();
}
