# One touch layer over the TS20 and the 2x MPR121 boards

Agreed design, 2026-09-18. **Not implemented.** Scope and shape were
chosen deliberately; see "Decisions already made" at the end before
revisiting them.

## Why

`sketchingpad1` uses one 20-channel TS20; `sketchingpad1_mpr` replaces it
with two 12-channel MPR121s because the TS20 went out of stock at LCSC.
The boards are otherwise identical: same XIAO nRF52840, same pins, the
same 20 pads in the same physical positions, the same 20 LEDs in the same
chain order.

Firmware has forked anyway. `hwtest_ts20` and `hwtest_mpr` are the same
sketch twice, and every BLE sketch still assumes a TS20. Worse, each
sketch carries its own copy of `led_to_pad`, and that table has been
wrong twice already -- a stale `proto1` table carried into three boards,
and `ble_broadcast_anim` putting its colour swatches on the wrong four
pads.

The goal is that sketches stop caring which board they are on, and that
the pad mapping exists once per board rev instead of once per sketch.

## What makes it possible

**The two sensors have disjoint I2C addresses.** TS20 at `0x6A`
(`TS20_ADDR_GND` in `TS20.h`), MPR121s at `0x5A` and `0x5B`. The board
can therefore be identified at runtime, one binary runs on both, and
nothing needs reflashing when swapping boards on the bench. No
compile-time board `#define`.

## What the design rests on

All of this is written up in `arduino/README.md`, in the two sensor
sections. Read both first. The load-bearing facts:

- **INT semantics are opposite.** The TS20's INT is a ~995 us pulse; the
  MPR121's is a level held low until the touch status registers are read
  (measured 2026-09-17). This is the one place the abstraction has to do
  real work rather than just dispatch.
- **Two MPR121s wired-OR onto one line** can re-assert in the gap between
  the two status reads, leaving the line low with no new edge behind it.
  The read has to chase the level down, bounded.
- **`led_to_pad` is board-rev and sensor specific.** Derive it from the
  netlist; never carry one forward. `arduino/README.md` has the
  derivation steps and the `proto1` validation table.

## Design

### Normalise to LED index, and the tables leave the sketches

Both boards put LED *n* at TP *n+1*. Make that the public index: bit *n*
of what the driver returns is LED *n*, which is TP *n+1* on the
silkscreen. Sketches then write `(touches >> n) & 1` and drop
`led_to_pad` entirely; each backend owns its own table privately.

That is the actual payoff. `ble_broadcast_anim` selecting a range of pads
by index becomes correct by construction, rather than by remembering that
the top row sits in the middle of the TS20's channel range.

### Two headers, copied per sketch

Following the `device_id_code.h` / `led_anim.h` / `anim_wire.h`
convention `arduino/README.md` documents, including its warning that the
copies must stay identical.

`arduino/touch_map.h` -- dependency-free, nothing but `<stdint.h>`: the
per-board tables and the sensor-bit to LED-index conversion. Kept apart
from the driver precisely so `arduino/tests/` can cover it without
stubbing out I2C.

`arduino/touch.h` -- the driver, over `TS20.h` and `Adafruit_MPR121.h`:

```c
bool        touchBegin();           // probes 0x6A, then 0x5A/0x5B
const char *touchName();            // "TS20" / "2x MPR121", for banners
uint32_t    touchRead();            // bit n = LED n = TP(n+1), 20 bits
uint8_t     touchIntPin();          // A3 on both revs, but let it say so
void        touchSetLowPower(bool); // TS20 auto/fast; MPR121 CONFIG2 ESI
bool        touchFault();           // TS20 noise flag; MPR121 OVCF
bool        touchArmWake();         // see below; false = do not sleep
```

`touchRead()` hides the MPR121's two transactions and the bounded
re-read-while-low loop, so callers cannot get that wrong.

### `touchArmWake()` is where the INT difference actually lives

The two orderings are opposite, which is why this cannot be a flag the
sketch branches on:

