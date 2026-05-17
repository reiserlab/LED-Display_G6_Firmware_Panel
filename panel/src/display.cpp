#include <ArduinoEigen.h>
#include "constants.h"
#include "display.h"

typedef struct {
    uint8_t i;
    uint8_t j;
} index_t;

index_t sch_to_pos_index(index_t sch_index);


Display::Display(queue_t &display_queue) : display_queue_(display_queue) {}

void Display::initialize() {
    col_pin_mask_ = 0;
    for (size_t i=0; i<PANEL_SIZE; i++) {
        gpio_init(COL_PIN[i]);
        col_pin_mask_ |= (uint64_t(1) << COL_PIN[i]);
    }
    row_pin_mask_ = 0;
    for (size_t i=0; i<PANEL_SIZE; i++) {
        gpio_init(ROW_PIN[i]);
        row_pin_mask_ |= (uint64_t(1) << ROW_PIN[i]);
    }

    gpio_set_dir_out_masked64(col_pin_mask_);
    gpio_clr_mask64(col_pin_mask_);

    gpio_set_dir_out_masked64(row_pin_mask_);
    gpio_set_mask64(row_pin_mask_);
}

void Display::update() {
    // Swap in a new pattern if available (non-blocking). New patterns always
    // override the current display state; for Oneshot they cause a single
    // scan and then idle; for Persistent they cause continuous refresh until
    // the next message replaces them.
    if (!queue_is_empty(&display_queue_)) {
        queue_try_remove(&display_queue_, &pat_);
        have_pattern_ = true;
        oneshot_pending_ = (pat_.mode() == DisplayMode::Oneshot);
    }

    if (!have_pattern_) {
        return;  // no pattern yet (boot state) — rows OFF, cols OFF
    }

    if (pat_.mode() == DisplayMode::Persistent) {
        // V3 Persistent: scan one frame; loop1() re-enters update() and we
        // scan again. Pattern stays loaded until next message.
        show();
    } else {
        // V1 Oneshot: scan exactly one full frame after the message arrives,
        // then go idle (display dark) until the next message.
        if (oneshot_pending_) {
            show();
            oneshot_pending_ = false;
        }
    }
}

void Display::show() {

    Eigen::Vector<uint8_t, PANEL_SIZE> pixel_value;
    Eigen::Vector<uint64_t, MAX_GRAY_LEVEL> col_clr_mask;
    uint8_t num_gray_level = GRAY_LEVEL_UMAP.at(pat_.gray_level());

    // Kludgey gray level delay (Stage 1 interim — Stage 2 replaces with BCM/PIO)
    size_t gray_level_delay = 0;
    switch (pat_.gray_level()) {
        case GrayLevel::Gray_2:
            gray_level_delay = 500;
            break;
        case GrayLevel::Gray_16:
            gray_level_delay = 100;
            break;
    }

    // V1 spec stretch (interim): scale ON time by stretch/255. stretch=0 -> strict off
    // (the row-on / col-set / col-clr sequence still runs, but with zero dwell the LEDs
    // never produce visible brightness). Stage 2 replaces this with BCM bit-plane
    // modulation per the spec's `base_T * stretch / 255` rule.
    uint8_t stretch = pat_.stretch();
    if (stretch == 0) {
        // Strict-off: don't even touch the matrix. Leave rows OFF (HIGH) and cols OFF (LOW).
        return;
    }
    size_t scaled_delay = (gray_level_delay * stretch) / 255;

    gpio_set_mask64(row_pin_mask_);
    for (size_t i=0; i<PANEL_SIZE; i++) {
        uint64_t col_set_mask = 0;
        col_clr_mask.setZero();
        for (size_t j=0; j<PANEL_SIZE; j++) {
            index_t sch_index {uint8_t(i), uint8_t(j)};
            index_t pos_index = sch_to_pos_index(sch_index);
            pixel_value(j) = pat_.matrix()(pos_index.i, pos_index.j);
            if (pixel_value(j) > 0) {
                digitalWrite(COL_PIN[j], HIGH);
                col_set_mask |= uint64_t(1) << COL_PIN[j];
                uint32_t mask_ind = min(pixel_value(j), num_gray_level-1);
                col_clr_mask(mask_ind) |= uint64_t(1) << COL_PIN[j];
            }
        }
        gpio_put(ROW_PIN[i],0);
        gpio_set_mask64(col_set_mask);
        volatile uint8_t tmp = 0;
        for (size_t k=0; k<num_gray_level; k++) {
            // Delay loop. Super kludgey. Need to do better timing.
            // Stage 1: scaled by pat_.stretch()/255 for V1 stretch semantics (interim;
            // Stage 2 replaces this engine with BCM-via-PIO and the proper spec stretch).
            for (size_t kk=0; kk<scaled_delay; kk++) {
                tmp++;
            }
            gpio_clr_mask64(col_clr_mask(k));
        }
        gpio_clr_mask64(col_set_mask);
        gpio_put(ROW_PIN[i], 1);
    }
}


index_t sch_to_pos_index(index_t sch_index) {
    index_t pos_index;
    if (sch_index.j % NUM_COLOR < NUM_COLOR/2) {
        if (sch_index.i < PANEL_SIZE/2) {
            pos_index.i = 2*sch_index.i;
            pos_index.j = sch_index.j;
        }
        else {
            pos_index.i = 2*(PANEL_SIZE - (sch_index.i+1));
            pos_index.j = NUM_COLOR/2 + sch_index.j;
        }
    }
    else {
        if (sch_index.i < PANEL_SIZE/2) {
            pos_index.i = 2*sch_index.i + 1;
            pos_index.j = sch_index.j - NUM_COLOR/2;
        }
        else {
            pos_index.i = 2*(PANEL_SIZE - (sch_index.i+1)) + 1;
            pos_index.j = sch_index.j;
        }
    }
    return pos_index;
}
