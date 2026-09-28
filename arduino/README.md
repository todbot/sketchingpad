# sketchingpad -- Arduino sketches

Arduino firmware for the sketchingpad: a 20-pad capacitive touch surface
with 20 side-view SK6812 LEDs, driven by a Seeed XIAO nRF52840 plugged
into a `sketchingpad` board. Touch is a TS20 on every board up to
`sketchingpad1`, and two MPR121s on `sketchingpad1_mpr` -- see the two
sensor sections below.

## Sketches

Sensor-specific sketches live under `ts20/` and `mpr/` and carry the
sensor as a suffix; the ones at the top level need no sensor, or work
with either through the shared touch layer.

| Sketch                            | What it does                        |
|-----------------------------------|-------------------------------------|
| `hwtest_leds`                     | Chases one pixel around the 20 LEDs |
| `hwtest_touch`                    | Pad -> LED, fading. Either sensor   |
| `hwtest_touch_lowpower`           | Pad -> LED, tiers. Either sensor    |
| `ble_broadcast`                   | Symmetric BLE broadcast, no pairing |
| `ble_broadcast_anim`              | Record an LED animation, broadcast  |
| `ble_keyboard_simple`             | BLE HID keyboard. Either sensor     |
| `ble_midi_simple`                 | BLE MIDI controller. Either sensor  |
| `sketchingpad_midi_simple`        | USB + BLE MIDI. Either sensor       |
| `ts20/hwtest_ts20`                | Pad -> LED, fading. Poll or INT     |
| `ts20/hwtest_lowpower_ts20`       | Pad -> LED, three power tiers       |
| `ts20/ble_keyboard_simple_ts20`   | BLE HID keyboard, no power mgmt     |
| `ts20/ble_keyboard_lowpower_ts20` | BLE HID keyboard, three power tiers |
| `ts20/ble_broadcast_anim_ts20`    | Record an LED animation, broadcast  |
| `mpr/hwtest_mpr`                  | Pad -> LED, on the 2x MPR121 board  |
| `mpr/hwtest_bench_mpr`            | 1-2 MPR121s, touch to serial        |

`hwtest_leds` and `hwtest_ts20` are Arduino ports of
`circuitpython/hwtest/code_ledtest.py` and `code_test_ts20.py`.

`hwtest_touch`, `hwtest_touch_lowpower`, `ble_broadcast_anim` and
`ble_keyboard_simple` are the sensor-agnostic versions of their `ts20/`
and `mpr/` counterparts, over the shared touch layer -- see that section.
They supersede `hwtest_ts20`, `hwtest_mpr`, `hwtest_lowpower_ts20`,
`ble_broadcast_anim_ts20` and `ble_keyboard_simple_ts20`, which stay
until the replacements have run on both boards.
`hwtest_bench_mpr` is not superseded: one chip, serial only, no LEDs, it
is a different instrument.

`arduino/tests/` holds host-side tests for the dependency-free headers;
see the last section.

## Board and libraries

Board: **Seeed XIAO nRF52840**, FQBN `Seeeduino:nrf52:xiaonRF52840`.

Libraries used:

- `TS20` -- todbot's TS20 Arduino library
- `Adafruit MPR121` -- for `sketchingpad1_mpr` only; pulls in
  `Adafruit BusIO`
- `Adafruit NeoPixel` -- Adafruit easy neopixel library
- `MIDI Library` -- FortySevenEffects, for the two MIDI sketches only

Adafruit_NeoPixel takes the nRF52 PWM + EasyDMA path, thus`show()` does 
not disable interrupts and will not disturb BLE radio events)

The Seeed core is derived from Adafruit's nRF52 core, so it ships with the
S140 SoftDevice, FreeRTOS, TinyUSB, and the Bluefruit library
(`bluefruit.h`, `BLEHidAdafruit`, `BLEDis`). **No BLE library needs to
be installed.**

## Pinout

From the `sketchingpad_proto2` board; `sketchingpad_proto1` differs, see below.

| Signal      | XIAO pin | Notes                         |
|-------------|----------|-------------------------------|
| `PWM_AUDIO` | D0 / A0  | to audio jack via RC filter   |
| `BUTTON`    | D10/MOSI | SW1 to GND, internal pull-up  |
| `TS20_INT`  | D3 / A3  | open collector, R1 pulls up   |
| `SDA`       | D4       | `Wire` default                |
| `SCL`       | D5       | `Wire` default                |
| `LED_DAT`   | D6 / TX  | also `PIN_SERIAL1_TX`         |
| `LED_POWER` | D7 / RX  | P-FET gate, LOW = rail on     |