- **MPR121**: read first. The read is what releases the held line; arming
  `SENSE_LOW` while it is still low latches DETECT and wakes the board
  the instant it sleeps.
- **TS20**: arm first. The pulse is long gone by then, so the final
  `ts20.touched()` read is what closes the software race -- the chip
  still holds the state in its output register.

So the driver owns the whole sequence: arm the pin as a `SYSTEM_OFF` wake
source in whatever order this sensor needs, and return false if activity
arrived during the arm. The sketch keeps its own button arming either
side of the call, as `hwtest_ts20_lowpower.ino` does today.

This makes `touch.h` nRF52-specific (`nrf_gpio_cfg_sense_input`). That is
fine, since both boards are XIAO nRF52840, but it is why the tables live
in the other header.

## Files

- `arduino/touch_map.h`, `arduino/touch.h` -- new, copied into each
  sketch folder that uses them.
- `arduino/hwtest_touch/hwtest_touch.ino` -- replaces `hwtest_ts20` and
  `hwtest_mpr`. Start from `hwtest_mpr.ino`, which already carries the
  diagnostics: I2C scan, ECR readback, OVCF, OOR, the `int=N->N` print
  and the `chatter:` counter. Keep each behind the backend that has it.
- `arduino/hwtest_touch_lowpower/hwtest_touch_lowpower.ino` -- replaces
  `hwtest_ts20_lowpower`; same three tiers, with `enterDeepSleep()`
  calling `touchArmWake()`.
- **Delete** `hwtest_ts20`, `hwtest_mpr` and `hwtest_ts20_lowpower` once
  the replacements run on hardware; git history keeps them. **Keep**
  `hwtest_mpr_bench` -- one chip, serial only, no LEDs. It is a different
  instrument, not a duplicate.
- `arduino/tests/touch_map_test.cpp` plus a Makefile entry.
- `arduino/README.md` -- fold the two sensor sections into one, keeping
  both sensors' measured behaviour; update the sketch table.
- BLE sketches stay on the TS20 this pass. Port them once the layer has
  run on both boards.

## Verification

1. `cd arduino/tests && make run`. The new test asserts each board's
   table is a permutation: every LED 0-19 covered exactly once, no sensor
   bit used twice, MPR121 bits within 0-23 with the four J2 electrodes
   unmapped. That is the bug class that has bitten twice, and it is
   catchable with no hardware at all.
2. `arduino-cli compile --warnings all -b Seeeduino:nrf52:xiaonRF52840`
   for both new sketches, and for **every permutation of any `#define`
   the docs tell a user to flip**. An `#if` branch nobody builds is where
   the warnings hide.
3. Flash on the two MPR121 breakouts. The banner must say `2x MPR121`,
   and touching an electrode must report the same LED index that
   `hwtest_mpr_bench` reports for it today.
4. Flash on a TS20 board (`sketchingpad_proto2` is built and was measured
   2026-09-16). Banner must say `TS20`. **The regression that matters:**
   every pad lights the same LED it lit under `hwtest_ts20`. If the merge
   breaks anything, it breaks here.
5. `hwtest_touch_lowpower` on the TS20 board: confirm DEEP sleep still
   wakes on touch, since that is the path whose arm ordering is reversed
   relative to the MPR121.

## Open, and worth saying out loud

- **No `sketchingpad1_mpr` board has been assembled.** Every MPR121
  result so far comes from breakouts on jumpers, so its table is
  validated for internal consistency but not against real PCB routing.
  Step 3 above cannot prove the mapping, only that it did not change.
- Whether U4's four J2 electrodes chatter untouched is still unmeasured.
  `hwtest_mpr`'s `chatter:` counter and the 0xFF threshold mitigation
  were both written against a prediction that has not been tested.

## Decisions already made

Asked and answered 2026-09-18, so do not re-litigate without reason:

- **Scope:** the hwtest sketches *and* the lowpower pair, because the
  lowpower path is where the sensors genuinely differ. Not the BLE
  sketches, not yet.
- **Shape:** copied headers per sketch, following the repo convention,
  rather than a new Arduino library.
