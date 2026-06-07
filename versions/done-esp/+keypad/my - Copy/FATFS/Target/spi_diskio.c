/* SPI SD card disk I/O driver for FatFS
 * SPI2: PB10=SCK, PB14=MISO, PB15=MOSI, PB12=CS (GPIO Output)
 *
 * Implements the Diskio_drvTypeDef callbacks expected by FatFS / ff_gen_drv.
 * Supports SD (v1/v2) and SDHC/SDXC cards.
 *
 * Assumptions:
 *  - hspi2 handle is initialised before FatFS mounts the drive.
 *  - CubeMX configures SPI2 in Full-Duplex Master, 8-bit, CPOL=0/CPHA=0.
 */

#include "spi_diskio.h"
#include "main.h"
#include <string.h>

/* ── SPI / CS helpers ───────────────────────────────────────────────────── */
#define SD_CS_PIN   GPIO_PIN_12
#define SD_CS_PORT  GPIOB

#define CS_LOW()   HAL_GPIO_WritePin(SD_CS_PORT, SD_CS_PIN, GPIO_PIN_RESET)
#define CS_HIGH()  HAL_GPIO_WritePin(SD_CS_PORT, SD_CS_PIN, GPIO_PIN_SET)

extern SPI_HandleTypeDef hspi2;

static inline uint8_t spi_tx(uint8_t b)
{
    uint8_t r = 0xFF;
    HAL_SPI_TransmitReceive(&hspi2, &b, &r, 1, 10);
    return r;
}

static inline void spi_rx_buf(uint8_t *buf, uint16_t len)
{
    memset(buf, 0xFF, len);
    HAL_SPI_TransmitReceive(&hspi2, buf, buf, len, len + 10);
}

static inline void spi_tx_buf(const uint8_t *buf, uint16_t len)
{
    uint8_t dummy[512];
    HAL_SPI_TransmitReceive(&hspi2, (uint8_t*)buf, dummy, len, len + 10);
}

/* ── SD command definitions ─────────────────────────────────────────────── */
#define CMD0    0     /* GO_IDLE_STATE */
#define CMD1    1     /* SEND_OP_COND (MMC) */
#define CMD8    8     /* SEND_IF_COND */
#define CMD9    9     /* SEND_CSD */
#define CMD16   16    /* SET_BLOCKLEN */
#define CMD17   17    /* READ_SINGLE_BLOCK */
#define CMD24   24    /* WRITE_BLOCK */
#define CMD41   41    /* APP_SEND_OP_COND */
#define CMD55   55    /* APP_CMD */
#define CMD58   58    /* READ_OCR */

/* ── Card type flags ────────────────────────────────────────────────────── */
#define CT_MMC   0x01
#define CT_SD1   0x02
#define CT_SD2   0x04
#define CT_SDHC  0x08

static uint8_t s_card_type = 0;
static DSTATUS s_status    = STA_NOINIT;

/* ── Low-level SD helpers ───────────────────────────────────────────────── */

static void sd_deselect(void)
{
    CS_HIGH();
    spi_tx(0xFF);   /* release clock */
}

static uint8_t sd_wait_ready(uint32_t timeout_ms)
{
    uint32_t t = HAL_GetTick();
    do {
        if (spi_tx(0xFF) == 0xFF) return 1;
    } while ((HAL_GetTick() - t) < timeout_ms);
    return 0;
}

static uint8_t sd_select(void)
{
    CS_LOW();
    spi_tx(0xFF);
    if (sd_wait_ready(500)) return 1;
    sd_deselect();
    return 0;
}

