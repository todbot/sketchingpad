
### Prior art for the LED power switch

This is a solved problem in the nRF52840 wireless-keyboard world; both
of the common controller boards do exactly this, for exactly this
reason ("addressable RGB LEDs draw up to 1 mA each when off").

**[nRFMicro](https://github.com/joric/nrfmicro)** -- closest reference,
and the schematic is KiCad source you can open:
[`hardware/nrfmicro.kicad_sch`](https://github.com/joric/nrfmicro/blob/main/hardware/nrfmicro.kicad_sch).
Q2 is an **AO3407** P-channel MOSFET in SOT-23 (30 V, 4.1 A), source on
the 3V3 rail (`nRF_VDD`), drain feeding the switched `EXT_VCC` bus, gate
driven by `POWER_PIN`. R9 is a 2M resistor from that gate to GND -- a
pull-*down*, traced from the schematic source -- so the rail is ON by
default and the MCU drives the gate high to cut it. That matters at
boot: the LEDs come up powered before firmware runs, rather than staying
dark on a board whose firmware has not started.

- [Pinout](https://github.com/joric/nrfmicro/wiki/Pinout) -- "P1.09 -
  POWER_PIN (EXT_VCC control since 1.0, P0.13 on nice!nano) (1-off,
  0-on)"
- [Components](https://github.com/joric/nrfmicro/wiki/Components) --
  part choice and the gate-resistor tradeoff: "There's a tradeoff in
  battery leak (I = 3.3V/2M = ~1.65 uA) vs mosfet opening time (t =
  3nC/3.3V*2M = ~1800uS)". Also suggests an **LDO with an enable pin**
  as an alternative -- "you get both regulated output and a trigger
  without pull-up resistors" -- which may suit us better, since the LED
  rail wants a regulated 3V3 anyway.

**[nice!nano](https://nicekeyboards.com/docs/nice-nano/pinout-schematic/)**
-- same circuit, different pin: "P0.13 on VCC shuts off the power to VCC
when you set it to high. This saves on battery immensely for LEDs of all
kinds that eat power even when off."

**[ZMK](https://zmk.dev/docs/behaviors/power)** -- the firmware side of
the same pattern, if you want precedent for how it gets exposed:
`&ext_power` with `EP_ON` / `EP_OFF` / `EP_TOG`, backed by a
`zmk,ext-power` devicetree node with a `control-gpios` property. See
also [power management config](https://zmk.dev/docs/config/power).

**[Adafruit MagTag](https://github.com/adafruit/Adafruit_MagTag_PCBs)**
-- same job, opposite gate polarity, and worth understanding before
copying either. From `Adafruit MagTag 2.9in Rev D.sch`: Q2 is an
**AO3415** P-FET, source on 3V3, drain feeding `V+` (the VDD of the four
WS2812Bs), gate on `NEOPOWER` = `IO21`, with **R15 = 100K from gate to
the source rail** -- a pull-*up*. So Vgs = 0 and the rail is **OFF** by
default; firmware drives IO21 low to enable. nRFMicro's 2M to GND is a
pull-*down*: rail **ON** by default, driven high to cut.

Both are correctly oriented high-side switches -- source on the supply
side, so the body diode blocks supply-to-load when off. Neither is
wrong; they differ in what happens when the MCU is *not* driving the
gate, and each put the resistor's current in the state where it is free:

| | nRFMicro | MagTag |
|---|---|---|
| Gate resistor | 2M to GND | 100K to source |
| Default (pin undriven) | rail ON | rail OFF |
| To change it | drive high | drive low |
| Resistor current | 0 default, 1.65 uA off | 0 off, 33 uA while LEDs lit |

nRFMicro is a Pro Micro drop-in, so `EXT_VCC` *is* the board's `VCC`
pin -- defaulting it off would leave any firmware that does not know
about `POWER_PIN` with a dead supply. Backward compatibility forces
default-ON, and the 2M keeps the resulting idle-state leak down to
1.65 uA (the Components wiki notes the tradeoff: a larger resistor leaks
less but transitions the gate more slowly). MagTag has no such
constraint and picks the safe default instead.

**For the sketchingpad, use the MagTag polarity: pull-up to the source
rail, default OFF, drive low to enable.** The reason is power-on and
reset, not sleep -- nRF52840 GPIOs retain their state through SYSTEM_ON
and through SYSTEM_OFF when configured, so the pin only floats before
firmware runs. With a pull-up, a board that is unprogrammed, mid-reset,
or crashed sits at ~0 mA instead of 10-20 mA. That is the entire reason
for adding the switch, so it should be the state the hardware falls back
to on its own.

**Counter-example worth knowing:** the [Adafruit QT Py
ESP32-S2](https://github.com/adafruit/Adafruit-QT-Py-ESP32-S2-PCB) skips
the FET entirely and wires its single NeoPixel's VDD straight to GPIO
`IO38` (net `NEOPIX_PWR` in the schematic). Fine for one LED at ~1 mA;
not an option for 20, which can pull well over an amp at full white.

### Which P-FET: AO3415, not AO3407

From the manufacturer datasheets (AO3415 Rev 7; AO3407):

| | AO3415 | AO3407 |
|---|---|---|
| VDS | -20 V | -30 V |
| VGS max | +/-8 V | +/-20 V |
| ID cont. | -4 A | -4.1 A |
| **VGS(th) max** | **-0.9 V** | **-2.4 V** |
| RDS(on) @ -4.5 V | 41 mOhm max | 87 mOhm max |
| RDS(on) @ -2.5 V | 53 mOhm max | *not specified* |
| RDS(on) @ -1.8 V | 65 mOhm max | *not specified* |
| Qg (total) | 9.3 nC typ @ -4.5 V | 4.6 nC typ @ -4.5 V |
| **Qgd (Miller)** | **2.2 nC typ** | **2.2 nC typ** |

We drive the gate from a 3.3 V GPIO, and that is what decides it.

The AO3407's threshold is **-2.4 V worst case**, leaving only 0.9 V of
overdrive at 3.3 V, and its RDS(on) is not characterised below -4.5 V --
so at our gate drive we would be operating in a region the manufacturer
does not guarantee. It works in practice (typical threshold is -1.9 V),
which is why nRFMicro gets away with it, but it is not a specified
operating point. The AO3415 is built for exactly this: threshold -0.9 V
worst case, characterised all the way down to -1.5 V, and its datasheet
says outright it is "suitable for use as a load switch applications"
with "operation with gate voltages as low as 1.8V".

The resistance difference matters more here than it looks. At 1.2 A the
AO3415 drops roughly 50 mV; the AO3407 at its -4.5 V spec point drops
over 100 mV, and more at 3.3 V drive. Our SK6812s already run on a 3.3 V
rail, which is low for them -- `issues.md` notes the backlit LEDs "do
not show blue very well", exactly the symptom of thin headroom, since
blue has the highest forward voltage. Do not spend another 100 mV on the
switch.

AO3407's one genuine advantage is half the total gate charge, which
matters for fast PWM switching. We switch this rail perhaps twice a
second, so it buys nothing -- and note `Qgd`, the Miller charge that
actually sets drain dV/dt during turn-on, is 2.2 nC typ for *both*
parts. On the parameter that governs inrush they are identical.

Its higher VDS and VGS ratings are likewise irrelevant at 3.3 V --
though note the AO3415's +/-8 V VGS limit means it cannot be reused
as-is on a 12 V rail.

### Gotchas for the LED rail switch

Kept because they explain why the circuit looks the way it does, and
because points 2 and 3 are firmware requirements, not board ones.

- **Size the FET for the LEDs, not the quiescent draw.** 20 SK6812s at
  full white is ~1.2 A, so size the part for that -- the AO3415A's 5 A
  rating covers it with room to spare.
- **Drive `LED_DAT` low when the rail is off.** With D6 still driving
  high into an unpowered SK6812, current sneaks in through the input
  protection diode and partially powers the strip -- which defeats the
  switch. Set the data pin low (or to an input) before cutting power.
- **Sequence the other way on startup:** enable the rail, let it settle,
  *then* drive data. A `show()` into a rail that is still coming up
  back-powers it through that same diode.
- **Add a series gate resistor for inrush -- the pull-up will not do
  it.** The LED rail carries 21x 100n, ~2 uF, and switching that on hard
  can sag 3V3 enough to reset the XIAO. The 100K pull-up does not help:
  the MCU actively sinks the gate to turn on, so turn-on is fast whatever
  the pull-up is, and the pull-up only slows turn-*off*, and only if the
  pin is released to high-Z instead of driven high. What limits inrush is
  a *separate* resistor in series between the GPIO and the gate (1-10k),
  slowing dV/dt in both directions. MagTag has no series resistor -- it
  only switches four LEDs. Note power-up is not a separate inrush event:
  the pin is an input, the pull-up holds the FET off, and nothing
  switches until firmware drives the gate low.
- **Check the gate drive, not just the current rating.** Confirm the
  part's Rds(on) is specified at a gate drive of 2.5 V or less, so 3.3 V
  puts it well into enhancement.
