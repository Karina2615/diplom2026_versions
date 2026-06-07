#pragma once
#include "ff_gen_drv.h"

/* SPI2 SD card FatFS disk driver
 *   SPI2: PB10=SCK, PB14=MISO, PB15=MOSI
 *   PB12  = CS  (GPIO Output, active LOW)
 */

extern Diskio_drvTypeDef  SPI_Driver;

/* Allow one new SPI init attempt (call before SDRec_Format or explicit retry).
 * Without this, SPI_Initialize returns STA_NOINIT instantly after first
 * failure — preventing repeated main-loop blocking by FatFS internals. */
void SPI_ResetInit(void);
