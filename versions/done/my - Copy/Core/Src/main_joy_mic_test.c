/* ============================================================
 * main_joy_mic_test.c — Тест KY-023 + MAX9814 + TTP229 + LCD 20×4
 *
 * Джойстик: ADC2 scan + DMA2/Stream2/CH1  (controllerstech підхід)
 *   ADC2 IN11 = PC1 → VRx (X)  rank 1 → joy_buf[0]
 *   ADC2 IN12 = PC2 → VRy (Y)  rank 2 → joy_buf[1]
 *   DMA2 Stream2 Channel1 → circular, без переривань CPU
 *
 * Мікрофон: ADC1 software polling (IN13 = PC3)
 * Кнопка джойстика: PE10, GPIO_PULLUP, натиск = LOW
 *
 * Клавіатура TTP229-BSF (16 клавіш):
 *   PE8 = SCL (вихід, push-pull)
 *   PE9 = SDO (вхід,  pull-up)
 *   Перемичка TP2 на модулі → GND  (режим 16 клавіш)
 *
 * LCD 20×4:
 *   Рядок 0: X:xxxx  Y:xxxx
 *   Рядок 1: Dir:XXXXX   [BTN]
 *   Рядок 2: KBD: xx  0x????
 *   Рядок 3: M:########---- xxxx
 *
 * hdma_adc1 — визначено в microphone.c; НЕ дублюємо тут.
 * hdma_spi3_tx — заглушка для extern у stm32f4xx_it.c.
 * ============================================================ */

#include "main.h"
#include "LCD1602.h"
#include "ttp229.h"
#include <stdio.h>
#include <string.h>

/* ── HAL handles ─────────────────────────────────────────────── */
I2C_HandleTypeDef  hi2c1;
ADC_HandleTypeDef  hadc1;   /* мікрофон — ADC1 polling          */
ADC_HandleTypeDef  hadc2;   /* джойстик — ADC2 scan + DMA       */
CRC_HandleTypeDef  hcrc;

/* hdma_adc1 визначено у microphone.c → задовольняє stm32f4xx_it.c extern  */
/* hdma_spi3_tx: заглушка для stm32f4xx_it.c                                */
DMA_HandleTypeDef  hdma_spi3_tx;

/* ── DMA handle для ADC2 (локальний — ззовні не потрібен) ───── */
static DMA_HandleTypeDef hdma_adc2;   /* DMA2 Stream2 Channel1 → ADC2 */

/* ── DMA-буфер джойстика: [0]=X, [1]=Y ─────────────────────── */
static volatile uint32_t joy_buf[2];   /* безперервно оновлюється DMA  */

/* ── Пороги (12-bit, 0..4095) ───────────────────────────────── *
 * Якщо стрілки не спрацьовують — подивись реальні X/Y на LCD,  *
 * і підбери пороги відповідно до фактичного діапазону стіка.   */
#define JOY_LO   800u
#define JOY_HI  3200u

/* ── Forward declarations ─────────────────────────────────────── */
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);
static void MX_DMA_Init(void);
static void MX_ADC1_Init(void);
static void MX_ADC2_Init(void);

/* ── LCD helper ───────────────────────────────────────────────── */
static void lcd_row(uint8_t row, const char *s)
{
    char buf[21];
    snprintf(buf, sizeof(buf), "%-20s", s);
    LCD_SetCursor(row, 0);
    LCD_WriteString(buf);
}

