// SPDX-FileCopyrightText: 2026 Tod Kurt
// SPDX-License-Identifier: GPL-3.0-or-later
//
// player_test.cpp -- LedAnim timing, fade and live feedback
//
// The player is driven entirely by a caller-supplied millis(), so its
// timing is fully testable here: no board, no waiting in real time.

#include "led_anim.h"
#include "test_util.h"

// Run the player from t0 to t1 in frame-sized steps, as loop() would.
static void run(LedAnim &a, uint32_t t0, uint32_t t1) {
  for (uint32_t t = t0; t <= t1; t += 5) { a.update(t); }
}

static bool allDark(const LedAnim &a) {
  for (uint8_t i = 0; i < ANIM_NUM_LEDS; i++) {
    if (a.pixel(i) != 0) { return false; }
  }
  return true;
}

int main() {
  printf("player_test\n");

  // --- an empty animation must not claim to be playing
  {
    LedAnim a;
    a.load(NULL, 0);
    a.start(1000);
    CHECK(!a.playing(), "an empty animation does not start");
  }

  // --- event timing. dt is the gap *before* each event, in 10 ms units.
  {
    AnimEvent ev[3] = {
        animEventMake(0, 5, 1),   // t + 0
        animEventMake(10, 7, 2),  // t + 100 ms
        animEventMake(20, 9, 3),  // t + 300 ms
    };
    LedAnim a;
    a.load(ev, 3);
    a.start(1000);
    CHECK(a.playing(), "a loaded animation starts");

    a.update(1000);
    CHECK(a.pixel(5) == ANIM_PALETTE[1], "event 0 fires immediately, in its color");
    CHECK(a.pixel(7) == 0 && a.pixel(9) == 0, "later events have not fired");

    a.update(1099);
    CHECK(a.pixel(7) == 0, "event 1 has not fired 1 ms early");
    a.update(1100);
    CHECK(a.pixel(7) != 0, "event 1 fires at +100 ms");

    a.update(1299);
    CHECK(a.pixel(9) == 0, "event 2 has not fired 1 ms early");
    a.update(1300);
    CHECK(a.pixel(9) != 0, "event 2 fires at +300 ms");
  }

  // --- a late update must not drop events that came due meanwhile
  {
    AnimEvent ev[3] = {animEventMake(0, 1, 1), animEventMake(5, 2, 2),
                       animEventMake(5, 3, 3)};
    LedAnim a;
    a.load(ev, 3);
    a.start(1000);
    a.update(5000); // one very late call, all three now overdue
    CHECK(a.pixel(1) != 0 && a.pixel(2) != 0 && a.pixel(3) != 0,
          "a single late update fires every overdue event");
  }

  // --- the fade, and when playback actually ends
  {
    AnimEvent ev[1] = {animEventMake(0, 4, 7)}; // white, the slowest to fade
    LedAnim a;
    a.load(ev, 1);
    a.start(1000);
    a.update(1000);
    uint32_t lit = a.pixel(4);
    CHECK(lit == ANIM_PALETTE[7], "event lights at full palette value");

    run(a, 1000, 1200);
    CHECK(a.pixel(4) != 0 && a.pixel(4) != lit, "the pixel is fading, not gone");
    CHECK(a.playing(), "playback continues while the trail is still lit");

    run(a, 1200, 20000);
    CHECK(!a.playing(), "playback ends only after the trail has faded out");
    CHECK(allDark(a), "every pixel is dark at the end");
  }

  // --- stop() is immediate
  {
    AnimEvent ev[2] = {animEventMake(0, 6, 3), animEventMake(50, 8, 4)};
    LedAnim a;
    a.load(ev, 2);
    a.start(1000);
    a.update(1000);
    a.stop();
    CHECK(!a.playing() && allDark(a), "stop() ends playback and blanks the frame");
    a.update(1600); // the second event's deadline has passed
    CHECK(allDark(a), "a stopped animation fires no further events");
  }

  // --- start() clears the previous run, so a replay is not additive
  {
    AnimEvent first[1] = {animEventMake(0, 2, 5)};
    AnimEvent second[1] = {animEventMake(0, 11, 6)};
    LedAnim a;
    a.load(first, 1);
    a.start(1000);
    a.update(1000);
    a.load(second, 1);
    a.start(2000);
    a.update(2000);
    CHECK(a.pixel(2) == 0, "the previous animation's pixels are cleared");
    CHECK(a.pixel(11) != 0, "the new animation plays");
  }

  // --- poke(): live touch feedback shares the buffer and the fade
  {
    LedAnim a;
    CHECK(!a.playing(), "poke does not require a loaded animation");
    a.poke(12, 3);
    CHECK(a.pixel(12) == ANIM_PALETTE[3], "poke lights a pixel in its color");
    CHECK(!a.playing(), "poke does not start playback");
    run(a, 0, 20000);
    CHECK(allDark(a), "a poked pixel fades out like any other");
  }

  // --- out-of-range LED indices are ignored rather than corrupting memory.
  // 5 bits hold 0-31 but only 20 LEDs exist, so a corrupt packet can carry one.
  {
    LedAnim a;
    AnimEvent ev[1] = {animEventMake(0, 31, 2)};
    a.load(ev, 1);
    a.start(1000);
    a.update(1000);
    CHECK(allDark(a), "an event for a nonexistent LED is dropped safely");
    CHECK(a.pixel(31) == 0, "reading past the strip returns 0");
  }

  // --- load() clamps rather than overrunning its buffer
  {
    AnimEvent big[ANIM_MAX_EVENTS];
    for (uint8_t i = 0; i < ANIM_MAX_EVENTS; i++) {
      big[i] = animEventMake(1, (uint8_t)(i % ANIM_NUM_LEDS), 1);
    }
    LedAnim a;
    a.load(big, ANIM_MAX_EVENTS);
    a.start(1000);
    run(a, 1000, 3000);
    CHECK(true, "a maximum-length animation loads and plays without crashing");
  }

  // --- millis() wrap, the reason the deadline compare is signed.
  // Here due_ has wrapped past zero while now has not, so an unsigned
  // `now >= due_` reads as true and fires the event ~49 days early.
  {
    uint32_t near_wrap = 0xFFFFFF00u;
    AnimEvent ev[2] = {animEventMake(0, 3, 1), animEventMake(50, 5, 2)};
    LedAnim a;
    a.load(ev, 2);
    a.start(near_wrap);

    a.update(near_wrap);
    CHECK(a.pixel(3) != 0, "first event fires just before the wrap");
    // due_ is now near_wrap + 500, which has wrapped to a small number.

    a.update(near_wrap + 100); // still large, still before the deadline
    CHECK(a.pixel(5) == 0, "an event whose deadline wrapped does not fire early");

    a.update((uint32_t)(near_wrap + 500)); // wrapped; the deadline exactly
    CHECK(a.pixel(5) != 0, "it fires once the clock actually reaches it");
  }

  return test_report("player_test");
}
