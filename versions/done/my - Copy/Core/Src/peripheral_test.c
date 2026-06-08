/* ============================================================
 * peripheral_test.c — Діагностика периферії Kamerton
 *
 * Плата: STM32F407G-DISC1
 * Виведення: LCD 20×4 (PCF8574 I2C) + LEDs PD12..PD15
 *
 * Використання:
 *   В main.c після всіх MX_*_Init() замість основного while(1):
 *       PeripheralTest_Run();   // нескінченна петля, не повертається
 *
 * Управління (TTP229 клавіатура):
 *   Кл.1 → Joystick    Кл.2 → Keyboard TTP229
 *   Кл.3 → Microphone  Кл.4 → SD card
 * ============================================================ */

#include "peripheral_test.h"
#include "main.h"
#include "LCD1602.h"
#include "ttp229.h"
#include "joystick.h"
#include "microphone.h"
#include "fatfs.h"          /* SDPath, SDFatFS */
#include "ff.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* ── On-board LEDs ──────────────────────────────────────────── */
#define LED_GREEN   GPIO_PIN_12
#define LED_ORANGE  GPIO_PIN_13
#define LED_RED     GPIO_PIN_14
#define LED_BLUE    GPIO_PIN_15
#define LED_PORT    GPIOD

static void leds_off(void)
{
    HAL_GPIO_WritePin(LED_PORT,
        LED_GREEN|LED_ORANGE|LED_RED|LED_BLUE, GPIO_PIN_RESET);
}
static void leds_on(uint16_t mask)
{
    leds_off();
    HAL_GPIO_WritePin(LED_PORT, mask, GPIO_PIN_SET);
}

/* ── LCD helpers ─────────────────────────────────────────────── */
static void lrow(uint8_t row, const char *s)
{
    char buf[21];
    snprintf(buf, sizeof(buf), "%-20s", s);
    LCD_SetCursor(row, 0);
    LCD_WriteString(buf);
}
static void lrowf(uint8_t row, const char *fmt, ...)
{
    char buf[21];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lrow(row, buf);
}

/* ── Режими ──────────────────────────────────────────────────── */
typedef enum {
    TST_JOYSTICK = 0,
    TST_KEYBOARD,
    TST_MICROPHONE,
    TST_SDCARD,
    TST_COUNT
} TestMode_t;

/* ─── Глобальний стан мікрофона ──────────────────────────────── */
static uint8_t  g_mic_running = 0;

/* ─── Результат SD-тесту (зберігається між оновленнями) ─────── */
static enum { SD_IDLE, SD_OK, SD_FAIL } g_sd_result = SD_IDLE;
static char g_sd_line1[21];
static char g_sd_line2[21];
static char g_sd_line3[21];

/* ============================================================
 * TEST 1: ДЖОЙСТИК
 *   X=PC1/ADC2_IN11, Y=PC2/ADC2_IN12, BTN=PE10 (pull-up, LOW=pressed)
 * ============================================================ */
static void joy_init(void)
{
    LCD_Clear();
    lrow(0, "=== JOYSTICK TEST ===");
    lrow(3, "Kl.1-4: zminyty test");
    leds_on(LED_GREEN);
}

