#pragma once
#include "main.h"

/* TTP229-BSF 16-key capacitive keyboard driver
 * 2-wire serial protocol: PE8 = SCL (output), PE9 = SDO (input)
 * TP2 pin on TTP229 module must be pulled to GND for 16-key mode */

void     TTP229_Init(void);
void     TTP229_Update(void);          /* call every loop iteration      */
uint8_t  TTP229_IsPressed(uint8_t k); /* k = 1..16, returns 1 if pressed */
uint8_t  TTP229_JustPressed(uint8_t k);
uint8_t  TTP229_JustReleased(uint8_t k);
uint16_t TTP229_GetRaw(void);          /* bitmask: bit(k-1) = key k      */
uint32_t TTP229_GetScanCount(void);    /* total completed scan cycles     */
