/* PO-33 K.O! style sampler
 * Pad samples stored as /SAMPLES/PADxx.WAV (16-bit mono PCM, 16 kHz)
 *
 * Recording uses the MIC_GetBuffer() ring buffer from microphone.c.
 * Playback mixes into the I2S DMA half/full callback via Sampler_FillAudio().
 */

#include "sampler.h"
#include "ttp229.h"
#include "microphone.h"
#include "ff.h"
#include <string.h>
#include <stdio.h>

/* ── WAV header (44 bytes, mono 16-bit 16 kHz) ──────────────────────────── */
#define WAV_SAMPLE_RATE   16000u
#define WAV_CHANNELS      1
#define WAV_BITS          16
#define WAV_BYTE_RATE     (WAV_SAMPLE_RATE * WAV_CHANNELS * WAV_BITS / 8)

#pragma pack(push,1)
typedef struct {
    char     riff[4];        /* "RIFF"                   */
    uint32_t file_size;      /* total size - 8           */
    char     wave[4];        /* "WAVE"                   */
    char     fmt[4];         /* "fmt "                   */
    uint32_t fmt_size;       /* 16                       */
    uint16_t audio_fmt;      /* 1 = PCM                  */
    uint16_t channels;       /* 1                        */
    uint32_t sample_rate;    /* 16000                    */
    uint32_t byte_rate;      /* 32000                    */
    uint16_t block_align;    /* 2                        */
    uint16_t bits_per_sample;/* 16                       */
    char     data[4];        /* "data"                   */
    uint32_t data_size;      /* PCM bytes                */
} WavHeader_t;
#pragma pack(pop)

static void wav_fill_header(WavHeader_t *h, uint32_t pcm_bytes)
{
    memcpy(h->riff, "RIFF", 4);
    h->file_size       = pcm_bytes + sizeof(WavHeader_t) - 8;
    memcpy(h->wave, "WAVE", 4);
    memcpy(h->fmt,  "fmt ", 4);
    h->fmt_size        = 16;
    h->audio_fmt       = 1;
    h->channels        = WAV_CHANNELS;
    h->sample_rate     = WAV_SAMPLE_RATE;
    h->byte_rate       = WAV_BYTE_RATE;
    h->block_align     = WAV_CHANNELS * WAV_BITS / 8;
    h->bits_per_sample = WAV_BITS;
    memcpy(h->data, "data", 4);
    h->data_size       = pcm_bytes;
}

/* ── Playback state (used by FillAudio ISR) ─────────────────────────────── */
#define PB_BUF_FRAMES  256    /* read-ahead buffer (512 bytes) */

static volatile uint8_t  s_pb_active = 0;
static int16_t  s_pb_buf[PB_BUF_FRAMES * 2]; /* double buffer */
static volatile uint16_t s_pb_buf_sel  = 0;   /* which half is being played */
static volatile uint16_t s_pb_pos      = 0;   /* position within active half */
static FIL      s_pb_file;

/* ── Recording state ────────────────────────────────────────────────────── */
static volatile uint8_t s_rec_active = 0;
static FIL      s_rec_file;
static uint32_t s_rec_bytes = 0;

/* ── Helpers ────────────────────────────────────────────────────────────── */
static void pad_path(char *out, uint8_t pad)  /* pad = 1..15 */
{
    snprintf(out, 32, "/SAMPLES/PAD%02u.WAV", (unsigned)pad);
}

static uint8_t pad_exists(uint8_t pad)
{
    char path[32];
    pad_path(path, pad);
    FILINFO fi;
    return (f_stat(path, &fi) == FR_OK) ? 1u : 0u;
}

static void ensure_samples_dir(void)
{
    f_mkdir("/SAMPLES");   /* silently fails if it exists */
}

/* ── Init ───────────────────────────────────────────────────────────────── */
void Sampler_Init(Sampler_t *s)
{
    memset(s, 0, sizeof(*s));

    ensure_samples_dir();

    for (uint8_t p = 1; p <= SAMPLER_PADS; p++)
        s->state[p-1] = pad_exists(p) ? SPAD_READY : SPAD_EMPTY;
}