Still spare: D1/A1, D8 and D9.

The pin numbers are `#define`s at the top of each sketch.



## Build and upload

```sh
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 arduino/hwtest_leds
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 arduino/ts20/hwtest_ts20
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 arduino/mpr/hwtest_mpr
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 arduino/mpr/hwtest_bench_mpr
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 arduino/ts20/ble_keyboard_simple_ts20
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 arduino/ts20/ble_keyboard_lowpower_ts20
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 arduino/ts20/hwtest_lowpower_ts20
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 arduino/ble_midi_simple
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 arduino/sketchingpad_midi_simple
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 arduino/ble_broadcast
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 arduino/ts20/ble_broadcast_anim_ts20
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 arduino/ble_broadcast_anim
```

Start from `ble_keyboard_simple_ts20` when bringing up a board or changing the
key map: it polls the TS20, sends a report on change, lights the LEDs,
and does nothing else. `ble_keyboard_lowpower_ts20` is the same keyboard with
interrupt-driven scanning, connection-parameter management and sleep.

Note that even `ble_keyboard_simple_ts20` has to drive `LED_POWER` (D7) low in
`setup()`. That is not power management -- the rail is off by default
because the P-FET gate is pulled up to its source, so the LEDs never
light without it.

`hwtest_ts20` has two modes, selected by `USE_INT_PIN` at the top of the
sketch: `0` polls the TS20 every loop, `1` reads it only when the INT
line fires and reports `INT MISSED` if a change slips past. See the
TS20 INT section above.

To upload, double-tap reset to get the UF2 bootloader, then:

```sh
arduino-cli board list           # find the port
arduino-cli upload -b Seeeduino:nrf52:xiaonRF52840 \
  -p /dev/cu.usbmodemXXXX arduino/hwtest_leds
```

## `touch.h`: touch abstraction layer

The `touch.h` and `touch_map.h` files are one driver abstracting away
for both sensors, so a sketch does not care which board it is on. 
Copied per sketch.

| Call                  | What it does                                  |
|-----------------------|-----------------------------------------------|
| `touchBegin()`        | Probes 0x6A, then 0x5A/0x5B, and brings it up |
| `touchName()`         | `"TS20"` / `"2x MPR121"`, for banners          |
| `touchRead()`         | Bit n = LED n = TP(n+1), 20 bits               |
| `touchReadPasses()`   | Status reads the last `touchRead()` took       |
| `touchIntPin()`       | A3 on both revs, but let it say so             |
| `touchSetLowPower()`  | TS20 auto/fast; MPR121 CONFIG2 ESI             |
| `touchFault()`        | TS20 noise flag; MPR121 OVCF                   |
| `touchArmWake()`      | Arms SYSTEM_OFF wake; false = do not sleep     |
| `touchSource(led)`    | `"ch3"` / `"U3 ELE6"`, for diagnostic prints   |
| `touchReport()`       | Whatever this sensor says about its own health |

**The boards identify themselves at runtime.** The TS20 is at I2C addr `0x6A` 
and the MPR121s at `0x5A`/`0x5B`, so `touchBegin()` probes and picks. One
binary runs on both and nothing needs reflashing to swap boards on the
bench. `touchName()` prints `TS20` or `2x MPR121` in the banner.

**Everything is in LED-index space.** Bit *n* of `touchRead()` is LED
*n*, which is TP*(n+1)* on the silkscreen, on both boards. Sketches
write `(touches >> n) & 1` and carry no `led_to_pad` of their own -- both
tables live in `touch_map.h`, one per board type (ts20 vs mpr). 
Selecting a range of pads by index becomes correct by construction.

`touchRead()` hides the MPR121's two transactions and the bounded
re-read-while-low loop, so callers cannot get that wrong.

`touchReadPasses()` retur value:  greater than 1 means a chip
re-asserted mid-read, and it is always 1 on the TS20.

`touchSetLowPower()` is a set-once call, not a per-tier toggle. Both
backends soft-reset on that write, which restarts calibration and would
swallow a touch in progress.

