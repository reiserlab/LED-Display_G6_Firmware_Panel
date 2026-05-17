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

        uint8_t stretch();
        void set_stretch(uint8_t stretch);

        DisplayMode mode();
        void set_mode(DisplayMode mode);

        PixelMatrix &matrix();

    protected:
        PixelMatrix matrix_ = PixelMatrix::Zero();
        GrayLevel gray_level_ = GrayLevel::Gray_2;
        uint8_t stretch_ = 0;
        DisplayMode mode_ = DisplayMode::Oneshot;  // V1 default; messenger overrides for V3 Persistent
};

#endif
