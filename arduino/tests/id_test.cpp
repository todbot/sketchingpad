// SPDX-FileCopyrightText: 2026 Tod Kurt
// SPDX-License-Identifier: GPL-3.0-or-later
//
// id_test.cpp -- device_id_code.h, the board's R/Y/G/B/M identity code
//
// The bug this suite exists for: the code is five base-5 digits, because
// there are five colors.  It began as five 2-bit slices back when there
// were four.  If anyone ever "simplifies" the modulus back to bit masking,
// M becomes unreachable and nothing else breaks -- the codes still look
// perfectly valid, and you would never catch it by glancing at one board.

#include "Arduino.h"

FakeFicr fake_ficr; // the stub declares it; one definition lives here

#include "device_id_code.h"
#include "test_util.h"

static const char *LETTERS = "RYGBM";
static const int NLETTERS = 5;

static void setId(uint32_t lo, uint32_t hi) {
  fake_ficr.DEVICEID[0] = lo;
  fake_ficr.DEVICEID[1] = hi;
}

// A spread of plausible device IDs.  Real FICR IDs are random, so anything
// well-mixed stands in fine.
static void nthId(uint32_t n) {
  setId(n * 2654435761u + 0x9e3779b9u, (n ^ 0x5bf03635u) * 2246822519u);
}

