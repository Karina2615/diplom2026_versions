/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file   fatfs.c
  * @brief  Code for fatfs applications — SPI SD card driver
  ******************************************************************************
  */
/* USER CODE END Header */
#include "fatfs.h"

uint8_t retSD;       /* Return value for SD */
char    SDPath[4];   /* SD logical drive path */
FATFS   SDFatFS;     /* File system object for SD logical drive */
FIL     SDFile;      /* File object for SD */

/* USER CODE BEGIN Variables */

/* USER CODE END Variables */

void MX_FATFS_Init(void)
{
    /*## FatFS: Link the SPI SD card driver ################################*/
    retSD = FATFS_LinkDriver(&SPI_Driver, SDPath);

    /* USER CODE BEGIN Init */
    /* USER CODE END Init */
}

/**
  * @brief  Gets Time from RTC (stub — returns fixed timestamp)
  */
DWORD get_fattime(void)
{
    /* USER CODE BEGIN get_fattime */
    /* Return 2025-01-01 00:00:00 as a fixed FAT timestamp */
    return ((DWORD)(2025 - 1980) << 25)
         | ((DWORD)1  << 21)
         | ((DWORD)1  << 16)
         | ((DWORD)0  << 11)
         | ((DWORD)0  << 5)
         | ((DWORD)0);
    /* USER CODE END get_fattime */
}

/* USER CODE BEGIN Application */

/* USER CODE END Application */