/* ── Update (called from main loop) ─────────────────────────────────────── */
void Sampler_Update(Sampler_t *s)
{
    TTP229_Update();   /* refresh key state */

    /* Track function key (key 16) */
    s->fn_held = TTP229_IsPressed(16);

    for (uint8_t p = 1; p <= SAMPLER_PADS; p++)
    {
        if (!TTP229_JustPressed(p)) continue;

        /* ── Start recording: fn_held + tap pad ──────────────────────── */
        if (s->fn_held && !s_rec_active && !s_pb_active)
        {
            char path[32];
            pad_path(path, p);
            ensure_samples_dir();

            if (f_open(&s_rec_file, path,
                       FA_CREATE_ALWAYS | FA_WRITE) == FR_OK)
            {
                /* Write placeholder header — will be updated on stop */
                WavHeader_t hdr;
                wav_fill_header(&hdr, 0);
                UINT bw;
                f_write(&s_rec_file, &hdr, sizeof(hdr), &bw);

                s_rec_bytes    = 0;
                s_rec_active   = 1;
                s->active_pad  = p;
                s->state[p-1]  = SPAD_RECORDING;
            }
            return;  /* one event per update cycle */
        }

        /* ── Stop recording: tap same pad while recording ─────────────── */
        if (s_rec_active && s->active_pad == p)
        {
            s_rec_active = 0;

            /* Patch WAV header with real size */
            f_lseek(&s_rec_file, 0);
            WavHeader_t hdr;
            wav_fill_header(&hdr, s_rec_bytes);
            UINT bw;
            f_write(&s_rec_file, &hdr, sizeof(hdr), &bw);
            f_close(&s_rec_file);

            s->state[p-1] = SPAD_READY;
            s->active_pad = 0;
            return;
        }

        /* ── Erase: fn_held + tap pad (pad must not be recording) ─────── */
        if (s->fn_held && s->state[p-1] == SPAD_READY)
        {
            char path[32];
            pad_path(path, p);
            f_unlink(path);
            s->state[p-1] = SPAD_EMPTY;
            return;
        }

        /* ── Play: tap pad with sample ────────────────────────────────── */
        if (!s->fn_held && s->state[p-1] == SPAD_READY && !s_rec_active)
        {
            /* Stop any currently playing pad */
            if (s_pb_active)
            {
                s_pb_active = 0;
                f_close(&s_pb_file);
                if (s->active_pad && s->active_pad <= SAMPLER_PADS)
                    s->state[s->active_pad-1] = SPAD_READY;
            }

            char path[32];
            pad_path(path, p);
            if (f_open(&s_pb_file, path, FA_READ) == FR_OK)
            {
                /* Skip WAV header */
                f_lseek(&s_pb_file, sizeof(WavHeader_t));

                /* Pre-fill first half of the double buffer */
                UINT br;
                f_read(&s_pb_file, s_pb_buf, PB_BUF_FRAMES * 2, &br);

                s_pb_pos      = 0;
                s_pb_buf_sel  = 0;
                s_pb_active   = 1;
                s->active_pad = p;
                s->state[p-1] = SPAD_PLAYING;
            }
        }
    }

    /* ── Feed recording data from microphone buffer ───────────────────── */
    if (s_rec_active)
    {
        /* MIC_GetBuffer returns pointer to filled PCM block and its size */
        uint16_t avail = 0;
        const int16_t *mic = MIC_GetBuffer(&avail);
        if (mic && avail)
        {
            UINT bw;
            f_write(&s_rec_file, mic, avail * 2, &bw);
            s_rec_bytes += bw;
        }
    }
}

/* ── FillAudio: called from I2S DMA half/full callback ──────────────────── */
/* Writes `frames` stereo int16 samples (left+right interleaved) into buf.   *
 * Since the sample is mono 16 kHz and I2S runs at 44.1 kHz we upsample     *
 * by nearest-neighbour (ratio ~2.756) and duplicate L→R.                   */

#define SRC_RATE  16000u
#define DST_RATE  44100u

void Sampler_FillAudio(int16_t *buf, uint16_t frames)
{
    if (!s_pb_active)
    {
        memset(buf, 0, frames * 4);
        return;
    }

    static uint32_t s_phase = 0;   /* fixed-point resampler accumulator     */

    for (uint16_t i = 0; i < frames; i++)
    {
        /* Advance source by SRC_RATE per DST_RATE output samples */
        uint16_t src_idx = (uint16_t)(s_phase >> 16);

        if (src_idx >= PB_BUF_FRAMES)
        {
            /* Need next half of double buffer */
            src_idx -= PB_BUF_FRAMES;
            s_phase  = (uint32_t)src_idx << 16;

            /* Swap buffer half and read next chunk from SD */
            UINT br = 0;
            f_read(&s_pb_file,
                   s_pb_buf,            /* always refill the same buffer */
                   PB_BUF_FRAMES * 2,
                   &br);

            if (br == 0)
            {
                /* End of file */
                s_pb_active = 0;
                f_close(&s_pb_file);
                memset(buf + i * 2, 0, (frames - i) * 4);
                s_phase = 0;
                return;
            }
        }

        int16_t sample = s_pb_buf[src_idx];
        buf[i * 2]     = sample;   /* L */
        buf[i * 2 + 1] = sample;   /* R */

        s_phase += (uint32_t)((SRC_RATE << 16) / DST_RATE);
    }
}