### `touchArmWake()` INT differences

The two orderings are opposite, which is why this cannot be a flag the
sketch branches on:

- **MPR121**: read first. The read is what releases the held-low line;
  arming `SENSE_LOW` while it is still low latches DETECT and wakes the
  board the instant it sleeps.
- **TS20**: arm first. The ~1 ms pulse is long gone by then, so the final
  read is what closes the software race -- the chip still holds the state
  in its output register.

So the driver owns the whole sequence and returns false if activity
arrived during the arm. The sketch keeps its own button arming either
side of the call. This is what makes `touch.h` nRF52-specific
(`nrf_gpio_cfg_sense_input`), and why the tables live in the other
header, which is portable and host-tested.

## `device_id_code.h`: telling boards apart

API for helping to identify a board both on BLE advertisements and LED colors.
Copied per board. 

Two boards running identical firmware are otherwise indistinguishable.
`device_id_code.h` hashes the chip's UID `NRF_FICR->DEVICEID` (FNV-1a
over all 8 bytes) and slices it 2 bits at a time into five letters drawn
from `R`/`Y`/`G`/`B`/`M` (red, amber, green, blue, magenta) -- e.g.
`RMGGB`. Some sketches that use it:

- `ble_keyboard_simple` advertises the BLE model `sketchingpad-RMGGB` and
  lights the code on five LEDs at the end of `setup()`, so you can read a
  board's identity off the board itself. It stays lit until the first
  touch.
- `ble_broadcast` derives its 16-bit on-air device ID from the same hash
  via `deviceIdShort()`, and prints both in its banner: `device a3f1
  (RMGGB)`. So a peer seen on air can be matched to a board on the bench.

Five letters from five colors is 5^5 = 3125 codes -- an identifier hint,
not a guarantee. Collisions get likely somewhere past a couple of dozen
boards. Because five is not a power of two, the code is read out as
base-5 digits of the hash rather than as bit slices.




## TS20 capsense touch chip

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

## MPR121 capsense touch chip (`sketchingpad1_mpr`)

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

#### One chip was the right instrument

With two chips wired-OR onto one line, a level still low after a read
could be either of them, so `int=N->N` is ambiguous by construction --
which is why the measurement above was taken with only one chip wired.
Repeat it that way if it is ever in doubt: unplug one, and the level
belongs to the chip you are reading.

`hwtest_bench_mpr` probes both addresses and drives whichever answer, so
one chip, two chips and a half-populated board are all ordinary cases.
`hwtest_mpr` requires both and stops if either is missing, which is right
for a sketch whose job is testing the assembled board.

One consequence is already certain, whichever way that goes. Two chips
on one open-collector line means a chip can re-assert in the gap between
the two status reads, leaving the line low with no new edge behind it --
and nothing would ever read it again. So the read chases the level down
rather than reading once, bounded to `MAX_INT_PASSES` because a chip
stuck low would otherwise hang `loop()`.


## Low power

Two sketches implement the power tiers. 
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
3. **LED rail gated at the P-FET**, not merely blanked. See below.
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

#### Measured, 2026-09-16

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

### Board changes this firmware assumes

All three are present on `sketchingpad_stripproto1` and
`sketchingpad_proto2`, and are no longer "next rev" items. Sketches
will still build without them, but the matching tier degrades.

1. **P-FET high-side load switch on the LED rail** -- Q1 (AO3415A),
   source on `+3.3V`, drain feeding `3V3_LED`, gate on `LED_POWER` (D7)
   with R4 100k pulling up to the source so the rail is **off** by
   default. Highest-value change on the board; see
   `docs/low-power-notes.md` for why that polarity and that part.
2. **SW1 pin 1 wired to D10/MOSI.** It used to float, so there was no
   button input at all. Now a wake source, and a press forces DEEP sleep.
3. **TS20 INT (U1 pin 19) brought to D3/A3.** Previously it only
   reached its 10k pull-up R1 and dead-ended. This is what makes
   interrupt-driven scanning and touch-to-wake possible; without it the
   firmware had to poll, and `SYSTEM_OFF` was unreachable because
   nothing could wake it.

## `ble_broadcast_anim`: animations over advertising

`ble_broadcast_anim` runs on either sensor over the shared touch layer;
`ts20/ble_broadcast_anim_ts20` is the original, TS20-only, and stays
until the port has run on hardware. Everything below describes both.

