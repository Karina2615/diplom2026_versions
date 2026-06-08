#pragma once
#include <Arduino.h>

enum JoyDir : uint8_t { JOY_NONE=0, JOY_UP, JOY_DOWN, JOY_LEFT, JOY_RIGHT };

class Joystick {
public:
    Joystick(uint8_t x_pin, uint8_t y_pin, uint8_t btn_pin);
    void begin();
    void update();

    JoyDir  dir()           const { return _event_dir; }    // direction this frame
    bool    btnPressed()    const { return _btn_event; }     // true one frame on press
    bool    anyEvent()      const { return _event_dir != JOY_NONE || _btn_event; }

    // Raw analog values (0-4095)
    int rawX() const { return _rx; }
    int rawY() const { return _ry; }

private:
    uint8_t _xp, _yp, _bp;
    int _cx, _cy;                // calibrated center
    int _rx, _ry;

    JoyDir  _cur_dir, _prev_dir, _event_dir;
    bool    _btn_cur, _btn_prev, _btn_event;
    uint32_t _last_dir_ms;

    static const int DEADZONE   = 700;   // out of 2048
    static const int REPEAT_MS  = 250;   // auto-repeat rate when held
};
