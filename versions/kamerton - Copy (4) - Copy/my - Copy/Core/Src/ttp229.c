/* TTP229-BSF 16-key capacitive keyboard driver
 * 2-wire serial interface (bit-bang)
 *   PE8  = SCL  (output, push-pull, active-low clock)
 *   PE9  = SDO  (input,  pull-up,   active-low data)
 *
 * TP2 jumper on the module MUST be shorted to GND → 16-key mode
 * Minimum read interval: 8 ms (chip scan time ~7 ms)
 */

#include "ttp229.h"

/* ── GPIO aliases ───────────────────────────────────────────────────────── */
#define TTP_SCL_PIN   GPIO_PIN_8
#define TTP_SCL_PORT  GPIOE
#define TTP_SDO_PIN   GPIO_PIN_9
#define TTP_SDO_PORT  GPIOE

#define SCL_HIGH()  HAL_GPIO_WritePin(TTP_SCL_PORT, TTP_SCL_PIN, GPIO_PIN_SET)
#define SCL_LOW()   HAL_GPIO_WritePin(TTP_SCL_PORT, TTP_SCL_PIN, GPIO_PIN_RESET)
#define SDO_READ()  HAL_GPIO_ReadPin (TTP_SDO_PORT, TTP_SDO_PIN)

/* Bit-bang delay: ~2 µs at 168 MHz (one NOP loop iteration ≈ 6 ns → 330 NOPs) */
static inline void ttp_delay_us(uint32_t us)
{
    /* Simple busy-wait using DWT if available, else NOP loop */
    uint32_t start = DWT->CYCCNT;
    uint32_t ticks = us * (SystemCoreClock / 1000000UL);
    while ((DWT->CYCCNT - start) < ticks) { __NOP(); }
}

/* ── State ──────────────────────────────────────────────────────────────── */
static uint16_t s_raw_curr  = 0;   /* current  key bitmask (bit k-1 = key k) */
static uint16_t s_raw_prev  = 0;   /* previous key bitmask                   */
static uint32_t s_last_tick = 0;   /* HAL tick of last successful read        */

/* ── Internal: read 16-bit key state from TTP229 ────────────────────────── */
static uint16_t ttp_read_raw(void)
{
    uint16_t data = 0;

    /* Start condition: SCL goes low */
    SCL_LOW();
    ttp_delay_us(2);

    for (int i = 0; i < 16; i++)
    {
        /* Clock pulse low→high→low, sample on rising edge                   *
         * TTP229 shifts out LSB first; active-LOW means 0 = pressed         */
        SCL_LOW();
        ttp_delay_us(2);

        uint8_t bit = (SDO_READ() == GPIO_PIN_RESET) ? 1u : 0u; /* invert */
        data |= (uint16_t)(bit << i);

        SCL_HIGH();
        ttp_delay_us(2);
    }

    /* End: leave SCL high (idle state) */
    SCL_HIGH();
    ttp_delay_us(2);

    return data;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void TTP229_Init(void)
{
    /* Enable DWT cycle counter for µs delays */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    /* GPIO must already be configured by CubeMX:
     *   PE8  → GPIO_Output, Push-Pull, No pull,  High speed
     *   PE9  → GPIO_Input,  Pull-Up
     * Just ensure SCL starts high (idle). */
    SCL_HIGH();

    s_raw_curr = s_raw_prev = 0;
    s_last_tick = 0;
}

void TTP229_Update(void)
{
    /* Rate-limit to ~8 ms to let TTP229 finish its scan cycle */
    uint32_t now = HAL_GetTick();
    if ((now - s_last_tick) < 8) return;
    s_last_tick = now;

    s_raw_prev = s_raw_curr;
    s_raw_curr = ttp_read_raw();
}

uint8_t TTP229_IsPressed(uint8_t k)
{
    if (k < 1 || k > 16) return 0;
    return (s_raw_curr >> (k - 1)) & 1u;
}

uint8_t TTP229_JustPressed(uint8_t k)
{
    if (k < 1 || k > 16) return 0;
    uint16_t mask = (uint16_t)(1u << (k - 1));
    return ((s_raw_curr & mask) && !(s_raw_prev & mask)) ? 1u : 0u;
}

uint8_t TTP229_JustReleased(uint8_t k)
{
    if (k < 1 || k > 16) return 0;
    uint16_t mask = (uint16_t)(1u << (k - 1));
    return (!(s_raw_curr & mask) && (s_raw_prev & mask)) ? 1u : 0u;
}

uint16_t TTP229_GetRaw(void)
{
    return s_raw_curr;
}
