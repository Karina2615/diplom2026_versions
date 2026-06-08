/* ============================================================
 * main_kbd_test.c  —  Тест клавіатури TTP229 + LCD 20×4
 *
 * Як використовувати:
 *   1. Перейменуй поточний Core/Src/main.c  →  main_full.c
 *   2. Перейменуй цей файл                  →  main.c
 *   3. Збери і прошивай.
 *   4. Після тесту поверни назад.
 *
 * Що ініціалізується:
 *   GPIO (всі порти), I2C1 (PB6/PB9), LCD 20×4, TTP229 (PE8/PE9)
 *
 * Що відображається на LCD:
 *   Рядок 0: "=== KBD TEST ==="
 *   Рядок 1: "Raw: 0x0000" (hex-маска натиснутих клавіш)
 *   Рядок 2: "Keys: __ __ __ __ __" (номери натиснутих, до 5)
 *   Рядок 3: "Last: k=??" (остання натиснута клавіша)
 *
 * Також: натискання клавіш блимає LED (PD12-PD15).
 * ============================================================ */

#include "main.h"
#include "LCD1602.h"
#include "ttp229.h"
#include <stdio.h>
#include <string.h>

/* ── HAL handles needed by LCD (hi2c1) and system ── */
I2C_HandleTypeDef  hi2c1;
CRC_HandleTypeDef  hcrc;   /* needed by stm32f4xx_hal_crc.c if linked */

/* ── Forward declarations ── */
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);

/* ── LEDs on PD12-PD15 (Discovery board) ── */
#define LED_PORT  GPIOD
#define LED_ALL   (GPIO_PIN_12|GPIO_PIN_13|GPIO_PIN_14|GPIO_PIN_15)

static void leds_init(void)
{
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_GPIOD_CLK_ENABLE();
    HAL_GPIO_WritePin(LED_PORT, LED_ALL, GPIO_PIN_RESET);
    g.Pin   = LED_ALL;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LED_PORT, &g);
}

/* ── LCD helpers ── */
static void lcd_row(uint8_t row, const char *s)
{
    char buf[21];
    snprintf(buf, sizeof(buf), "%-20s", s);
    LCD_SetCursor(row, 0);
    LCD_WriteString(buf);
}

/* ── Main ── */
int main(void)
{
    HAL_Init();
    SystemClock_Config();

    MX_GPIO_Init();
    MX_I2C1_Init();
    leds_init();

    LCD_Init();

    /* Banner */
    lcd_row(0, "=== KBD TEST ===");
    lcd_row(1, "Raw: 0x0000         ");
    lcd_row(2, "Keys: --            ");
    lcd_row(3, "Last: --            ");

    TTP229_Init();

    uint8_t  last_key  = 0;
    uint32_t blink_end = 0;

    while (1)
    {
        TTP229_Update();

        uint16_t raw = TTP229_GetRaw();

        /* ── Рядок 1: hex-маска ── */
        char buf[21];
        snprintf(buf, sizeof(buf), "Raw: 0x%04X         ", (unsigned)raw);
        lcd_row(1, buf);

        /* ── Рядок 2: список натиснутих (до 5 клавіш) ── */
        char keys_str[21];
        memset(keys_str, ' ', 20);
        keys_str[20] = '\0';
        int pos = 0;
        int found = 0;
        for (uint8_t k = 1; k <= 16 && found < 5; k++)
        {
            if (TTP229_IsPressed(k))
            {
                int n = snprintf(keys_str + pos, sizeof(keys_str) - pos,
                                 "%2u ", (unsigned)k);
                pos   += n;
                found++;

                /* Запам'ятовуємо останню натиснуту */
                last_key = k;
            }
        }
        if (found == 0) {
            strncpy(keys_str, "(none)              ", 20);
        }
        lcd_row(2, keys_str);

        /* ── Рядок 3: остання клавіша ── */
        if (last_key > 0) {
            snprintf(buf, sizeof(buf), "Last: k=%-2u          ", (unsigned)last_key);
        } else {
            snprintf(buf, sizeof(buf), "Last: --            ");
        }
        lcd_row(3, buf);

        /* ── LED: мигає при натисканні ── */
        if (raw != 0) {
            blink_end = HAL_GetTick() + 100;
            HAL_GPIO_WritePin(LED_PORT, LED_ALL, GPIO_PIN_SET);
        }
        if (HAL_GetTick() >= blink_end) {
            HAL_GPIO_WritePin(LED_PORT, LED_ALL, GPIO_PIN_RESET);
        }

        HAL_Delay(50);   /* оновлення ~20 разів/сек */
    }
}

/* ── SystemClock_Config: 168 MHz (копія з основного main.c) ── */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState            = RCC_HSE_ON;
    RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource       = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLM            = 8;
    RCC_OscInitStruct.PLL.PLLN            = 336;
    RCC_OscInitStruct.PLL.PLLP            = RCC_PLLP_DIV2;
    RCC_OscInitStruct.PLL.PLLQ            = 7;
    HAL_RCC_OscConfig(&RCC_OscInitStruct);

    RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                     | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;
    HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5);
}

/* ── GPIO: лише те що потрібно для TTP229 і LED ── */
static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();

    /* PE8 = TTP229 SCL (output, idle HIGH) */
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_8, GPIO_PIN_SET);
    g.Pin   = GPIO_PIN_8;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOE, &g);

    /* PE9 = TTP229 SDO (input, pull-up) */
    g.Pin  = GPIO_PIN_9;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOE, &g);
}

/* ── I2C1: PB6=SCL, PB9=SDA, 100 kHz ── */
static void MX_I2C1_Init(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_I2C1_CLK_ENABLE();

    g.Pin       = GPIO_PIN_6 | GPIO_PIN_9;
    g.Mode      = GPIO_MODE_AF_OD;
    g.Pull      = GPIO_NOPULL;
    g.Speed     = GPIO_SPEED_FREQ_LOW;
    g.Alternate = GPIO_AF4_I2C1;
    HAL_GPIO_Init(GPIOB, &g);

    hi2c1.Instance             = I2C1;
    hi2c1.Init.ClockSpeed      = 100000;
    hi2c1.Init.DutyCycle       = I2C_DUTYCYCLE_2;
    hi2c1.Init.OwnAddress1     = 0;
    hi2c1.Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.OwnAddress2     = 0;
    hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode   = I2C_NOSTRETCH_DISABLE;
    HAL_I2C_Init(&hi2c1);
}

/* ── Error_Handler (обов'язково) ── */
void Error_Handler(void)
{
    __disable_irq();
    /* Мигання червоним LED при помилці */
    while (1)
    {
        HAL_GPIO_TogglePin(GPIOD, GPIO_PIN_14);
        for (volatile int i = 0; i < 1000000; i++) {}
    }
}