static void joy_tick(void)
{
    static uint32_t last = 0;
    if (HAL_GetTick() - last < 120) return;
    last = HAL_GetTick();

    Joystick_Update();
    uint16_t x   = Joystick_GetX();
    uint16_t y   = Joystick_GetY();
    uint8_t  btn = Joystick_IsPressed();

    const char *dir = "CENTER  ";
    if      (x < 1648) dir = "LEFT << ";
    else if (x > 2448) dir = "RIGHT >>";
    else if (y < 1648) dir = "UP ^^^  ";
    else if (y > 2448) dir = "DOWN vvv";

    lrowf(1, "X:%4u  Y:%4u", x, y);
    lrowf(2, "Dir: %-8s Btn:%s", dir, btn ? "ON " : "OFF");

    HAL_GPIO_WritePin(LED_PORT, LED_ORANGE,
                      btn ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* ============================================================
 * TEST 2: КЛАВІАТУРА TTP229 (16 клавіш)
 *   SCL=PE8 (output), SDO=PE9 (input pull-up)
 *   TTP229 active-LOW: 0=pressed → driver інвертує → 1=pressed
 * ============================================================ */
static void kbd_init(void)
{
    LCD_Clear();
    lrow(0, "== KEYBOARD TTP229 =");
    lrow(3, "Kl.1-4: zminyty test");
    leds_on(LED_ORANGE);
}

static void kbd_tick(void)
{
    static uint32_t last = 0;
    if (HAL_GetTick() - last < 30) return;
    last = HAL_GetTick();

    TTP229_Update();
    uint16_t raw = TTP229_GetRaw();

    /* Рядок 1: клавіші 1-8 */
    char r1[21] = "1:. 2:. 3:. 4:.     ";
    for (uint8_t k = 1; k <= 4; k++)
        r1[(k-1)*4 + 2] = (raw & (1u << (k-1))) ? 'X' : '.';
    lrow(1, r1);

    /* Рядок 2: клавіші 5-8 */
    char r2[21] = "5:. 6:. 7:. 8:.     ";
    for (uint8_t k = 5; k <= 8; k++)
        r2[(k-5)*4 + 2] = (raw & (1u << (k-1))) ? 'X' : '.';
    lrow(2, r2);

    /* Рядок 3: яка зараз натиснута */
    char info[21] = "Press: none         ";
    for (uint8_t k = 1; k <= 16; k++) {
        if (raw & (1u << (k-1))) {
            snprintf(info, sizeof(info), "Press: K%02u  (0x%04X)", k, raw);
            break;
        }
    }
    lrow(3, info);

    /* Бітова карта 1-16 у рядку 3 */
    /* LED: блимає якщо будь-яка клавіша натиснута */
    if (raw) HAL_GPIO_TogglePin(LED_PORT, LED_RED);
    else     HAL_GPIO_WritePin(LED_PORT, LED_RED, GPIO_PIN_RESET);
}

/* ============================================================
 * TEST 3: МІКРОФОН MAX9814
 *   PC3 = ADC1_IN13, TIM3 TRGO, DMA2 Stream0 (16 кГц)
 * ============================================================ */
static void mic_init(void)
{
    LCD_Clear();
    lrow(0, "=== MIC MAX9814 ====");
    lrow(1, "Initializing...     ");
    lrow(2, "                    ");
    lrow(3, "Kl.1-4: zminyty test");
    leds_on(LED_BLUE);

    if (!g_mic_running) {
        MIC_Init();
        MIC_Start();
        g_mic_running = 1;
    }
}

static void mic_tick(void)
{
    MIC_Process();   /* pitch detection (non-blocking) */

    static uint32_t last = 0;
    if (HAL_GetTick() - last < 200) return;
    last = HAL_GetTick();

    float   freq  = MIC_GetFrequency();
    uint8_t valid = MIC_IsSignalValid();

    /* Рядок 1: частота */
    if (valid && freq > 0.0f)
        lrowf(1, "Freq: %7.2f Hz      ", freq);
    else
        lrow(1, "Freq: --- (silence) ");

    /* Рядок 2: статус */
    lrowf(2, "Signal: %-12s", valid ? "DETECTED !" : "no signal...");

    /* Рядок 3: текстова VU-смуга  50..2000 Гц → 0..12 символів */
    char vu[21];
    memset(vu, '-', 12);
    vu[12] = ' '; vu[13] = '\0';
    if (valid && freq >= 50.0f) {
        int bars = (int)(12.0f * log10f(freq / 50.0f) / log10f(2000.0f / 50.0f));
        if (bars > 12) bars = 12;
        for (int i = 0; i < bars; i++) vu[i] = '#';
    }
    lrowf(3, "[%-12s]", vu);

    if (valid) HAL_GPIO_TogglePin(LED_PORT, LED_BLUE);
    else       HAL_GPIO_WritePin(LED_PORT, LED_BLUE, GPIO_PIN_RESET);
}

/* ============================================================
 * TEST 4: SD-КАРТА (SPI2 + FatFS)
 *   PB10=SCK, PB14=MISO, PB15=MOSI, PB12=CS (GPIO Output)
 * ============================================================ */
static void sd_init(void)
{
    LCD_Clear();
    lrow(0, "=== SD CARD TEST ====");
    lrow(1, "Testing...           ");
    lrow(2, "                    ");
    lrow(3, "Please wait...      ");
    leds_on(LED_BLUE | LED_GREEN);

    g_sd_result = SD_IDLE;

    /* ── Монтування ── */
    FRESULT fr = f_mount(&SDFatFS, SDPath, 1);
    if (fr != FR_OK) {
        snprintf(g_sd_line1, sizeof(g_sd_line1), "Mount FAIL (err=%d) ", (int)fr);
        snprintf(g_sd_line2, sizeof(g_sd_line2), "Perevirte SD-kartu  ");
        snprintf(g_sd_line3, sizeof(g_sd_line3), "abo kontakty SPI    ");
        g_sd_result = SD_FAIL;
        leds_on(LED_RED);
        goto done;
    }

    /* ── Вільне місце ── */
    {
        DWORD   fre_clust = 0;
        FATFS  *fsp = &SDFatFS;
        f_getfree(SDPath, &fre_clust, &fsp);
        uint32_t tot_sect = (fsp->n_fatent - 2) * fsp->csize;
        uint32_t fre_sect = fre_clust * fsp->csize;
        uint32_t tot_mb   = tot_sect / 2048u;
        uint32_t fre_mb   = fre_sect / 2048u;
        snprintf(g_sd_line1, sizeof(g_sd_line1),
                 "Mount OK! T:%luMB F:%luMB", tot_mb, fre_mb);
    }

    /* ── Запис тестового файлу ── */
    {
        FIL f;
        const char *path    = "/KAMERTON_TEST.TXT";
        const char *payload = "Kamerton SD test OK\r\n";
        UINT bw = 0;

        fr = f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE);
        if (fr != FR_OK) {
            snprintf(g_sd_line2, sizeof(g_sd_line2),
                     "Open FAIL (err=%d)  ", (int)fr);
            snprintf(g_sd_line3, sizeof(g_sd_line3), "SD write error!     ");
            g_sd_result = SD_FAIL;
            leds_on(LED_RED);
            goto unmount;
        }
        f_write(&f, payload, strlen(payload), &bw);
        f_close(&f);

        snprintf(g_sd_line2, sizeof(g_sd_line2),
                 "Write OK (%u bytes) ", (unsigned)bw);

        /* ── Читання назад ── */
        char rbuf[40] = {0};
        UINT br = 0;
        fr = f_open(&f, path, FA_READ);
        if (fr == FR_OK) {
            f_read(&f, rbuf, sizeof(rbuf) - 1, &br);
            f_close(&f);
        }
        f_unlink(path);   /* прибираємо тестовий файл */

        int ok = (br > 0) && (memcmp(rbuf, payload, strlen(payload)) == 0);
        if (ok) {
            snprintf(g_sd_line3, sizeof(g_sd_line3),
                     "Read OK! SD works!  ");
            g_sd_result = SD_OK;
            leds_on(LED_GREEN);
        } else {
            snprintf(g_sd_line3, sizeof(g_sd_line3),
                     "Read MISMATCH! FAIL ");
            g_sd_result = SD_FAIL;
            leds_on(LED_RED);
        }
    }

unmount:
    f_mount(NULL, SDPath, 0);
done:
    lrow(1, g_sd_line1);
    lrow(2, g_sd_line2);
    lrow(3, g_sd_line3);
}

static void sd_tick(void)
{
    /* Результат вже відображено в sd_init() — просто блимаємо LED */
    static uint32_t last = 0;
    if (HAL_GetTick() - last < 500) return;
    last = HAL_GetTick();

    if (g_sd_result == SD_OK)
        HAL_GPIO_TogglePin(LED_PORT, LED_GREEN);
    else if (g_sd_result == SD_FAIL)
        HAL_GPIO_TogglePin(LED_PORT, LED_RED);
}

/* ============================================================
 * ГОЛОВНИЙ ЦИКЛ ТЕСТУВАННЯ
 * ============================================================ */
void PeripheralTest_Run(void)
{
    /* ── LED GPIO ─────────────────────────────────────────────── */
    {
        GPIO_InitTypeDef g = {0};
        __HAL_RCC_GPIOD_CLK_ENABLE();
        g.Pin   = LED_GREEN|LED_ORANGE|LED_RED|LED_BLUE;
        g.Mode  = GPIO_MODE_OUTPUT_PP;
        g.Pull  = GPIO_NOPULL;
        g.Speed = GPIO_SPEED_FREQ_LOW;
        HAL_GPIO_Init(LED_PORT, &g);
        leds_off();
    }

    /* ── Периферія ─────────────────────────────────────────────── */
    TTP229_Init();
    Joystick_Init();
    LCD_Init();
    LCD_LoadGraphIcons();

    /* ── Вступний екран ──────────────────────────────────────── */
    LCD_Clear();
    lrow(0, "=== DIAGNOSTYKA ====");
    lrow(1, " Kl.1: Joystick    ");
    lrow(2, " Kl.2: Keyboard    ");
    lrow(3, " Kl.3: Mic Kl.4:SD ");
    leds_on(LED_ORANGE | LED_BLUE);
    HAL_Delay(2500);

    /* ── Стан машини ─────────────────────────────────────────── */
    TestMode_t mode      = TST_JOYSTICK;
    TestMode_t prev_mode = (TestMode_t)0xFF;   /* перший запуск */

    joy_init();
    prev_mode = TST_JOYSTICK;

    while (1)
    {
        /* ── Опитування клавіатури ─────────────────────────── */
        TTP229_Update();

        TestMode_t new_mode = mode;
        if (TTP229_JustPressed(1)) new_mode = TST_JOYSTICK;
        if (TTP229_JustPressed(2)) new_mode = TST_KEYBOARD;
        if (TTP229_JustPressed(3)) new_mode = TST_MICROPHONE;
        if (TTP229_JustPressed(4)) new_mode = TST_SDCARD;

        /* ── Перемикання режиму ─────────────────────────────── */
        if (new_mode != mode) {
            mode = new_mode;
            leds_off();

            switch (mode) {
                case TST_JOYSTICK:   joy_init(); break;
                case TST_KEYBOARD:   kbd_init(); break;
                case TST_MICROPHONE: mic_init(); break;
                case TST_SDCARD:     sd_init();  break;
                default: break;
            }
        }

        /* ── Поточний тест (виклик кожну ітерацію) ─────────── */
        switch (mode) {
            case TST_JOYSTICK:   joy_tick(); break;
            case TST_KEYBOARD:   kbd_tick(); break;
            case TST_MICROPHONE: mic_tick(); break;
            case TST_SDCARD:     sd_tick();  break;
            default: break;
        }

        HAL_Delay(5);
    }
}
