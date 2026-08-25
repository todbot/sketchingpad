print("Hello World!")

import time, board, neopixel, rainbowio

LED_PIN = board.P0_09   # nice!nano / NRF52840 supermini
NUM_LEDS = 20

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
