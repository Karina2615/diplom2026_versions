#include "ttp229.h"

TTP229::TTP229(uint8_t scl_pin, uint8_t sdo_pin)
    : _scl(scl_pin), _sdo(sdo_pin), _cur(0), _prev(0), _last_ms(0) {}

void TTP229::begin() {
    pinMode(_scl, OUTPUT);
    pinMode(_sdo, INPUT);      // TTP229 drives SDO; no pull-up needed (chip has internal)
    digitalWrite(_scl, HIGH);  // idle state
    delay(15);                 // chip startup time
}

// TTP229-BSF 2-wire protocol:
// Host clocks SCL, chip shifts out key states on SDO.
// Bit = 0 means key is PRESSED (active-low).
// Recovery time between reads: ≥ 1.5 ms (we enforce 8 ms for full chip scan cycle).
uint16_t TTP229::readBits() {
    // Enforce minimum inter-read gap
    while ((millis() - _last_ms) < 8) { /* wait for chip scan refresh */ }

    uint16_t keys = 0;

    // Start: drive SCL low to signal data transfer
    digitalWrite(_scl, LOW);
    delayMicroseconds(100);   // allow SDO to be driven by chip

    for (int i = 0; i < 16; i++) {
        digitalWrite(_scl, HIGH);
        delayMicroseconds(4);
        if (!digitalRead(_sdo)) {   // active-low: 0 = pressed
            keys |= (uint16_t)(1 << i);
        }
        digitalWrite(_scl, LOW);
        delayMicroseconds(4);
    }

    // End: return SCL to idle high
    digitalWrite(_scl, HIGH);
    _last_ms = millis();
    return keys;
}

void TTP229::update() {
    _prev = _cur;
    _cur  = readBits();
}

bool TTP229::isPressed(uint8_t key) {
    if (key < 1 || key > 16) return false;
    return (_cur >> (key - 1)) & 1;
}

bool TTP229::justPressed(uint8_t key) {
    if (key < 1 || key > 16) return false;
    uint16_t m = 1u << (key - 1);
    return (_cur & m) && !(_prev & m);
}

bool TTP229::justReleased(uint8_t key) {
    if (key < 1 || key > 16) return false;
    uint16_t m = 1u << (key - 1);
    return !(_cur & m) && (_prev & m);
}
