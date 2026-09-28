// SPDX-FileCopyrightText: 2026 Tod Kurt
// SPDX-License-Identifier: GPL-3.0-or-later
//
// touch_map.h -- the sketchingpad pad tables, normalised to LED index
//
// Dependency-free on purpose -- <stdint.h> and nothing else -- so
// arduino/tests can cover it with no I2C to stub out.  The driver that
// uses it is touch.h.
//
// The public index is the LED number: bit n of what these functions
// return is LED n, which is TP(n+1) on the silkscreen, on both boards.
// Sketches index by LED and never see a channel or an electrode.
//
// Each table is board-rev *and* sensor specific.  A new rev needs a new
// table derived from its own netlist; the three steps and the proto1
// validation table are in arduino/README.md.
#pragma once

#include <stdint.h>

#define TOUCH_NUM_LEDS 20

// U4's ELE4-7 are TOUCH21-24, brought out to the J2 header rather than to
// pads.  ECR's ELE_EN enables ELE0..ELE(n-1) contiguously, so they cannot
// be switched off without also losing ELE8-11, which are real pads.
#define TOUCH_MPR_UNUSED_MASK (0x0f0000UL)

// sketchingpad_proto2 and sketchingpad1, one TS20.  Value is the TS20
// channel, where channel index = m - 1 for pinfunction CSm.
//
//            TP17 TP18 TP19 TP20              4  5  6  7
//   TP1  TP2  TP3  TP4   TP5  TP6  TP7  TP8   0  1  2  3   8  9 10 11
//   TP9 TP10 TP11 TP12  TP13 TP14 TP15 TP16  16 17 18 19  12 13 14 15
static const uint8_t touch_led_to_ts20[TOUCH_NUM_LEDS] = {
    0, 1, 2, 3, 8, 9, 10, 11, 16, 17,
    18, 19, 12, 13, 14, 15, 4, 5, 6, 7,
};

// sketchingpad1_mpr, two MPR121s.  Value is the bit in the combined word,
// where bit = chip * 12 + ELEn: U3 (0x5A) is bits 0-11, U4 (0x5B) 12-23.
//
//            TP17 TP18 TP19 TP20             10 11  0  1
//   TP1  TP2  TP3  TP4   TP5  TP6  TP7  TP8   6  7  8  9   2  3  4  5
//   TP9 TP10 TP11 TP12  TP13 TP14 TP15 TP16  20 21 22 23  12 13 14 15
static const uint8_t touch_led_to_mpr[TOUCH_NUM_LEDS] = {
    6, 7, 8, 9, 2, 3, 4, 5, 20, 21,
    22, 23, 12, 13, 14, 15, 10, 11, 0, 1,
};

static inline uint32_t touchMapBits(const uint8_t *led_to_sensor,
                                    uint32_t sensor_bits) {
  uint32_t leds = 0;
  for (uint8_t n = 0; n < TOUCH_NUM_LEDS; n++) {
    if ((sensor_bits >> led_to_sensor[n]) & 1) { leds |= 1UL << n; }
  }
  return leds;
}

/// 20 TS20 channel bits -> 20 LED bits.
static inline uint32_t touchMapTS20(uint32_t channels) {
  return touchMapBits(touch_led_to_ts20, channels);
}

/// 24 combined MPR121 electrode bits -> 20 LED bits.  The J2 four are
/// dropped by the table, which reaches no bit of TOUCH_MPR_UNUSED_MASK,
/// so there is nothing to mask off here.
static inline uint32_t touchMapMPR(uint32_t electrodes) {
  return touchMapBits(touch_led_to_mpr, electrodes);
}
