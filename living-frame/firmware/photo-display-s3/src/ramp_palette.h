// ---------------------------------------------------------------------------
//  ramp_palette.h -- the ramp phase's 16 palette entries (rung 3b / task 0180).
//
//  Shared by the firmware (src/main.cpp, which folds it into the ISR's expansion
//  table) and by tools/render16.cpp (which renders the preview Stephen will
//  photograph), so the preview cannot drift from what the board sends.
//
//  Black appears three times (indices 0 / 4 / 8) so each channel's four patches
//  sit at consecutive indices and "level 0" is black in every row without a
//  branch -- the same trick the 64-colour packing used for R/G/B order.
//
//  Index  Colour          DAC byte (B<<4)|(G<<2)|R
//     0   black           0x00
//     1   R LSB           0x01
//     2   R MSB           0x02
//     3   R both          0x03
//     4   black           0x00
//     5   G LSB           0x04
//     6   G MSB           0x08
//     7   G both          0x0c
//     8   black           0x00
//     9   B LSB           0x10
//    10   B MSB           0x20
//    11   B both          0x30
//    12   grey LSB        0x15
//    13   grey MSB        0x2a
//    14   grey both       0x3f  (= white: all three channels at full)
//    15   unused black    0x00
// ---------------------------------------------------------------------------
#pragma once

#include <stdint.h>

static const uint8_t RAMP_PAL[16] = {
    0x00, 0x01, 0x02, 0x03,
    0x00, 0x04, 0x08, 0x0c,
    0x00, 0x10, 0x20, 0x30,
    0x15, 0x2a, 0x3f, 0x00,
};

#define RAMP_PAL_N 16
