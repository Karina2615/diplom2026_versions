/* TTP229-BSF 16-key capacitive keyboard driver
 * PE8 = SCL  (output, push-pull)
 * PE9 = SDO  (input,  pull-up, active-LOW)
 *
 * TP2 jumper on module → GND for 16-key mode.
 *
 * Reading strategy:
 *   One raw read per TTP229_Update call (every 10 ms).
 *   The TTP229 latches its scan result (~7 ms cycle) and holds it stable
 *   on the serial bus, so one read per poll is perfectly reliable.
 *   Ghost/noise filtering is done in Task_Keypad via a 4-scan confirmation
 *   window (4 × 10 ms = 40 ms) — each scan comes from a different TTP229
 *   internal cycle, giving truly independent observations.
 */

#include "ttp229.h"

#define TTP_SCL_PIN   GPIO_PIN_8
#define TTP_SCL_PORT  GPIOE
#define TTP_SDO_PIN   GPIO_PIN_9
#define TTP_SDO_PORT  GPIOE

#define SCL_HI()  HAL_GPIO_WritePin(TTP_SCL_PORT, TTP_SCL_PIN, GPIO_PIN_SET)
#define SCL_LO()  HAL_GPIO_WritePin(TTP_SCL_PORT, TTP_SCL_PIN, GPIO_PIN_RESET)
#define SDO()     HAL_GPIO_ReadPin (TTP_SDO_PORT, TTP_SDO_PIN)

static inline void dly(uint32_t us)
{
    /* NOP-loop delay — independent of DWT/debug subsystem.
     * At 96 MHz one loop iteration ≈ 4 cycles (~42 ns).
     * us × 35 gives ≥ 1.4 µs per unit — safely ≥ 1 µs.             */
    volatile uint32_t n = us * 35u;
    while (n--) { __NOP(); }
}

static uint16_t s_raw_curr   = 0;
static uint16_t s_raw_prev   = 0;
static uint32_t s_last_tick  = 0;
static uint32_t s_scan_count = 0;

/* One 16-bit read from TTP229 (3 µs pulse width — ample margin) */
static uint16_t raw_read(void)
{
    uint16_t data = 0;
    SCL_LO(); dly(3);                      /* start: first falling edge     */
    for (int i = 0; i < 16; i++) {
        SCL_LO(); dly(3);                  /* i=0: no-op; i>0: falling edge */
        if (SDO() == GPIO_PIN_RESET)       /* active-LOW → invert           */
            data |= (uint16_t)(1u << i);
        SCL_HI(); dly(3);
    }
    SCL_HI(); dly(3);
    return data;
}

void TTP229_Init(void)
{
    SCL_HI();
    s_raw_curr = s_raw_prev = 0;
    s_last_tick = s_scan_count = 0;
}

void TTP229_Update(void)
{
    uint32_t now = HAL_GetTick();
    if ((now - s_last_tick) < 10u) return;
    s_last_tick = now;

    s_raw_prev = s_raw_curr;
    s_raw_curr = raw_read();
    s_scan_count++;
}

uint8_t  TTP229_IsPressed(uint8_t k)   { return k>=1&&k<=16 ? (s_raw_curr>>(k-1))&1u : 0u; }
uint8_t  TTP229_JustPressed(uint8_t k) {
    if (k<1||k>16) return 0u;
    uint16_t m=(uint16_t)(1u<<(k-1));
    return ((s_raw_curr&m)&&!(s_raw_prev&m))?1u:0u;
}
uint8_t  TTP229_JustReleased(uint8_t k){
    if (k<1||k>16) return 0u;
    uint16_t m=(uint16_t)(1u<<(k-1));
    return (!(s_raw_curr&m)&&(s_raw_prev&m))?1u:0u;
}
uint16_t TTP229_GetRaw(void)       { return s_raw_curr; }
uint32_t TTP229_GetScanCount(void) { return s_scan_count; }
