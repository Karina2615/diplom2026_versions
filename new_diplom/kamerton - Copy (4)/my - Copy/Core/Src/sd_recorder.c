/*
 * sd_recorder.c — SD-card based multi-slot voice recorder
 *
 * Uses FatFS (SPI-based SD card, SPI2, PB12=CS).
 * Files are stored as raw signed 16-bit PCM at 16 kHz in:
 *   0:/RECORDINGS/REC_NNN.PCM
 *
 * Two recording modes:
 *   • SDRec_SaveFromRecorder()  — saves the RAM recorder buffer to SD (≤4 s)
 *   • SDRec_LiveStart/Feed/Stop — streams microphone data directly to SD,
 *                                 supporting 60+ second recordings.
 *
 * All public functions are called from the main loop (not ISR-safe).
 */

#include "sd_recorder.h"
#include "recorder.h"
#include "microphone.h"
#include "fatfs.h"
#include "ff.h"
#include "spi_diskio.h"
#include <string.h>
#include <stdio.h>

/* ── FatFS handles (from fatfs.c) ───────────────────────────────────────── */
extern FATFS SDFatFS;
extern char  SDPath[4];

/* ── Yield callback (called during long SD operations) ─────────────────── */
static SDRec_YieldFn_t s_yield = NULL;

void SDRec_SetYield(SDRec_YieldFn_t fn)
{
    s_yield = fn;
}

static inline void sd_yield(void)
{
    if (s_yield) s_yield();
}

/* ── Module state ───────────────────────────────────────────────────────── */
static bool          s_mounted = false;
static uint8_t       s_count   = 0u;
static SDRec_Entry_t s_entries[SD_REC_MAX];

/* ── Live streaming state ───────────────────────────────────────────────── */
static bool     s_live_active = false;
static FIL      s_live_file;
static uint32_t s_live_bytes  = 0u;
/* Staging buffer: accumulate 1024 samples (2 KB) before each SD write.
 * At 9070 Hz / 1024-sample DMA half-buffer this flushes every ~113 ms.
 * The file is pre-allocated with f_expand so cluster-boundary FAT writes
 * never stall during streaming — see SDRec_LiveStart(). */
#define LIVE_STAGE_SAMPLES  1024u
static int16_t  s_live_stage[LIVE_STAGE_SAMPLES];
static uint16_t s_live_stage_n = 0u;
static char     s_live_fname[16];   /* filename of current live file */

/* ── Recorder-only noise cleanup (applies to the SAVED .PCM only) ─────────── *
 * The chord/pitch detectors and the sampler read the unfiltered mic stream    *
 * (microphone.c → CHORD_FeedSamples / REC_FeedSamples), so none of them are   *
 * touched by this.  Two stages run per sample inside SDRec_LiveFeed():        *
 *   1) DC blocker  — 1st-order HPF (~7 Hz corner @ 9070 Hz) removes the mic's *
 *      DC bias and sub-audible rumble that the ×8 gain would otherwise amplify.*
 *   2) Soft noise gate — a peak envelope (instant attack / slow release)      *
 *      drives a soft downward expander.  During genuine silence the gain eases*
 *      to GATE_FLOOR to suppress the AGC hiss; it opens instantly on real     *
 *      sound and the slow release leaves note decays intact (no pumping).     */
#define DC_R          0.995f   /* DC-blocker pole (~7 Hz corner @ 9070 Hz)     */
#define GATE_SHUT     200.0f   /* envelope below this → full attenuation       */
#define GATE_OPEN     800.0f   /* envelope above this → unity gain             */
#define GATE_FLOOR    0.30f    /* residual gain in silence (-10.5 dB)          */
#define GATE_REL      0.0008f  /* release coefficient (~140 ms tail)           */
#define GATE_GSMOOTH  0.006f   /* applied-gain slew (~18 ms) — no zipper noise */
static float s_dc_x1   = 0.0f; /* DC blocker: previous input  x[n-1]          */
static float s_dc_y1   = 0.0f; /* DC blocker: previous output y[n-1]          */
static float s_gate_env = 0.0f;/* gate envelope follower                       */
static float s_gate_g   = 1.0f;/* smoothed applied gain                        */