One board records a short LED animation by touch and broadcasts it;
every other board in range plays it. No pairing, no connections, any
number of participants, all running this same firmware. The transport is
`ble_broadcast`; what is new is the animation layer.

Touch pads 1-16 to record -- the first touch starts it, ~1.5 s of
silence ends it, and then it plays locally *and* goes out over the air.
The top row picks the color for everything recorded after it; touching
the already-selected swatch again reaches its second variant, so four
pads cover all eight palette entries. Remember the top row is TS20
channels 4-7, not 16-19; see the pad-numbering section above.

### Wire format

Two bytes per event, nine events per packet:

```
AnimEvent (2 bytes)
  dt        : 8 bits   units of 10 ms, 0-2550 ms between events
  led       : 5 bits   0-19
  color     : 3 bits   index into an 8-entry firmware palette

AnimMsg (25 bytes)
  company_id   : 2     0xFFFF, as in ble_broadcast
  device_id    : 2     deviceIdShort() from device_id_code.h
  anim_id      : 1     increments per animation; dedupes replays
  chunk_idx    : 1     0-7
  total_events : 1     in EVERY packet, so any chunk reveals the total
  events       : 18    9 x AnimEvent, zero-padded on the last chunk
```

The 25-byte ceiling is not arbitrary. `BLEAdvertisingData::addData()`
guards `_count + len + 2 <= 31`, and the flags element has already spent
3, so an MSD element can carry at most 26 bytes.

`total_events` rides in every packet rather than a chunk count so a
receiver that misses chunk 0 still knows how many chunks to expect. Max
8 chunks = 72 events. There is no version field: `scan_callback` tests
`len == ANIM_MSG_LEN` exactly, so a format change changes the length and
old packets are ignored by construction.

The sender advertises each chunk for 200 ms and repeats the whole cycle
three times -- advertising is unacknowledged, so repetition is the
entire reliability mechanism. `SCAN_WINDOW` is raised to a 100% receive
duty cycle, unlike `ble_broadcast`: a missed chunk costs a whole
animation. Halve it to get `ble_broadcast`'s power behaviour back.

### Three things that shape the code

- **`isNewMessage()` from `ble_broadcast` cannot be used here.** It is
  1-deep per peer and treats any differing sequence number as new, so it
  would drop exactly the chunk repeats that reliability depends on. The
  peer table instead holds `last_played_anim`.
- **`scan_callback()` must not touch the LEDs.** It runs on the
  SoftDevice event task, and NeoPixel `show()` blocks with interrupts
  disabled for over a millisecond across 20 pixels -- which would make
  us miss the advertisements we are collecting. It parks packets in a
  4-deep ring; `loop()` drains and renders. Getting this wrong looks
  like a flaky radio, not a timing bug.
- **A sender never sees its own broadcast** (`scan_callback` filters on
  `device_id`), so local playback is kicked off from the send path.

### The two headers

`led_anim.h` is the non-blocking player: `load()`, `start()`,
`update(now_ms)` returning true when the frame changed, `pixel(i)`, and
`poke()` for live touch feedback. `anim_wire.h` is the packing and the
reassembler. Both depend on nothing but `<stdint.h>` -- no NeoPixel, no
BLE -- so they drop into any sketch and run on a host. They are copied
per sketch, the same convention as `device_id_code.h`; see Shared
headers below for which copy to edit.

## `ble_midi_simple`: MIDI over BLE

The MIDI counterpart to `ble_keyboard_simple`: poll the sensor, send on
a change, light the LEDs, nothing else. It runs on either sensor through
the shared touch layer, and it pairs the way the keyboard does -- the
BLE MIDI characteristic is `SECMODE_ENC_NO_MITM` in both directions
(`BLEMidi.cpp:153`), so an unbonded host gets nothing.

Messages go out through the FortySevenEffects MIDI library, which has
to be installed (`arduino-cli lib install "MIDI Library"`), over the
`BLEMidi` transport the core already ships -- the pairing the core's own
`blemidi` example uses. `MIDI_CREATE_BLE_INSTANCE(blemidi)` puts a
`SerialMIDI<BLEMidi>` wrapper in between, and `MIDI.begin()` reaches
`BLEMidi::begin()` down that chain, so it replaces the `blemidi.begin()`
a Bluefruit sketch would otherwise call.

