# sketchingpad -- CircuitPython programs

Each program directory holds the files to copy to the CIRCUITPY root.
The two full programs run on either board: `touch.py` probes I2C at
startup and finds the TS20 or the two MPR121s.

* `sketchingpad_midi_simple/` -- USB + BLE MIDI controller. Both
  transports run at once and neither is required. The CircuitPython
  version of `arduino/sketchingpad_midi_simple`, same pad map and notes
* `ble_keyboard_lowpower/` -- BLE HID keyboard, two power tiers. Sleeps
  in `alarm.light_sleep_until_alarms()` on the touch INT line rather
  than polling I2C

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