/* ── Мікрофон: ADC1 polling, 128 відліків peak-to-peak ───────── */
static uint16_t mic_amplitude(void)
{
    ADC_ChannelConfTypeDef cfg = {0};
    cfg.Channel      = ADC_CHANNEL_13;   /* PC3 */
    cfg.Rank         = 1;
    cfg.SamplingTime = ADC_SAMPLETIME_56CYCLES;
    HAL_ADC_ConfigChannel(&hadc1, &cfg);

    uint16_t mn = 4095u, mx = 0u;
    for (uint16_t i = 0; i < 128u; i++) {
        HAL_ADC_Start(&hadc1);
        if (HAL_ADC_PollForConversion(&hadc1, 5) == HAL_OK) {
            uint16_t v = (uint16_t)HAL_ADC_GetValue(&hadc1);
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
    }
    return (mx > mn) ? (uint16_t)(mx - mn) : 0u;
}

/* ── Mic ASCII bar (12 символів) ─────────────────────────────── */
static void amp_bar(uint16_t amp, char *out12)
{
    uint32_t val = (uint32_t)amp * 12u / 2000u;
    if (val > 12u) val = 12u;
    for (int i = 0; i < 12; i++) out12[i] = (i < (int)val) ? '#' : '-';
    out12[12] = '\0';
}

/* ============================================================
 * main()
 * ============================================================ */
int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_I2C1_Init();

    LCD_Init();
    lcd_row(0, "JOY + MIC  TEST");
    lcd_row(1, "Init ADC+DMA...");
    lcd_row(2, "");
    lcd_row(3, "");
    HAL_Delay(300);

    /* Клавіатура TTP229 */
    TTP229_Init();          /* вмикає DWT, SCL → HIGH       */

    /* Порядок: DMA → ADC → Start */
    MX_DMA_Init();          /* DMA2 Stream2 для ADC2        */
    MX_ADC2_Init();         /* scan 2 канали, continuous     */
    MX_ADC1_Init();         /* single channel, polling для мік */

    /* Запускаємо ADC2 + DMA — joy_buf[0/1] оновлюється безперервно */
    HAL_ADC_Start_DMA(&hadc2, (uint32_t*)joy_buf, 2);

    /* Показуємо пороги (2 с) */
    {
        char tmp[21];
        snprintf(tmp, sizeof(tmp), "LO<%-4u  HI>%-4u", JOY_LO, JOY_HI);
        lcd_row(0, "JOY thresholds:");
        lcd_row(1, tmp);
        lcd_row(2, "Move stick fully!");
        lcd_row(3, "Start in 2s...");
        HAL_Delay(2000);
    }

    /* LEDs: PD12=green PD13=orange PD14=red PD15=blue */
    {
        GPIO_InitTypeDef g = {0};
        __HAL_RCC_GPIOD_CLK_ENABLE();
        HAL_GPIO_WritePin(GPIOD,
            GPIO_PIN_12|GPIO_PIN_13|GPIO_PIN_14|GPIO_PIN_15, GPIO_PIN_RESET);
        g.Pin   = GPIO_PIN_12|GPIO_PIN_13|GPIO_PIN_14|GPIO_PIN_15;
        g.Mode  = GPIO_MODE_OUTPUT_PP;
        g.Pull  = GPIO_NOPULL;
        g.Speed = GPIO_SPEED_FREQ_LOW;
        HAL_GPIO_Init(GPIOD, &g);
    }

    uint16_t peak       = 0;
    uint32_t peak_decay = 0;

    while (1)
    {
        /* ── Читаємо X і Y з DMA-буфера ── */
        uint16_t jx = (uint16_t)(joy_buf[0] & 0x0FFFu);
        uint16_t jy = (uint16_t)(joy_buf[1] & 0x0FFFu);

        /* ── Кнопка SW: PE10 LOW = натиснута ── */
        uint8_t jb = (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_10) == GPIO_PIN_RESET)
                     ? 1u : 0u;

        /* ── Клавіатура TTP229 ── */
        TTP229_Update();                        /* зчитуємо стан (~8 мс rate-limit всередині) */
        uint16_t kbd_raw = TTP229_GetRaw();     /* бітова маска: біт k-1 = клавіша k          */

        /* Знаходимо першу натиснуту клавішу (1..16), 0 = жодна */
        uint8_t kbd_key = 0;
        for (uint8_t k = 1; k <= 16; k++) {
            if (TTP229_IsPressed(k)) { kbd_key = k; break; }
        }

        /* ── Мікрофон ── */
        uint16_t amp = mic_amplitude();
        if (amp >= peak) {
            peak       = amp;
            peak_decay = HAL_GetTick();
        } else if (HAL_GetTick() - peak_decay > 1000u) {
            peak = (peak > 50u) ? (uint16_t)(peak - 50u) : 0u;
        }

        /* ── Визначення напрямку (фіксовані пороги) ── */
        const char *dir;
        uint8_t led_left = 0, led_right = 0, led_ud = 0;

        if      (jx < JOY_LO) { dir = "LEFT  "; led_left  = 1; }
        else if (jx > JOY_HI) { dir = "RIGHT "; led_right = 1; }
        else if (jy < JOY_LO) { dir = "UP    "; led_ud    = 1; }
        else if (jy > JOY_HI) { dir = "DOWN  "; led_ud    = 1; }
        else                   { dir = "CENTER"; }

        /* ── LCD рядок 0: сирі значення ── */
        char buf[21];
        snprintf(buf, sizeof(buf), "X:%-4u  Y:%-4u", jx, jy);
        lcd_row(0, buf);

        /* ── LCD рядок 1: напрямок + кнопка ── */
        snprintf(buf, sizeof(buf), "Dir:%-6s %s",
                 dir, jb ? "[BTN!]" : "      ");
        lcd_row(1, buf);

        /* ── LCD рядок 2: клавіатура TTP229 ──────────────────────── *
         * "KBD: xx  0x?????"                                         *
         *   xx   = номер натиснутої клавіші (1..16) або "--"         *
         *   0x?? = сирий bitmask (hex)                               */
        {
            if (kbd_key > 0)
                snprintf(buf, sizeof(buf), "KBD:%-2u   0x%04X", kbd_key, kbd_raw);
            else
                snprintf(buf, sizeof(buf), "KBD:--   0x%04X", kbd_raw);
            lcd_row(2, buf);
        }

        /* ── LCD рядок 3: мікрофон ── */
        {
            char bar[13];
            amp_bar(amp, bar);
            snprintf(buf, sizeof(buf), "M:%s%4u", bar, amp);
            lcd_row(3, buf);
        }

        /* ── LEDs ── */
        HAL_GPIO_WritePin(GPIOD,
            GPIO_PIN_12|GPIO_PIN_13|GPIO_PIN_14|GPIO_PIN_15, GPIO_PIN_RESET);
        if (jb)        HAL_GPIO_WritePin(GPIOD, GPIO_PIN_12, GPIO_PIN_SET);
        if (led_right) HAL_GPIO_WritePin(GPIOD, GPIO_PIN_13, GPIO_PIN_SET);
        if (led_left)  HAL_GPIO_WritePin(GPIOD, GPIO_PIN_14, GPIO_PIN_SET);
        if (led_ud)    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_15, GPIO_PIN_SET);
        if (amp > 600u)
            HAL_GPIO_WritePin(GPIOD,
                GPIO_PIN_12|GPIO_PIN_13|GPIO_PIN_14|GPIO_PIN_15, GPIO_PIN_SET);

        HAL_Delay(100);
    }
}

