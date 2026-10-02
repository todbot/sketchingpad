# sketchingpad hardware notes

Board-side facts the firmware leans on: what each touch chip does, how
its INT line behaves, and the board changes the sketches assume. The
power tiers and budget are here too, at the end. Pinout, the MIDI pad
map and the rest of the firmware side are in `arduino/README.md`; the
P-FET part choice and the LED rail's gotchas are in
`low-power-notes.md`.

<!--ts-->
* [Board features the firmware assumes](#board-features-the-firmware-assumes)
* [TS20 capsense touch chip (board sketchingpad1)](#ts20-capsense-touch-chip-board-sketchingpad1)
  * [Pad numbering is not TS20 channel order](#pad-numbering-is-not-ts20-channel-order)
  * [The TS20 INT line](#the-ts20-int-line)
* [MPR121 capsense touch chip (board sketchingpad1_mpr)](#mpr121-capsense-touch-chip-board-sketchingpad1_mpr)
  * [Pad numbering is not MPR121 electrode order either](#pad-numbering-is-not-mpr121-electrode-order-either)
  * [Confirmed on the assembled board, 2026-09-30](#confirmed-on-the-assembled-board-2026-09-30)
  * [Four electrodes you cannot turn off](#four-electrodes-you-cannot-turn-off)
  * [CONFIG2 is the power knob](#config2-is-the-power-knob)
  * [The MPR121 IRQ line](#the-mpr121-irq-line)
    * [Two chips on one INT line](#two-chips-on-one-int-line)
* [Low power concerns](#low-power-concerns)
  * [hwtest_lowpower_ts20: the tiers without a radio](#hwtest_lowpower_ts20-the-tiers-without-a-radio)
    * [Measurements 2026-09-16](#measurements-2026-09-16)
  * [Power budget](#power-budget)
<!--te-->


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
`TS20_INT` renamed `TOUCH_INT` and is still D3/A3.
`hwtest_mpr` is the bring-up sketch. 
`hwtest_bench_mpr` is to test while the boards get fabbed: 
no LEDs, printing each touch and release as (chip, electrode, `TPn`, and LED index). 

Both sketches have the same `led_to_pad` table, which has
to be verified with an ordered sweep, as below.

`hwtest_mpr` masks U4's four J2 electrodes and parks their thresholds at 0xFF 
so they cannot chatter the shared IRQ line. 
The bench sketch leaves them live and prints them as `no pad`,
because there you want to find out whether they would. 
Untouched `no pad` lines appearing on their own show if there's chatter.

- **U3 is 0x5A** (ADDR to GND), **U4 is 0x5B** (ADDR to VDD).
- **Both /IRQ pins share one net**, pulled up by R1 10k. They are open
  collector, wired-OR. /IRQ going low says *something* changed, not which chip.
  Every edge reads both.
- **REXT (R8, R9) must be 75k 1%.** wrong values trips OVCF
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

### Confirmed on the assembled board, 2026-09-30

The first assembled `sketchingpad1_mpr`, running `arduino/hwtest_touch`,
read over USB serial. The banner, identical across four resets:

```
i2c scan: 0x5a 0x5b
sensor: 2x MPR121
config2=20 touch/release=12/6 autoconfig=1
U3 @0x5a: ECR=8c OVCF=0 OOR=000 ACFF=0 ARFF=0
U4 @0x5b: ECR=8c OVCF=0 OOR=000 ACFF=0 ARFF=0
mode: INT on D3 (idle level 1), watchdog 500 ms
```

TP1 to TP20 was then swept in silkscreen order. 
All 20 pads reported, each from exactly one electrode, 
entry for entry identical to `touch_led_to_mpr`:

```
LED   0  1  2  3   4  5  6  7   8  9 10 11  12 13 14 15  16 17 18 19
TP    1  2  3  4   5  6  7  8   9 10 11 12  13 14 15 16  17 18 19 20
chip U3 U3 U3 U3  U3 U3 U3 U3  U4 U4 U4 U4  U4 U4 U4 U4  U3 U3 U3 U3
ELE   6  7  8  9   2  3  4  5   8  9 10 11   0  1  2  3  10 11  0  1
bit   6  7  8  9   2  3  4  5  20 21 22 23  12 13 14 15  10 11  0  1
```

Two things this quick test did **not** settle. 
- No read needed a second pass (`passes=1` throughout), so the wired-OR 
re-read below is unexercised rather than confirmed. 
- No current was measured, so the `CONFIG2` table further down is still datasheet-at-1.8V.

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
all 24 electrodes. Confirmed 2026-09-30 on the assembled board, where
those four are short traces to an unpopulated J2 rather than breakout
pads: both chips still come up `OOR=000 ACFF=0 ARFF=0`.

Whether they *chatter* is a separate question, and still open. A finger
on them registers, as `no pad` lines, so they do work as electrodes;
what has not been observed is them toggling on their own. The 2026-09-30
capture is weak evidence at best -- no ELE4-7 touch and no `chatter:`
line in 150 s -- because the board was being swiped throughout, and the
question is what an idle one does.

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


## Low power concerns

The "lowpower" sketches implement the power tiers. 
`ble_keyboard_lowpower_ts20` uses BLE SoftDevice `sd_power_system_off()`
The simpler `hwtest_lowpower_ts20` uses `NRF_POWER->SYSTEM_OFF=1` 
and is simpler to put a power meter on, since BLE can dominate power consumption.

`ble_keyboard_lowpower_ts20` runs three tiers:

| Tier | Entered | BLE | LED rail | Cost |
|------|---------|-----|----------|------|
| ACTIVE | any touch | connected, latency 0 | on | ~150-400 uA |
| IDLE | 10 s untouched | connected, latency 20 | **off** | ~30-60 uA |
| DEEP | 5 min untouched, or button | SYSTEM_OFF | off | ~1-3 uA |

**Neither connected tier polls the TS20.** Its INT line drives a GPIO
interrupt that releases a FreeRTOS semaphore, so `loop()` blocks in
`xSemaphoreTake()` and the CPU sleeps between touches instead of waking
15-100 times a second to ask over I2C. The 250 ms semaphore timeout only
services the tier timers and does no bus traffic.

**DEEP is a one-way door.** Waking from `SYSTEM_OFF` resets the chip, so
the SoftDevice restarts and BLE has to reconnect, roughly 1-3 s. The
touch that woke the board is long gone by then, so it is a wake gesture
and not a keystroke. That is why DEEP sits behind a minutes-long timer
while IDLE, which keeps the link up, does the routine saving.

The rest, roughly in order of payoff:

1. **Sleep between events.** `delay()` on this core is `vTaskDelay()`
   (`cores/nRF5/delay.c:48`), so idling drops to the FreeRTOS idle task
   and WFE -- SYSTEM_ON sleep, ~2-3 uA for the MCU. The core builds with
   `configUSE_TICKLESS_IDLE 1`, so the 1024 Hz tick is suppressed while
   idle rather than waking the CPU every millisecond. Nothing in
   `loop()` busy-waits.
2. **Peripheral latency** (`slave_latency` in `ble_gap.h`). On the
   ACTIVE -> IDLE transition the sketch asks
   the central to let it skip 20 connection events (~630 ms effective,
   well inside the 4 s supervision timeout). This goes through
   `BLEConnection::requestConnectionParameter()`, *not*
   `Bluefruit.Periph.setConn*()` -- the latter only writes the preferred
   parameters used at connection setup, so calling it on a live
   connection compiles, runs, and saves nothing. The central may refuse,
   so nothing downstream assumes it took.
3. **LED rail gated at the P-FET**, not merely blanked. The rail is off
   until `LED_POWER` (D7) is driven low; the circuit is under
   Board features the firmware assumes, above.
4. **TS20 auto mode** rather than fast mode, for lower continuous
   current on the sensor. Set once before `begin()`: changing it later
   soft-resets the chip and restarts calibration, which would swallow a
   touch in progress.
5. **DC/DC regulator** enabled, TX power at -4 dBm, the Bluefruit
   advertising LED off, and `Serial` untouched unless USB is actually
   attached (checked via `NRF_POWER->USBREGSTATUS` *before*
   `Bluefruit.begin()` enables the SoftDevice, since POWER becomes a
   restricted peripheral after that).

The core's `initVariant()` already turns off the XIAO's onboard RGB LEDs
and disables the battery-sense divider, so the sketches do not repeat
that.

### `hwtest_lowpower_ts20`: the tiers without a radio

Same visible behaviour as `hwtest_ts20` -- touch a pad, its LED lights
and fades -- with the power techniques underneath and no BLE at all.
ACTIVE and IDLE differ only by the LED rail here; DEEP is still
`SYSTEM_OFF` woken by the INT line or the button.

Dropping BLE makes two things *simpler*, not harder. The SoftDevice is
enabled only inside `Bluefruit.begin()`, so without it POWER is not a
restricted peripheral:

```c
NRF_POWER->DCDCEN = 1;      // plain write; an SVC call with BLE up
NRF_POWER->SYSTEMOFF = 1;   // was sd_power_system_off()
```

It also means `DCDCEN` reads back reliably, so the banner prints it. The
BLE sketch can only report the SVC return code, because the readback is
unreliable once the SoftDevice owns POWER.

One thing this sketch does better than `ble_keyboard_lowpower_ts20`: the
block time follows what is on screen. `hwtest_ts20` runs a flat
`delay(10)` -- 100 Hz forever -- purely to animate a fade that usually
is not running. Here 100 Hz applies only while a pixel is actually lit,
and a dark board wakes 4x/sec for the tier timers alone.

#### Measurements 2026-09-16

Confirmed on hardware over USB:

- **`DCDCEN=1` holds**, so the DC/DC inductors are populated on this
  XIAO. That was the open risk behind the `USE_DCDC` guard.
- **Tier timers are exact.** With `DEEP_TIMEOUT_MS` set to 1 minute,
  IDLE to DEEP measured 50.023 s. 50 s is correct: both timers run from
  `last_touch_ms`, not from each other, so DEEP fires at T+60 s when
  IDLE already fired at T+10 s.
- **Wake from DEEP measured 2.42 s -- but that number is mostly the
  serial guard, not the wake.** `setup()` has
  `while (!Serial && millis() < 2000)`, and `millis()` runs from reset,
  so after waking it blocks until USB CDC finishes re-enumerating. The
  rest is ~50 ms button debounce plus the ~100 ms TS20 power-up delay.
  The real wake is likely ~300-400 ms, and on battery the 2 s
  disappears entirely because `Serial` is never touched.

To measure the wake properly, drive a spare pin (D1/A1, D8, D9 are
free) high at the top of `setup()` and low before `SYSTEMOFF`, and scope
the gap. `NRF_POWER->RESETREAS & POWER_RESETREAS_OFF_Msk` is also worth
reading at boot: that bit is set only on a System-OFF wake via GPIO
DETECT, and it is the only way to *confirm* `SYSTEM_OFF` was entered
rather than inferring it from the reset. It needs clearing by writing
1s back, or the bits accumulate.

Note that with USB attached the DEEP tier's **current** is meaningless
-- you are powered from the host and the USB peripheral is live. Timing
is fine to measure this way; microamps need a battery run with USB
detached.

### Power budget

| Source                                 | Draw                     |
|----------------------------------------|--------------------------|
| nRF52840 + SoftDevice, SYSTEM_OFF      | ~1-3 uA                  |
| nRF52840 + SoftDevice, SYSTEM_ON idle  | ~2-3 uA                  |
| BLE connected, 30 ms, latency 0        | ~150-400 uA              |
| BLE connected, 30 ms, latency 20       | ~30-60 uA (if accepted)  |
| TS20 scanning, auto mode               | ~0.1-1 mA                |
| 20x SK6812 quiescent, rail on          | ~10-20 mA                |
| 20x SK6812, **rail switched off**      | **0**                    |

That last line is the whole point of the P-FET. Before it, 20 ungated
SK6812s put ~10-20 mA on top of everything the firmware could do,
dominating it by roughly 100x -- the savings were real but invisible at
the battery. With the rail gated, IDLE and DEEP actually reach the
numbers above.