int main() {
  printf("id_test\n");

  const int SWEEP = 20000;

  // --- every letter must appear at every position.  A bitmask regression
  // leaves M at zero everywhere; a botched digit loop leaves one position
  // stuck.  Both show up here and nowhere else.
  {
    int seen[DEVICE_ID_CODE_LEN][NLETTERS] = {{0}};
    bool all_known = true;
    for (int i = 0; i < SWEEP; i++) {
      nthId((uint32_t)i);
      char code[DEVICE_ID_CODE_LEN + 1];
      deviceIdCode(code);
      if (code[DEVICE_ID_CODE_LEN] != '\0') { all_known = false; }
      for (int p = 0; p < DEVICE_ID_CODE_LEN; p++) {
        const char *hit = strchr(LETTERS, code[p]);
        if (hit == NULL) { all_known = false; continue; }
        seen[p][hit - LETTERS]++;
      }
    }
    CHECK(all_known, "every character is one of R/Y/G/B/M, and the code is terminated");

    bool every = true;
    for (int p = 0; p < DEVICE_ID_CODE_LEN; p++) {
      for (int l = 0; l < NLETTERS; l++) {
        if (seen[p][l] == 0) {
          printf("       position %d never produced %c\n", p, LETTERS[l]);
          every = false;
        }
      }
    }
    CHECK(every, "all 5 letters occur at all 5 positions");

    // Not a claim about hash quality, just that no letter is nearly
    // unreachable -- 10% of an even 20% share is a very loose floor.
    bool balanced = true;
    for (int p = 0; p < DEVICE_ID_CODE_LEN; p++) {
      for (int l = 0; l < NLETTERS; l++) {
        if (seen[p][l] < SWEEP / 50) { balanced = false; }
      }
    }
    CHECK(balanced, "no letter is starved at any position");
    printf("       position 0 spread:");
    for (int l = 0; l < NLETTERS; l++) {
      printf("  %c=%d", LETTERS[l], seen[0][l]);
    }
    printf("\n");
  }

  // --- the colors must agree with the letters, position by position.
  // The two functions share deviceIdDigits(); this pins them together.
  {
    bool agree = true;
    for (int i = 0; i < 500; i++) {
      nthId((uint32_t)i);
      char code[DEVICE_ID_CODE_LEN + 1];
      uint32_t colors[DEVICE_ID_CODE_LEN];
      deviceIdCode(code);
      deviceIdColors(colors);
      for (int p = 0; p < DEVICE_ID_CODE_LEN; p++) {
        const char *hit = strchr(LETTERS, code[p]);
        if (hit == NULL || colors[p] != DEVICE_ID_CODE_COLORS[hit - LETTERS]) {
          agree = false;
        }
      }
    }
    CHECK(agree, "deviceIdColors() matches deviceIdCode() at every position");
  }

  // --- both FICR words must feed the hash.  ble_broadcast used to fold
  // DEVICEID[0] alone; reverting to that would make boards differing only
  // in the high word collide, which is silent and miserable to chase.
  {
    setId(0x11223344, 0x00000000);
    char a[DEVICE_ID_CODE_LEN + 1];
    deviceIdCode(a);
    uint16_t short_a = deviceIdShort();

    bool hi_matters = false, hi_matters_short = false;
    for (uint32_t h = 1; h < 64; h++) {
      setId(0x11223344, h);
      char b[DEVICE_ID_CODE_LEN + 1];
      deviceIdCode(b);
      if (strcmp(a, b) != 0) { hi_matters = true; }
      if (deviceIdShort() != short_a) { hi_matters_short = true; }
    }
    CHECK(hi_matters, "DEVICEID[1] affects the letter code");
    CHECK(hi_matters_short, "DEVICEID[1] affects the short id");

    setId(0x00000000, 0x55667788);
    deviceIdCode(a);
    short_a = deviceIdShort();
    bool lo_matters = false, lo_matters_short = false;
    for (uint32_t l = 1; l < 64; l++) {
      setId(l, 0x55667788);
      char b[DEVICE_ID_CODE_LEN + 1];
      deviceIdCode(b);
      if (strcmp(a, b) != 0) { lo_matters = true; }
      if (deviceIdShort() != short_a) { lo_matters_short = true; }
    }
    CHECK(lo_matters, "DEVICEID[0] affects the letter code");
    CHECK(lo_matters_short, "DEVICEID[0] affects the short id");
  }

  // --- pure functions of the ID: same board, same answer, every time
  {
    setId(0xDEADBEEF, 0xCAFEF00D);
    char first[DEVICE_ID_CODE_LEN + 1];
    deviceIdCode(first);
    uint16_t first_short = deviceIdShort();
    bool stable = true;
    for (int i = 0; i < 10; i++) {
      char again[DEVICE_ID_CODE_LEN + 1];
      deviceIdCode(again);
      if (strcmp(first, again) != 0 || deviceIdShort() != first_short) {
        stable = false;
      }
    }
    CHECK(stable, "code and short id are stable for a fixed device");

    int code_changed = 0, short_changed = 0;
    for (int i = 0; i < 2000; i++) {
      nthId((uint32_t)i);
      char c[DEVICE_ID_CODE_LEN + 1];
      deviceIdCode(c);
      if (strcmp(c, first) != 0) { code_changed++; }
      if (deviceIdShort() != first_short) { short_changed++; }
    }
    CHECK(code_changed > 1900 && short_changed > 1900,
          "different devices get different codes and short ids");

    // There is deliberately no cross-check that a changed short implies a
    // changed code, or the reverse.  Both are projections of one hash, but
    // of different widths -- 3125 possible codes against 65536 shorts --
    // so each collides independently and neither implies the other.  What
    // actually guards the shared-hash property is the DEVICEID[1] test
    // above: fold only the low word, as ble_broadcast once did, and the
    // short stops tracking the code.
  }

  // --- the only buffer in the header
  {
    setId(0x01020304, 0x05060708);
    char code[DEVICE_ID_CODE_LEN + 1];
    deviceIdCode(code);

    char name[32];
    deviceIdName(name, sizeof(name), "sketchingpad");
    char expect[32];
    snprintf(expect, sizeof(expect), "sketchingpad-%s", code);
    CHECK(strcmp(name, expect) == 0, "deviceIdName() is <prefix>-<code>");

    char tiny[8];
    memset(tiny, 'x', sizeof(tiny));
    deviceIdName(tiny, sizeof(tiny), "sketchingpad");
    CHECK(tiny[sizeof(tiny) - 1] == '\0', "a short buffer is still terminated");
    CHECK(strlen(tiny) == sizeof(tiny) - 1, "a short buffer is filled, not overrun");

    char exact[DEVICE_ID_CODE_LEN + 2];
    deviceIdName(exact, sizeof(exact), "");
    CHECK(strcmp(exact + 1, code) == 0, "an empty prefix still yields the code");
  }

  // --- table sizes, so the digit range and the tables cannot drift apart
  {
    CHECK(sizeof(DEVICE_ID_CODE_LETTERS) == DEVICE_ID_CODE_NCOLORS,
          "letter table has one entry per color");
    CHECK(sizeof(DEVICE_ID_CODE_COLORS) / sizeof(DEVICE_ID_CODE_COLORS[0]) ==
              DEVICE_ID_CODE_NCOLORS,
          "color table has one entry per color");
    CHECK(DEVICE_ID_CODE_NCOLORS == NLETTERS,
          "this test's alphabet matches the header's");
    bool distinct = true;
    for (int i = 0; i < DEVICE_ID_CODE_NCOLORS; i++) {
      for (int j = i + 1; j < DEVICE_ID_CODE_NCOLORS; j++) {
        if (DEVICE_ID_CODE_LETTERS[i] == DEVICE_ID_CODE_LETTERS[j]) { distinct = false; }
        if (DEVICE_ID_CODE_COLORS[i] == DEVICE_ID_CODE_COLORS[j]) { distinct = false; }
      }
    }
    CHECK(distinct, "letters and colors are all distinct, so a code is readable");
  }

  return test_report("id_test");
}