/* ── Hold buffer: captures DMA data that arrives during an SD flush ─────── *
 * The DMA fires every ~56 ms (512 samples @ 9070 Hz).  A long SD write     *
 * (cluster boundary ~30-50 ms) may happen during that window.  live_flush() *
 * calls hold_mic_data() after f_write returns, which snapshots whatever DMA *
 * data arrived, then drains it into the staging buffer — zero data loss.    */
#define LIVE_HOLD_SAMPLES  1024u
static int16_t  s_hold_buf[LIVE_HOLD_SAMPLES];
static uint16_t s_hold_n = 0u;

static void hold_mic_data(void)
{
    if (!s_live_active) return;
    uint16_t avail = 0u;
    const int16_t *mic = MIC_GetBuffer(&avail);
    if (mic && avail && avail <= LIVE_HOLD_SAMPLES) {
        memcpy(s_hold_buf, mic, (size_t)(avail * 2u));
        s_hold_n = avail;
    }
}

/* ── Mount error code (for UI diagnostics) ─────────────────────────────── */
static FRESULT s_mount_err = FR_OK;

FRESULT SDRec_GetMountError(void) { return s_mount_err; }

/* ── Internal: mount if not already mounted ─────────────────────────────── */
/* NOTE: called from the main loop — must NOT block (no HAL_Delay).        */
static bool ensure_mounted(void)
{
    if (s_mounted) return true;
    s_mount_err = f_mount(&SDFatFS, SDPath, 1);
    if (s_mount_err == FR_OK) {
        s_mounted = true;
        return true;
    }
    return false;
}

/* ── Format SD card as FAT32 and re-mount ───────────────────────────────── */
bool SDRec_Format(void)
{
    /* Force dismount first */
    f_mount(NULL, SDPath, 0);
    s_mounted = false;

    /* Allow SPI_Initialize() to try again — by default it fast-fails after the
     * first attempt to prevent main-loop blocking.  Here the user explicitly
     * requested a format, so one real attempt is appropriate. */
    SPI_ResetInit();

    /* Work buffer — 8 sectors; larger buffer speeds up format.            */
    static uint8_t work[4096];

    /* FM_FAT32  — force FAT32 (ignores volume size).
     * au=32768  — explicit 32 KB cluster size; auto-select (0) can abort
     *             on some card sizes when the computed cluster count falls
     *             outside the FAT32 valid range.  32 KB works for any card
     *             from ~64 MB up to 2 TB and matches Windows behaviour on
     *             large cards.                                             */
    FRESULT fr = f_mkfs(SDPath, FM_FAT32, 32768, work, sizeof(work));
    if (fr != FR_OK) return false;

    /* Re-mount after format */
    if (!ensure_mounted()) return false;

    f_mkdir(SD_REC_DIR_PATH);
    SDRec_Scan();
    return true;
}

/* ── Init ───────────────────────────────────────────────────────────────── */
void SDRec_Init(void)
{
    s_mounted      = false;
    s_count        = 0u;
    s_live_active  = false;
    s_live_bytes   = 0u;
    s_live_stage_n = 0u;
    memset(s_entries, 0, sizeof(s_entries));

    /* Boot-time mount with a bounded retry.
     * The SPI disk layer latches "already tried" after ONE attempt
     * (spi_diskio.c: s_init_tried) so the main loop never blocks re-initialising
     * an absent card.  Side effect: if the card is merely slow to finish
     * power-up at boot, that single failed attempt would mark it "no card" for
     * the entire session even though it is physically present and healthy.
     * Boot is the one place we can afford to block briefly, so clear the latch
     * and give the card several real attempts before giving up. */
    for (int attempt = 0; attempt < 5; attempt++) {
        if (ensure_mounted()) break;                 /* mounted OK             */
        if (s_mount_err != FR_NOT_READY &&
            s_mount_err != FR_DISK_ERR) break;       /* card responds (e.g. not
                                                        FAT32) — retry useless  */
        SPI_ResetInit();                             /* clear one-shot latch    */
        HAL_Delay(120);                              /* let the card power up    */
    }

    if (!s_mounted) return;

    f_mkdir(SD_REC_DIR_PATH);   /* create directory, ignore FR_EXIST */
    SDRec_Scan();
}

