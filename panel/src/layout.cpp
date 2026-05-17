// Stage 2 (S2.1) — schematic ↔ layout maps implementation.
//
// Ported verbatim from G6_Panels_Test_Firmware/test_firmware/single_led/src/utilities.cpp
// (lines 5-41). NUM_COLOR comes from constants.h (= 4 for G6 panels).

#include "layout.h"
#include "constants.h"

uint8_t layout_to_sch_row[PANEL_SIZE][PANEL_SIZE];
uint8_t layout_to_sch_col[PANEL_SIZE][PANEL_SIZE];

// Will's original coordinate conversion logic — schematic (si, sj) → layout (li, lj).
// Identical to the iorodeo Display::sch_to_pos_index() that this replaces.
static void convert_schematic_to_layout(uint8_t si, uint8_t sj,
                                        uint8_t &li, uint8_t &lj) {
    if (sj % NUM_COLOR < NUM_COLOR / 2) {
        if (si < PANEL_SIZE / 2) {
            li = 2 * si;
            lj = sj;
        } else {
            li = 2 * (PANEL_SIZE - (si + 1));
            lj = NUM_COLOR / 2 + sj;
        }
    } else {
        if (si < PANEL_SIZE / 2) {
            li = 2 * si + 1;
            lj = sj - NUM_COLOR / 2;
        } else {
            li = 2 * (PANEL_SIZE - (si + 1)) + 1;
            lj = sj;
        }
    }
}

void init_index_maps() {
    // For every schematic (row, col) compute the layout (row, col), then
    // store the REVERSE mapping: layout → schematic.
    for (uint8_t si = 0; si < PANEL_SIZE; si++) {
        for (uint8_t sj = 0; sj < PANEL_SIZE; sj++) {
            uint8_t li, lj;
            convert_schematic_to_layout(si, sj, li, lj);
            layout_to_sch_row[li][lj] = si;
            layout_to_sch_col[li][lj] = sj;
        }
    }
}
