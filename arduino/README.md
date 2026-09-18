# sketchingpad -- Arduino sketches

Arduino firmware for the sketchingpad: a 20-pad capacitive touch surface
with 20 side-view SK6812 LEDs, driven by a Seeed XIAO nRF52840 plugged
into a `sketchingpad` board. Touch is a TS20 on every board up to
`sketchingpad1`, and two MPR121s on `sketchingpad1_mpr` -- see the two
sensor sections below.

## Sketches

| Sketch                   | What it does                                |
|--------------------------|---------------------------------------------|
| `hwtest_leds`            | Chases one pixel around the 20 LEDs         |
| `hwtest_ts20`            | Touched pad -> LED, fading. Poll or INT mode |
| `hwtest_ts20_lowpower`   | Touched pad -> LED, three power tiers        |
| `hwtest_mpr`             | Touched pad -> LED, on the 2x MPR121 board    |
| `ble_keyboard_simple`    | BLE HID keyboard, no power management        |
| `ble_keyboard_lowpower`  | BLE HID keyboard, three power tiers          |
| `ble_broadcast`          | Symmetric BLE broadcast messaging, no pairing |
| `ble_broadcast_anim`     | Record an LED animation by touch, broadcast it |

`hwtest_leds` and `hwtest_ts20` are Arduino ports of
`circuitpython/hwtest/code_ledtest.py` and `code_test_ts20.py`.

`arduino/tests/` holds host-side tests for the dependency-free headers;
see the last section.

## Board and libraries

Board: **Seeed XIAO nRF52840**, FQBN `Seeeduino:nrf52:xiaonRF52840`.

Libraries used:

- `TS20` -- todbot's TS20 Arduino library
- `Adafruit MPR121` -- for `sketchingpad1_mpr` only; pulls in
  `Adafruit BusIO`
- `Adafruit NeoPixel` -- Adafruit easy neopixel library

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
| `PWM_AUDIO` | D0 / A0  | net at the header only        |
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
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 arduino/hwtest_ts20
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 arduino/hwtest_mpr
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 \
  arduino/ble_keyboard_simple
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 \
  arduino/ble_keyboard_lowpower
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 \
  arduino/hwtest_ts20_lowpower
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 arduino/ble_broadcast
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 \
  arduino/ble_broadcast_anim
```

Start from `ble_keyboard_simple` when bringing up a board or changing the
key map: it polls the TS20, sends a report on change, lights the LEDs,
and does nothing else. `ble_keyboard_lowpower` is the same keyboard with
interrupt-driven scanning, connection-parameter management and sleep.

Note that even `ble_keyboard_simple` has to drive `LED_POWER` (D7) low in
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

## Telling boards apart: `device_id_code.h`

Two boards running identical firmware are otherwise indistinguishable.
`device_id_code.h` hashes the chip's factory `NRF_FICR->DEVICEID` (FNV-1a
over all 8 bytes) and slices it 2 bits at a time into five letters drawn
from `R`/`Y`/`G`/`B`/`M` (red, amber, green, blue, magenta) -- e.g.
`RMGGB`. Two sketches use it:

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

**The file is deliberately duplicated, not shared.** Arduino cannot share
a header between sketch folders without making it a library, and the
point of this one is that it drops into any nRF52 sketch with no install
step: it is header-only and pulls in nothing but the core. Copy it in and
`#include "device_id_code.h"`. The copies must stay identical:

```sh
diff arduino/ble_keyboard_simple/device_id_code.h \
     arduino/ble_broadcast/device_id_code.h
```



## TS20 capsense touch chip

### Pad numbering is not TS20 channel order

The `TPn` refdes silkscreened on the board is **not** the index the
firmware uses. The firmware indexes everything by TS20 channel, and on
proto2 those two numberings disagree:

```
physical layout, TP refdes        the same pads, by TS20 channel
       17 18 19 20                        4  5  6  7
  1  2  3  4  5  6  7  8            0  1  2  3   8  9 10 11
  9 10 11 12 13 14 15 16           16 17 18 19  12 13 14 15
```

TP1-4 are channels 0-3, but TP17-20 are channels 4-7 -- so the top row
sits in the *middle* of the channel range, not at the end. Anything that
selects a range of pads by index has to account for that; assuming the top
row was the top of the range is what put `ble_broadcast_anim`'s color
swatches on the wrong four pads.

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
  because it is characterising the line; `ble_keyboard_lowpower` uses
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
`hwtest_mpr` is the bring-up sketch; it is `hwtest_ts20` with the
sensor swapped and diagnostics added.

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
       17 18 19 20                       10 11  0  1
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

Which is also why the sketch prints a `chatter:` line, counting IRQ
assertions whose read turned up no change in `touches`. Masked chatter is
otherwise invisible -- the edge counter climbs and nothing else moves. On
an untouched board that counter should stay flat; if it climbs, the 0xFF
thresholds did not take.

With autoconfig enabled this matters more, not less. Those four will
likely report Out Of Range, and `ARE` (auto-reconfiguration) then retries
an OOR channel on *every sampling interval*, indefinitely.

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

### The MPR121 IRQ line: not yet characterised

**Unmeasured, as of 2026-09-17.** The MPR121 datasheet says only that
IRQ is "triggered any time a touch or release is detected" and stops
there. AN3895, which the datasheet cites for the serial interface, is
purely the I2C protocol -- addressing, START/STOP, auto-increment reads
-- and never mentions IRQ. So whether it is a level held until the touch
status registers are read, as it is widely reported to be, is not
established by anything in `docs/`.

It matters, because a held level would remove the `SYSTEM_OFF` arming
race the TS20's ~1 ms pulse creates (see above): `SENSE_LOW` would latch
on its own, and the only rule would be to read both chips immediately
before arming so a stale assertion does not wake the board instantly.

`hwtest_mpr` settles it the same way `hwtest_ts20` settled the TS20's --
empirically. It prints the INT pin level immediately before and after
the status read as `int=N->N`, alongside the same edge counter, 500 ms
watchdog and `INT MISSED` check.

One consequence is already certain, whichever way that goes. Two chips
on one open-collector line means a chip can re-assert in the gap between
the two status reads, leaving the line low with no new edge behind it --
and nothing would ever read it again. So the read chases the level down
rather than reading once, bounded to `MAX_INT_PASSES` because a chip
stuck low would otherwise hang `loop()`.

## Low power

Two sketches implement the power tiers. 
`ble_keyboard_lowpower` uses BLE SoftDevice `sd_power_system_off()`
The simpler `hwtest_ts20_lowpower` uses `NRF_POWER->SYSTEM_OFF=1` 
and is simpler to put a power meter on, since BLE can dominate power consumption.

`ble_keyboard_lowpower` runs three tiers:

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

### `hwtest_ts20_lowpower`: the tiers without a radio

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

One thing this sketch does better than `ble_keyboard_lowpower`: the
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
per sketch, the same convention as `device_id_code.h`.

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