/* Send SD SPI command, return R1 response byte */
static uint8_t sd_cmd(uint8_t cmd, uint32_t arg)
{
    uint8_t crc = 0xFF;
    if (cmd == CMD0) crc = 0x95;
    if (cmd == CMD8) crc = 0x87;

    if (cmd & 0x80) {   /* ACMD = CMD55 + cmd */
        cmd &= 0x7F;
        uint8_t r = sd_cmd(CMD55, 0);
        if (r > 1) return r;
    }

    /* Wait for card ready (except after CMD0) */
    sd_deselect();
    if (!sd_select()) return 0xFF;

    spi_tx(0x40 | cmd);
    spi_tx((uint8_t)(arg >> 24));
    spi_tx((uint8_t)(arg >> 16));
    spi_tx((uint8_t)(arg >> 8));
    spi_tx((uint8_t)(arg));
    spi_tx(crc);

    uint8_t r;
    uint8_t n = 10;
    do { r = spi_tx(0xFF); } while ((r & 0x80) && --n);
    return r;
}

/* Receive a data block (512 bytes) */
static uint8_t sd_read_datablock(uint8_t *buf, uint16_t len)
{
    uint8_t token;
    uint32_t t = HAL_GetTick();
    do { token = spi_tx(0xFF); } while ((token == 0xFF) && ((HAL_GetTick() - t) < 200));
    if (token != 0xFE) return 0;

    spi_rx_buf(buf, len);
    spi_tx(0xFF); spi_tx(0xFF);   /* discard CRC */
    return 1;
}

/* Transmit a data block (512 bytes) */
static uint8_t sd_write_datablock(const uint8_t *buf, uint8_t token)
{
    if (!sd_wait_ready(500)) return 0;

    spi_tx(token);
    if (token == 0xFD) return 1;   /* stop-tran token */

    spi_tx_buf(buf, 512);
    spi_tx(0xFF); spi_tx(0xFF);   /* dummy CRC */

    uint8_t resp = spi_tx(0xFF) & 0x1F;
    return (resp == 0x05) ? 1 : 0;
}

/* ── FatFS Diskio callbacks ─────────────────────────────────────────────── */

static DSTATUS SPI_Initialize(BYTE lun)
{
    (void)lun;
    uint8_t n, cmd, ty, ocr[4];

    /* Power-up sequence: ≥74 clock pulses with CS high */
    CS_HIGH();
    for (n = 10; n; n--) spi_tx(0xFF);

    s_card_type = 0;

    uint32_t t = HAL_GetTick();

    if (sd_cmd(CMD0, 0) == 1)   /* Enter idle */
    {
        if (sd_cmd(CMD8, 0x1AA) == 1)   /* SDv2? */
        {
            /* R7 response */
            for (n = 0; n < 4; n++) ocr[n] = spi_tx(0xFF);
            if (ocr[2] == 0x01 && ocr[3] == 0xAA)
            {
                /* Wait for leaving idle (ACMD41 with HCS) */
                while ((HAL_GetTick() - t) < 1000 && sd_cmd(0x80|CMD41, 1UL<<30));
                if ((HAL_GetTick() - t) < 1000 && sd_cmd(CMD58, 0) == 0)
                {
                    for (n = 0; n < 4; n++) ocr[n] = spi_tx(0xFF);
                    ty = (ocr[0] & 0x40) ? CT_SD2 | CT_SDHC : CT_SD2;
                }
                else ty = 0;
            }
            else ty = 0;
        }
        else   /* SDv1 or MMC */
        {
            if (sd_cmd(0x80|CMD41, 0) <= 1)
            {
                ty  = CT_SD1;
                cmd = 0x80|CMD41;   /* ACMD41 for SD */
            }
            else
            {
                ty  = CT_MMC;
                cmd = CMD1;         /* CMD1 for MMC */
            }
            while ((HAL_GetTick() - t) < 1000 && sd_cmd(cmd, 0));
            if ((HAL_GetTick() - t) >= 1000 || sd_cmd(CMD16, 512) != 0)
                ty = 0;
        }
        s_card_type = ty;
    }

    sd_deselect();

    if (s_card_type)
    {
        s_status &= ~STA_NOINIT;
        return s_status;
    }
    return STA_NOINIT;
}

static DSTATUS SPI_GetStatus(BYTE lun)
{
    (void)lun;
    return s_status;
}

