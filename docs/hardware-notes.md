# sketchingpad hardware notes

Board-side facts the firmware leans on: what each touch chip does, how
its INT line behaves, and the board changes the sketches assume. Pinout,
the MIDI pad map and everything firmware-side are in
`arduino/README.md`; the P-FET part choice and the LED rail's gotchas
are in `low-power-notes.md`.


## Board features the firmware assumes

All three below are present on `sketchingpad1` and `sketchingpad1_mpr`.
If bench testing and don't have these wired, sketches still work, but can't
do true low-power.

1. **LED power rail on D7** A PFET high-side load switching LED power rail 
    with R4 100k pulling up to the source so the rail is **off** by default.
    See `low-power-notes.md` for more details.
2. **SW1 pin 1 on D10/MOSI.** Can be used as a wakeup source
3. **Touch chip INT on to D3/A3**. Makes interrupt-driven scanning and 
    touch-to-wake possible; without it the irmware had to poll, 
    and `SYSTEM_OFF` was not possible.


## TS20 capsense touch chip (board `sketchingpad1`)

### Pad numbering is not TS20 channel order

The `TPn` refdes silkscreened on the board is **not** the index the
firmware uses. The firmware indexes everything by TS20 channel, and on
the sketchingpad1 (aka proto2) board, those two numberings disagree:

```
physical layout, TP refdes        the same pads, by TS20 channel
       17 18 19 20                        4  5  6  7
  1  2  3  4  5  6  7  8            0  1  2  3   8  9 10 11
  9 10 11 12 13 14 15 16           16 17 18 19  12 13 14 15
```

TP1-4 are channels 0-3, but TP17-20 are channels 4-7 -- so the top row
sits in the *middle* of the channel range, not at the end. Anything that
selects a range of pads by index has to account for that; assuming the top
row was the top of the range is what put `ble_broadcast_anim_ts20`'s color
swatches on the wrong four pads. Sketches over the shared touch layer
index by LED instead, where the top row really is 16-19, so the same
mistake is not available to them.

**`led_to_pad` is board-rev specific and must be re-derived, not carried
forward.** proto1 and proto2 have entirely different tables. A proto1 table
survived unnoticed into stripproto1 and proto2 firmware, where the LEDs lit
under the wrong pads. Derive it in three steps:

1. LED chain order from the netlist (`LEDn.DOUT` to `LEDn+1.DIN`); on both
   revs this is plain refdes order.
2. Pad to channel from the netlist: the `TPn` net reaching `U1` lands on a
   pin whose pinfunction is `CSm`, and **channel index = m - 1** (channel 0
   is CS1). Confirmed on hardware with `hwtest_ts20`.
3. Nearest-neighbour match of `LED` to `TP` footprint positions from the
   `.kicad_pcb`. Each LED is ~5.8 mm from its pad and >=12 mm from the next
   nearest, so there is no ambiguity.

Validate any such script against proto1 first: it must reproduce that
board's historical table
`{18,1,3,4,6,9,19,2,5,8,17,0,10,11,16,15,14,13,12,7}` exactly.

### The TS20 INT line

- **The TS20 chip needs no INT-specific configuration.** It asserts after the
  library's ordinary `begin()`, which writes GTRL1/GTRL2, port control
  and calibration -- none of it INT-related. Verified with `hwtest_ts20`
  in INT mode, which cross-checks every
  interrupt against a 500 ms watchdog read and prints `INT MISSED` if
  the touch state ever changes without an edge. Sliding a finger across
  all 20 pads produced no misses.
- **It is a ~1 ms active-low pulse, not a level.** Scoped at 995 us.
  The edge counter steps by exactly 2 per change (one falling, one
  rising) and the pin always reads high by the time `loop()` samples it.
- **It fires on release as well as touch**, so a `touches == 0`
  transition is announced too.
- **The pin never tells you the state**, only that state changed. Every
  edge must be followed by an I2C read. `hwtest_ts20` uses `CHANGE`
  because it is characterising the line; `ble_keyboard_lowpower_ts20` uses
  `FALLING`, one event per change, since it reads the sensor either way.

Two consequences for `SYSTEM_OFF`:

- 995 us is ample for GPIO `SENSE`/`DETECT`, which is asynchronous
  (it has to be -- the GPIO clock domain is off in `SYSTEM_OFF`).
  Even assuming the wake path needed a few 32 kHz cycles, ~30 us, there
  is 30x margin.
