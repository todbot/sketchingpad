# SPDX-FileCopyrightText: 2026 Tod Kurt
# SPDX-License-Identifier: GPL-3.0-or-later
#
# code.py -- a Swarmatron-style drone machine on sketchingpad, CircuitPython
#
# Ported from synthiota's swarmsynth.  synthtools' SwarmSynth: eight
# oscillators on one pitch, fanned apart by a single "swarm" control and
# each drifting independently.  Audio is PWM out D0 to the headphone jack.
#
# Synthiota's pots, screen, sliders and encoder have no counterpart here,
# so what they set is in the constants below, and the top four pads stand
# in for the span ribbon and the octave buttons:
#
#              TP17 TP18 TP19 TP20    spread -  spread +  octave -  octave +
#   TP1  TP2  TP3  TP4   TP5  TP6  TP7  TP8     transpose -24 -12 -7 0 +7 ...
#   TP9 TP10 TP11 TP12  TP13 TP14 TP15 TP16     the 8 scale degrees
#
# Latch is on at start: a note keeps sounding after you let go, so a hand
# is free for the spread pads, and tapping the sounding pad stops it.  The
# button (SW1) toggles latch.
#
# Note budget: SwarmSynth is mono, so a swarm costs SWARM_COUNT Notes,
# doubling while a steal overlaps the releasing swarm with the new one,
# against synthio's 24.  That is why RELEASE stays short: rolling across
# pads with a longer tail stacks a third releasing swarm and the oldest
# one gets dropped mid-decay.
#
# Everything here is in LED-index space: bit n is LED n, which is TP(n+1)
# on the silkscreen.

import time

import _bleio
import audiomixer
import audiopwmio
import board
import busio
import digitalio
import keypad
import led_rail
import neopixel
import synthio
import tmidi
import touch
import usb_midi
from synthtools import Patch, SwarmSynth

PIN_SDA = board.D4
PIN_SCL = board.D5
PIN_LED_DATA = board.D6
PIN_LED_POWER = board.D7  # P-FET gate, LOW = rail on
PIN_TOUCH_INT = board.D3
PIN_AUDIO = board.D0
PIN_BUTTON = board.D10  # SW1 to GND, no external pull-up

SAMPLE_RATE = 22050
LED_BRIGHTNESS = 0.15
# Lit LEDs put their PWM on the 3.3 V the audio swings to, so they are
# audible.  Fading them out after the last touch leaves a drone in quiet.
# False keeps the indicators lit.
LED_FADE = True
LED_FADE_AFTER = 2.0  # seconds after the last touch
LED_FADE_TIME = 1.0
VELOCITY = 110  # touch pads have no velocity

# The knobs synthiota had.  WAVE is one of SAW SQU TRI SIN ASAW ATRI ASQU
# SSQU.  If audio glitches, lower SWARM_COUNT first.
WAVE = "SAW"
SWARM_COUNT = 8
SWARM_DRIFT = 0.003
FILT_F = 2200
FILT_Q = 0.9
GLIDE = 0.25
RELEASE = 1.0

# The outermost oscillator's offset in octaves.  Most of what is worth
# hearing is under 0.05, so the spread pads ramp it by a ratio, not a
# step: a linear ramp would skip straight past it.
SPREAD = 0.01
SPREAD_MIN = 0.002  # below this a spread- pad drops to unison
SPREAD_MAX = 0.4
SPREAD_RATE = 2.0  # doublings per second while held

OCTAVES = (24, 36, 48, 60, 72)  # C1..C5: the bottom row's root
DEGREES = (0, 2, 3, 5, 7, 8, 10, 12)  # natural minor, left to right
TRANSPOSE = (-24, -12, -7, 0, 7, 12, 19, 24)

PADS_DEGREE = 8  # LEDs 8-15, TP9-16
PADS_TRANSPOSE = 0  # LEDs 0-7, TP1-8
PAD_SPREAD_DOWN = 16  # TP17
PAD_SPREAD_UP = 17  # TP18
PAD_OCT_DOWN = 18  # TP19
PAD_OCT_UP = 19  # TP20

HELD_COLOR = 0x00AAFF
RINGING_COLOR = 0x002A44  # latched, or still ringing out
TRANSPOSE_COLOR = 0xFF6600
SPREAD_COLOR = 0x00FF33
OCT_COLOR = 0xFF00FF

CC_SPREAD = 1  # mod wheel, the span ribbon
CC_CUTOFF = 74

# ------------------------------------------------------------- LED rail

