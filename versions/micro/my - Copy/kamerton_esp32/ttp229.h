#pragma once
#include <Arduino.h>

// Driver for TTP229-BSF 16-key capacitive keyboard
// 2-wire serial mode (SCL driven by MCU, SDO output from TTP229)
// Hardware config: TP2 (OPT2) must be pulled LOW for 16-key mode
class TTP229 {
public:
    TTP229(uint8_t scl_pin, uint8_t sdo_pin);
    void begin();

    // Call once per main loop iteration
    void update();

    bool isPressed(uint8_t key);     // key 1-16
    bool justPressed(uint8_t key);   // true for exactly one update() call
    bool justReleased(uint8_t key);

    // Returns bitmask: bit N-1 set = key N pressed
    uint16_t getRaw() const { return _cur; }

private:
    uint8_t  _scl, _sdo;
    uint16_t _cur, _prev;
    uint32_t _last_ms;

    uint16_t readBits();
};
