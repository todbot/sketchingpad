# SPDX-FileCopyrightText: 2026 Tod Kurt
# SPDX-License-Identifier: GPL-3.0-or-later
#
# touch.py -- one touch driver over the TS20 and the 2x MPR121 boards
#
# The CircuitPython counterpart of arduino/shared/touch.h, same API in
# Python spelling: begin(), name(), read(), source(), fault(), report().
#
# sketchingpad1 has one 20-channel TS20 at 0x6A; sketchingpad1_mpr
# replaces it with two 12-channel MPR121s at 0x5A and 0x5B.  The addresses
# are disjoint, so the board is identified at runtime and one code.py runs
# on both.
#
# Everything here is in LED-index space: bit n of read() is LED n, which
# is TP(n+1) on the silkscreen.  Callers never see a channel or electrode.
#
# Three places this deliberately differs from touch.h, each because the
# CircuitPython driver underneath differs -- see the functions for detail:
#   set_low_power()  is a begin() argument instead
#   report()         expects ECR 0x8F, not the Arduino's 0x8C
#   arm_wake()       does the sensor half only; CircuitPython builds its
#                    wake alarm at sleep time, not in advance

import time

import adafruit_mpr121
import board
import digitalio
import ts20

NUM_LEDS = 20

TS20_ADDR = 0x6A
MPR_ADDRS = (0x5A, 0x5B)
MPR_NAMES = ("U3", "U4")

DEFAULT_SENSITIVITY = 15  # TS20: 0 = most sensitive, 15 = least

# CONFIG2 bits 2-0 are ESI, the electrode sample interval, and most of the
# sensor's idle current.  What it costs is latency: the touch status update
# rate is ESI x SFI, so 0x24 responds in 64 ms rather than 4 ms.
#   0x20 = ESI 1 ms   393 uA/chip      0x24 = ESI 16 ms   29 uA/chip
CONFIG2_RUN = 0x20
CONFIG2_LOWPOWER = 0x24

# Not in the library's register constants.
_MPR121_OOR_L = 0x02
_MPR121_OOR_H = 0x03

# What adafruit_mpr121.reset() leaves in the ECR: baseline tracking on,
# ELE_EN 15.  The Arduino library writes 0x8C for the same "running"
# state, so touch.h checks against that; checking for 0x8C here would warn
# on every healthy boot.
ECR_RUN = 0x8F

# Two MPR121s wired-OR onto one line means a chip can re-assert in the gap
# between the two status reads, leaving the line low with no new edge
# behind it -- and nothing would ever read it again.  So read() chases the
# level down, bounded, because a chip stuck low would hang the caller.
MAX_PASSES = 4

# Value is the sensor bit for that LED: the TS20 channel, or for the
# MPR121s the bit in the combined word, chip * 12 + ELEn.  Derived from the
# netlist per board rev -- arduino/shared/touch_map.h is the source of
# truth and these must stay identical to it.
#
#            TP17 TP18 TP19 TP20
#   TP1  TP2  TP3  TP4   TP5  TP6  TP7  TP8
#   TP9 TP10 TP11 TP12  TP13 TP14 TP15 TP16
LED_TO_TS20 = (0, 1, 2, 3, 8, 9, 10, 11, 16, 17, 18, 19, 12, 13, 14, 15, 4, 5, 6, 7)
LED_TO_MPR = (6, 7, 8, 9, 2, 3, 4, 5, 20, 21, 22, 23, 12, 13, 14, 15, 10, 11, 0, 1)

_ts20 = None
_mpr = None
_int = None
_fault = False
_ovcf = [False, False]
_passes = 0


def begin(i2c, *, sensitivity=DEFAULT_SENSITIVITY, low_power=False, int_pin=board.D3):
    """Probe 0x6A then 0x5A/0x5B and bring up whichever sensor answers.

    Returns True if one was found.

    ``low_power`` is the begin() argument that touch.h spells
    touchSetLowPower().  It belongs here rather than in a setter: the
    CircuitPython TS20 driver only takes fast_mode at construction, and
    touch.h's own note says to set this once in setup anyway, since both
    backends soft-reset on the write and would swallow a touch in
    progress.
    """
    global _ts20, _mpr, _int

    while not i2c.try_lock():
        pass
    found = i2c.scan()
    i2c.unlock()

    # Input with no pull: R1/R16 already pulls this up on every board rev.
    # Set even though nothing here waits on an edge -- read() tests the
    # level to decide whether a chip re-asserted mid-read.
    _int = digitalio.DigitalInOut(int_pin)
    _int.switch_to_input()

    if TS20_ADDR in found:
        _ts20 = ts20.TS20(i2c, sensitivity=sensitivity, fast_mode=not low_power)
        return True

    if not all(a in found for a in MPR_ADDRS):
        return False

    _mpr = [adafruit_mpr121.MPR121(i2c, address=a) for a in MPR_ADDRS]
    cfg = CONFIG2_LOWPOWER if low_power else CONFIG2_RUN
    for chip in _mpr:
        chip._write_register_byte(adafruit_mpr121.MPR121_CONFIG2, cfg)

    # Park the four J2 electrodes at an unreachable threshold so they
    # cannot chatter the shared IRQ.  Leaving them out of LED_TO_MPR is not
    # enough: the line is wired-OR in hardware and read() watches it.
    for e in range(4, 8):
        _mpr[1][e].threshold = 0xFF

    time.sleep(0.1)  # those writes cycle the chip through Stop Mode
    return True


