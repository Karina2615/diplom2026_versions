/*
 * LCD1602.c — HD44780-based 2004A LCD driver via PCF8574 I2C backpack
 *
 * Hardware
 * --------
 * The PCF8574 is an 8-bit I/O expander on the I2C bus.  A single byte
 * written to its address controls all 8 pins simultaneously.
 * Standard black-module pin mapping:
 *
 *   PCF8574 pin | HD44780 signal
 *   ------------|---------------
 *   P0          | RS  (Register Select: 0 = command, 1 = data)
 *   P1          | RW  (always 0 = write in this driver)
 *   P2          | EN  (Enable pulse — data latched on falling edge)
 *   P3          | BL  (Backlight, active high)
 *   P4          | D4
 *   P5          | D5
 *   P6          | D6
 *   P7          | D7
 *
 * Protocol
 * --------
 * The HD44780 is driven in 4-bit mode.  Each byte (command or data) is
 * sent as two nibbles, upper first.  For each nibble:
 *   1. Write byte with EN=0 (data stable)
 *   2. Write byte with EN=1 (rising edge — HD44780 samples on fall)
 *   3. Write byte with EN=0 (falling edge — latch)
 *
 * At 100 kHz I2C, each byte transfer takes ~90 µs, which comfortably
 * satisfies the HD44780 timing requirements (EN pulse ≥ 450 ns,
 * command execution ≤ 37 µs).  Only the Clear Display and Return Home
 * commands need an explicit 2 ms delay (execution time > 1.52 ms).
 *
 * I2C bus sharing
 * ---------------
 * Uses hi2c1 (I2C1, PB6 SCL / PB9 SDA), shared with the CS43L22 audio
 * codec.  All calls are blocking and the application is single-threaded,
 * so no arbitration logic is required.
 */

#include "LCD1602.h"

/* -----------------------------------------------------------------------
 * PCF8574 bit positions
 * --------------------------------------------------------------------- */
#define PCF_RS   0x01u   /* P0 — Register Select */
#define PCF_RW   0x02u   /* P1 — Read/Write (always 0) */
#define PCF_EN   0x04u   /* P2 — Enable */
#define PCF_BL   0x08u   /* P3 — Backlight */
/* D4–D7 occupy bits 4–7 naturally */

/* Backlight state — kept high (on) throughout normal operation */
static uint8_t blState = PCF_BL;

/* I2C1 handle is owned by main.c */
extern I2C_HandleTypeDef hi2c1;

/* -----------------------------------------------------------------------
 * Low-level helpers
 * --------------------------------------------------------------------- */

/* Write one byte to the PCF8574 */
static inline void PCF_Write(uint8_t byte)
{
    HAL_I2C_Master_Transmit(&hi2c1, LCD_I2C_ADDR, &byte, 1u, 10u);
}

/*
 * Send a nibble (upper 4 bits of `nibble` parameter) to the HD44780.
 * rs=0 → command register, rs=PCF_RS → data register.
 *
 * Sequence:  set data + EN=0  →  EN=1  →  EN=0  (EN pulse)
 * Two I2C bytes total (~180 µs at 100 kHz, well above the 450 ns minimum).
 */
static void LCD_SendNibble(uint8_t nibble, uint8_t rs)
{
    /* Upper 4 bits of nibble go to D7..D4; lower bits hold control lines */
    uint8_t base = (nibble & 0xF0u) | blState | rs;   /* EN=0 */
    PCF_Write(base | PCF_EN);                          /* EN=1 */
    PCF_Write(base);                                   /* EN=0 — latch */
}

/* -----------------------------------------------------------------------
 * Public send primitives
 * --------------------------------------------------------------------- */

void LCD_SendCommand(uint8_t cmd)
{
    LCD_SendNibble(cmd,         0u);      /* upper nibble, RS=0 */
    LCD_SendNibble(cmd << 4u,  0u);      /* lower nibble, RS=0 */
}

void LCD_SendData(uint8_t data)
{
    LCD_SendNibble(data,        PCF_RS); /* upper nibble, RS=1 */
    LCD_SendNibble(data << 4u, PCF_RS); /* lower nibble, RS=1 */
}

/* -----------------------------------------------------------------------
 * Initialisation
 *
 * Follows the HD44780 "Initialising by Instruction" flowchart (datasheet
 * Figure 24) for 4-bit mode, adapted for indirect access via PCF8574.
 *
 * The tricky part: the controller may power up in 8-bit mode and we must
 * coax it into 4-bit mode by sending the upper nibble 0x3 three times
 * (with correct delays), then 0x2 to switch to 4-bit.  After that every
 * command uses two nibbles.
 * --------------------------------------------------------------------- */
