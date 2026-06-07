/*
 * note_seq.c — FatFS backend for the sampler note-event sequencer
 *
 * Files: 0:/NOTES/NOTE_NNN.NOT  (raw NoteEvent_t array, 8 bytes each)
 *
 * Does NOT manage the SD mount — assumes sd_recorder has already called
 * f_mount.  All public functions return false if the FS is not ready.
 */

#include "note_seq.h"
#include "sd_recorder.h"   /* for SDRec_IsMounted() */
#include "ff.h"
#include <stdio.h>
#include <string.h>

/* ── Internal state ─────────────────────────────────────────────────── */
static uint8_t      s_count = 0u;
static NSeq_Entry_t s_entries[NSEQ_MAX];

/* ── Find next available filename ──────────────────────────────────── */
static bool find_next_fname(char *out, size_t out_sz)
{
    for (uint32_t n = 1u; n <= 999u; n++) {
        char candidate[56];
        snprintf(candidate, sizeof(candidate), "%s/NOTE_%03lu.NOT",
                 NSEQ_DIR_PATH, (unsigned long)n);
        FILINFO fi;
        if (f_stat(candidate, &fi) != FR_OK) {
            snprintf(out, out_sz, "NOTE_%03lu.NOT", (unsigned long)n);
            return true;
        }
    }
    return false;
}

/* ── Init ──────────────────────────────────────────────────────────── */
bool NSeq_Init(void)
{
    s_count = 0u;
    memset(s_entries, 0, sizeof(s_entries));
    if (!SDRec_IsMounted()) return false;
    f_mkdir(NSEQ_DIR_PATH);   /* create directory; ignore FR_EXIST */
    return NSeq_Scan();
}

/* ── Scan ──────────────────────────────────────────────────────────── */
bool NSeq_Scan(void)
{
    if (!SDRec_IsMounted()) return false;

    s_count = 0u;
    memset(s_entries, 0, sizeof(s_entries));

    DIR     dir;
    FILINFO fno;

    if (f_opendir(&dir, NSEQ_DIR_PATH) != FR_OK) {
        f_mkdir(NSEQ_DIR_PATH);
        if (f_opendir(&dir, NSEQ_DIR_PATH) != FR_OK) return false;
    }

    while (s_count < NSEQ_MAX) {
        FRESULT res = f_readdir(&dir, &fno);
        if (res != FR_OK || fno.fname[0] == '\0') break;
        if (fno.fattrib & AM_DIR) continue;

        const char *dot = strrchr(fno.fname, '.');
        if (!dot || strcmp(dot, ".NOT") != 0) continue;

        size_t nlen = strlen(fno.fname);
        if (nlen >= sizeof(s_entries[0].fname)) nlen = sizeof(s_entries[0].fname) - 1u;
        memcpy(s_entries[s_count].fname, fno.fname, nlen);
        s_entries[s_count].fname[nlen] = '\0';

        uint32_t n_ev = (uint32_t)(fno.fsize / sizeof(NoteEvent_t));
        s_entries[s_count].n_events = n_ev;

        /* Read the last event to get the sequence duration (one seek/read per file) */
        s_entries[s_count].duration_ms = 0u;
        if (n_ev > 0u) {
            char path[56];
            snprintf(path, sizeof(path), "%s/%s", NSEQ_DIR_PATH, s_entries[s_count].fname);
            FIL fil;
            if (f_open(&fil, path, FA_READ) == FR_OK) {
                FSIZE_t last_pos = (FSIZE_t)((n_ev - 1u) * sizeof(NoteEvent_t));
                NoteEvent_t ev;
                UINT br;
                if (f_lseek(&fil, last_pos) == FR_OK &&
                    f_read(&fil, &ev, sizeof(NoteEvent_t), &br) == FR_OK &&
                    br == sizeof(NoteEvent_t)) {
                    s_entries[s_count].duration_ms = ev.t_ms;
                }
                f_close(&fil);
            }
        }
        s_count++;
    }
    f_closedir(&dir);

    /* Sort by filename (insertion sort, ascending) */
    for (uint8_t i = 1u; i < s_count; i++) {
        NSeq_Entry_t tmp = s_entries[i];
        int8_t j = (int8_t)(i - 1);
        while (j >= 0 && strcmp(s_entries[j].fname, tmp.fname) > 0) {
            s_entries[j + 1u] = s_entries[j];
            j--;
        }
        s_entries[j + 1u] = tmp;
    }

    return true;
}

/* ── Getters ───────────────────────────────────────────────────────── */
uint8_t NSeq_GetCount(void)  { return s_count; }

const NSeq_Entry_t *NSeq_GetEntry(uint8_t idx)
{
    return (idx < s_count) ? &s_entries[idx] : NULL;
}

/* ── Save ──────────────────────────────────────────────────────────── */
bool NSeq_Save(const NoteEvent_t *evts, uint16_t n)
{
    if (!SDRec_IsMounted() || !evts || n == 0u) return false;

    f_mkdir(NSEQ_DIR_PATH);

    char fname[16];
    if (!find_next_fname(fname, sizeof(fname))) return false;

    char path[56];
    snprintf(path, sizeof(path), "%s/%s", NSEQ_DIR_PATH, fname);

    FIL fil;
    if (f_open(&fil, path, FA_CREATE_NEW | FA_WRITE) != FR_OK) return false;

    UINT bw;
    bool ok = (f_write(&fil, evts, (UINT)(n * sizeof(NoteEvent_t)), &bw) == FR_OK
               && bw == (UINT)(n * sizeof(NoteEvent_t)));

    f_close(&fil);

    if (!ok) { f_unlink(path); return false; }

    NSeq_Scan();
    return true;
}

/* ── Load ──────────────────────────────────────────────────────────── */
bool NSeq_Load(uint8_t idx, NoteEvent_t *evts, uint16_t cap, uint16_t *n_out)
{
    if (!SDRec_IsMounted() || idx >= s_count || !evts || !n_out) return false;

    char path[56];
    snprintf(path, sizeof(path), "%s/%s", NSEQ_DIR_PATH, s_entries[idx].fname);

    FIL fil;
    if (f_open(&fil, path, FA_READ) != FR_OK) { *n_out = 0u; return false; }

    uint32_t to_read = s_entries[idx].n_events;
    if (to_read > (uint32_t)cap) to_read = (uint32_t)cap;

    UINT br;
    bool ok = (f_read(&fil, evts, (UINT)(to_read * sizeof(NoteEvent_t)), &br) == FR_OK
               && br > 0u);

    f_close(&fil);

    *n_out = (uint16_t)(br / sizeof(NoteEvent_t));
    return ok && (*n_out > 0u);
}

/* ── Delete ────────────────────────────────────────────────────────── */
bool NSeq_Delete(uint8_t idx)
{
    if (!SDRec_IsMounted() || idx >= s_count) return false;

    char path[56];
    snprintf(path, sizeof(path), "%s/%s", NSEQ_DIR_PATH, s_entries[idx].fname);

    if (f_unlink(path) != FR_OK) return false;

    NSeq_Scan();
    return true;
}