Two of the library's conventions are not the wire's, and each fails
silently rather than loudly:

- **Channels are 1-16.** Channel 0 is `MIDI_CHANNEL_OMNI`, and a send on
  it returns having transmitted nothing (`MIDI.hpp:168-173`).
- **`sendPitchBend()` is centered on 0**, taking -8192..8191. It adds
  the 8192 offset itself and does not clamp, so a value already centered
  on 8192 overflows 14 bits and arrives as garbage.

Both are why `MIDI_CHANNEL` is 1 and the `BEND_*` constants are signed.

Not yet run on hardware, on either board rev. It compiles clean under
`--warnings all`.

### Pad map

The 16 pads of the bottom two rows are notes -- C major pentatonic,
climbing left to right and bottom to top, fixed velocity 100 on channel
1:

```
  TP1  TP2  TP3  TP4   TP5  TP6  TP7  TP8    G3 A3 C4 D4  E4 G4 A4 C5
  TP9 TP10 TP11 TP12  TP13 TP14 TP15 TP16    C2 D2 E2 G2  A2 C3 D3 E3
```

The top row of four is two momentary control pairs:

| Pads        | Held                  | Nothing held in the pair |
|-------------|-----------------------|--------------------------|
| TP17 / TP18 | bend 0 / bend 16383   | bend 8192, centered      |
| TP19 / TP20 | CC1 = 64 / CC1 = 127  | CC1 = 0                  |

Both bend pads at once cancel to center. Both mod pads at once send
127 -- the higher value wins. Both values are recomputed from the whole pad
word on every change, so rolling off one pad of a pair lands on what the
other still asks for instead of springing back.

The top row is *not* sensor channels 16-19 on either board -- see the
pad-numbering sections above. `led_to_note` and the four `PAD_*`
constants are indexed by LED, which is TP(n+1), so they read straight
off the silkscreen.

### Stuck notes

A disconnect leaves whatever was sounding to ring on the host, and there
is no link left to silence it over. What the sketch can do it does on
the way back in: when `notifyEnabled()` goes true it sends all-notes-off
(CC123), recenters bend, zeroes CC1, and replays from an empty pad word
so pads still being held come back as fresh note-ons.

Sends are gated on `notifyEnabled()`, not just `connected()`: a host can
hold a connection without ever subscribing, and the notification would
go nowhere.

Past that gate a refused send is silent. `BLEMidi::send()` returns a
bool, but the library's send methods return `void` and discard it, and
`send()` is not virtual, so a subclass cannot intercept it either. A
notification the SoftDevice will not queue -- 16 pads released at once
is the way to provoke it -- is therefore a dropped note-off that shows
up only as a note that will not stop. Serial still logs every pad
change, so serial disagreeing with what you hear localises it.

## `sketchingpad_midi_simple`: the same controller over USB as well

`ble_midi_simple` with USB MIDI alongside the radio. The pad map, the
note table and the control pairs are identical; what is new is that both
transports run at once and neither is required. Unplugged it is a BLE
controller, with nothing paired it is a USB controller, and plugged into
a host that is also paired it plays both at once. Every message goes to
whichever transports are up at that instant.

One MIDI library, two transports, so there are two instances --
`MIDI_USB` over TinyUSB's `Adafruit_USBD_MIDI` and `MIDI_BLE` over the
core's `BLEMidi`. `MIDI_CREATE_BLE_INSTANCE()` is not used because it
hardcodes the instance name to `MIDI`. Four small senders
(`midiNoteOn()` and friends) fan out and hold all the gating, so the pad
code never learns how many transports there are.

Not yet run on hardware, on either board rev, and the USB half has never
been enumerated by a host. It compiles clean under `--warnings all`.

### The two readiness tests are not equally strong

BLE knows the host subscribed (`blemidi.notifyEnabled()`). USB only
knows the host enumerated the device (`TinyUSBDevice.mounted()`) --
`Adafruit_USBD_MIDI` exposes no per-endpoint state, so there is nothing
better to ask. A write to an endpoint nobody opened is dropped rather
than blocking, so the weaker test costs nothing; it just cannot promise
anyone is listening.

### Catching a transport up