/* ============================================================
 * Peripheral Init
 * ============================================================ */

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef o = {0};
    RCC_ClkInitTypeDef c = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    o.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    o.HSEState       = RCC_HSE_ON;
    o.PLL.PLLState   = RCC_PLL_ON;
    o.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    o.PLL.PLLM       = 8;
    o.PLL.PLLN       = 336;
    o.PLL.PLLP       = RCC_PLLP_DIV2;
    o.PLL.PLLQ       = 7;
    HAL_RCC_OscConfig(&o);

    c.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                     | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    c.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    c.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    c.APB1CLKDivider = RCC_HCLK_DIV4;
    c.APB2CLKDivider = RCC_HCLK_DIV2;
    HAL_RCC_ClockConfig(&c, FLASH_LATENCY_5);
}

static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();

    /* PC1=VRx, PC2=VRy (ADC2), PC3=MIC (ADC1) — аналогові входи */
    g.Pin  = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3;
    g.Mode = GPIO_MODE_ANALOG;
    g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOC, &g);

    /* PE10 = SW кнопка джойстика */
    g.Pin  = GPIO_PIN_10;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOE, &g);

    /* PE8 = TTP229 SCL (вихід, push-pull) */
    g.Pin   = GPIO_PIN_8;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOE, &g);

    /* PE9 = TTP229 SDO (вхід з підтяжкою вгору) */
    g.Pin  = GPIO_PIN_9;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOE, &g);
}

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

