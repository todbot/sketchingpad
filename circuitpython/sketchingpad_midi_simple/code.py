# SPDX-FileCopyrightText: 2026 Tod Kurt
# SPDX-License-Identifier: GPL-3.0-or-later
#
# code.py -- sketchingpad as a USB + BLE MIDI controller, CircuitPython
#
# The CircuitPython version of arduino/sketchingpad_midi_simple.  Same pad
# map, same notes, same control pairs.  Both transports run at once and
# neither is required: unplugged it is a BLE controller, with nothing
# paired it is a USB controller, plugged into a paired host it plays both.
#
# Poll the sensor, send on a change, light the LEDs.  Nothing else -- no
# sleep, no power tiers.  Runs on both boards: I2C is probed at startup,
# TS20 at 0x6A or the two MPR121s at 0x5A and 0x5B.
#
# Both sensors sit behind touch.py, the counterpart of the Arduino
# arduino/shared/touch.h: this file never sees a channel or an electrode.
#
# Libraries: tmidi, adafruit_ble, adafruit_ble_midi, neopixel, plus ts20
# and adafruit_mpr121 by way of touch.py.  Copy touch.py and boot.py to
# the CIRCUITPY root alongside this file -- without boot.py usb_midi.ports
# is empty, and the banner says so.
#
# Everything here is in LED-index space: bit n is LED n, which is TP(n+1)
# on the silkscreen.

import time

import adafruit_ble_midi
import board
import busio
import digitalio
import microcontroller
import neopixel
import touch as touch
import tmidi
import usb_midi
from adafruit_ble import BLERadio
from adafruit_ble.advertising.standard import ProvideServicesAdvertisement

# sketchingpad1 / sketchingpad1_mpr -> XIAO nRF52840
PIN_SDA = board.D4
PIN_SCL = board.D5
PIN_LED_DATA = board.D6
PIN_LED_POWER = board.D7  # P-FET gate, LOW = rail on
PIN_TOUCH_INT = board.D3  # open collector, R1 pulls it up; both /IRQ on _mpr

NUM_LEDS = touch.NUM_LEDS
LED_BRIGHTNESS = 0.15
TS20_SENSITIVITY = 15  # 0 = most sensitive, 15 = least

NOTE_COLOR = (255, 0, 255)
BEND_COLOR = (0, 255, 255)
MOD_COLOR = (255, 96, 0)

MIDI_CHANNEL = 0  # tmidi counts channels from 0, so this is channel 1
MIDI_VELOCITY = 100
CC_MODULATION = 1
CC_ALL_NOTES_OFF = 123

# tmidi's pitch_bend property is centered on 0, like the wire value minus
# 8192.  See bend_message() for the trap in the other way of setting it.
BEND_MIN = -8192
BEND_CENTER = 0
BEND_MAX = 8191

# The 16 note pads, C major pentatonic climbing left to right and bottom to
# top, so the low row is the low octaves:
#
#   TP1  TP2  TP3  TP4   TP5  TP6  TP7  TP8    G3 A3 C4 D4  E4 G4 A4 C5
#   TP9 TP10 TP11 TP12  TP13 TP14 TP15 TP16    C2 D2 E2 G2  A2 C3 D3 E3
NUM_NOTE_PADS = 16
LED_TO_NOTE = (
    55, 57, 60, 62, 64, 67, 69, 72,  # TP1-8,  mid row, G3 up to C5
    36, 38, 40, 43, 45, 48, 50, 52,  # TP9-16, low row, C2 up to E3
)

# The top row of four, TP17-TP20.  Swap a pair to flip its direction.
PAD_BEND_DOWN = 16  # TP17
PAD_BEND_UP = 17  # TP18
PAD_MOD_MID = 18  # TP19
PAD_MOD_MAX = 19  # TP20

ID_CODE_LEN = 5
ID_LETTERS = "RYGBM"
ID_COLORS = ((255, 0, 0), (255, 96, 0), (0, 255, 0), (0, 0, 255), (255, 0, 255))
ID_LEDS = (8, 9, 10, 11, 12)  # bottom row's left five, TP9-13