/* ── Scan ───────────────────────────────────────────────────────────────── */
bool SDRec_Scan(void)
{
    if (!ensure_mounted()) return false;

    s_count = 0u;
    memset(s_entries, 0, sizeof(s_entries));

    DIR     dir;
    FILINFO fno;

    if (f_opendir(&dir, SD_REC_DIR_PATH) != FR_OK) {
        f_mkdir(SD_REC_DIR_PATH);
        if (f_opendir(&dir, SD_REC_DIR_PATH) != FR_OK) return false;
    }

    while (s_count < SD_REC_MAX) {
        FRESULT res = f_readdir(&dir, &fno);
        if (res != FR_OK || fno.fname[0] == '\0') break;
        if (fno.fattrib & AM_DIR) continue;

        char *dot = strrchr(fno.fname, '.');
        if (!dot || strcmp(dot, ".PCM") != 0) continue;

        size_t nlen = strlen(fno.fname);
        if (nlen >= sizeof(s_entries[0].fname))
            nlen = sizeof(s_entries[0].fname) - 1u;

        memcpy(s_entries[s_count].fname, fno.fname, nlen);
        s_entries[s_count].fname[nlen] = '\0';
        s_entries[s_count].n_samples   = (uint32_t)(fno.fsize / 2u);
        s_count++;
    }

    f_closedir(&dir);

    /* Sort by filename (insertion sort, ascending) */
    for (uint8_t i = 1u; i < s_count; i++) {
        SDRec_Entry_t tmp = s_entries[i];
        int8_t j = (int8_t)(i - 1);
        while (j >= 0 && strcmp(s_entries[j].fname, tmp.fname) > 0) {
            s_entries[j + 1u] = s_entries[j];
            j--;
        }
        s_entries[j + 1u] = tmp;
    }

    return true;
}

/* ── Getters ────────────────────────────────────────────────────────────── */
uint8_t SDRec_GetCount(void)  { return s_count;  }
bool    SDRec_IsMounted(void) { return s_mounted; }

const SDRec_Entry_t *SDRec_GetEntry(uint8_t idx)
{
    return (idx < s_count) ? &s_entries[idx] : NULL;
}

/* ── Find next available filename ──────────────────────────────────────── */
static bool find_next_fname(char *out, size_t out_sz)
{
    for (uint32_t n = 1u; n <= 999u; n++) {
        char candidate[48];
        snprintf(candidate, sizeof(candidate), "%s/REC_%03lu.PCM",
                 SD_REC_DIR_PATH, (unsigned long)n);
        FILINFO fi;
        if (f_stat(candidate, &fi) != FR_OK) {
            snprintf(out, out_sz, "REC_%03lu.PCM", (unsigned long)n);
            return true;
        }
    }
    return false;
}

/* ── Save recorder RAM buffer → SD ─────────────────────────────────────── */
bool SDRec_SaveFromRecorder(void)
{
    if (!ensure_mounted()) return false;

    f_mkdir(SD_REC_DIR_PATH);

    uint32_t n = REC_GetSamplesRecorded();
    if (n == 0u) return false;

    char fname[16];
    if (!find_next_fname(fname, sizeof(fname))) return false;

    char path[56];
    snprintf(path, sizeof(path), "%s/%s", SD_REC_DIR_PATH, fname);

    FIL fil;
    FRESULT fr = f_open(&fil, path, FA_CREATE_NEW | FA_WRITE);
    if (fr != FR_OK) {
        s_mounted = false;
        if (!ensure_mounted()) return false;
        f_mkdir(SD_REC_DIR_PATH);
        if (!find_next_fname(fname, sizeof(fname))) return false;
        snprintf(path, sizeof(path), "%s/%s", SD_REC_DIR_PATH, fname);
        if (f_open(&fil, path, FA_CREATE_NEW | FA_WRITE) != FR_OK) return false;
    }

    static int16_t wbuf[256];
    uint32_t written      = 0u;
    bool     ok           = true;
    uint32_t yield_cnt    = 0u;

    while (written < n) {
        uint32_t chunk = n - written;
        if (chunk > 256u) chunk = 256u;

        for (uint32_t i = 0u; i < chunk; i++)
            wbuf[i] = REC_GetSample(written + i);

        UINT bw;
        if (f_write(&fil, wbuf, (UINT)(chunk * 2u), &bw) != FR_OK
            || bw != (UINT)(chunk * 2u)) {
            ok = false;
            break;
        }
        written += chunk;

        /* Yield every 8 chunks (~4 KB) to keep audio buffer fed */
        if (++yield_cnt >= 8u) { yield_cnt = 0u; sd_yield(); }
    }

    f_close(&fil);

    if (!ok) {
        f_unlink(path);
        return false;
    }

    SDRec_Scan();
    return true;
}

