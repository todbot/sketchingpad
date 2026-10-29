# sketchingpad -- Arduino sketches

Arduino firmware for the sketchingpad: a 20-pad capacitive touch surface
with 20 side-view SK6812 LEDs, driven by a Seeed XIAO nRF52840 plugged
into a `sketchingpad` board. There are two board variants: 
`sketchingpad1` has one TS20 touch chip, `sketchingpad1_mpr` has two MPR121 touch chips.
The Arduino sketches work with either board.
See `docs/hardware-notes.md` for detailed info about both chips. 

<!--ts-->
* [Sketches](#sketches)
  * [Installing pre-built sketches](#installing-pre-built-sketches)
* [Building sketches](#building-sketches)
  * [Arduino core and libraries needed](#arduino-core-and-libraries-needed)
  * [Installing prerequisites with arduino-cli](#installing-prerequisites-with-arduino-cli)
  * [Build and upload](#build-and-upload)
* [Building firmware releases](#building-firmware-releases)
  * [Building web flasher releases](#building-web-flasher-releases)
* [Sketchingpad libraries](#sketchingpad-libraries)
  * [touch.h: touch abstraction layer](#touchh-touch-abstraction-layer)
    * [touchArmWake() INT differences](#toucharmwake-int-differences)
  * [device_id_code.h: telling boards apart](#device_id_codeh-telling-boards-apart)
  * [led_anim.h and anim_wire.h: anim layer](#led_animh-and-anim_wireh-anim-layer)
    * [Anim wire format](#anim-wire-format)
    * [Notes about anim code:](#notes-about-anim-code)
* [Sketch details](#sketch-details)
  * [ble_broadcast_anim: animations over advertising](#ble_broadcast_anim-animations-over-advertising)
  * [sketchingpad_party: ble_broadcast_anim, with power tiers](#sketchingpad_party-ble_broadcast_anim-with-power-tiers)
  * [ble_keyboard: the keyboard, with power tiers](#ble_keyboard-the-keyboard-with-power-tiers)
  * [ble_midi_simple: MIDI over BLE](#ble_midi_simple-midi-over-ble)
    * [Pad map](#pad-map)
    * [Stuck notes](#stuck-notes)
  * [sketchingpad_midi_simple: BLE/USB MIDI controller](#sketchingpad_midi_simple-bleusb-midi-controller)
    * [BLE and USB readiness tests](#ble-and-usb-readiness-tests)
    * [Transport resync](#transport-resync)
    * [usb_midi.setCables(1) is not optional](#usb_midisetcables1-is-not-optional)
    * [Seeed Arduino core has outdated USB libraries](#seeed-arduino-core-has-outdated-usb-libraries)
    * [Re-enumerating to get updated USB descriptors](#re-enumerating-to-get-updated-usb-descriptors)
  * [sketchingpad_gamepad: BLE/USB gamepad](#sketchingpad_gamepad-bleusb-gamepad)
* [Shared headers](#shared-headers)
* [Host tests](#host-tests)
<!--te-->

## Sketches


| Sketch                            | What it does                        |
|-----------------------------------|-------------------------------------|
| `hwtest_leds`                     | Chases one pixel around the 20 LEDs |
| `hwtest_touch`                    | Pad->LED, fading, either sensor     |
| `hwtest_touch_lowpower`           | Pad->LED, fading, power tiers, either sensor |
| `ble_broadcast`                   | Symmetric BLE broadcast, no pairing |
| `ble_broadcast_anim`              | Record an LED animation, broadcast  |
| `sketchingpad_party`              | Record an LED animation, with the power tiers |
| `ble_keyboard_simple`             | BLE HID keyboard, no power tiers    |
| `ble_keyboard`                    | BLE HID keyboard, with power tiers  |
| `ble_midi_simple`                 | BLE MIDI controller, no power tiers |
| `sketchingpad_midi_simple`        | USB + BLE MIDI                      |
| `sketchingpad_gamepad`            | USB + BLE HID gamepad               |

Sensor-specific sketches live in `ts20/` and `mpr/`. 
They are older and shouldn't be used.

`arduino/tests/` holds host-side tests for the dependency-free headers;
see the last section.

### Installing pre-built sketches

There are [pre-built .zip files for released sketches](https://github.com/todbot/sketchingpad/releases).

The easiest install is the
[sketchingpad flasher](https://todbot.github.io/sketchingpad/): pick a
sketch, click Flash. It needs Web Serial (Chrome, Firefox, Edge). The first
flash asks for the serial port twice, once for the running sketch and
once for the bootloader it resets into.

Or use the CLI tool `adafruit-nrfutil`:

```sh
pip3 install adafruit-nrfutil
adafruit-nrfutil dfu serial --touch 1200 -pkg hwtest_touch.zip \
  -p /dev/cu.usbmodemXXXX -b 115200 --singlebank
```
where `/dev/cu.usbmodemXXXX` is the name of your computer's  serial port with 
an awake (not deep sleep) sketchingpad board is plugged in. 
Flashing takes about 12 seconds.


## Building sketches

If you want to compile the sketches yourself, here's what you need. 
Many sketches have a DEBUG flag that, if enabled, print a bunch of useful
stuff to serial.

### Arduino core and libraries needed

Sketches are designed for the **Seeed XIAO nRF52840** (FQBN `Seeeduino:nrf52:xiaonRF52840`), 
but the PCB can  work with any XIAO/QTpy-format board. 

Libraries needed:

- `TS20` -- todbot's TS20 touch library
- `Adafruit MPR121` -- Adafruit MPR121 touch library
- `Adafruit NeoPixel` -- Adafruit easy neopixel library
- `Adafruit TinyUSB` -- Adafruit Arduino wrapper on TinyUSB
- `MIDI Library` -- FortySevenEffects MIDI library, for the two MIDI sketches only

`Adafruit NeoPixel` takes the nRF52 PWM + EasyDMA path, thus `show()` does 
not disable interrupts and will not disturb BLE radio events)

`Adafruit TinyUSB` overrides ([mostly](#usb_midisetcables1-is-not-optional)) 
the very old one shipped in the Seeed nRF52 core (1.7.0 vs 3.7.7).

The Seeed nRF52 core is a fork of Adafruit's nRF52 core: 
ships with S140 SoftDevice, FreeRTOS, TinyUSB, and the Bluefruit library
(`bluefruit.h`, `BLEHidAdafruit`, `BLEDis`). No extra BLE library needed.

### Installing prerequisites with arduino-cli

[`arduino-cli`](https://docs.arduino.cc/arduino-cli/installation/)
is very cool and highly recommended.  You can use it to install the core, all
needed libraries, and compile & upload the sketch.

```sh
arduino-cli config add board_manager.additional_urls \
     https://files.seeedstudio.com/arduino/package_seeeduino_boards_index.json
arduino-cli core update-index
arduino-cli core install Seeeduino:nrf52

arduino-cli update
arduino-cli lib install "TS20" \
                        "Adafruit MPR121" \
                        "Adafruit NeoPixel" \
                        "Adafruit TinyUSB Library" \
                        "MIDI Library" 
```

In the Arduino IDE: add the same URL under *Settings > Additional boards
manager URLs*, install "Seeed nRF52 Boards" from the Boards Manager, and
the libraries above from the Library Manager.

### Build and upload

```sh
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 arduino/hwtest_leds
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 arduino/hwtest_touch
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 arduino/sketchingpad_midi_simple
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 arduino/sketchingpad_gamepad
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 arduino/ble_keyboard_simple
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 arduino/ble_broadcast_anim
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 arduino/sketchingpad_party
```

Start with `hwtest_touch` to see if board works. 
Fall back to `hwtest_leds` to just see if LEDs work.

Then try `ble_keyboard_simple` when bringing up a board or changing the
key map: it polls the touch chip(s), sends a report on change, lights the LEDs,
and does nothing else.

To upload a sketch:

```sh
arduino-cli board list           # find the port
arduino-cli upload -b Seeeduino:nrf52:xiaonRF52840 \
  -p /dev/cu.usbmodemXXXX arduino/hwtest_touch \
  --upload-property upload.wait_for_upload_port=false
```

The last line skips a wait of several seconds: the bootloader comes back
on the same port name (on MacOS), so arduino-cli never sees the new port it waits
for. Or from `arduino/`, `make flash hwtest_touch` does all of this.

## Building firmware releases

`make -C arduino release` builds every sketch except the older `ts20/` and
`mpr/` ones into `arduino/release/<sketch>.zip`, 
a DFU package. Flashing one needs `adafruit-nrfutil` and a 
[release zip](https://github.com/todbot/sketchingpad/releases).
No core or libraries need to be installed. 

```sh
pip3 install adafruit-nrfutil
adafruit-nrfutil dfu serial --touch 1200 -pkg sketchingpad_party.zip \
  -p /dev/cu.usbmodemXXXX -b 115200 --singlebank
```

`--touch 1200` puts XIAO into DFU bootloader the same way `arduino-cli upload`
does, no double-tap reset needed. 

- One package per sketch covers both board revs: `touch.h` picks the
  sensor at runtime.
- The board has to be awake. In DEEP (`SYSTEM_OFF`) there is no USB to
  touch, so wake it with a pad or the button first.
- Packages require the S140 7.3.0 SoftDevice, which stock XIAOs ship
  with.
- Sketches build as committed, `DEBUG` settings included.

### Building web flasher releases

The sketchingpad flasher page in `web/index.html` is built on
[nrfutil-web](https://github.com/takkaO/nrfutil-web). Publishing a
github release runs `.github/workflows/pages.yml`. This builds every
sketch at the release's tag, plus CircuitPython as a DFU zip (see
`circuitpython/README.md`), attaches the zips to the release (replacing
any of the same name), and copies them onto the site (due to CORS).
Running the workflow by hand redeploys the latest release without
building.

To try it locally: `make -C ../circuitpython release` for the
CircuitPython zip if wanted, `make release web` in `arduino/`, then
`python3 -m http.server -d ../web` and open `http://localhost:8000`.


## Sketchingpad libraries

### `touch.h`: touch abstraction layer

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

**Boards identify themselves at runtime based on touch chip.** The TS20 is at I2C addr `0x6A` 
and the MPR121s at `0x5A`/`0x5B`, so `touchBegin()` probes and picks. One
binary runs on both and nothing needs reflashing to swap boards on the
bench. `touchName()` prints `TS20` or `2x MPR121` in the banner.

**We think in LED-index space.** Bit *n* of `touchRead()` is LED
*n*, which is TP*(n+1)* on the silkscreen, on both boards. Sketches
write `(touches >> n) & 1` and carry no `led_to_pad` of their own -- both
tables live in `touch_map.h`, one per board type (ts20 vs mpr). 
Selecting a range of pads by index becomes correct by construction.

`touchRead()` hides the MPR121's two transactions and the bounded
re-read-while-low loop.

`touchReadPasses()` return value: > 1 means a chip re-asserted mid-read, 
and it is always 1 on the TS20.

`touchSetLowPower()` is a set-once call, not a per-tier toggle. Both
backends soft-reset on that write, which restarts calibration and would
swallow a touch in progress.

#### `touchArmWake()` INT differences

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

### `device_id_code.h`: telling boards apart

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


### `led_anim.h` and `anim_wire.h`: anim layer

`led_anim.h` is the non-blocking player: `load()`, `start()`,
`update(now_ms)` returning true when the frame changed, `pixel(i)`, and
`poke()` for live touch feedback. `anim_wire.h` is the packing and the
reassembler. Both depend on nothing but `<stdint.h>` -- no NeoPixel, no
BLE -- so they drop into any sketch and run on a host. They are copied
per sketch, the same convention as `device_id_code.h`; see Shared
headers below for which copy to edit.

#### Anim wire format

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
entire reliability mechanism. Chunks go out at a 20 ms advertising
interval, about 10 events per chunk, as non-connectable non-scannable
adverts, so no airtime goes on listening for requests nobody sends.

A board advertises only while sending. Every advert a peer hears pauses
that peer's scanner until `scan_callback` calls `resume()`
(`BLEScanner.cpp` defers the callback to a task), so with tens of boards
idle adverts would cost each receiver listening time and carry nothing.

The sender stops scanning for the length of a send. A 100% scan duty
cycle takes the radio from the board's own advertiser: on two
sketchingpad1_mpr boards, each chunk reached the other board only in
the first ~70 ms of its 200 ms, and idle adverts only 20-45% of the
time. `SCAN_WINDOW` is raised to a 100% receive
duty cycle, unlike `ble_broadcast`: a missed chunk costs a whole
animation. Halve it to get `ble_broadcast`'s power behaviour back.

#### Notes about anim code:

- **`isNewMessage()` from `ble_broadcast` cannot be used here.** It is
  1-deep per peer and treats any differing sequence number as new, so it
  would drop exactly the chunk repeats that reliability depends on. The
  peer table instead holds `last_played_anim`.
- **`scan_callback()` must not touch the LEDs.** It runs on the
  SoftDevice event task, and NeoPixel `show()` blocks with interrupts
  disabled for over a millisecond across 20 pixels -- which would make
  us miss the advertisements we are collecting. It parks packets in a
  16-deep ring; `loop()` drains and renders. Getting this wrong looks
  like a flaky radio, not a timing bug.
- **A sender never sees its own broadcast** (`scan_callback` filters on
  `device_id`), so local playback is kicked off from the send path.



## Sketch details

### `ble_broadcast_anim`: animations over advertising

`ble_broadcast_anim` runs on either sensor over the shared touch layer.

Records a short LED animation by touch and broadcasts it.
Every other board in range plays it. 
No pairing, no connections, any number of participants, all running this same firmware. 
The transport is `ble_broadcast`. The led_anim.h animation layer is new.

Touch pads 1-16 to record (the bottom row)-- the first touch starts it, 
~1.5 s of silence ends it, and then it plays locally *and* goes out over the air.
The top row picks the color for everything recorded after it; touching
the already-selected swatch again reaches its second variant, so four
pads cover all eight palette entries. 

`#define DEBUG (1)` at the top of the sketch narrates everything to
Serial, each line stamped with `millis()`:
- touches and recorded events;
- each send step;
- received chunks;
- rail on/off;
- playback start and end, with frame and lit-pixel counts;
- the reset reason at boot.

`DEBUG (0)` prints nothing and skips the 3 s wait for USB at boot. The
one exception is the fatal "No touch sensor" line.

### `sketchingpad_party`: ble_broadcast_anim, with power tiers

Originally named `ble_broadcast_anim_lp`.

Same game and same packets as `ble_broadcast_anim`, plus the techniques
from `hwtest_touch_lowpower`:
- touch INT and a semaphore instead of polling;
- the LED rail gated off in IDLE;
- frames only while something moves;
- `touchSetLowPower()` and DC/DC;
- Serial only with USB attached;
- `SYSTEM_OFF` woken by touch or the button.

| Tier   | Entered                                  | Scan          | LED rail |
|--------|------------------------------------------|---------------|----------|
| ACTIVE | touch, or an animation played            | 100%          | on       |
| IDLE   | 10 s with neither                        | 30 ms per 1 s | off      |
| DEEP   | 5 min no touch, none received; or button | none          | off      |

The scanner is the new problem. Unlike the keyboard, a broadcast
receiver cannot park on a latency-20 connection: it has to listen, and
100% scanning is ~5-6 mA (datasheet, 1M PHY). IDLE sniffs instead, at a
3% duty cycle, so roughly 0.2 mA by the same figure. Not measured yet.

Sniffing only works because a sender cycles its chunks for
`PREAMBLE_MS` (1.2 s) before its counted passes, longer than the 1 s
sniff interval. Any sniff window lands in it, hearing a chunk switches
the scanner to 100% for `RX_HOLD_MS`, and the next cycle completes the
animation. An ACTIVE receiver doesn't wait out the preamble: it has
every chunk after one cycle, 200 ms per chunk. The packet format is
unchanged, so party and plain boards play together. The one gap: a
plain `ble_broadcast_anim` sender has no preamble, so an IDLE party
board can miss it. An ACTIVE one will not.

White (the green swatch, touched twice) plays as a rainbow: each LED
takes the hue of the moment it lights and keeps it as it fades, so a
stroke leaves a rainbow trail. It is still white on the wire, so a
plain `ble_broadcast_anim` board shows it as white.

DEEP waits on received animations as well as touches, so a board in a
busy group stays awake to hear it. Once in DEEP it hears nothing until
touched.

`touchSetLowPower(true)` costs recording resolution on the MPR121:
CONFIG2 0x24 updates touch status every ~64 ms, not ~4 ms, so fast rolls
record coarser than the 10 ms `dt` can carry. Staying at 0x20 costs
~0.7 mA for the two chips, more than the whole IDLE sniff budget.

### `ble_keyboard`: the keyboard, with power tiers

Originally named `ble_keyboard_lowpower`.

`ble_keyboard_simple`'s key map and ID code, with the tiers of
`ble_keyboard_lowpower_ts20`, on either sensor. Same techniques as
`sketchingpad_party`, but a keyboard keeps its connection, so IDLE
saves power with peripheral latency instead of a sniffing scanner.

| Tier   | Entered                  | Connection           | LED rail |
|--------|--------------------------|----------------------|----------|
| ACTIVE | touch                    | latency 0            | on       |
| IDLE   | 10 s untouched           | latency 20 (~630 ms) | off      |
| DEEP   | 5 min untouched; button  | dropped, SYSTEM_OFF  | off      |

The latency request goes through `BLEConnection`, and the central may
refuse it. Waking from DEEP resets the board and it has to reconnect,
so the waking tap types nothing; a pad still held once it reconnects
does.

### `ble_midi_simple`: MIDI over BLE

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

#### Pad map

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
pad-numbering sections in `docs/hardware-notes.md`. `led_to_note` and
the four `PAD_*` constants are indexed by LED, which is TP(n+1), so they
read straight off the silkscreen.

#### Stuck notes

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

### `sketchingpad_midi_simple`: BLE/USB MIDI controller

Like `ble_midi_simple` but with USB MIDI. The pad map, the
note table and the control pairs are identical. Both USB & BLE
transports run at once and neither is required. 
Every message goes to whichever transports are up at that instant.

Two MIDI library instances: 
`MIDI_USB` over TinyUSB's `Adafruit_USBD_MIDI` and `MIDI_BLE` over the
core's `BLEMidi`. `MIDI_CREATE_BLE_INSTANCE()` is not used because it
hardcodes the instance name to `MIDI`. Four small senders
(`midiNoteOn()` and friends) fan out and hold all the gating, so the pad
code never learns how many transports there are.

On macOS the two transports are found in different places. USB MIDI
appears wherever the host lists MIDI ports. BLE MIDI does **not** appear
in System Settings -> Bluetooth the way the HID keyboard sketches do; it
is paired from Audio MIDI Setup -> MIDI Studio -> Bluetooth. A generic
BLE scanner sees it too, though the name is in the scan response rather
than the advertising packet, so a scanner may show the device before it
shows the name.

Run on a TS20 board, which is the only rev assembled: USB MIDI
enumerates and sends, and BLE MIDI advertises and pairs. Not yet run on
`sketchingpad1_mpr`. Compiles clean under `--warnings all`.

#### BLE and USB readiness tests

BLE knows the host subscribed (`blemidi.notifyEnabled()`). USB only
knows the host enumerated the device (`TinyUSBDevice.mounted()`) --
`Adafruit_USBD_MIDI` exposes no per-endpoint state, so there is nothing
better to ask. A write to an endpoint nobody opened is dropped rather
than blocking, so the weaker test costs nothing; it just cannot promise
anyone is listening.

#### Transport resync

`ble_midi_simple` handles a reconnect by zeroing `last_touches`, which
replays every held pad as a fresh note-on. With two transports that
would re-send held notes to the transport that was already playing them,
so this sketch aims the catch-up at the one that just arrived:
`resyncHost()` sends it all-notes-off, then the current bend, mod and
held notes, and leaves `last_touches` alone.

#### `usb_midi.setCables(1)` is not optional

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

#### Seeed Arduino core has outdated USB libraries

The Seeed Arduino core ships with 1.7.0 of Adafruit_TinyUSB library.
The current version is 3.7.7.  I submitted a PR on Seeed's core to update it.

The stock `midi_test.ino` from the Adafruit TinyUSB library, 
unmodified, enumerates USB MIDI on `adafruit:nrf52:feather52840sense` 
but does not on `Seeeduino:nrf52:xiaonRF52840`. 
Adding `usb_midi.setCables(1)` as its only change makes it enumerate on the 
Seeed core as well.

Like the upstream Adafruit core, Seeed's `platform.txt` adds

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

The proper fix is to update the Seeed Arduino core, but as a workaround, 
`setCables(1)` works. No core patching required and anyone can clone
this repo and build it as-is.

#### Re-enumerating to get updated USB descriptors

`MIDI_USB.begin()` adds an interface, which only edits the configuration
descriptor in RAM. If the host has already read that descriptor, the new
interface is invisible -- it does not appear until the device
re-enumerates.

To force re-enumation, add this to `setup()`:

```c
if (TinyUSBDevice.mounted()) {
  TinyUSBDevice.detach();
  delay(10);
  TinyUSBDevice.attach();
}
```

### `sketchingpad_gamepad`: BLE/USB gamepad

A HID gamepad over USB and BLE at once, built the same way as
`sketchingpad_midi_simple`: neither transport is required, and every
report goes to whichever ones are up. A host that has both sees two
controllers.

```
            TP17 TP18 TP19 TP20             b11 b12 b13 b14
  TP1  TP2  TP3  TP4   TP5  TP6  TP7  TP8    b1   ^   ^  b2   b3 b4 b5 b6
  TP9 TP10 TP11 TP12  TP13 TP14 TP15 TP16    <    v   v   >   b7 b8 b9 b10
```

The left block is a D-pad driving the hat, with two pads each for up
and down. Opposite directions cancel, and adjacent ones give the
diagonals. The other 14 pads are buttons 1-14, set in `led_to_button`.
The six axes are always 0.

Both transports send TinyUSB's `hid_gamepad_report_t` from
`TUD_HID_REPORT_DESC_GAMEPAD`, which is identical in the core's bundled
TinyUSB and in 3.7.7. USB uses `Adafruit_USBD_HID` and BLE uses the
core's `BLEHidGamepad`.

A gamepad report is the whole state, not an event, so there are no
stuck notes and no resync messages. Each transport keeps a copy of the
last report it accepted, and it is sent the current report whenever the
two differ. So a send refused for a busy endpoint, or for a BLE host
that has not subscribed yet, retries on the next loop. A transport that
comes back gets caught up the same way.

`usb_hid` is default-constructed, and every field is set in `setup()`
before `begin()`. That is the same defence as
[`setCables(1)`](#usb_midisetcables1-is-not-optional): a garbage
constructor value makes `begin()` fail silently. The banner prints
whether `begin()` succeeded.

BLE pairs from the OS Bluetooth settings, like the keyboard. To check
the hat and each button, use a browser Gamepad API tester such as
gamepadtester.com, or `joy.cpl` on Windows.

Not yet run on hardware, on either board rev. It compiles clean under
`--warnings all`.

## Shared headers

Five headers are copied into the sketches that use them --
`device_id_code.h`, `touch.h`, `touch_map.h`, `led_anim.h` and
`anim_wire.h`, 29 copies in all. **The copies must stay identical.**
`arduino/shared/` holds the only ones edited by hand; every copy in a
sketch folder is generated from it.

```sh
make -C arduino sync     # shared/ -> every sketch copy
make -C arduino check    # fail, naming names, if any copy has drifted
```

`arduino/tests` compiles against `shared/`, and `make run` there runs
`check` first: suites passing against `shared/` say nothing about a copy
that has drifted away from it.

Copying at all is deliberate. A sketch folder that carries its own
headers is self-contained -- it opens in the Arduino IDE with those
headers as editable tabs, and zips up and sends as-is. Nothing in the
Arduino toolchain offers that *and* a single copy:

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