- Because it is a pulse, there is a software race: a touch landing
  between "decide to sleep" and "arm SENSE" leaves no level behind and
  would be slept through. `enterDeepSleep()` closes it by arming SENSE
  *first*, then doing a final `ts20.touched()` read -- the TS20 still
  holds the state in its output register even though the pulse is over.
  Anything arriving after the arm is caught by hardware, since a SENSE
  condition already met when `SYSTEM_OFF` is entered wakes immediately.

SW1 is the opposite case: a held level, so `enterDeepSleep()` waits for
release before arming `SENSE_LOW`, or it would wake the instant it slept.


## MPR121 capsense touch chip (board `sketchingpad1_mpr`)

The TS20 went out of stock at LCSC, so `sketchingpad1_mpr` replaces it
with two 12-channel MPR121s. Same 20 pads, same 20 LEDs, same pins --
`TS20_INT` is just renamed `TOUCH_INT` and is still D3/A3.
`hwtest_mpr` is the bring-up sketch. `hwtest_bench_mpr` is the smaller
one: no LEDs, printing each touch and release as chip, electrode, `TPn`
and LED index. It probes both addresses and uses whichever answer, so a
pair of breakouts on jumpers, a single chip, and a half-populated board
are all ordinary cases -- which is also what makes it the sketch that
says which of the two chips is at fault when `hwtest_mpr` cannot.

Both sketches carry the same `led_to_pad` table -- the bench one inverts
it at startup rather than keeping a second copy -- and it prints the
whole map per chip in its banner, so a wrong table shows up before you
touch anything.

One deliberate difference: `hwtest_mpr` masks U4's four J2 electrodes and
parks their thresholds at 0xFF so they cannot chatter the shared IRQ
line. The bench sketch leaves them live and prints them as `no pad`,
because there you want to find out whether they would. Untouched `no pad`
lines appearing on their own is the answer.

- **U3 is 0x5A** (ADDR to GND), **U4 is 0x5B** (ADDR to VDD).
- **Both /IRQ pins share one net**, pulled up by R1 10k. They are open
  collector, so this is wired-OR and intended -- but the line says only
  that *something* changed, never which chip. Every edge reads both.
- **REXT (R8, R9) must be 75k 1%.** A wrong value here trips OVCF
  (status register 0x01 bit 7), on which the chip drops to Stop Mode and
  *discards* subsequent ECR writes. `Adafruit_MPR121::begin()` returns
  true anyway, so the symptom is a chip that answers on I2C, answers
  register reads, and never reports a touch. `hwtest_mpr` reads the ECR
  back and prints OVCF for exactly this reason.

### Pad numbering is not MPR121 electrode order either

Re-derived from the `sketchingpad1_mpr` netlist, same three steps as the
TS20 table above, with `TPn` landing on a pin whose pinfunction is
`ELEm` on one of the two chips. The combined word this firmware uses is
**bit = chip * 12 + ELEm**, so U3 occupies bits 0-11 and U4 bits 12-23.

```
physical layout, TP refdes        the same pads, by combined bit
     17 18      19 20                  10 11       0  1
  1  2  3  4  5  6  7  8            6  7  8  9   2  3  4  5
  9 10 11 12 13 14 15 16           20 21 22 23  12 13 14 15
```

TP and LED footprint positions are byte-identical to `sketchingpad1`, so
LED*n* still sits at TP*n*; only the sensor side of the table moved.

### Four electrodes you cannot turn off

U4's ELE4-7 are TOUCH21-24, brought out to the J2 header rather than to
pads. ECR's `ELE_EN` field enables ELE0..ELE(n-1) contiguously, so they
cannot be disabled without also losing ELE8-11, which are real pads.
Left floating they can chatter, and every spurious touch pulls the
*shared* IRQ down -- a spurious wake, once there is a sleeping tier.
`hwtest_mpr` parks their per-channel touch thresholds at 0xFF and masks
bits 16-19 in software. Masking alone would not help: the interrupt is
hardware.

Which is also why the `hwtest_mpr` sketch prints a `chatter:` line, counting IRQ
assertions whose read turned up no change in `touches`. Masked chatter is
otherwise invisible -- the edge counter climbs and nothing else moves. On
an untouched board that counter should stay flat; if it climbs, the 0xFF
thresholds did not take.

