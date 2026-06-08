#include "joystick.h"

Joystick::Joystick(uint8_t x_pin, uint8_t y_pin, uint8_t btn_pin)
    : _xp(x_pin), _yp(y_pin), _bp(btn_pin),
      _cx(2048), _cy(2048), _rx(2048), _ry(2048),
      _cur_dir(JOY_NONE), _prev_dir(JOY_NONE), _event_dir(JOY_NONE),
      _btn_cur(false), _btn_prev(false), _btn_event(false),
      _last_dir_ms(0) {}

void Joystick::begin() {
    pinMode(_bp, INPUT_PULLUP);
    analogSetAttenuation(ADC_11db);   // 0-3.6 V range

    // Calibrate center with 32-sample average
    long sx = 0, sy = 0;
    for (int i = 0; i < 32; i++) {
        sx += analogRead(_xp);
        sy += analogRead(_yp);
        delay(1);
    }
    _cx = (int)(sx / 32);
    _cy = (int)(sy / 32);
}

void Joystick::update() {
    _rx = analogRead(_xp);
    _ry = analogRead(_yp);

    int dx = _rx - _cx;
    int dy = _ry - _cy;

    // Button (active-low, debounced via prev state)
    _btn_prev  = _btn_cur;
    _btn_cur   = !digitalRead(_bp);
    _btn_event = (_btn_cur && !_btn_prev);

    // Direction detection with deadzone
    JoyDir new_dir = JOY_NONE;
    if      (dy < -DEADZONE) new_dir = JOY_UP;
    else if (dy >  DEADZONE) new_dir = JOY_DOWN;
    else if (dx < -DEADZONE) new_dir = JOY_LEFT;
    else if (dx >  DEADZONE) new_dir = JOY_RIGHT;

    _event_dir = JOY_NONE;
    uint32_t now = millis();

    if (new_dir != JOY_NONE) {
        bool direction_changed = (new_dir != _prev_dir);
        bool repeat_elapsed    = (now - _last_dir_ms) >= (uint32_t)REPEAT_MS;

        if (direction_changed || repeat_elapsed) {
            _event_dir     = new_dir;
            _last_dir_ms   = now;
        }
    }

    _prev_dir = new_dir;
    _cur_dir  = new_dir;
}
