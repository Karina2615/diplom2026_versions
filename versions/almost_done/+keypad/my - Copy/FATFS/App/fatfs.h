/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file   fatfs.h
  * @brief  Header for fatfs applications — SPI SD card
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef __fatfs_H
#define __fatfs_H
#ifdef __cplusplus
 extern "C" {
#endif

#include "ff.h"
#include "ff_gen_drv.h"
#include "spi_diskio.h"   /* SPI_Driver */

/* USER CODE BEGIN Includes */
/* USER CODE END Includes */

extern uint8_t retSD;       /* Return value for SD          */
extern char    SDPath[4];   /* SD logical drive path        */
extern FATFS   SDFatFS;     /* File system object for SD    */
extern FIL     SDFile;      /* File object for SD           */

void MX_FATFS_Init(void);

/* USER CODE BEGIN Prototypes */
/* USER CODE END Prototypes */
#ifdef __cplusplus
}
#endif
#endif /*__fatfs_H */
