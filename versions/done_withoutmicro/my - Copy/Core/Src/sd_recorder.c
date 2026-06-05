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
/* Staging buffer: batch 512-sample (1 KB) writes to reduce write frequency */
#define LIVE_STAGE_SAMPLES  512u
static int16_t  s_live_stage[LIVE_STAGE_SAMPLES];
static uint16_t s_live_stage_n = 0u;
static char     s_live_fname[16];   /* filename of current live file */

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

    if (!ensure_mounted()) return;

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

    s_live_active  = true;
    s_live_bytes   = 0u;
    s_live_stage_n = 0u;
    return true;
}

/* Internal: flush staging buffer to SD */
static bool live_flush(void)
{
    if (s_live_stage_n == 0u) return true;

    UINT bw;
    FRESULT fr = f_write(&s_live_file,
                         s_live_stage,
                         (UINT)(s_live_stage_n * 2u),
                         &bw);
    if (fr != FR_OK || bw == 0u) return false;

    s_live_bytes  += bw;
    s_live_stage_n = 0u;
    sd_yield();   /* keep audio buffer fed after each write */
    return true;
}

bool SDRec_LiveFeed(const int16_t *pcm, uint16_t n)
{
    if (!s_live_active) return false;

    for (uint16_t i = 0u; i < n; i++) {
        s_live_stage[s_live_stage_n++] = pcm[i];
        if (s_live_stage_n >= LIVE_STAGE_SAMPLES) {
            if (!live_flush()) return false;
        }
    }
    return true;
}

bool SDRec_LiveStop(void)
{
    if (!s_live_active) return false;

    live_flush();   /* write any remaining samples */
    f_close(&s_live_file);

    s_live_active  = false;
    s_live_stage_n = 0u;

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
