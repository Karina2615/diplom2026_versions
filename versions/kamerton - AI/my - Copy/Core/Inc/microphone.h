/*
 * microphone.h — PDM microphone capture and pitch detection
 *
 * Hardware:  MP45DT02 MEMS microphone on STM32F407G-DISC1
 *   PB10 = I2S2_CK   (PDM clock output to mic)
 *   PC3  = I2S2_SD   (PDM data  input  from mic)
 *
 * Signal chain:
 *   I2S2 master RX (DMA1/Stream3) → PDM_Filter (mono, ×64) → 16 kHz PCM
 *   → 1024-sample autocorrelation → detected pitch [Hz]
 */

#ifndef MICROPHONE_H
#define MICROPHONE_H

#include "stm32f4xx_hal.h"
#include <stdbool.h>

/* DMA handle — declared here so stm32f4xx_hal_msp.c can reference it */
extern DMA_HandleTypeDef hdma_spi2_rx;

/* Initialise I2S2 peripheral and reconfigure PDM filter for mono 16 kHz */
void  MIC_Init(void);

/* Start / stop DMA reception */
void  MIC_Start(void);
void  MIC_Stop(void);

/*
 * Run pitch detection on the accumulated PCM window.
 * Call from the main loop; returns immediately if no new window is ready.
 */
void  MIC_Process(void);

/* Query results (valid after MIC_Process) */
float MIC_GetFrequency(void);   /* detected fundamental in Hz; 0 = silence  */
bool  MIC_IsSignalValid(void);  /* true if confidence threshold was exceeded */

#endif /* MICROPHONE_H */
