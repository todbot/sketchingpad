
# SketchingPad - 20-key BLE touch pad


<img src="./docs/sketchingpad1-render.jpg" width=350>


## Features

* 20-pad capacitive touch surface with 20 SK6812 addressable LEDs backlighting the touchpads
* LiPo battery jack with power switch
* Tact switch to switch modes, wake up, or go into deep sleep
* Audio out jack for audio fun
* Driven by any XIAO/QTPy board, but designed for XIAO nRF52840 BLE-capable w/ LiPo charging

Two Board variations because of supply chain madness:

- `sketchingpad1` -- TS20 touch chip
- `sketchingpad1_mpr` -- 2x MPR121 touch chips

Unless otherwise noted, software runs on both sketchingpad boards.


## Software

### Arduino

Most of the apps are Arduino sketches, see [`arduino/README.md`](./arduino/README.md) 
for details on Arduino firmware options including: how to flash, how to build, 
pinouts, the touch layer, the BLE LED animation packet layer, 
the power tiers, etc. Examples of what's available:

* `hwtest_leds`           -- chases one pixel around the 20 LEDs
* `hwtest_touch`          -- pad -> LED with a fading trail, no BLE
* `hwtest_touch_lowpower` -- pad -> LED, fading, plus three power tiers, no BLE
* `ble_broadcast_anim`    -- record an animation, broadcast it
* `ble_broadcast_anim_lp`  -- record an animation, broadcast it, with power tiers
* `ble_keyboard_simple`   -- BLE HID keyboard, no power mgmt
* `ble_keyboard_lowpower` -- BLE HID keyboard, with power tiers

more sketches in [arduino/README.md](./arduino/README.md).

`arduino/tests/` holds host-native tests for the dependency-free
headers: `cd arduino/tests && make run`.

### CircuitPython

CircuitPython is much easier to work with, but can't get the same low-power
modes available in Arduino. 

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
See `circuitpython/notes-lowpower.md` for why only two power tiers and not three.

### Other

`docs/hardware-notes.md` is the board side: both touch chips, their pad
numbering, their INT lines, and the board changes the firmware assumes.
`docs/` also holds `low-power-notes.md` and the touch-chip datasheets.



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



