# sketchingpad -- CircuitPython programs

Each program directory holds the files to copy to the CIRCUITPY root.
The full programs run on either board: `touch.py` probes I2C at
startup and finds the TS20 or the two MPR121s.

* `sketchingpad_midi_simple/` -- USB + BLE MIDI controller. Both
  transports run at once and neither is required. The CircuitPython
  version of `arduino/sketchingpad_midi_simple`, same pad map and notes
* `ble_keyboard_lowpower/` -- BLE HID keyboard, two power tiers. Sleeps
  in `alarm.light_sleep_until_alarms()` on the touch INT line rather
  than polling I2C
* `swarmsynth/` -- Swarmatron-style drone synth, audio out the jack.
  Ported from synthiota's swarmsynth on synthtools' `SwarmSynth`:
  bottom row is 8 scale degrees, middle row transposes, TP17/TP18 narrow
  and widen the swarm, TP19/TP20 shift octave, the button latches.
  Plays USB MIDI notes in and sends them out, CC 1 spread, CC 74 cutoff.
  Needs its `boot.py` for MIDI, like `sketchingpad_midi_simple`

Bring-up programs, in `hwtest/`. Copy one to the root **as `code.py`**:

* `code_ledtest.py` -- LED chase, no touch chip
* `code_test_ts20.py` -- pad -> LED. TS20 only: it drives `ts20`
  directly rather than going through `touch.py`
* `code_pwmaudio_test1.py` -- synthio tone out D0, no touch chip

## Copying to the board

A CIRCUITPY root is one flat pile of files, so `touch.py` is copied per
program and belongs beside `code.py`, not in a subfolder. The copies
are pinned byte-identical by `tests/copies_test.py`, so edit whichever
one and let the test catch the rest.

`sketchingpad_midi_simple` also needs its `boot.py` at the root, or
`usb_midi.ports` is empty and there is no USB MIDI device. `boot.py`
runs on a hard reset only -- a soft reload will not re-run it.

## Installing from the web flasher

The flasher page has a CircuitPython section: flash CircuitPython, then
install a program. `make release` here builds what it serves:

* `release/circuitpython-<version>.zip` -- CircuitPython itself
* `release/cp-<program>.zip` -- each program's files plus the `.mpy`
  libraries it imports, found by `circup install --auto` against the
  pinned CircuitPython and bundle versions in the `Makefile`

CIRCUITPY is on the QSPI flash, where DFU can't reach, so the page
writes a program over the raw REPL: `storage.unsafe_disable_usb_drive()`,
then each file in base64 chunks, then a reset. It replaces `code.py`,
`boot.py` and `lib/` (and `code.txt`, `main.py`, `main.txt`, which would
run instead) and leaves other files alone. `tools/repl_write_test.py`
is the same thing over pyserial, for checking a board or a new
CircuitPython version.

### Installing CircuitPython

circuitpython.org ships the XIAO only as a UF2, and reaching the UF2
drive takes the reset button. The DFU zip installs over USB serial
like an Arduino sketch: from the web flasher page, or

```sh
adafruit-nrfutil dfu serial -t 1200 -b 115200 --singlebank \
  -p /dev/cu.usbmodemXXXX -pkg release/circuitpython-10.3.1.zip
```

The 1200 bps touch works from an Arduino sketch and from CircuitPython
alike.  The version is pinned in the `Makefile`; bumping it renames the
zip, so `web/sketches.json` needs the new name too.

## Libraries and tests

Libraries come from circup; see `requirements.txt`.

```sh
cd circuitpython/tests && make run
```

Host tests for the touch layer, needing no board and no install. They
also pin the pad tables in `touch.py` against
`arduino/shared/touch_map.h`, which nothing else does. See
`tests/README.md`.

`notes-lowpower.md` covers why there are two power tiers here and three
in the Arduino sketches.
