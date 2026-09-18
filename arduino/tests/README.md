# Host tests

`led_anim.h` and `anim_wire.h` (in `../ts20/ble_broadcast_anim_ts20/`) depend only on
`<stdint.h>` -- no Arduino, no NeoPixel, no BLE -- so they compile and run
natively. That is deliberate: a chunk-reassembly bug is miserable to
diagnose over the air and trivial to catch here.

```sh
cd arduino/tests
make run
```

Builds with `-fsanitize=address,undefined`. An out-of-bounds write in the
reassembler is silent on the nRF52 and fatal here, which is most of the
value.

The headers are copied per sketch, the same convention as
`device_id_code.h`. `INCLUDES` in the Makefile names the copy under test;
point it elsewhere to test a different sketch's copy.

## Suites

`wire_test.cpp` -- the on-air format and reassembly:

- `AnimMsg` is 25 bytes and `3 + 25 + 2 <= 31`, so flags plus the
  manufacturer-data element still fit one advertising packet. This is the
  constraint the whole protocol is shaped around, so it is asserted rather
  than assumed.
- Event packing across all 32 LED slots and 8 colors.
- Chunk-count arithmetic at its boundaries.
- Reassembly in order, in reverse, and with every packet duplicated.
- **Losing any single chunk** blocks completion until it is redelivered.
- Two senders interleaved; a third not locked out by stalled partials.
- Partials expire; in-window partials are left alone.
- Malformed packets: zero, oversized, and out-of-range chunk indices.
- Single-event and 72-event animations; the short final chunk is padded.

`id_test.cpp` -- `device_id_code.h`, the R/Y/G/B/M board identity:

- **Every letter appears at every position**, swept over 20000 synthetic
  device IDs. This is the suite's reason to exist: the code is five base-5
  digits because there are five colors, and it was five 2-bit slices back
  when there were four. Revert the modulus to bit masking and M becomes
  unreachable while every code still looks perfectly valid.
- `deviceIdColors()` agrees with `deviceIdCode()` position by position.
- **Both FICR words feed the hash.** `ble_broadcast` once folded
  `DEVICEID[0]` alone; going back to that would silently collide boards
  differing only in the high word.
- Code and short id are stable for a fixed device and differ across
  devices.
- `deviceIdName()` formatting, and that an undersized buffer is terminated
  rather than overrun.
- Letters and colors are all distinct, so a code stays readable.

It builds against `stubs/Arduino.h`, which supplies `snprintf`, `size_t`
and a writable fake `NRF_FICR`. The header under test is unmodified: it
includes `<Arduino.h>` and must keep doing so, since that is where
`NRF_FICR` comes from and the header promises to drop into any nRF52
sketch. The cost of the stub is that these tests do not prove the header
compiles against the real `Arduino.h` -- the sketch builds cover that.

`touch_map_test.cpp` -- `touch_map.h`, the per-board pad tables:

- **Each table is a permutation**: every LED 0-19 covered exactly once,
  no sensor bit used twice, every bit within its sensor's range. This is
  the bug class that has bitten twice on hardware.
- Each sensor bit alone lights exactly its own LED, which is what proves
  the table and the mapper agree. A mapper that read the table backwards
  still produces a permutation, so the structural checks alone pass it.
- **Sensor bits ascend within every group of four pads.** Two transposed
  entries are still a permutation and still round-trip, so nothing else
  notices them; both boards route the pads in blocks of four, so this
  does. Found by mutating the header -- the earlier suite passed against
  a swapped pair, which is exactly the wrong-table bug it exists for.
- U4's ELE4-7 reach no LED. They have no pad and cannot be disabled in
  the ECR, so if they ever chatter they must still paint nothing.
- The two tables differ, and TP17-20 is the top of neither sensor's
  range. Sharing a table across revs, and assuming the top row was the
  top of the range, are the two mistakes already made.

It includes only `<stdint.h>`, not `stubs/Arduino.h`. That it builds
with no stub at all is the proof that the table/driver split held.

`player_test.cpp` -- `LedAnim` timing:

- Events fire on their deadline, not a millisecond early.
- One late `update()` still fires everything that came due.
- The trail fades, and playback ends only once it has faded out.
- `stop()` and re-`start()` clear state rather than accumulating.
- `poke()` (live touch feedback) shares the buffer and the fade.
- Out-of-range LED indices are dropped: 5 bits hold 0-31 but only 20 LEDs
  exist, so a corrupt packet can carry one.
- The `millis()` wrap, which is why the deadline compare is signed.

## On the wrap test

The obvious version of that test is worthless. If `now` and the deadline
are equal, a signed and an unsigned comparison agree, so the test passes
against the bug it is meant to catch. It only bites when the deadline has
wrapped past zero and `now` has not -- then `now >= due_` is true and the
event fires about 49 days early. The test sets that case up specifically.

That hole was found by mutating the header and re-running: any test that
still passes against a deliberately broken header is not testing anything.
Three others were found the same way and fixed. Worth repeating that
exercise after adding tests here -- it is the only way to know a new
assertion does anything.

## Two things deliberately not tested

Deleting the duplicate-chunk short-circuit in `AnimReassembler::feed()`
changes nothing observable: the repeat rewrites identical bytes and the
received-mask OR is idempotent. It is an optimization, not a correctness
guard, so no test covers it.

There is no cross-check that a changed `deviceIdShort()` implies a changed
`deviceIdCode()`, or the reverse. Both are projections of one hash but at
different widths -- 3125 possible codes against 65536 shorts -- so each
collides independently and neither implies the other. An earlier draft
asserted it and was simply wrong. What does guard the shared-hash property
is the `DEVICEID[1]` test.
