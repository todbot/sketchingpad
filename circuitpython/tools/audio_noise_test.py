#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Tod Kurt
# SPDX-License-Identifier: GPL-3.0-or-later
#
# audio_noise_test.py -- which part of the board is in the idle audio?
#
# Brings the board up one piece at a time over the REPL, holding silence
# for a while after each, so the noise can be heard to arrive.  Each stage
# is announced by that many short beeps, since you will be listening, not
# reading:
#
#   1 beep   audio only: PWM on D0, synth silent, BLE off
#   2 beeps  + touch chip(s) configured and scanning, not read
#   3 beeps  + the code polling touch as fast as swarmsynth does
#   4 beeps  + LED rail on, strip dark
#
#   python3 audio_noise_test.py [/dev/cu.usbmodemXXXX] [seconds per stage]
#
# Needs CircuitPython, and touch.py and led_rail.py on CIRCUITPY (any
# installed program carries them).  Close the Serial Monitor first.

import sys

import repl_write_test as t

SETUP = """
import board, busio, digitalio, time, _bleio
import audiopwmio, audiomixer, synthio
_bleio.adapter.enabled = False
audio = audiopwmio.PWMAudioOut(board.D0)
mixer = audiomixer.Mixer(sample_rate=22050, channel_count=1, buffer_size=4096)
synth = synthio.Synthesizer(sample_rate=22050, channel_count=1)
audio.play(mixer)
mixer.voice[0].play(synth)
def beeps(n):
    for _ in range(n):
        b = synthio.Note(1000, amplitude=0.3)
        synth.press(b); time.sleep(0.12); synth.release(b); time.sleep(0.25)
    time.sleep(0.5)
"""

STAGES = [
    ("audio only", ""),
    ("touch scanning", """
import touch
i2c = busio.I2C(board.D5, board.D4)
print(touch.begin(i2c, int_pin=board.D3), touch.name())"""),
    ("touch polled", "poll = True"),
    ("LED rail on, dark", """
import led_rail, neopixel
gate = digitalio.DigitalInOut(board.D7)
gate.switch_to_output(value=True)
led_rail.on(gate)
_bleio.adapter.enabled = False
leds = neopixel.NeoPixel(board.D6, 20, brightness=0.15, auto_write=False)
leds.fill(0); leds.show()"""),
]


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else t.find_port()
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 8
    ser = t.open_port(port)
    t.raw_repl(ser)
    t.run(ser, SETUP + "\npoll = False")
    for n, (name, code) in enumerate(STAGES, 1):
        print("stage %d: %s" % (n, name), flush=True)
        if code:
            out = t.run(ser, code)
            if out:
                print("  " + out)
        hold = """
beeps(%d)
end = time.monotonic() + %f
while time.monotonic() < end:
    if poll:
        touch.read()
""" % (n, secs)
        t.run(ser, hold, wait=secs + 10)
    print("done; the board is left in the REPL, reset it to run code.py")
    ser.write(b"\x02")
    ser.close()


if __name__ == "__main__":
    main()
