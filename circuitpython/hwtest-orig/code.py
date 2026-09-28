# SPDX-FileCopyrightText: 2026 Tod Kurt
# SPDX-License-Identifier: GPL-3.0-or-later

print("Hello World!")

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

#i2c = board.I2C()  # 
i2c = busio.I2C(scl=i2c_scl_pin, sda=i2c_sda_pin) #, frequency=1_000_000)
ts20 = ts20.TS20(i2c)

touch_to_led = (18,1,3,4,6,9, 19,2,5,8, 17,0,10,11, 16,15,14,13,12, 7 )
dim_by = 5
while True:
    leds[:] = [[max(i-dim_by,0) for i in l] for l in leds] # dim all by (dim_by,dim_by,dim_by)
    c = rainbowio.colorwheel( time.monotonic()*100 )
    #print("i:", i, "c:", c)
    touches = ts20.read_touches()
    #print("touches:",touches)
    for i,t in enumerate(touches):
        if i<20:
            if t:
                n = touch_to_led.index(i)
                print("i=", i, "->", n)
                leds[n] = c
        
    time.sleep(0.01)
