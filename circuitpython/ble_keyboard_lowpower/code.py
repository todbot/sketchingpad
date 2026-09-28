# SPDX-FileCopyrightText: 2026 Tod Kurt
# SPDX-License-Identifier: GPL-3.0-or-later
#
# code.py -- sketchingpad as a low-power BLE HID keyboard, CircuitPython
#
# Touching a pad types a character over BLE and lights its LED.  Between
# events the board sleeps in alarm.light_sleep_until_alarms(), woken by the
# touch INT line rather than by polling I2C.
#
# The sensor sits behind touch.py, so this runs on the MPR121 board too and
# works in LED-index space: bit n is LED n, which is TP(n+1) on the
# silkscreen.  Copy touch.py to the CIRCUITPY root alongside this file.
#
# Two tiers, not the Arduino sketch's three.  See notes-lowpower.md for why
# there is no DEEP tier: CircuitPython's "deep sleep" on nRF52 is not System
# OFF, it is this same idle loop plus a reset, so it costs the same and
# loses all state.
#
# Board: Seeed XIAO nRF52840 (the "Sense" CircuitPython build works; it only
# adds IMU/mic pins this board does not populate).

import time

import alarm
import board
import busio
import digitalio
import neopixel
import touch
from adafruit_ble import BLERadio
from adafruit_ble.advertising.standard import ProvideServicesAdvertisement
from adafruit_ble.services.standard.hid import HIDService
from adafruit_hid.keyboard import Keyboard
from adafruit_hid.keycode import Keycode

# Pins traced from the sketchingpad_proto2 netlist.  Note BUTTON is D10
# (P1.15): SW1 is NOT on D2, whatever the Arduino sketch used to say.
# proto2 and sketchingpad1 wire the TS20 identically, so the pad tables in
# touch.py cover both.
PIN_SDA = board.D4
PIN_SCL = board.D5
PIN_LED_DATA = board.D6
PIN_LED_POWER = board.D7  # P-FET gate, LOW = rail on
PIN_TOUCH_INT = board.D3  # open collector, R1 pulls it up; both /IRQ on _mpr
PIN_BUTTON = board.D10  # SW1 to GND

NUM_LEDS = 20
LED_BRIGHTNESS = 0.15
TS20_SENSITIVITY = 15  # 0 = most sensitive, 15 = least
TOUCH_COLOR = (255, 0, 255)

ACTIVE_TIMEOUT_S = 10.0  # ACTIVE -> IDLE
TICK_ACTIVE_S = 0.25
TICK_IDLE_S = 2.0

# The slave-latency substitute.  CircuitPython cannot set slave latency at
# all, so the only lever is the interval itself -- which unlike latency also
# slows down the response to the *next* touch.  See notes-lowpower.md.
CONN_INTERVAL_ACTIVE_MS = 15.0
CONN_INTERVAL_IDLE_MS = 500.0

# Indexed by LED, which is TP(n+1), so the table reads in silkscreen order
# and is the same on both boards:
#
#            TP17 TP18 TP19 TP20               1  2  3  4
#   TP1  TP2  TP3  TP4   TP5  TP6  TP7  TP8    a  s  d  f   g  h  j  k
#   TP9 TP10 TP11 TP12  TP13 TP14 TP15 TP16    z  x  c  v   b  n  m  ,
led_to_key = (
    Keycode.A, Keycode.S, Keycode.D, Keycode.F,  # TP1-4,   mid left
    Keycode.G, Keycode.H, Keycode.J, Keycode.K,  # TP5-8,   mid right
    Keycode.Z, Keycode.X, Keycode.C, Keycode.V,  # TP9-12,  low left
    Keycode.B, Keycode.N, Keycode.M, Keycode.COMMA,  # TP13-16, low right
    Keycode.ONE, Keycode.TWO, Keycode.THREE, Keycode.FOUR,  # TP17-20, top row
)

# ----------------------------------------------------------------- LED rail

led_power = digitalio.DigitalInOut(PIN_LED_POWER)
led_power.switch_to_output(value=True)  # rail off until we ask for it

# Both are built and torn down by set_rail(); only one exists at a time,
# because D6 is either a NeoPixel output or a plain pin held low, never both.
leds = None
data_low = None
rail_on = False


def set_rail(on):
    """Sequencing matters in both directions.  Up: enable the rail and let
    ~2 uF of decoupling settle before clocking data in.  Down: blank the
    strip, then hold the data line low, or current sneaks in through the
    SK6812 input protection diode and partly powers the rail we just
    switched off.  D6 stays driven low for as long as the rail is off.
    """
    global rail_on, leds, data_low
    if on == rail_on:
        return
    if on:
        if data_low is not None:
            data_low.deinit()
            data_low = None
        led_power.value = False  # P-FET on
        time.sleep(0.002)
        leds = neopixel.NeoPixel(
            PIN_LED_DATA, NUM_LEDS, brightness=LED_BRIGHTNESS, auto_write=False
        )
        leds.fill(0)
        leds.show()
        rail_on = True
    else:
        leds.fill(0)
        leds.show()
        leds.deinit()
        leds = None
        data_low = digitalio.DigitalInOut(PIN_LED_DATA)
        data_low.switch_to_output(value=False)
        led_power.value = True  # P-FET off (gate to source)
        rail_on = False


