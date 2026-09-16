# sketchingpad -- Arduino sketches

Arduino firmware for the sketchingpad: a 20-pad capacitive touch surface
(TS20) with 20 side-view SK6812 LEDs, driven by a Seeed XIAO nRF52840
plugged into a `sketchingpad` board. (current proto: `sketchingpad_proto2`)

## Sketches

| Sketch                   | What it does                                |
|--------------------------|---------------------------------------------|
| `hwtest_leds`            | Chases one pixel around the 20 LEDs         |
| `hwtest_ts20`            | Touched pad -> LED, fading. Poll or INT mode |
| `ble_keyboard_simple`    | BLE HID keyboard, no power management        |
| `ble_keyboard_lowpower`  | BLE HID keyboard, three power tiers          |
| `ble_broadcast`          | Symmetric BLE broadcast messaging, no pairing |

`hwtest_leds` and `hwtest_ts20` are Arduino ports of
`circuitpython/hwtest/code_ledtest.py` and `code_test_ts20.py`.

## Board and libraries

Board: **Seeed XIAO nRF52840**, FQBN `Seeeduino:nrf52:xiaonRF52840`.

Libraries used:

- `TS20` -- todbot's TS20 Arduino library
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
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 \
  arduino/ble_keyboard_simple
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 \
  arduino/ble_keyboard_lowpower
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 arduino/ble_broadcast
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

## Low power concerns

## Low power

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
2. **Peripheral latency.** (slave_latency in ble_gap.h) On the ACTIVE -> IDLE transition the sketch asks
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

All three were added to `sketchingpad_stripproto1` and are no longer
"next rev" items. Sketches will still build without them, but the
matching tier degrades.

1. **P-FET high-side load switch on the LED rail** -- Q1 (AO3415A),
   source on `+3.3V`, drain feeding `3V3_LED`, gate on `LED_POWER` (D7)
   with R4 100k pulling up to the source so the rail is **off** by
   default. Highest-value change on the board; see the sections below
   for why that polarity and that part.
2. **SW1 pin 1 wired to D10/MOSI.** It used to float, so there was no
   button input at all. Now a wake source, and a press forces DEEP sleep.
3. **TS20 INT (U1 pin 19) brought to D3/A3.** Previously it only
   reached its 10k pull-up R1 and dead-ended. This is what makes
   interrupt-driven scanning and touch-to-wake possible; without it the
   firmware had to poll, and `SYSTEM_OFF` was unreachable because
   nothing could wake it.

### Prior art for the LED power switch

This is a solved problem in the nRF52840 wireless-keyboard world; both
of the common controller boards do exactly this, for exactly this
reason ("addressable RGB LEDs draw up to 1 mA each when off").