void LCD_Init(void)
{
    HAL_Delay(50u);                /* > 40 ms after VCC rises to 2.7 V */

    PCF_Write(blState);            /* all LCD lines low, backlight on  */
    HAL_Delay(10u);

    /* Step 1–3: send "Function Set 8-bit" three times (upper nibble only) */
    LCD_SendNibble(0x30u, 0u);  HAL_Delay(5u);   /* > 4.1 ms */
    LCD_SendNibble(0x30u, 0u);  HAL_Delay(1u);   /* > 100 µs */
    LCD_SendNibble(0x30u, 0u);  HAL_Delay(1u);

    /* Step 4: switch to 4-bit bus */
    LCD_SendNibble(0x20u, 0u);  HAL_Delay(1u);

    /* From here every command = 2 nibbles ----------------------------- */

    /* Function Set: 4-bit bus, 2-line display, 5×8 font */
    LCD_SendCommand(0x28u);  HAL_Delay(1u);

    /* Display control: display off */
    LCD_SendCommand(0x08u);  HAL_Delay(1u);

    /* Clear display (> 1.52 ms — delay is inside LCD_Clear) */
    LCD_Clear();

    /* Entry mode: increment cursor, no display shift */
    LCD_SendCommand(0x06u);  HAL_Delay(1u);

    /* Display control: display on, cursor off, blink off */
    LCD_SendCommand(0x0Cu);  HAL_Delay(1u);
}

/* -----------------------------------------------------------------------
 * Standard operations
 * --------------------------------------------------------------------- */

void LCD_Clear(void)
{
    LCD_SendCommand(0x01u);
    HAL_Delay(2u);   /* clear + return home: execution time > 1.52 ms */
}

void LCD_SetCursor(uint8_t row, uint8_t col)
{
    /*
     * 2004A DDRAM base addresses per row (HD44780 datasheet, Table 2):
     *   Row 0 → 0x00,  Row 1 → 0x40,  Row 2 → 0x14,  Row 3 → 0x54
     */
    static const uint8_t rowBase[4] = { 0x00u, 0x40u, 0x14u, 0x54u };
    LCD_SendCommand(0x80u | (rowBase[row % 4u] + col));
}

void LCD_WriteString(char *str)
{
    while (*str) {
        LCD_SendData((uint8_t)*str++);
    }
}

/* -----------------------------------------------------------------------
 * Custom character support (CGRAM, 8 slots, 5×8 pixels each)
 * --------------------------------------------------------------------- */
void LCD_CreateCustomChar(uint8_t location, uint8_t *charmap)
{
    location &= 0x07u;                          /* only 8 slots (0–7) */
    LCD_SendCommand(0x40u | (location << 3u));  /* set CGRAM address  */
    for (int i = 0; i < 8; i++) {
        LCD_SendData(charmap[i]);
    }
    /* Return to DDRAM (required after CGRAM write) */
    LCD_SendCommand(0x80u);
}

/*
 * Bar-graph characters: each char fills n rows from the bottom.
 * Used by Task_LCD in main.c for the animated EQ on row 3 and the
 * volume bar on row 2.
 *   Custom char index 0 = 1-pixel bar (char code 0)
 *   Custom char index 7 = 8-pixel bar (char code 7, full block)
 */
static const uint8_t BAR_CHARS[8][8] = {
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x1F }, /* height 1 */
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x1F,0x1F }, /* height 2 */
    { 0x00,0x00,0x00,0x00,0x00,0x1F,0x1F,0x1F }, /* height 3 */
    { 0x00,0x00,0x00,0x00,0x1F,0x1F,0x1F,0x1F }, /* height 4 */
    { 0x00,0x00,0x00,0x1F,0x1F,0x1F,0x1F,0x1F }, /* height 5 */
    { 0x00,0x00,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F }, /* height 6 */
    { 0x00,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F }, /* height 7 */
    { 0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F }, /* height 8 */
};

void LCD_LoadGraphIcons(void)
{
    for (uint8_t i = 0u; i < 8u; i++) {
        LCD_CreateCustomChar(i, (uint8_t *)BAR_CHARS[i]);
    }
}

void LCD_UpdateVolBar(uint8_t volume)
{
    (void)volume; /* volume bar is drawn directly in main.c Task_LCD */
}
