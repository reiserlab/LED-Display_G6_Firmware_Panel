#ifndef PATTERN_H
#define PATTERN_H
#include <ArduinoEigen.h>
//#include "constants.h"
#include "protocol.h"

using PixelMatrix = Eigen::Matrix<uint8_t, PANEL_SIZE, PANEL_SIZE>; 

class Pattern {

    public:
        Pattern();

        uint8_t &at(size_t i, size_t j);

        GrayLevel gray_level();
        void set_gray_level(GrayLevel gray_level);

        // Brightness modulation: 0..255 = duty cycle scale (0 = strict off,
        // 255 = full brightness). With the fixed-period scan in Display::show()
        // this is literally the per-LED duty cycle factor applied on top of
        // the bit-plane weights. Renamed from "stretch" (issue #52) — see
        // g6_01-panel-protocol.md § Duty Cycle Value.
        uint8_t duty_cycle();
        void set_duty_cycle(uint8_t duty_cycle);

        DisplayMode mode();
        void set_mode(DisplayMode mode);

        PixelMatrix &matrix();

    protected:
        PixelMatrix matrix_ = PixelMatrix::Zero();
        GrayLevel gray_level_ = GrayLevel::Gray_2;
        uint8_t duty_cycle_ = 0;
        DisplayMode mode_ = DisplayMode::Oneshot;  // default; messenger overrides for Persistent (cmd 0x11 / 0x31)
};

#endif
