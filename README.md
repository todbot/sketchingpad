
# SketchingPad


<img src="./docs/sketchingpad1-render.jpg" width=350>


## Features



## Software


### Arduino

### CircuitPython



## Enclosure

As seen in the photos, there is a simple [3d-printable enclosure](./enclosure). 
It's fast to print and you can snap the PCB in to quickly get started,
or use M2.5 screws and nuts to secure the PCB more permanently.


## Pins used

* BUTTON     - D10/MOSI (SW1 to GND, needs internal pull-up)
* TOUCH_INT  - D3 / A3  (open collector, R1 pulls up)
* SDA        - D4       (Arduino Wire default)
* SCL        - D5       (Arduino Wire default)
* LED_DAT    - D6 / TX
* LED_POWER  - D7 / RX  (P-FET gate: LOW = rail on)


## Schematics

There are two versions of the board:

- `sketchingpad1` - uses one TS20 touch chip
- `sketchingpad1_mpr1` - uses two MPR121 touch chips

KiCad source files available in the "schematics" directory. 

### sketchingpad1  (TS20)

[<img src="./docs/sketchingpad1-sch.png" width=700>](./schematics/sketchingpad1/sketchingpad1-sch.pdf)


### sketchingpad1-mpr  (2xMPR121)

[<img src="./docs/sketchingpad1_mpr-sch.png" width=700>](./schematics/sketchingpad1/sketchingpad1_mpr-sch.pdf)



