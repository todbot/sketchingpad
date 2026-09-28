// SPDX-FileCopyrightText: 2026 Tod Kurt
// SPDX-License-Identifier: GPL-3.0-or-later
//
// touch_map_test.cpp -- touch_map.h, the per-board pad tables
//
// The bug this suite exists for has bitten twice on real hardware: a
// proto1 table carried forward into three boards, and ble_broadcast_anim_ts20
// putting its colour swatches on the wrong four pads.  Both are the same
// shape -- a table that is no longer a permutation of the pads it claims
// to cover -- and both are catchable with no board attached.

#include "touch_map.h"
#include "test_util.h"

static void checkPermutation(const char *name, const uint8_t *table,
                             uint8_t nbits, uint32_t unused_mask) {
  int seen[32] = {0};
  bool in_range = true;
  bool unused_clear = true;

  for (int n = 0; n < TOUCH_NUM_LEDS; n++) {
    uint8_t bit = table[n];
    if (bit >= nbits) {
      in_range = false;
      continue;
    }
    if ((unused_mask >> bit) & 1) { unused_clear = false; }
    seen[bit]++;
  }

  bool once = true;
  for (int b = 0; b < 32; b++) {
    if (seen[b] > 1) { once = false; }
  }

  printf(" %s\n", name);
  CHECK(in_range, "every sensor bit is within the sensor's range");
  CHECK(once, "no sensor bit is mapped to two LEDs");
  CHECK(unused_clear, "no LED is mapped to an unusable sensor bit");
}

// Round-tripping every single-bit word is what actually proves the table
// and the mapper agree: a mapper that read the table backwards still
// produces a permutation, so the structural checks above would pass.
static void checkRoundTrip(const char *name, const uint8_t *table,
                           uint32_t (*map)(uint32_t)) {
  bool ok = true;
  for (int n = 0; n < TOUCH_NUM_LEDS; n++) {
    if (map(1UL << table[n]) != (1UL << n)) { ok = false; }
  }
  printf(" %s\n", name);
  CHECK(ok, "each sensor bit alone lights exactly its own LED");
}

int main() {
  printf("touch_map_test\n");

  checkPermutation("TS20, 20 channels", touch_led_to_ts20, 20, 0);
  checkPermutation("2x MPR121, 24 electrodes", touch_led_to_mpr, 24,
                   TOUCH_MPR_UNUSED_MASK);

  checkRoundTrip("TS20", touch_led_to_ts20, touchMapTS20);
  checkRoundTrip("2x MPR121", touch_led_to_mpr, touchMapMPR);

  printf(" both tables cover all 20 LEDs\n");
  {
    // A permutation of 20 distinct in-range bits onto 20 LEDs can still
    // leave an LED dark only if the mapper drops it, so assert the whole
    // word rather than the table alone.
    uint32_t all = (1UL << TOUCH_NUM_LEDS) - 1;
    CHECK(touchMapTS20(0xfffffUL) == all, "all 20 TS20 channels -> all 20 LEDs");
    CHECK(touchMapMPR(0xffffffUL) == all,
          "all 24 MPR121 electrodes -> all 20 LEDs");
  }

  printf(" the J2 four reach no LED\n");
  {
    // U4's ELE4-7 have no pad and cannot be disabled in the ECR, so if
    // they ever chatter they must still paint nothing.  The table is what
    // guarantees that -- it maps no LED into those four bits -- so this
    // fails the moment one does.
    CHECK(touchMapMPR(TOUCH_MPR_UNUSED_MASK) == 0,
          "U4 ELE4-7 alone light nothing");
  }

  printf(" the two tables are genuinely different\n");
  {
    // Same 20 pads in the same physical places, different sensor: sharing
    // a table between the revs is the regression that has happened twice.
    bool differs = false;
    for (int n = 0; n < TOUCH_NUM_LEDS; n++) {
      if (touch_led_to_ts20[n] != touch_led_to_mpr[n]) { differs = true; }
    }
    CHECK(differs, "the MPR121 table is not the TS20 table");
  }

  printf(" the top row is not the top of either range\n");
  {
    // TP17-20 are LEDs 16-19 and sit in the *middle* of the TS20 channel
    // range.  Assuming otherwise is what misplaced ble_broadcast_anim_ts20's
    // colour swatches.
    CHECK(touch_led_to_ts20[16] == 4 && touch_led_to_ts20[19] == 7,
          "TS20: TP17-20 are channels 4-7");
    CHECK(touch_led_to_mpr[16] == 10 && touch_led_to_mpr[19] == 1,
          "MPR121: TP17-20 are combined bits 10, 11, 0, 1");
  }

  printf(" each group of four pads is a contiguous run of sensor bits\n");
  {
    // The structural check that catches a wrong table rather than a
    // malformed one.  Both boards route the pads in blocks of four -- the
    // TP1-4, TP5-8, TP9-12, TP13-16 and TP17-20 groups of the layout
    // diagrams in arduino/README.md -- and within a block the sensor bits
    // ascend by one.  A permutation with two entries transposed is still a
    // permutation, so nothing above notices it; this does.
    bool ts20_runs = true, mpr_runs = true;
    for (int n = 0; n < TOUCH_NUM_LEDS - 1; n++) {
      if (n % 4 == 3) { continue; } // group boundary: no relation
      if (touch_led_to_ts20[n + 1] != touch_led_to_ts20[n] + 1) {
        ts20_runs = false;
      }
      // TP17-20 is the one block split across U3's electrode numbering,
      // running ELE10, ELE11, ELE0, ELE1.
      if (n == 17) { continue; }
      if (touch_led_to_mpr[n + 1] != touch_led_to_mpr[n] + 1) {
        mpr_runs = false;
      }
    }
    CHECK(ts20_runs, "TS20: bits ascend within every group of four");
    CHECK(mpr_runs, "MPR121: bits ascend within every group of four");
    CHECK(touch_led_to_mpr[18] == 0 && touch_led_to_mpr[17] == 11,
          "MPR121: TP17-20 wraps U3 ELE11 -> ELE0, and only there");
  }

  return test_report("touch_map_test");
}