/* DMA2 Stream2 Channel1 → ADC2 joystick (circular, без NVIC) */
static void MX_DMA_Init(void)
{
    __HAL_RCC_DMA2_CLK_ENABLE();

    hdma_adc2.Instance                 = DMA2_Stream2;
    hdma_adc2.Init.Channel             = DMA_CHANNEL_1;   /* ADC2 → CH1 */
    hdma_adc2.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_adc2.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_adc2.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_adc2.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
    hdma_adc2.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
    hdma_adc2.Init.Mode                = DMA_CIRCULAR;
    hdma_adc2.Init.Priority            = DMA_PRIORITY_LOW;
    hdma_adc2.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    HAL_DMA_Init(&hdma_adc2);

    /* Прив'язуємо DMA до hadc2 */
    __HAL_LINKDMA(&hadc2, DMA_Handle, hdma_adc2);

    /* NVIC для DMA2_Stream2 НЕ вмикаємо:
     * обробника немає у stm32f4xx_it.c, DMA працює тихо в circular mode */
}

/* ADC2: scan mode, 2 канали, continuous, DMA → joy_buf[2] */
static void MX_ADC2_Init(void)
{
    ADC_ChannelConfTypeDef cfg = {0};

    __HAL_RCC_ADC2_CLK_ENABLE();

    hadc2.Instance                   = ADC2;
    hadc2.Init.ClockPrescaler        = ADC_CLOCK_SYNC_PCLK_DIV4;
    hadc2.Init.Resolution            = ADC_RESOLUTION_12B;
    hadc2.Init.ScanConvMode          = ENABLE;          /* читає 2 канали  */
    hadc2.Init.ContinuousConvMode    = ENABLE;          /* безперервно     */
    hadc2.Init.DiscontinuousConvMode = DISABLE;
    hadc2.Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_NONE;
    hadc2.Init.ExternalTrigConv      = ADC_SOFTWARE_START;
    hadc2.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
    hadc2.Init.NbrOfConversion       = 2;
    hadc2.Init.DMAContinuousRequests = ENABLE;
    hadc2.Init.EOCSelection          = ADC_EOC_SEQ_CONV;
    HAL_ADC_Init(&hadc2);

    /* Rank 1 → IN11 = PC1 = VRx (вісь X) */
    cfg.Channel      = ADC_CHANNEL_11;
    cfg.Rank         = 1;
    cfg.SamplingTime = ADC_SAMPLETIME_56CYCLES;
    HAL_ADC_ConfigChannel(&hadc2, &cfg);

    /* Rank 2 → IN12 = PC2 = VRy (вісь Y) */
    cfg.Channel = ADC_CHANNEL_12;
    cfg.Rank    = 2;
    HAL_ADC_ConfigChannel(&hadc2, &cfg);
}

/* ADC1: single channel, software polling → мікрофон (IN13=PC3) */
static void MX_ADC1_Init(void)
{
    __HAL_RCC_ADC1_CLK_ENABLE();

    hadc1.Instance                   = ADC1;
    hadc1.Init.ClockPrescaler        = ADC_CLOCK_SYNC_PCLK_DIV4;
    hadc1.Init.Resolution            = ADC_RESOLUTION_12B;
    hadc1.Init.ScanConvMode          = DISABLE;
    hadc1.Init.ContinuousConvMode    = DISABLE;
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_NONE;
    hadc1.Init.ExternalTrigConv      = ADC_SOFTWARE_START;
    hadc1.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
    hadc1.Init.NbrOfConversion       = 1;
    hadc1.Init.DMAContinuousRequests = DISABLE;
    hadc1.Init.EOCSelection          = ADC_EOC_SINGLE_CONV;
    HAL_ADC_Init(&hadc1);
}

/* ── Error_Handler ─────────────────────────────────────────────── */
void Error_Handler(void)
{
    __disable_irq();
    while (1) {
        HAL_GPIO_TogglePin(GPIOD, GPIO_PIN_14);
        for (volatile int i = 0; i < 1000000; i++) {}
    }
}
