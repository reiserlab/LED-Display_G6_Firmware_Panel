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


uint8_t Pattern::stretch() {
    return stretch_;
}


void Pattern::set_stretch(uint8_t stretch) {
    stretch_ = stretch;
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