They were expected to report Out Of Range under autoconfig, which would
have mattered: `ARE` (auto-reconfiguration) retries an OOR channel on
*every sampling interval*, indefinitely. **They do not.** Measured
2026-09-17 on two MPR121 breakouts, both chips came up
`OOR=000 ACFF=0 ARFF=0` -- autoconfig found working charge settings for
all 24 electrodes. Recheck on the assembled board, where those four are
short traces to an unpopulated J2 rather than breakout pads, but there is
no reason to expect a small capacitance to defeat the search.

Whether they *chatter* is a separate question and still unmeasured. A
finger on them registers, as `no pad` lines, so they do work as
electrodes -- what has not been observed is them toggling on their own.

### CONFIG2 is the power knob

`CONFIG2` (0x5D) bits 2-0 are ESI, the electrode sample interval, and it
is most of the sensor's idle current. `Adafruit_MPR121::begin()` writes
`0x20`, which is ESI = 1 ms -- the chip's most expensive setting, and
not the part's own reset default of `0x24`.

| CONFIG2 | ESI    | IDD per chip |
|---------|--------|--------------|
| `0x20`  | 1 ms   | 393 uA       |
| `0x24`  | 16 ms  | 29 uA        |
| `0x25`  | 32 ms  | 17 uA        |
| `0x27`  | 128 ms | 8 uA         |

Two chips, so double it. The datasheet's IDD table is headed
"VDD and VREG = 1.8V" and this board runs 3.3V, so treat those as ratios
to measure against rather than predictions.

What it buys is latency: the touch status update rate is **ESI x SFI**,
and SFI defaults to 4 samples, so `0x24` responds in 64 ms rather than
4 ms. A fast interval while playing and a slow one while idle is the
obvious answer, but switching it means a Stop -> Run transition, which
restarts baseline tracking and reruns autoconfig -- the same caveat as
`ts20.setFastMode()`. `CONFIG2_VALUE` at the top of `hwtest_mpr` is
there to sweep against a current meter.

### The MPR121 IRQ line

**It is a level, held low until the touch status registers are read.**
Not the TS20's ~1 ms pulse. Measured 2026-09-17 on U3 with
`hwtest_bench_mpr`, `USE_INT_PIN 1`:

```
[INT ] edges=1 int=0->1     touch
touched   ELE6   TP1   led 0
[INT ] edges=3 int=0->1     release
```

`int=0->1` is the whole result: the line was already low when `loop()`
got there and went high immediately after the status read, on release as
well as on touch. The edge count corroborates it -- one falling edge for
the touch, then two more for our own read's rising edge and the release's
falling edge. A pulse would have gone high on its own and read `1->1`.

It holds with both chips wired-OR onto the line, confirmed the same day:
still exactly two edges per event and `int=0->1`, whichever chip raised
it, with no `INT MISSED`.

This had to be measured. The datasheet says only that IRQ is "triggered
any time a touch or release is detected" and stops there, and AN3895,
which it cites for the serial interface, is purely the I2C protocol --
addressing, START/STOP, auto-increment reads -- and never mentions IRQ.

The consequence is the one that matters for a sleeping tier: a held level
removes the `SYSTEM_OFF` arming race the TS20's pulse creates (see
above). `SENSE_LOW` latches on its own, so the only rule is to read both
chips immediately before arming, or a stale assertion wakes the board the
instant it sleeps.

Both sketches print `int=N->N` around the status read, alongside an edge
counter, a 500 ms watchdog and the `INT MISSED` check, the same way
`hwtest_ts20` settled the TS20's line.

#### Two chips on one INT line

When the INT line is triggered, either chip could be causing it. 

`hwtest_bench_mpr` probes both addresses and drives whichever answer, so
one chip, two chips and a half-populated board are all ordinary cases.
`hwtest_mpr` requires both and stops if either is missing, which is right
for a sketch whose job is testing the assembled board.

Two chips on one open-collector line means a chip can re-assert in the gap 
between the two status reads, leaving the line low with no new edge behind it --
and nothing would ever read it again. So the read chases the level down
rather than reading once, bounded to `MAX_INT_PASSES` because a chip
stuck low would otherwise hang `loop()`.