`ble_midi_simple` handles a reconnect by zeroing `last_touches`, which
replays every held pad as a fresh note-on. With two transports that
would re-send held notes to the transport that was already playing them,
so this sketch aims the catch-up at the one that just arrived:
`resyncHost()` sends it all-notes-off, then the current bend, mod and
held notes, and leaves `last_touches` alone.

### `usb_midi.setCables(1)` is not optional

`Adafruit_USBD_MIDI`'s constructor defaults to one cable, but on this core
`_n_cables` reads back as garbage by the time `setup()` runs -- 206 on the
board this was found on. `makeItfDesc()` sizes the interface descriptor
from it:

```
TUD_MIDI_DESC_HEAD_LEN(34) + TUD_MIDI_DESC_JACK_LEN(30) * n
                           + 2 * TUD_MIDI_DESC_EP_LEN(13 + n)
```

At `n = 206` that asks for 6652 bytes. `_desc_cfg_buffer` is 256 and CDC
has already taken 75, so `addInterface()` cannot fit it, returns 0, and
`Adafruit_USBD_MIDI::begin()` gives up -- **silently**, because
`MIDI.begin()` throws its return value away. The device enumerates with a
75-byte CDC-only descriptor and no USB MIDI, which looks exactly like a
host that never opened the port.

`usb_midi.setCables(1)` before `MIDI_USB.begin()` fixes it: writes to the
object do stick, so it is only the value from construction that is lost.
A constructor argument would not help -- whatever loses it happens before
`setup()` runs. With the cable count right the descriptor goes 75 -> 167
bytes and MIDIStreaming appears. The sketch no longer prints this; it
was dumped from `tud_descriptor_configuration_cb(0)` while this was being
chased, which is the way to check it again:

```
usb desc: 167 bytes, 4 interfaces
  itf 0: class 02 sub 02
  itf 1: class 0a sub 00
  itf 2: class 01 sub 01
  itf 3: class 01 sub 03  <- MIDIStreaming
```

### Why the field is lost: two TinyUSB layouts in one binary

It is the Seeed core, not this sketch. The stock `midi_test.ino` from the
Adafruit TinyUSB library, unmodified, enumerates USB MIDI on
`adafruit:nrf52:feather52840sense` and does not on
`Seeeduino:nrf52:xiaonRF52840`.

Seeed's `platform.txt` adds

```
-I{runtime.platform.path}/libraries/Adafruit_TinyUSB_Arduino/src/arduino
```

to every compilation unit, and `cores/nRF5/Arduino.h` does a bare
`#include "Adafruit_USBD_CDC.h"`, which resolves there -- to the core's
**bundled TinyUSB 1.7.0**, not the version being compiled. Both copies
guard with `ADAFRUIT_USBD_DEVICE_H_`, so the newer header is then skipped
in that unit, and the two definitions disagree:

| bundled TinyUSB | `STRING_DESCRIPTOR_MAX` | |
|-----------------|-------------------------|---|
| Seeed 1.1.13    | 1.7.0 | **8** |
| Adafruit 1.7.0  | 3.6.0 | 12 |
| standalone, the one actually compiled | 3.7.7 | 12 |

`_desc_str_arr[STRING_DESCRIPTOR_MAX]` makes `Adafruit_USBD_Device` 16
bytes smaller on one side than the other. The Adafruit core escapes it
only because the copy it bundles happens to agree.

**No core patching is required.** `setCables(1)` is the whole fix: the
store goes through a library method after the damage, so it does not
depend on anything in the user's Arduino installation. Anyone can clone
this repo and build it as-is.

Two layouts in one binary is a property of that core's build rather than
of this sketch, and this fixes the one symptom it produced here. The
exposure is narrow -- only private members differ, and everything outside
the library goes through methods compiled in the library's own unit --
but it is not nil. `setup()` prints the before/after descriptor length and
flags it when the interface was not added, so a recurrence shows up in a
serial capture instead of as a missing MIDI port.

### Re-enumerating when the host got there first

`MIDI_USB.begin()` adds an interface, which only edits the configuration
descriptor in RAM. If the host has already read that descriptor, the new
interface is invisible -- it does not appear until the device
re-enumerates.

On this core the host usually *has* already read it. `main()` starts the
USB stack before `setup()` runs, in a high-priority FreeRTOS task, and
`usb_hardware_init()` does not wait for a VBUS event when VBUS is already
live:

