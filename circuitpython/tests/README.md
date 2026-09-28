# CircuitPython host tests

`touch.py` talks to two sensors over I2C, but what it actually *does* is
read a register, keep the right bits and put them somewhere. That part
runs anywhere. These suites run it against stub sensors, on a laptop, with
no board attached.

```sh
cd circuitpython/tests
make run
```

Stdlib `unittest` and nothing else, the same way `arduino/tests` needs
only a compiler. A suite that has to be installed first is a suite that
stops being run.

## Why this exists at all

`arduino/tests` covers the C side and `make -C arduino check` keeps the
copied headers honest, but that check only syncs `.h` files.
`circuitpython/sketchingpad_midi_simple/touch.py` carries a **second copy
of the pad tables, in another language**, and nothing in either build
makes the two agree. That is the gap `pad_map_test.py` closes.

## Suites

`pad_map_test.py` -- the tables:

- **Both pad tables are byte-for-byte identical to
  `arduino/shared/touch_map.h`.** The one check the C suite cannot make,
  and the reason this directory was worth creating.
- The structural invariants ported from `arduino/tests/touch_map_test.cpp`:
  each table a permutation, each sensor bit lighting only its own LED,
  bits ascending by exactly one inside each group of four.
- That ascending check has one documented exception -- TP17-20 runs U3
  ELE10, ELE11, ELE0, ELE1 -- so the wrap is **pinned separately**
  (`LED_TO_MPR[17] == 11`, `[18] == 0`). An exception that is not pinned
  is just a hole. Writing this test without the exception is how the
  real table first looked wrong to me; the C suite was right.
- U4's ELE4-7 reach no LED. They have no pad and cannot be disabled in
  the ECR, so if they ever chatter they must still paint nothing.
- `code.py`'s note table: 16 notes in MIDI range, both rows ascending,
  the low row below the middle row, every note on the major pentatonic,
  and `min`/`max` landing where the startup banner claims they do.
- The bend constants are centred on **0**, not 8192. `tmidi`'s
  `pitch_bend` property is the wire value minus 8192, so constants
  carried over from a 0-16383 scheme would still look perfectly
  plausible and bend the wrong way.

`code.py` is read with `ast`, not imported: it ends in `while True`.

`touch_test.py` -- the driver:

- Every TS20 channel and every MPR121 electrode lights its own LED, which
  is what proves the table and the mapper agree. A mapper reading the
  table backwards still produces a permutation.
- **OVCF survives the read.** `touched()` masks to 12 bits and would drop
  bit 15, which is the difference between an idle chip and one that has
  faulted to Stop Mode and is ignoring writes -- and that failure
  otherwise looks exactly like nobody touching anything. Also that OVCF
  is not itself mistaken for a touch.
- A held-low IRQ line causes a bounded re-read. Two chips wired-OR onto
  one line means one can re-assert between the two status reads, leaving
  the line low with no edge behind it and nothing to read it again.
- **`report()` is quiet on a healthy chip.** The CircuitPython driver
  leaves `ECR` at `0x8F` where the Arduino one leaves `0x8C`; checking
  for `0x8C` here would warn on every healthy boot, which is how a
  diagnostic gets trained out of usefulness. It still complains when a
  chip really has left Run Mode, and names out-of-range electrodes.
- Only U4's four J2 electrodes get parked at `0xFF`, and no others.
- `low_power` reaches both backends -- `fast_mode=False` on the TS20,
  the slower `CONFIG2` on the MPR121s. It is a `begin()` argument rather
  than a setter because the CircuitPython TS20 driver only takes
  `fast_mode` at construction.
- Probing: no sensor, one MPR121 alone (a wiring fault, not a supported
  board), and TS20 winning when everything answers at once.
- `release_int()` frees D3, because CircuitPython will not hand the same
  pin to an `alarm.pin.PinAlarm` while a `DigitalInOut` holds it, and
  `read()` keeps working afterwards.

## On the stubs

`stubs/` holds four modules standing in for `board`, `digitalio`, `ts20`
and `adafruit_mpr121`. Two details in them are load-bearing:

- `digitalio.DigitalInOut.value` starts **True**. The real line is pulled
  up by R1/R16 and only a sensor pulls it down; a stub starting at False
  would send every test down the IRQ-still-asserted path.
- `adafruit_mpr121.MPR121.status` is the full **16-bit** register, and the
  stub's reset defaults include `ECR = 0x8F`. A stub that returned `0x8C`
  would hide the Arduino/CircuitPython difference rather than pin it.

The cost of stubs is the usual one: these tests do not prove `touch.py`
works against the real chips, only that it is asking them for the right
things. Hardware is still Tod's half of the loop.

## Mutation-tested

Per `arduino/tests/README.md`: a test that still passes against a
deliberately broken source is not testing anything. Eleven mutations were
applied to `touch.py` and `code.py` and every one was caught --
transposed table entries, a table borrowed from the wrong board rev,
dropping OVCF, not chasing the held-low IRQ, checking `ECR` against
`0x8C`, skipping the J2 parking, ignoring `low_power` on either backend, a
note off the pentatonic, bend constants on the old 0-16383 scheme, and the
two note rows swapped.

Worth repeating after adding assertions here. It is the only way to know a
new one does anything.