/* ── Load SD recording into recorder RAM buffer ─────────────────────────── */
bool SDRec_Load(uint8_t idx)
{
    if (!ensure_mounted()) return false;
    if (idx >= s_count)    return false;

    char path[56];
    snprintf(path, sizeof(path), "%s/%s",
             SD_REC_DIR_PATH, s_entries[idx].fname);

    FIL fil;
    if (f_open(&fil, path, FA_READ) != FR_OK) return false;

    uint32_t total = s_entries[idx].n_samples;
    if (total > REC_BUFFER_SIZE) total = REC_BUFFER_SIZE;

    REC_LoadChunkBegin(total);

    static int16_t rbuf[256];
    uint32_t loaded    = 0u;
    bool     ok        = true;
    uint32_t yield_cnt = 0u;

    while (loaded < total) {
        uint32_t chunk = total - loaded;
        if (chunk > 256u) chunk = 256u;

        UINT br;
        if (f_read(&fil, rbuf, (UINT)(chunk * 2u), &br) != FR_OK || br == 0u) {
            ok = false;
            break;
        }

        uint32_t got = (uint32_t)(br / 2u);
        REC_LoadChunk(rbuf, got);
        loaded += got;

        if (++yield_cnt >= 8u) { yield_cnt = 0u; sd_yield(); }
    }

    f_close(&fil);
    REC_LoadChunkEnd();

    return ok && (loaded > 0u);
}

