#pragma once
#include "main.h"

/* Joystick module driver
 * ADC2 software polling (~10 Hz)
 *   PC1  = ADC2_IN11  → X-axis  (left/right)
 *   PC2  = ADC2_IN12  → Y-axis  (up/down)
 *   PE10 = GPIO_Input, Pull-Up  → push button (active LOW)
 *
 * Deadzone: raw values 0..4095, centre ~2048 ±400 = no movement
 */

typedef enum {
    JOY_NONE  = 0,
    JOY_UP,
    JOY_DOWN,
    JOY_LEFT,
    JOY_RIGHT,
    JOY_PRESS      /* button click */
} JoyEvent_t;

void       Joystick_Init(void);
void       Joystick_Update(void);   /* call every main-loop iteration        */
JoyEvent_t Joystick_GetEvent(void); /* returns one event then clears it      */
uint8_t    Joystick_IsPressed(void);/* current button state (1 = held)       */
uint16_t   Joystick_GetX(void);     /* raw ADC X 0-4095                      */
uint16_t   Joystick_GetY(void);     /* raw ADC Y 0-4095                      */
