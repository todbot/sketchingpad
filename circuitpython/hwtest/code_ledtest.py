print("Hello World!")

import time, board, digitalio, neopixel, rainbowio

# sketchingpad_proto2 -> XIAO nRF52840:
#   LED_DAT   = D6
#   LED_POWER = D7, P-FET gate: low = rail on
LED_PIN = board.D6
LED_POWER_PIN = board.D7
NUM_LEDS = 20

# The LED rail hangs off a P-FET whose gate is pulled up to its source, so
# the rail is OFF until D7 is driven low.  Without this the LEDs never light.
led_power = digitalio.DigitalInOut(LED_POWER_PIN)
led_power.switch_to_output(value=False)
time.sleep(0.002)  # let ~2 uF of rail decoupling settle before clocking data

leds = neopixel.NeoPixel(LED_PIN, NUM_LEDS, brightness=0.2)
leds.fill(0)

i = 0
while True:
    leds.fill(0)
    c = rainbowio.colorwheel( time.monotonic()*100 )
    print("i:", i, "c:", c)
    leds[i] = c
    i = (i + 1) % NUM_LEDS
    time.sleep(0.3)
