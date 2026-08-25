# code_test_ts20.py -- light up a LED for each touched pad
#
# Sensitivity is 0 (most sensitive) to 15 (least) and can be changed
# per pad at runtime, see below.

import time, board, busio
import neopixel, rainbowio
import ts20

# nice!nano / NRF52840 supermini
i2c_sda_pin = board.P0_17
i2c_scl_pin = board.P0_20
led_pin = board.P0_09

num_leds = 20

leds = neopixel.NeoPixel(led_pin, num_leds, brightness=0.2)
leds.fill(0)

i2c = busio.I2C(scl=i2c_scl_pin, sda=i2c_sda_pin)  # , frequency=1_000_000)
touch = ts20.TS20(i2c, sensitivity=15)

# tweak individual pads that read too hot or too cold, e.g.:
# touch[3].sensitivity = 12
# touch[7].sensitivity = 15

print("sensitivity:", touch.sensitivity)
print("pad 0 needs a %.2f%% capacitance change" % touch.sensitivity_percent(0))

touch_to_led = (18, 1, 3, 4, 6, 9, 19, 2, 5, 8,
                17, 0, 10, 11, 16, 15, 14, 13, 12, 7)
dim_by = 5

while True:
    # dim all by (dim_by,dim_by,dim_by)
    leds[:] = [[max(i - dim_by, 0) for i in l] for l in leds]
    c = rainbowio.colorwheel(time.monotonic() * 100)
    for i, t in enumerate(touch.touched_pads):
        if t:
            n = touch_to_led.index(i)
            print("pad", i, "-> led", n)
            leds[n] = c

    time.sleep(0.01)