```c
// USB power may already be ready at this time -> no event generated
// We need to invoke the handler based on the status initially
if (usb_reg & POWER_USBREGSTATUS_VBUSDETECT_Msk) {
  tusb_hal_nrf_power_event(NRFX_POWER_USB_EVT_DETECTED);
}
```

Plugged in at boot -- the normal case when testing USB MIDI -- the device
attaches and enumerates with CDC only, before `setup()` gets a turn. So
`setup()` bounces the connection after adding the interface:

```c
if (TinyUSBDevice.mounted()) {
  TinyUSBDevice.detach();
  delay(10);
  TinyUSBDevice.attach();
}
```

This is the idiom the newer Adafruit TinyUSB examples use, for exactly
this reason; the example bundled with the Seeed core predates it. The
startup banner prints which way the race went, because a missing USB MIDI
interface otherwise looks identical to a host that never opened it.

TinyUSB's `tud_task()` runs in its own FreeRTOS task here, so `loop()`
services nothing.

## Shared headers

Five headers are copied into the sketches that use them --
`device_id_code.h`, `touch.h`, `touch_map.h`, `led_anim.h` and
`anim_wire.h`, 24 copies in all. **The copies must stay identical.**
`arduino/shared/` holds the only ones edited by hand; every copy in a
sketch folder is generated from it.

```sh
make -C arduino sync     # shared/ -> every sketch copy
make -C arduino check    # fail, naming names, if any copy has drifted
```

`arduino/tests` compiles against `shared/`, and `make run` there runs
`check` first: suites passing against `shared/` say nothing about a copy
that has drifted away from it.

Copying at all is deliberate, and `docs/touch-layer-plan.md` adopts it.
A sketch folder that carries its own headers is self-contained -- it
opens in the Arduino IDE with those headers as editable tabs, and zips
up and sends as-is. Nothing in the Arduino toolchain offers that *and* a
single copy:

- **`sketch.yaml` profiles** can name a local library by path (`dir:`,
  arduino-cli 1.3.0 and later), and would pin the platform and every
  library version besides. The Arduino IDE ignores `sketch.yaml`
  entirely, so the IDE and the CLI would build against different
  libraries.
- **Symlinks** compile fine under arduino-cli, but the IDE will not show
  or let you edit a symlinked `.h`, and git on Windows checks symlinks
  out as text files containing the path.
- **A real Arduino library** works in both, but only once it is
  installed into the sketchbook -- a per-machine step a fresh clone does
  not carry -- and the headers stop being tabs beside the `.ino`.

So the copies stay, and `make check` is what keeps them honest.

## Host tests

`arduino/tests/` builds the dependency-free headers natively and runs
assertions against them. A chunk-reassembly bug is miserable to diagnose
over the air and trivial to catch here.

```sh
cd arduino/tests
make run
```

Built with `-fsanitize=address,undefined`: an out-of-bounds write in the
reassembler is silent on the nRF52 and fatal here, which is most of the
value. `build/` is gitignored.

- **`wire_test.cpp`** -- `AnimMsg` size and the 31-byte advert budget,
  event packing, chunk arithmetic, reassembly in order / reversed /
  duplicated, losing any single chunk and redelivering it, two senders
  interleaved, partial expiry, malformed packets.
- **`player_test.cpp`** -- event timing, a single late update firing
  everything overdue, the fade, `stop()` and re-`start()`, `poke()`,
  out-of-range LED indices, and the `millis()` wrap.
- **`id_test.cpp`** -- `device_id_code.h`: every letter reachable at
  every position over 20000 synthetic IDs, colors agreeing with letters,
  both FICR words feeding the hash, `deviceIdName()` truncation.

`id_test.cpp` builds against `stubs/Arduino.h`, which supplies
`snprintf`, `size_t` and a writable fake `NRF_FICR`. The header under
test is unmodified -- it includes `<Arduino.h>` and must keep doing so,
since that is where `NRF_FICR` comes from. The cost is that these tests
do not prove the header compiles against the real `Arduino.h`; the
sketch builds cover that.

**Mutate the headers when you add a test.** Every one of these suites
had holes that only showed up by deliberately breaking the header and
re-running -- a test that still passes against broken code is not
testing anything. `tests/README.md` records what was found that way,
and the two mutants deliberately left uncovered.