def name():
    if _ts20 is not None:
        return "TS20"
    if _mpr is not None:
        return "2x MPR121"
    return "none"


def int_pin():
    """The shared INT/IRQ DigitalInOut, D3 on every board rev.

    Call release_int() before alarm.pin.PinAlarm() wants this pin:
    CircuitPython will not hand the same pin to an alarm while a
    DigitalInOut still holds it.
    """
    return _int


def release_int():
    """Free the INT pin so a PinAlarm can claim it. read() then single-passes."""
    global _int
    if _int is not None:
        _int.deinit()
        _int = None


def read():
    """Bit n = LED n = TP(n+1), 20 bits."""
    global _fault, _passes

    if _ts20 is not None:
        _passes = 1
        channels = _ts20.touched()
        _fault = _ts20.noise_detected  # a second I2C read
        return _map(LED_TO_TS20, channels)

    if _mpr is None:
        return 0

    buf = bytearray(2)
    _passes = 0
    while True:
        electrodes = 0
        for i, chip in enumerate(_mpr):
            # Not touched(), which masks to 12 bits and so discards OVCF --
            # the difference between a chip that is idle and one that has
            # faulted to Stop Mode and is ignoring writes.
            chip._read_register_bytes(adafruit_mpr121.MPR121_TOUCHSTATUS_L, buf, 2)
            raw = buf[0] | (buf[1] << 8)
            _ovcf[i] = bool((raw >> 15) & 1)
            electrodes |= (raw & 0x0FFF) << (12 * i)
        _passes += 1
        if _passes >= MAX_PASSES or _int is None or _int.value:
            break

    _fault = _ovcf[0] or _ovcf[1]
    return _map(LED_TO_MPR, electrodes)


def _map(table, bits):
    leds = 0
    for led in range(NUM_LEDS):
        if (bits >> table[led]) & 1:
            leds |= 1 << led
    return leds


def read_passes():
    """Status reads the last read() took. >1 means a chip re-asserted
    mid-read; always 1 on the TS20."""
    return _passes


def fault():
    """TS20 noise flag, or either MPR121's OVCF. Updated by read()."""
    return _fault


def source(led):
    """Where LED n comes from, for diagnostic prints."""
    if led >= NUM_LEDS:
        return "?"
    if _ts20 is not None:
        return "ch%d" % LED_TO_TS20[led]
    bit = LED_TO_MPR[led]
    return "%s ELE%d" % (MPR_NAMES[bit // 12], bit % 12)


def arm_wake():
    """Read the sensor in the order that makes a wake source safe to arm,
    and report whether sleeping is still a good idea. False means a touch
    arrived: do not sleep.

    Only the sensor half of touch.h's touchArmWake().  That function also
    arms nRF GPIO SENSE, which has no CircuitPython equivalent -- an
    alarm.pin.PinAlarm is built at the moment of sleeping, so the caller
    does that itself, after this returns True.

    The read still has to happen first on the MPR121: the read is what
    releases the held-low line, and a PinAlarm on a pin that is already low
    fires the instant you sleep. On the TS20 the ~1 ms pulse is long gone,
    and the read closes the software race instead, the chip still holding
    the state in its output register. Same call either way, which is why
    this is not a flag the sketch branches on.
    """
    return read() == 0


def report():
    """Whatever this sensor can say about its own health, printed."""
    if _ts20 is not None:
        print("TS20: sensitivity %d, pad 0 needs a %.2f%% capacitance change, noise=%d"
              % (_ts20.sensitivity[0], _ts20.sensitivity_percent(0),
                 _ts20.noise_detected))
        return
    if _mpr is None:
        return

    buf = bytearray(1)
    for i, chip in enumerate(_mpr):
        chip._read_register_bytes(adafruit_mpr121.MPR121_ECR, buf, 1)
        ecr = buf[0]
        chip._read_register_bytes(adafruit_mpr121.MPR121_CONFIG2, buf, 1)
        config2 = buf[0]
        chip._read_register_bytes(_MPR121_OOR_L, buf, 1)
        oor_l = buf[0]
        chip._read_register_bytes(_MPR121_OOR_H, buf, 1)
        oor_h = buf[0]
        oor = ((oor_h & 0x0F) << 8) | oor_l

        print("%s @0x%02x: ECR=%02x CONFIG2=%02x OVCF=%d OOR=%03x"
              % (MPR_NAMES[i], MPR_ADDRS[i], ecr, config2, _ovcf[i], oor))

        # A chip that has tripped OVCF discards writes and stays in Stop
        # Mode, which otherwise looks exactly like nobody touching anything.
        if ecr != ECR_RUN:
            print("  ECR should be %02x -- %s is not in Run Mode" % (ECR_RUN, MPR_NAMES[i]))
        if oor:
            print("  out-of-range electrodes: no working charge setting for these")
