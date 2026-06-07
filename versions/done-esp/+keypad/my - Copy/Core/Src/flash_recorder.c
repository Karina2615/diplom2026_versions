/*
 * flash_recorder.c — Single-slot audio recorder using STM32F407 internal flash
 *
 * One recording is stored in Sector 5 (0x08020000, 128 KB).
 * Each new save erases and overwrites the same sector.
 * 48 000 samples × 2 B + 8 B header = 96 008 B < 131 072 B (128 KB) ✓
 *
 * Slot header (first 8 bytes of sector 5):
 *   [0..3]  magic  = 0xDEADCAFE  → recording present
 *   [4..7]  n_samp = uint32_t    → number of int16_t samples stored
 *   [8+]    pcm[]  = int16_t     → raw mono PCM @ 16 kHz
 */

#include "flash_recorder.h"
#include "recorder.h"
#include "stm32f4xx_hal.h"

#define FLASH_MAGIC        0xDEADCAFEu
#define FLASH_HEADER_BYTES 8u

#define REC_SECTOR_BASE    0x08020000u   /* sector 5 — only slot used */
#define REC_HAL_SECTOR     FLASH_SECTOR_5

static bool g_has_recording = false;   /* true when magic is valid in flash */

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

void FLASH_REC_Init(void)
{
    uint32_t magic = *(volatile uint32_t *)REC_SECTOR_BASE;
    g_has_recording = (magic == FLASH_MAGIC);
}

bool FLASH_REC_IsSlotUsed(uint8_t idx)
{
    (void)idx;
    return g_has_recording;
}

uint8_t FLASH_REC_GetUsedSlots(void)
{
    return g_has_recording ? 1u : 0u;
}

bool FLASH_REC_IsFull(void)
{
    return false;   /* single slot — always allow overwrite */
}

uint8_t FLASH_REC_GetLastSavedSlot(void)
{
    return g_has_recording ? 0u : 0xFFu;
}

const int16_t *FLASH_REC_GetSlotDataPtr(uint8_t idx)
{
    (void)idx;
    if (!g_has_recording) return NULL;
    return (const int16_t *)(REC_SECTOR_BASE + FLASH_HEADER_BYTES);
}

uint32_t FLASH_REC_GetSlotSampleCount(uint8_t idx)
{
    (void)idx;
    if (!g_has_recording) return 0u;
    return *(volatile uint32_t *)(REC_SECTOR_BASE + 4u);
}

/* -----------------------------------------------------------------------
 * FLASH_REC_SaveFromRecorder
 *
 * Always targets Sector 5 (0x08020000).  Previous recording is erased.
 *
 *   Step 1  HAL_FLASHEx_Erase   — erase 128 KB sector (all bits → 1)
 *   Step 2  HAL_FLASH_Program WORD   @ base+0  — magic  0xDEADCAFE
 *   Step 3  HAL_FLASH_Program WORD   @ base+4  — n_samp count
 *   Step 4  while loop HALFWORD      @ base+8… — one int16_t per iteration
 *
 * BLOCKING (~1-2 s).  Caller must mute audio before calling.
 * --------------------------------------------------------------------- */
bool FLASH_REC_SaveFromRecorder(void)
{
    uint32_t n_samples = REC_GetSamplesRecorded();
    if (n_samples == 0u) return false;

    HAL_FLASH_Unlock();

    /* Step 1 — erase sector 5 */
    FLASH_EraseInitTypeDef er = {
        .TypeErase    = FLASH_TYPEERASE_SECTORS,
        .Sector       = REC_HAL_SECTOR,
        .NbSectors    = 1u,
        .VoltageRange = FLASH_VOLTAGE_RANGE_3,
    };
    uint32_t page_error = 0u;
    if (HAL_FLASHEx_Erase(&er, &page_error) != HAL_OK) {
        HAL_FLASH_Lock();
        return false;
    }

    /* Step 2 — write magic */
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                          REC_SECTOR_BASE,
                          (uint64_t)FLASH_MAGIC) != HAL_OK) {
        HAL_FLASH_Lock();
        return false;
    }

    /* Step 3 — write sample count */
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                          REC_SECTOR_BASE + 4u,
                          (uint64_t)n_samples) != HAL_OK) {
        HAL_FLASH_Lock();
        return false;
    }

    /* Step 4 — write PCM samples as HALFWORD (2 B each) */
    uint32_t write_addr = REC_SECTOR_BASE + FLASH_HEADER_BYTES;
    uint32_t sample_idx = 0u;

    while (sample_idx < n_samples) {
        uint64_t val = (uint64_t)(uint16_t)REC_GetSample(sample_idx);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,
                              write_addr, val) != HAL_OK) {
            HAL_FLASH_Lock();
            return false;
        }
        write_addr += 2u;
        sample_idx++;
    }

    HAL_FLASH_Lock();

    g_has_recording = true;
    return true;
}

/* -----------------------------------------------------------------------
 * FLASH_REC_EraseAll — erase the single sector, clear state flag
 * BLOCKING (~1-2 s).  Caller must mute audio.
 * --------------------------------------------------------------------- */
void FLASH_REC_EraseAll(void)
{
    HAL_FLASH_Unlock();

    FLASH_EraseInitTypeDef er = {
        .TypeErase    = FLASH_TYPEERASE_SECTORS,
        .Sector       = REC_HAL_SECTOR,
        .NbSectors    = 1u,
        .VoltageRange = FLASH_VOLTAGE_RANGE_3,
    };
    uint32_t page_error = 0u;
    HAL_FLASHEx_Erase(&er, &page_error);

    HAL_FLASH_Lock();

    g_has_recording = false;
}