**[nRFMicro](https://github.com/joric/nrfmicro)** -- closest reference,
and the schematic is KiCad source you can open:
[`hardware/nrfmicro.kicad_sch`](https://github.com/joric/nrfmicro/blob/main/hardware/nrfmicro.kicad_sch).
Q2 is an **AO3407** P-channel MOSFET in SOT-23 (30 V, 4.1 A), source on
the 3V3 rail (`nRF_VDD`), drain feeding the switched `EXT_VCC` bus, gate
driven by `POWER_PIN`. R9 is a 2M resistor from that gate to GND -- a
pull-*down*, traced from the schematic source -- so the rail is ON by
default and the MCU drives the gate high to cut it. That matters at
boot: the LEDs come up powered before firmware runs, rather than staying
dark on a board whose firmware has not started.

- [Pinout](https://github.com/joric/nrfmicro/wiki/Pinout) -- "P1.09 -
  POWER_PIN (EXT_VCC control since 1.0, P0.13 on nice!nano) (1-off,
  0-on)"
- [Components](https://github.com/joric/nrfmicro/wiki/Components) --
  part choice and the gate-resistor tradeoff: "There's a tradeoff in
  battery leak (I = 3.3V/2M = ~1.65 uA) vs mosfet opening time (t =
  3nC/3.3V*2M = ~1800uS)". Also suggests an **LDO with an enable pin**
  as an alternative -- "you get both regulated output and a trigger
  without pull-up resistors" -- which may suit us better, since the LED
  rail wants a regulated 3V3 anyway.

**[nice!nano](https://nicekeyboards.com/docs/nice-nano/pinout-schematic/)**
-- same circuit, different pin: "P0.13 on VCC shuts off the power to VCC
when you set it to high. This saves on battery immensely for LEDs of all
kinds that eat power even when off."

**[ZMK](https://zmk.dev/docs/behaviors/power)** -- the firmware side of
the same pattern, if you want precedent for how it gets exposed:
`&ext_power` with `EP_ON` / `EP_OFF` / `EP_TOG`, backed by a
`zmk,ext-power` devicetree node with a `control-gpios` property. See
also [power management config](https://zmk.dev/docs/config/power).

**[Adafruit MagTag](https://github.com/adafruit/Adafruit_MagTag_PCBs)**
-- same job, opposite gate polarity, and worth understanding before
copying either. From `Adafruit MagTag 2.9in Rev D.sch`: Q2 is an
**AO3415** P-FET, source on 3V3, drain feeding `V+` (the VDD of the four
WS2812Bs), gate on `NEOPOWER` = `IO21`, with **R15 = 100K from gate to
the source rail** -- a pull-*up*. So Vgs = 0 and the rail is **OFF** by
default; firmware drives IO21 low to enable. nRFMicro's 2M to GND is a
pull-*down*: rail **ON** by default, driven high to cut.

Both are correctly oriented high-side switches -- source on the supply
side, so the body diode blocks supply-to-load when off. Neither is
wrong; they differ in what happens when the MCU is *not* driving the
gate, and each put the resistor's current in the state where it is free:

| | nRFMicro | MagTag |
|---|---|---|
| Gate resistor | 2M to GND | 100K to source |
| Default (pin undriven) | rail ON | rail OFF |
| To change it | drive high | drive low |
| Resistor current | 0 default, 1.65 uA off | 0 off, 33 uA while LEDs lit |

nRFMicro is a Pro Micro drop-in, so `EXT_VCC` *is* the board's `VCC`
pin -- defaulting it off would leave any firmware that does not know
about `POWER_PIN` with a dead supply. Backward compatibility forces
default-ON, and the 2M keeps the resulting idle-state leak down to
1.65 uA (the Components wiki notes the tradeoff: a larger resistor leaks
less but transitions the gate more slowly). MagTag has no such
constraint and picks the safe default instead.

**For the sketchingpad, use the MagTag polarity: pull-up to the source
rail, default OFF, drive low to enable.** The reason is power-on and
reset, not sleep -- nRF52840 GPIOs retain their state through SYSTEM_ON
and through SYSTEM_OFF when configured, so the pin only floats before
firmware runs. With a pull-up, a board that is unprogrammed, mid-reset,
or crashed sits at ~0 mA instead of 10-20 mA. That is the entire reason
for adding the switch, so it should be the state the hardware falls back
to on its own.

**Counter-example worth knowing:** the [Adafruit QT Py
ESP32-S2](https://github.com/adafruit/Adafruit-QT-Py-ESP32-S2-PCB) skips
the FET entirely and wires its single NeoPixel's VDD straight to GPIO
`IO38` (net `NEOPIX_PWR` in the schematic). Fine for one LED at ~1 mA;
not an option for 20, which can pull well over an amp at full white.

### Which P-FET: AO3415, not AO3407

From the manufacturer datasheets (AO3415 Rev 7; AO3407):

| | AO3415 | AO3407 |
|---|---|---|
| VDS | -20 V | -30 V |
| VGS max | +/-8 V | +/-20 V |
| ID cont. | -4 A | -4.1 A |
| **VGS(th) max** | **-0.9 V** | **-2.4 V** |
| RDS(on) @ -4.5 V | 41 mOhm max | 87 mOhm max |
| RDS(on) @ -2.5 V | 53 mOhm max | *not specified* |
| RDS(on) @ -1.8 V | 65 mOhm max | *not specified* |
| Qg (total) | 9.3 nC typ @ -4.5 V | 4.6 nC typ @ -4.5 V |
| **Qgd (Miller)** | **2.2 nC typ** | **2.2 nC typ** |

We drive the gate from a 3.3 V GPIO, and that is what decides it.

The AO3407's threshold is **-2.4 V worst case**, leaving only 0.9 V of
overdrive at 3.3 V, and its RDS(on) is not characterised below -4.5 V --
so at our gate drive we would be operating in a region the manufacturer
does not guarantee. It works in practice (typical threshold is -1.9 V),
which is why nRFMicro gets away with it, but it is not a specified
operating point. The AO3415 is built for exactly this: threshold -0.9 V
worst case, characterised all the way down to -1.5 V, and its datasheet
says outright it is "suitable for use as a load switch applications"
with "operation with gate voltages as low as 1.8V".

The resistance difference matters more here than it looks. At 1.2 A the
AO3415 drops roughly 50 mV; the AO3407 at its -4.5 V spec point drops
over 100 mV, and more at 3.3 V drive. Our SK6812s already run on a 3.3 V
rail, which is low for them -- `issues.md` notes the backlit LEDs "do
not show blue very well", exactly the symptom of thin headroom, since
blue has the highest forward voltage. Do not spend another 100 mV on the
switch.

AO3407's one genuine advantage is half the total gate charge, which
matters for fast PWM switching. We switch this rail perhaps twice a
second, so it buys nothing -- and note `Qgd`, the Miller charge that
actually sets drain dV/dt during turn-on, is 2.2 nC typ for *both*
parts. On the parameter that governs inrush they are identical.

Its higher VDS and VGS ratings are likewise irrelevant at 3.3 V --
though note the AO3415's +/-8 V VGS limit means it cannot be reused
as-is on a 12 V rail.

### Gotchas for the LED rail switch

Kept because they explain why the circuit looks the way it does, and
because points 2 and 3 are firmware requirements, not board ones.

- **Size the FET for the LEDs, not the quiescent draw.** 20 SK6812s at
  full white is ~1.2 A, so size the part for that -- the AO3415A's 5 A
  rating covers it with room to spare.
- **Drive `LED_DAT` low when the rail is off.** With D6 still driving
  high into an unpowered SK6812, current sneaks in through the input
  protection diode and partially powers the strip -- which defeats the
  switch. Set the data pin low (or to an input) before cutting power.
- **Sequence the other way on startup:** enable the rail, let it settle,
  *then* drive data. A `show()` into a rail that is still coming up
  back-powers it through that same diode.
- **Add a series gate resistor for inrush -- the pull-up will not do
  it.** The LED rail carries 21x 100n, ~2 uF, and switching that on hard
  can sag 3V3 enough to reset the XIAO. The 100K pull-up does not help:
  the MCU actively sinks the gate to turn on, so turn-on is fast whatever
  the pull-up is, and the pull-up only slows turn-*off*, and only if the
  pin is released to high-Z instead of driven high. What limits inrush is
  a *separate* resistor in series between the GPIO and the gate (1-10k),
  slowing dV/dt in both directions. MagTag has no series resistor -- it
  only switches four LEDs. Note power-up is not a separate inrush event:
  the pin is an input, the pull-up holds the FET off, and nothing
  switches until firmware drives the gate low.
- **Check the gate drive, not just the current rating.** Confirm the
  part's Rds(on) is specified at a gate drive of 2.5 V or less, so 3.3 V
  puts it well into enhancement.
