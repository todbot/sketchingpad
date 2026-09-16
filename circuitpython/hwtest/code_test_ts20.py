# code_test_ts20.py -- light up a LED for each touched pad
#
# Sensitivity is 0 (most sensitive) to 15 (least) and can be changed
# per pad at runtime, see below.
#
# Board: Seeed XIAO nRF52840 on sketchingpad_proto2.

import time, board, busio, digitalio
import neopixel, rainbowio
import ts20

# sketchingpad_proto2 -> XIAO nRF52840:
#   SDA       = D4 (board.SDA)
#   SCL       = D5 (board.SCL)
#   LED_DAT   = D6
#   LED_POWER = D7, P-FET gate: low = rail on
i2c_sda_pin = board.SDA
i2c_scl_pin = board.SCL
led_pin = board.D6
led_power_pin = board.D7

num_leds = 20

# The LED rail hangs off a P-FET whose gate is pulled up to its source, so
# the rail is OFF until D7 is driven low.  Without this the LEDs never light.
led_power = digitalio.DigitalInOut(led_power_pin)
led_power.switch_to_output(value=False)
time.sleep(0.002)  # let ~2 uF of rail decoupling settle before clocking data

leds = neopixel.NeoPixel(led_pin, num_leds, brightness=0.2)
leds.fill(0)

i2c = busio.I2C(scl=i2c_scl_pin, sda=i2c_sda_pin, frequency=400_000)
touch = ts20.TS20(i2c, sensitivity=15)

# tweak individual pads that read too hot or too cold, e.g.:
# touch[3].sensitivity = 12
# touch[7].sensitivity = 15

print("sensitivity:", touch.sensitivity)
print("pad 0 needs a %.2f%% capacitance change" % touch.sensitivity_percent(0))

# Index is LED number, value is the TS20 channel beside it.  Note the
# channel is not the TPn number on the silkscreen: TP1-4 are channels 0-3
# but TP17-20 are channels 4-7.  This table is board-rev specific -- it is
# proto2 and stripproto1; proto1 is completely different.  See
# arduino/README.md for how to re-derive it for a new board.
led_to_pad = (0, 1, 2, 3, 8, 9, 10, 11, 16, 17,
              18, 19, 12, 13, 14, 15, 4, 5, 6, 7)
pad_to_led = [led_to_pad.index(p) for p in range(num_leds)]

dim_by = 5

while True:
    # dim all by (dim_by,dim_by,dim_by)
    leds[:] = [[max(i - dim_by, 0) for i in l] for l in leds]
    c = rainbowio.colorwheel(time.monotonic() * 100)
    for i, t in enumerate(touch.touched_pads):
        if t:
            n = pad_to_led[i]
            print("pad", i, "-> led", n)
            leds[n] = c

    time.sleep(0.01)
