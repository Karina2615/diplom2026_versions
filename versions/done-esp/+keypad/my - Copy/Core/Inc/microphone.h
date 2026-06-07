/* microphone.h — MAX9814 microphone capture and pitch detection
 *
 * Hardware: MAX9814 analog mic on STM32F407G-DISC1
 *   PC3 = ADC1_IN13  (analog input from MAX9814 OUT pin)
 *
 * Signal chain:
 *   TIM3 trigger (16 kHz) → ADC1 CH13 DMA → 16-bit PCM ring buffer
 *   → 1024-sample autocorrelation → detected pitch [Hz]
 *
 * MIC_GetBuffer() lets the sampler read raw PCM for recording.
 */

#ifndef MICROPHONE_H
#define MICROPHONE_H

#include "stm32f4xx_hal.h"
#include <stdbool.h>

/* DMA handle — declared here so stm32f4xx_hal_msp.c can reference it */
extern DMA_HandleTypeDef hdma_adc1;

/* Initialise ADC1 + TIM3 trigger + DMA */
void  MIC_Init(void);

/* Start / stop DMA conversion */
void  MIC_Start(void);
void  MIC_Stop(void);

/*
 * Run pitch detection on the accumulated PCM window.
 * Call from the main loop; returns immediately if no new window is ready.
 */
void  MIC_Process(void);

/* Query results (valid after MIC_Process) */
float MIC_GetFrequency(void);    /* detected fundamental in Hz; 0 = silence  */
uint8_t MIC_IsSignalValid(void); /* 1 if confidence threshold was exceeded    */

/* Raw PCM access for sampler recording
 * Returns pointer to the last filled half-buffer and its length in samples.
 * Pointer is valid until next MIC_Process() call.
 */
const int16_t* MIC_GetBuffer(uint16_t *out_samples);

#endif /* MICROPHONE_H */
