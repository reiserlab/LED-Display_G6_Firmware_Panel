#include "pattern.h"

Pattern::Pattern() {}

GrayLevel Pattern::gray_level() {
    return gray_level_;
}

uint8_t &Pattern::at(size_t i, size_t j) {
    return matrix_(i,j);
}

void Pattern::set_gray_level(GrayLevel gray_level) {
    gray_level_ = gray_level;
}


uint8_t Pattern::duty_cycle() {
    return duty_cycle_;
}


void Pattern::set_duty_cycle(uint8_t duty_cycle) {
    duty_cycle_ = duty_cycle;
}


DisplayMode Pattern::mode() {
    return mode_;
}


void Pattern::set_mode(DisplayMode mode) {
    mode_ = mode;
}


PixelMatrix &Pattern::matrix() {
    return matrix_;
}