def show_touches(touches):
    if not rail_on:
        return
    for i in range(NUM_LEDS):
        leds[i] = TOUCH_COLOR if (touches >> i) & 1 else 0
    leds.show()


# --------------------------------------------------------------------- init

i2c = busio.I2C(PIN_SCL, PIN_SDA, frequency=400_000)

# int_pin=None because PIN_TOUCH_INT belongs to the PinAlarm below, and a
# DigitalInOut holding it would make light_sleep_until_alarms() fail.  Only
# the MPR121 path reads the line, so the TS20 board loses nothing.
#
# low_power=True alternates fast/slow sensing to save power.  It is a
# begin() argument because the driver applies it in its one startup reset;
# changing it later would soft-reset the chip and restart calibration.
if not touch.begin(i2c, sensitivity=TS20_SENSITIVITY, low_power=True, int_pin=None):
    raise RuntimeError("No touch sensor found at 0x6A or 0x5A+0x5B")

hid = HIDService()
kbd = Keyboard(hid.devices)
radio = BLERadio()
radio.name = "sketchingpad"
advert = ProvideServicesAdvertisement(hid)


def set_conn_interval(ms):
    """The setter does no clamping, and the SoftDevice rejects intervals
    that violate conn_sup_timeout*4 > (1+latency)*interval with a bare
    ValueError.  The supervision timeout is fixed at 4000 ms and is not
    settable from Python, so an over-long interval is simply refused.
    """
    for conn in radio.connections:
        try:
            conn.connection_interval = ms
        except ValueError:
            pass  # central refused it; we keep whatever it assigned


def button_down():
    """Read SW1.  The pin must be free whenever light_sleep_until_alarms()
    runs, so never hold this DigitalInOut across a sleep -- build it, read
    it, deinit it.
    """
    b = digitalio.DigitalInOut(PIN_BUTTON)
    b.switch_to_input(pull=digitalio.Pull.UP)
    down = not b.value
    b.deinit()
    return down


# --------------------------------------------------------------------- loop

set_rail(True)
radio.start_advertising(advert)

last_touches = 0
last_touch_time = time.monotonic()
active = True
set_conn_interval(CONN_INTERVAL_ACTIVE_MS)

while True:
    # Rebuild the alarms every pass: light_sleep_until_alarms() ends with an
    # internal alarm_reset() that uninits the GPIOTE channels.
    #
    # Exactly two PinAlarms, and it must stay two.  The nordic port keeps
    # pin wake on the low-power GPIO SENSE path only up to 2 pins; a third
    # silently switches all of them to dedicated GPIOTE channels with HFCLK
    # forced on, which the port's own comment prices at ~10x sleep current.
    #
    # value=False is a falling-edge GPIOTE event underneath, despite the
    # Python API calling it "level" -- which is what the TS20 needs, since
    # its INT is a ~1 ms active-low pulse rather than a held level.
    woke = alarm.light_sleep_until_alarms(
        alarm.pin.PinAlarm(PIN_TOUCH_INT, value=False, pull=False),  # R1 pulls up
        alarm.pin.PinAlarm(PIN_BUTTON, value=False, pull=True),  # SW1 to GND
        alarm.time.TimeAlarm(
            monotonic_time=time.monotonic()
            + (TICK_ACTIVE_S if active else TICK_IDLE_S)
        ),
    )

    if not radio.connected and not radio.advertising:
        radio.start_advertising(advert)

    # The button means "go idle now".  It cannot mean "deep sleep" the way
    # it does in the Arduino sketch, because there is no deep sleep here
    # worth entering -- see notes-lowpower.md.
    if isinstance(woke, alarm.pin.PinAlarm) and woke.pin is PIN_BUTTON:
        while button_down():
            time.sleep(0.01)  # wait for release, or we re-wake instantly
        last_touch_time = time.monotonic() - ACTIVE_TIMEOUT_S

    # The INT line only says "something changed", never what, so every wake
    # needs a read.  Reading unconditionally also covers the tick, which
    # catches a state change whose pulse we somehow missed.
    touches = touch.read()

    if touches != last_touches:
        # Rebuild the whole report from current pad state so chords and
        # roll-offs are not dropped.  The boot keyboard report holds six
        # keycodes; a seventh simultaneous pad has nowhere to go.
        pressed = [led_to_key[n] for n in range(NUM_LEDS) if (touches >> n) & 1][:6]
        if radio.connected:
            kbd.release_all()
            if pressed:
                kbd.press(*pressed)
        show_touches(touches)
        last_touches = touches
        if touches:
            last_touch_time = time.monotonic()

    # Waking on an INT pulse with touches == 0 is a release event, so it
    # deliberately does not leave IDLE -- nothing is being touched.  The
    # cost is that the first real touch afterwards is reported at the IDLE
    # interval before the tier bump takes effect, i.e. up to
    # CONN_INTERVAL_IDLE_MS late.  Slave latency would avoid that; we do
    # not have it.  This is the concrete price of finding 2, in notes.
    idle_for = time.monotonic() - last_touch_time
    if active and idle_for >= ACTIVE_TIMEOUT_S:
        active = False
        set_rail(False)
        set_conn_interval(CONN_INTERVAL_IDLE_MS)
    elif not active and touches:
        active = True
        set_rail(True)
        set_conn_interval(CONN_INTERVAL_ACTIVE_MS)
        show_touches(touches)