def device_id_digits():
    """FNV-1a over the CPU's unique ID, sliced into five base-5 digits.

    The same hash arduino/shared/device_id_code.h uses, but whether the
    bytes arrive in the same order is unverified: that reads FICR DEVICEID
    as two 32-bit words, this gets whatever microcontroller.cpu.uid hands
    back.  The banner prints the raw uid so the two can be compared.
    """
    h = 2166136261
    for b in microcontroller.cpu.uid:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    digits = []
    for _ in range(ID_CODE_LEN):
        digits.append(h % len(ID_LETTERS))
        h //= len(ID_LETTERS)
    return digits


ID_DIGITS = device_id_digits()
ID_CODE = "".join(ID_LETTERS[d] for d in ID_DIGITS)

# ------------------------------------------------------------- LED rail

# The rail hangs off a P-FET whose gate is pulled up to its source, so it
# is off until D7 is driven low.  Without this the LEDs never light.
led_power = digitalio.DigitalInOut(PIN_LED_POWER)
led_power.switch_to_output(value=False)
time.sleep(0.002)  # let ~2 uF of rail decoupling settle before clocking data

leds = neopixel.NeoPixel(
    PIN_LED_DATA, NUM_LEDS, brightness=LED_BRIGHTNESS, auto_write=False
)
leds.fill(0)
leds.show()

# ---------------------------------------------------------------- touch

i2c = busio.I2C(PIN_SCL, PIN_SDA)

# Probes both addresses, configures whichever answers, and parks the four
# J2 electrodes on the MPR121 board.
if not touch.begin(i2c, sensitivity=TS20_SENSITIVITY, int_pin=PIN_TOUCH_INT):
    raise RuntimeError("No touch sensor found at 0x6A or 0x5A+0x5B")


# ----------------------------------------------------------------- MIDI

midi_ports = []

if usb_midi.ports:
    midi_usb = tmidi.MIDI(midi_out=usb_midi.ports[1])
    midi_ports.append(("usb", midi_usb, lambda: True))
else:
    midi_usb = None

midi_service = adafruit_ble_midi.MIDIService()
midi_ble = tmidi.MIDI(midi_out=midi_service)
ble = BLERadio()
ble.name = "sketchingpad-" + ID_CODE
advertisement = ProvideServicesAdvertisement(midi_service)
midi_ports.append(("ble", midi_ble, lambda: ble.connected))

# Which transports were up last pass, so arrivals can be spotted as edges.
was_ready = {name: False for name, _, _ in midi_ports}


def bend_message(value):
    """A pitch bend message for a value centered on 0.

    Built through the property rather than Message(PITCH_BEND, value):
    that constructor only treats data0 as a bend value when it falls
    outside 0-127, so the center value 0 would be taken as a raw data byte
    and sent as a full bend *down*.
    """
    msg = tmidi.Message(tmidi.PITCH_BEND, channel=MIDI_CHANNEL)
    msg.pitch_bend = value
    return msg


def send_all(msg):
    """Out to whichever transports are up at this instant."""
    for _, midi, ready in midi_ports:
        if ready():
            midi.send(msg)


def note_on(note):
    send_all(tmidi.Message(tmidi.NOTE_ON, note, MIDI_VELOCITY, channel=MIDI_CHANNEL))


def note_off(note):
    send_all(tmidi.Message(tmidi.NOTE_OFF, note, 0, channel=MIDI_CHANNEL))


def control_change(cc, value):
    send_all(tmidi.Message(tmidi.CC, cc, value, channel=MIDI_CHANNEL))


def bend_for(touches):
    """Both pads of the pair held is treated as neither: the two directions
    cancel rather than one of them winning on read order.
    """
    down = (touches >> PAD_BEND_DOWN) & 1
    up = (touches >> PAD_BEND_UP) & 1
    if down == up:
        return BEND_CENTER
    return BEND_MIN if down else BEND_MAX


def mod_for(touches):
    if (touches >> PAD_MOD_MAX) & 1:
        return 127
    if (touches >> PAD_MOD_MID) & 1:
        return 64
    return 0


