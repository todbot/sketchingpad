# Host tests

`led_anim.h` and `anim_wire.h` (in `../ble_broadcast_anim/`) depend only on
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
Three others were found the same way and fixed.

One known equivalent mutant: deleting the duplicate-chunk short-circuit in
`AnimReassembler::feed()` changes nothing observable, because the repeat
rewrites identical bytes and the received-mask OR is idempotent. It is an
optimization, not a correctness guard, so no test covers it.
