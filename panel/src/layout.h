#ifndef LAYOUT_H
#define LAYOUT_H
// Stage 2 (S2.1) — schematic ↔ layout index maps.
//
// Ported verbatim from G6_Panels_Test_Firmware/test_firmware/single_led/src/utilities.cpp
// (lines 5-41). The convert_schematic_to_layout() body is identical to the
// original Will-authored coordinate logic preserved in iorodeo's
// Display::sch_to_pos_index() (now deleted from display.cpp).
//
// Usage: bcm.cpp's precompute walks layout pixels (lr, lc), reads pat.matrix()(lr, lc),
// then looks up the SCHEMATIC (sch_row, sch_col) via layout_to_sch_row[lr][lc]
// and layout_to_sch_col[lr][lc]. The schematic row drives row_on_mask[sch_row]
// (which row pin to pull LOW) and the schematic col indexes into the PIO
// column word.

#include <Arduino.h>
#include "constants.h"

// Layout → schematic lookup tables. Populated by init_index_maps() at boot.
// NOT std::map or Eigen — plain C arrays so static init order doesn't matter
// and they cost only 2 * 20 * 20 = 800 bytes of BSS.
extern uint8_t layout_to_sch_row[PANEL_SIZE][PANEL_SIZE];
extern uint8_t layout_to_sch_col[PANEL_SIZE][PANEL_SIZE];

// Populate the lookup tables. Must be called once from setup1() before any
// BCM precompute happens.
void init_index_maps();

#endif