def resync(midi, touches):
    """Bring one transport up to the state the pads are already in.

    Aimed at the transport that just arrived, not at all of them: sending
    held notes again to a transport that was already playing them would
    double them up.
    """
    midi.send(tmidi.Message(tmidi.CC, CC_ALL_NOTES_OFF, 0, channel=MIDI_CHANNEL))
    midi.send(tmidi.Message(tmidi.CC, CC_MODULATION, mod_for(touches), channel=MIDI_CHANNEL))
    midi.send(bend_message(bend_for(touches)))
    for led in range(NUM_NOTE_PADS):
        if (touches >> led) & 1:
            midi.send(
                tmidi.Message(tmidi.NOTE_ON, LED_TO_NOTE[led], MIDI_VELOCITY, channel=MIDI_CHANNEL)
            )


def color_for(led):
    if led < NUM_NOTE_PADS:
        return NOTE_COLOR
    if led in (PAD_BEND_DOWN, PAD_BEND_UP):
        return BEND_COLOR
    return MOD_COLOR


def pad_name(led):
    if led < NUM_NOTE_PADS:
        return "note %d" % LED_TO_NOTE[led]
    return {
        PAD_BEND_DOWN: "bend down",
        PAD_BEND_UP: "bend up",
        PAD_MOD_MID: "mod 64",
        PAD_MOD_MAX: "mod 127",
    }[led]


def show_device_id_code():
    """Lit at startup; loop() only redraws on a change, so it stays up
    until the first touch."""
    leds.fill(0)
    for i, digit in enumerate(ID_DIGITS):
        leds[ID_LEDS[i]] = ID_COLORS[digit]
    leds.show()


# ----------------------------------------------------------------- setup

ble.start_advertising(advertisement)
show_device_id_code()

# Seed from a real read rather than 0, so a sensor reporting anything at
# all on the first pass does not wipe the ID code before it can be seen.
last_touches = touch.read()
last_bend = bend_for(last_touches)
last_mod = mod_for(last_touches)

print('sketchingpad_midi_simple: sensor %s, pair with "%s"' % (touch.name(), ble.name))
print("MIDI channel %d, velocity %d, notes %d-%d" % (
    MIDI_CHANNEL + 1, MIDI_VELOCITY, LED_TO_NOTE[8], LED_TO_NOTE[7]))
print("id code %s from uid %s" % (
    ID_CODE, "".join("%02x" % b for b in microcontroller.cpu.uid)))
if midi_usb is None:
    print("usb_midi.ports is empty -- copy boot.py to CIRCUITPY and reboot")
print("id code on LEDs %d-%d, first read %05x" % (ID_LEDS[0], ID_LEDS[-1], last_touches))
touch.report()

# ------------------------------------------------------------------ loop

while True:
    touches = touch.read()

    # Whatever a transport was left holding when it went away could not be
    # silenced while it was gone, so each one is caught up as it returns.
    for name, midi, ready in midi_ports:
        now = ready()
        if now and not was_ready[name]:
            resync(midi, touches)
            print("%s up, host state resynced" % name)
        elif not now and was_ready[name]:
            print("%s gone" % name)
        was_ready[name] = now

    if touches != last_touches:
        changed = touches ^ last_touches

        print("touches %05x" % touches)
        for led in range(NUM_LEDS):
            if not (changed >> led) & 1:
                continue
            on = (touches >> led) & 1
            print("  led %2d (TP%2d) <- %-9s %s %s" % (
                led, led + 1, touch.source(led), "down" if on else "up  ", pad_name(led)))
            if led < NUM_NOTE_PADS:
                if on:
                    note_on(LED_TO_NOTE[led])
                else:
                    note_off(LED_TO_NOTE[led])

        # Rebuilt from the whole pad word rather than from the pad that
        # moved, so a roll-off inside a pair lands on the value the other
        # pad still asks for instead of on the spring-back.
        bend = bend_for(touches)
        mod = mod_for(touches)
        if bend != last_bend:
            send_all(bend_message(bend))
            print("  bend %d" % bend)
            last_bend = bend
        if mod != last_mod:
            control_change(CC_MODULATION, mod)
            print("  cc%d %d" % (CC_MODULATION, mod))
            last_mod = mod

        for i in range(NUM_LEDS):
            leds[i] = color_for(i) if (touches >> i) & 1 else 0
        leds.show()

        last_touches = touches

    time.sleep(0.01)