# The rail is off until D7 is driven low.
led_power = digitalio.DigitalInOut(PIN_LED_POWER)
led_power.switch_to_output(value=True)
led_rail.on(led_power)
# Nothing here uses BLE, and every radio burst is a current pulse on the
# 3.3 V the audio PWM swings to.
_bleio.adapter.enabled = False

leds = neopixel.NeoPixel(
    PIN_LED_DATA, touch.NUM_LEDS, brightness=LED_BRIGHTNESS, auto_write=False
)
leds.fill(0)
leds.show()

# ---------------------------------------------------------------- touch

i2c = busio.I2C(PIN_SCL, PIN_SDA)
if not touch.begin(i2c, int_pin=PIN_TOUCH_INT):
    raise RuntimeError("No touch sensor found at 0x6A or 0x5A+0x5B")

button = keypad.Keys((PIN_BUTTON,), value_when_pressed=False, pull=True)

# ---------------------------------------------------------------- synth

# fmt: off
patch = Patch(name="swarm drone", synth_type="swarm", wave=WAVE,
              swarm_count=SWARM_COUNT, swarm_spread=SPREAD,
              swarm_drift=SWARM_DRIFT,
              filt_type="LPF", filt_f=FILT_F, filt_q=FILT_Q,
              amp_env=[0.25, 0.20, 0.9, RELEASE],
              vib_rate=0.0, vib_depth=0.0,
              glide_time=GLIDE)
# fmt: on

audio = audiopwmio.PWMAudioOut(PIN_AUDIO)
# The big buffer rides out the loop stalling on I2C and LED writes.
mixer = audiomixer.Mixer(sample_rate=SAMPLE_RATE, channel_count=1,
                         buffer_size=4096)
sio = synthio.Synthesizer(sample_rate=SAMPLE_RATE, channel_count=1)
audio.play(mixer)
mixer.voice[0].play(sio)

# The Nyquist clamp is baked into the block graph at construction.
SwarmSynth.FILT_F_MAX = SAMPLE_RATE * 0.45
synth = SwarmSynth(sio, patch)

# ----------------------------------------------------------------- MIDI

if usb_midi.ports:
    midi = tmidi.MIDI(midi_in=usb_midi.ports[0], midi_out=usb_midi.ports[1])
else:
    midi = None


def send(kind, note, vel):
    if midi:
        midi.send(tmidi.Message(kind, note, vel))


# ---------------------------------------------------------------- state

oct_i = 1
trans_i = TRANSPOSE.index(0)
held = []  # degree pads down, in press order; last one wins (mono)
last_pad = None  # degree pad the drone is sounding, None if MIDI drove it
sounding = None  # midi note the synth is actually playing, None if silent
latch = True
last_touch = time.monotonic()  # for LED_FADE


def play(pad):
    """Sound the drone at degree `pad`, gliding from wherever it is.

    No synth.note_off() first: mono note_on() steals what is sounding and
    freezes its bend on the way out.  The MIDI stream still needs the off.
    """
    global sounding, last_pad
    last_pad = pad
    note = OCTAVES[oct_i] + TRANSPOSE[trans_i] + DEGREES[pad]
    if sounding is not None:
        send(tmidi.NOTE_OFF, sounding, 0)
    synth.note_on(note, VELOCITY)
    send(tmidi.NOTE_ON, note, VELOCITY)
    sounding = note


def silence():
    global sounding
    if sounding is not None:
        synth.note_off(sounding)
        send(tmidi.NOTE_OFF, sounding, 0)
        sounding = None


def replay():
    """Glide a held drone to a new octave or transposition.  A drone that
    came in over MIDI is played as sent, so it waits for the next pad."""
    if sounding is not None and last_pad is not None:
        play(last_pad)


def pads_changed(touches, changed):
    global trans_i, oct_i
    for led in range(touch.NUM_LEDS):
        if not (changed >> led) & 1:
            continue
        down = (touches >> led) & 1
        if PADS_DEGREE <= led < PADS_DEGREE + 8:
            pad = led - PADS_DEGREE
            if down:
                if latch and pad == last_pad and sounding is not None:
                    silence()
                else:
                    play(pad)
                held.append(pad)
                continue
            # `held` tracks what is physically down even while latched, so
            # the LEDs stay honest; latch only suppresses the sound change.
            if pad in held:
                held.remove(pad)
            if not latch:
                if held:
                    play(held[-1])
                else:
                    silence()
        elif not down:
            continue
        elif PADS_TRANSPOSE <= led < PADS_TRANSPOSE + 8:
            trans_i = led - PADS_TRANSPOSE
            replay()
        elif led in (PAD_OCT_DOWN, PAD_OCT_UP):
            d = 1 if led == PAD_OCT_UP else -1
            oct_i = min(max(oct_i + d, 0), len(OCTAVES) - 1)
            replay()


