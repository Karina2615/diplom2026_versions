#ifndef LCD1602_H
#define LCD1602_H

#include "stm32f4xx_hal.h"

/* Display geometry — 2004A module (20 columns × 4 rows) */
#define LCD_COLS  20u
#define LCD_ROWS   4u

/*
 * PCF8574 I2C address (8-bit, HAL convention = 7-bit shifted left by 1).
 * Default 0x4E  (= 0x27 << 1) when all three A0/A1/A2 jumpers are open.
 * Change to 0x7E (= 0x3F << 1) if your module has all jumpers closed.
 */
#ifndef LCD_I2C_ADDR
#define LCD_I2C_ADDR  (0x27u << 1u)
#endif

// Основні функції
void LCD_Init(void);
void LCD_Clear(void);
void LCD_SetCursor(uint8_t row, uint8_t col);
void LCD_WriteString(const char *str);
void LCD_SendData(uint8_t data); // <--- ДОДАНО!

// Графіка та спецсимволи
void LCD_CreateCustomChar(uint8_t location, uint8_t *charmap); // <--- ДОДАНО!
void LCD_UpdateVolBar(uint8_t volume);
void LCD_LoadGraphIcons(void);

#endif