/* ── Delete a recording ─────────────────────────────────────────────────── */
bool SDRec_Delete(uint8_t idx)
{
    if (!ensure_mounted()) return false;
    if (idx >= s_count)    return false;

    char path[56];
    snprintf(path, sizeof(path), "%s/%s",
             SD_REC_DIR_PATH, s_entries[idx].fname);

    if (f_unlink(path) != FR_OK) return false;

    SDRec_Scan();
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Live streaming recording — supports 60+ second recordings
 * ═══════════════════════════════════════════════════════════════════════════ */

bool     SDRec_IsLiveRecording(void) { return s_live_active; }
uint32_t SDRec_GetLiveBytes(void)    { return s_live_bytes;  }

bool SDRec_LiveStart(void)
{
    if (s_live_active) return false;
    if (!ensure_mounted()) return false;

    f_mkdir(SD_REC_DIR_PATH);

    if (!find_next_fname(s_live_fname, sizeof(s_live_fname))) return false;

    char path[56];
    snprintf(path, sizeof(path), "%s/%s", SD_REC_DIR_PATH, s_live_fname);

    FRESULT fr = f_open(&s_live_file, path, FA_CREATE_NEW | FA_WRITE);
    if (fr != FR_OK) {
        s_mounted = false;
        if (!ensure_mounted()) return false;
        f_mkdir(SD_REC_DIR_PATH);
        if (!find_next_fname(s_live_fname, sizeof(s_live_fname))) return false;
        snprintf(path, sizeof(path), "%s/%s", SD_REC_DIR_PATH, s_live_fname);
        if (f_open(&s_live_file, path, FA_CREATE_NEW | FA_WRITE) != FR_OK)
            return false;
    }

    /* Pre-allocate 2 MB of contiguous clusters to eliminate cluster-boundary
     * FAT updates (and their associated erase latency) during streaming.
     * At 9070 Hz × 2 B this covers ~110 s of recording.  If the card has
     * insufficient contiguous space, f_expand fails silently — we fall back
     * to normal on-demand allocation.  The tail is trimmed on LiveStop. */
    f_expand(&s_live_file, (FSIZE_t)(2u * 1024u * 1024u), 1);

    s_live_active  = true;
    s_live_bytes   = 0u;
    s_live_stage_n = 0u;
    s_hold_n       = 0u;
    /* Reset noise-cleanup filters so a recording never starts with a transient
     * carried over from the previous one.  Start the gate fully open so the
     * first instants are never attenuated; it closes only after real silence. */
    s_dc_x1   = 0.0f;
    s_dc_y1   = 0.0f;
    s_gate_env = GATE_OPEN;
    s_gate_g   = 1.0f;
    s_yield        = hold_mic_data;   /* capture DMA data during SD writes */
    return true;
}

/* Internal: flush staging buffer to SD.
 * After f_write returns (which may have blocked for a cluster-boundary erase),
 * sd_yield() → hold_mic_data() captures whatever DMA half-buffer arrived
 * during the write.  The captured samples are then drained back into the
 * staging buffer so they are included in the next flush — zero data loss. */
static bool live_flush(void)
{
    if (s_live_stage_n == 0u) return true;

    uint16_t n   = s_live_stage_n;
    s_live_stage_n = 0u;
    s_hold_n       = 0u;   /* reset hold so hold_mic_data() can fill it fresh */

    UINT bw;
    FRESULT fr = f_write(&s_live_file, s_live_stage, (UINT)(n * 2u), &bw);
    if (fr != FR_OK || bw == 0u) return false;

    s_live_bytes += bw;

    sd_yield();   /* → hold_mic_data() captures DMA data that arrived during write */

    /* Drain hold buffer back into staging for next flush */
    if (s_hold_n > 0u) {
        for (uint16_t i = 0u; i < s_hold_n; i++) {
            if (s_live_stage_n < LIVE_STAGE_SAMPLES)
                s_live_stage[s_live_stage_n++] = s_hold_buf[i];
        }
        s_hold_n = 0u;
    }
    return true;
}

bool SDRec_LiveFeed(const int16_t *pcm, uint16_t n)
{
    if (!s_live_active) return false;

    for (uint16_t i = 0u; i < n; i++) {
        /* ── Stage 1: DC blocker  y[n] = x[n] - x[n-1] + R*y[n-1] ─────────── */
        float x = (float)pcm[i];
        float y = x - s_dc_x1 + DC_R * s_dc_y1;
        s_dc_x1 = x;
        s_dc_y1 = y;

        /* ── Stage 2: soft noise gate ─────────────────────────────────────── */
        float a = (y < 0.0f) ? -y : y;                 /* rectified level      */
        if (a > s_gate_env) s_gate_env = a;            /* instant attack       */
        else                s_gate_env += (a - s_gate_env) * GATE_REL; /* release */

        float g;
        if      (s_gate_env >= GATE_OPEN) g = 1.0f;
        else if (s_gate_env <= GATE_SHUT) g = GATE_FLOOR;
        else    g = GATE_FLOOR + (1.0f - GATE_FLOOR) *
                    ((s_gate_env - GATE_SHUT) / (GATE_OPEN - GATE_SHUT));
        s_gate_g += (g - s_gate_g) * GATE_GSMOOTH;     /* slew the applied gain */
        y *= s_gate_g;

        /* ── Round + clamp back to int16 ─────────────────────────────────── */
        int32_t o = (int32_t)(y + (y >= 0.0f ? 0.5f : -0.5f));
        if (o >  32767) o =  32767;
        if (o < -32768) o = -32768;

        /* Check BEFORE writing to prevent buffer overflow when the hold-drain
         * left staging exactly full on the previous flush. */
        if (s_live_stage_n >= LIVE_STAGE_SAMPLES) {
            if (!live_flush()) return false;
        }
        s_live_stage[s_live_stage_n++] = (int16_t)o;
    }
    return true;
}

bool SDRec_LiveStop(void)
{
    if (!s_live_active) return false;

    live_flush();   /* write any remaining samples */
    /* Trim the pre-allocated tail back to the actual number of bytes written */
    f_lseek(&s_live_file, (FSIZE_t)s_live_bytes);
    f_truncate(&s_live_file);
    f_close(&s_live_file);

    s_live_active  = false;
    s_live_stage_n = 0u;
    s_hold_n       = 0u;
    s_yield        = NULL;   /* stop capturing DMA data after stop */

    if (s_live_bytes == 0u) {
        /* Nothing written — delete the empty file */
        char path[56];
        snprintf(path, sizeof(path), "%s/%s", SD_REC_DIR_PATH, s_live_fname);
        f_unlink(path);
        return false;
    }

    SDRec_Scan();
    return true;
}