def ramp_spread(touches, dt):
    down = (touches >> PAD_SPREAD_DOWN) & 1
    up = (touches >> PAD_SPREAD_UP) & 1
    if down == up:
        return
    v = synth.swarm_spread
    if up:
        v = min(max(v, SPREAD_MIN) * 2 ** (SPREAD_RATE * dt), SPREAD_MAX)
    else:
        v = v * 2 ** (-SPREAD_RATE * dt)
        if v < SPREAD_MIN:
            v = 0.0
    synth.swarm_spread = v


def check_midi():
    global sounding, last_pad
    while midi and (msg := midi.receive()):
        if msg.type == tmidi.NOTE_ON and msg.velocity:
            synth.note_on(msg.note, msg.velocity)
            sounding = msg.note
            last_pad = None
        elif msg.type in (tmidi.NOTE_OFF, tmidi.NOTE_ON):
            if msg.note == sounding:
                synth.note_off(msg.note)
                sounding = None
        elif msg.type == tmidi.CC:
            if msg.data0 == CC_SPREAD:
                synth.swarm_spread = SPREAD_MAX * (msg.data1 / 127) ** 3
            elif msg.data0 == CC_CUTOFF:
                synth.filt_f = 100 + 5900 * msg.data1 / 127
            elif msg.data0 in (120, 123):
                synth.all_notes_off()
                del held[:]
                sounding = None


def dim(color, frac):
    return tuple(int(((color >> s) & 0xFF) * frac) for s in (16, 8, 0))


def fade_level(now):
    if not LED_FADE:
        return 1.0
    t = (now - last_touch - LED_FADE_AFTER) / LED_FADE_TIME
    # Coarse steps, since each change costs a show().
    return round(min(max(1.0 - t, 0.0), 1.0) * 20) / 20


def colors(touches):
    out = [0] * touch.NUM_LEDS
    for pad in range(8):
        if pad in held:
            out[PADS_DEGREE + pad] = HELD_COLOR
        elif pad == last_pad and sounding is not None:
            out[PADS_DEGREE + pad] = RINGING_COLOR
    out[PADS_TRANSPOSE + trans_i] = TRANSPOSE_COLOR
    # The spread pads glow with the spread, so a latched drone shows how
    # wide it is.
    s = (synth.swarm_spread / SPREAD_MAX) ** (1 / 3)
    for led in (PAD_SPREAD_DOWN, PAD_SPREAD_UP):
        on = (touches >> led) & 1
        out[led] = SPREAD_COLOR if on else dim(SPREAD_COLOR, 0.05 + 0.5 * s)
    lit = oct_i / (len(OCTAVES) - 1)
    out[PAD_OCT_DOWN] = dim(OCT_COLOR, 0.05 + 0.5 * (1 - lit))
    out[PAD_OCT_UP] = dim(OCT_COLOR, 0.05 + 0.5 * lit)
    return out


# ----------------------------------------------------------------- loop

print("swarmsynth: sensor %s, %d Hz, %d oscillators" % (
    touch.name(), SAMPLE_RATE, SWARM_COUNT))
if midi is None:
    print("usb_midi.ports is empty -- copy boot.py to CIRCUITPY and reboot")
touch.report()

last_touches = 0
last_colors = None
last_t = time.monotonic()

while True:
    now = time.monotonic()
    dt = now - last_t
    last_t = now

    touches = touch.read()
    if touches:
        last_touch = now
    if touches != last_touches:
        pads_changed(touches, touches ^ last_touches)
        last_touches = touches
    ramp_spread(touches, dt)

    event = button.events.get()
    if event and event.pressed:
        last_touch = now
        latch = not latch
        print("latch", "on" if latch else "off")
        if not latch and not held:  # else a latched drone rings forever
            silence()

    check_midi()

    # Only on a change: every show() is a stall the audio buffer must cover.
    c = (colors(touches), fade_level(now))
    if c != last_colors:
        for i, v in enumerate(c[0]):
            leds[i] = v
        leds.brightness = LED_BRIGHTNESS * c[1]
        leds.show()
        last_colors = c