static DRESULT SPI_Read(BYTE lun, BYTE *buf, DWORD sector, UINT count)
{
    (void)lun;
    if (!count) return RES_PARERR;
    if (s_status & STA_NOINIT) return RES_NOTRDY;

    /* SDSC uses byte address, SDHC uses sector address */
    if (!(s_card_type & CT_SDHC)) sector *= 512;

    DRESULT res = RES_ERROR;

    if (count == 1)
    {
        if ((sd_cmd(CMD17, sector) == 0) && sd_read_datablock(buf, 512))
            res = RES_OK;
    }
    else
    {
        /* Multi-block read */
        if (sd_cmd(18, sector) == 0)   /* READ_MULTIPLE_BLOCK */
        {
            do {
                if (!sd_read_datablock(buf, 512)) break;
                buf += 512;
            } while (--count);
            sd_cmd(12, 0);   /* STOP_TRANSMISSION */
            if (!count) res = RES_OK;
        }
    }

    sd_deselect();
    return res;
}

static DRESULT SPI_Write(BYTE lun, const BYTE *buf, DWORD sector, UINT count)
{
    (void)lun;
    if (!count) return RES_PARERR;
    if (s_status & STA_NOINIT)  return RES_NOTRDY;
    if (s_status & STA_PROTECT) return RES_WRPRT;

    if (!(s_card_type & CT_SDHC)) sector *= 512;

    DRESULT res = RES_ERROR;

    if (count == 1)
    {
        if ((sd_cmd(CMD24, sector) == 0) && sd_write_datablock(buf, 0xFE))
            res = RES_OK;
    }
    else
    {
        if (s_card_type & CT_SD1) sd_cmd(0x80|23, count);   /* ACMD23 pre-erase */
        if (sd_cmd(25, sector) == 0)   /* WRITE_MULTIPLE_BLOCK */
        {
            do {
                if (!sd_write_datablock(buf, 0xFC)) break;
                buf += 512;
            } while (--count);
            if (!sd_write_datablock(0, 0xFD)) res = RES_ERROR; /* stop token */
            else if (!count) res = RES_OK;
        }
    }

    sd_deselect();
    return res;
}

static DRESULT SPI_Ioctl(BYTE lun, BYTE cmd, void *buf)
{
    (void)lun;
    if (s_status & STA_NOINIT) return RES_NOTRDY;

    DRESULT res = RES_ERROR;
    uint8_t csd[16];

    switch (cmd)
    {
    case CTRL_SYNC:
        if (sd_select()) { sd_deselect(); res = RES_OK; }
        break;

    case GET_SECTOR_COUNT:
        if ((sd_cmd(CMD9, 0) == 0) && sd_read_datablock(csd, 16))
        {
            DWORD sectors;
            if ((csd[0] >> 6) == 1)   /* SDv2 CSD */
            {
                DWORD cs = ((DWORD)(csd[7] & 0x3F) << 16)
                         | ((DWORD)csd[8] << 8)
                         |  (DWORD)csd[9];
                sectors = (cs + 1) << 10;
            }
            else                       /* SDv1 CSD */
            {
                uint8_t n  = (csd[5] & 0x0F) + ((csd[10] & 0x80) >> 7) + ((csd[9] & 0x03) << 1) + 2;
                DWORD   cs = ((DWORD)(csd[6] & 0x03) << 10)
                           | ((DWORD)csd[7] << 2)
                           | ((csd[8] & 0xC0) >> 6);
                sectors = (cs + 1) << (n - 9);
            }
            *(DWORD*)buf = sectors;
            res = RES_OK;
        }
        sd_deselect();
        break;

    case GET_SECTOR_SIZE:
        *(WORD*)buf = 512;
        res = RES_OK;
        break;

    case GET_BLOCK_SIZE:
        *(DWORD*)buf = 128;   /* assume 64kB erase block */
        res = RES_OK;
        break;

    default:
        res = RES_PARERR;
    }
    return res;
}

/* ── Driver registration structure ─────────────────────────────────────── */
Diskio_drvTypeDef SPI_Driver = {
    SPI_Initialize,
    SPI_GetStatus,
    SPI_Read,
    SPI_Write,
    SPI_Ioctl,
};
