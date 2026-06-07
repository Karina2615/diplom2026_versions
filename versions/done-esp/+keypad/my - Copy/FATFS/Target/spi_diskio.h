#pragma once
#include "ff_gen_drv.h"

/* SPI2 SD card FatFS disk driver
 *   SPI2: PB10=SCK, PB14=MISO, PB15=MOSI
 *   PB12  = CS  (GPIO Output, active LOW)
 */

extern Diskio_drvTypeDef  SPI_Driver;
