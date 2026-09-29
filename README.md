
# SketchingPad


<img src="./docs/sketchingpad1-render.jpg" width=350>


## Features

* 20-pad capacitive touch surface with 20 side-view SK6812
  addressable LEDs backlighting the touchpads
* Driven by a XIAO/QTPy board, specifically XIAO nRF52840 BLE-capable w/ LiPo charging
* LiPo batery jack with power switch
* Tact switch to switch modes, wake up, or go into deep sleep
* Audio out jack for synth fun

## Uses

* Broadcast animations fun toy
* BLE/USB HID keyboard macropad
* BLE/USB MIDI controller


## Software

`arduino/README.md` is the reference for the firmware: pinouts, the
touch layer, the power tiers and the numbers measured on hardware.
The lists below are an index of what is in the tree.

`docs/hardware-notes.md` is the board side: both touch chips, their pad
numbering, their INT lines, and the board changes the firmware assumes.
`docs/` also holds `low-power-notes.md` and the touch-chip datasheets.

### Arduino

Sensor-agnostic, at the top of `arduino/`. 

* `hwtest_leds`           -- chases one pixel around the 20 LEDs
* `hwtest_touch`          -- pad -> LED with a fading trail
* `hwtest_touch_lowpower` -- same, plus three power tiers, no BLE
* `ble_broadcast`         -- symmetric BLE broadcast, no pairing
* `ble_broadcast_auto`    -- `ble_broadcast`, auto-sending a counter
* `ble_broadcast_anim`    -- record an animation, broadcast it
* `ble_keyboard_simple`   -- BLE HID keyboard, no power mgmt

more sketches in [arduino/README.md](./arduino/README.md).

`arduino/tests/` holds host-native tests for the dependency-free
headers: `cd arduino/tests && make run`.

### CircuitPython

Each program directory holds the files to copy to the CIRCUITPY root.

* `sketchingpad_midi_simple` -- USB + BLE MIDI controller
* `ble_keyboard_lowpower` -- low-power BLE HID keyboard, 
   two power tiers rather than the Arduino sketch's three. 

These work on both versions of the board (TS20 & MPR121). 
Other programs listed in [circuitpython/README.md](./circuitpython/README.md)

Host tests for the touch layer, needing no board and no install:
`cd circuitpython/tests && make run`. They also pin the pad tables in
`touch.py` against `arduino/shared/touch_map.h`, which nothing else does.

Libraries come from circup; see `circuitpython/requirements.txt`.
`circuitpython/notes-lowpower.md` covers why there are two power tiers and not three.


## Enclosure

As seen in the photos, there is a simple [3d-printable enclosure](./enclosure). 
It's fast to print and you can snap the PCB in to quickly get started,
or use M2.5 screws and nuts to secure the PCB more permanently.


## Pins used

* BUTTON     - D10/MOSI (SW1 to GND, needs internal pull-up)
* TOUCH_INT  - D3 / A3  (open collector, R1 pulls up)
* SDA        - D4       (Arduino Wire default)
* SCL        - D5       (Arduino Wire default)
* LED_DAT    - D6 / TX
* LED_POWER  - D7 / RX  (P-FET gate: LOW = rail on)


## Schematics

There are two versions of the board:

- `sketchingpad1` - uses one TS20 touch chip
- `sketchingpad1_mpr` - uses two MPR121 touch chips

KiCad source files available in the "schematics" directory. 

### sketchingpad1  (TS20)

[<img src="./docs/sketchingpad1-sch.png" width=700>](./schematics/sketchingpad1/sketchingpad1-sch.pdf)


### sketchingpad1-mpr  (2xMPR121)

[<img src="./docs/sketchingpad1_mpr-sch.png" width=700>](./schematics/sketchingpad1_mpr/sketchingpad1_mpr-sch.pdf)



