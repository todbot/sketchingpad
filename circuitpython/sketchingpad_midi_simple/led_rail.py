# SPDX-FileCopyrightText: 2026 Tod Kurt
# SPDX-License-Identifier: GPL-3.0-or-later
#
# led_rail.py -- switch the LED rail on without CircuitPython's brownout
# safe mode
#
# The rail's P-FET snaps ~2 uF of strip decoupling onto 3.3 V and the dip
# crosses the nRF52 power-fail comparator, which CircuitPython arms at
# 2.7 V and answers with "Power dipped" safe mode.  The chip itself rides
# it out, as the Arduino sketches show.  So the comparator is switched
# off for the moment the rail comes on.
#
# The SoftDevice owns the POWER block while it runs, and a write to it
# then is an SDK fatal error, so it is stopped first.  That drops any BLE
# connection: call this at startup, not with a link up.

import time

import _bleio
import memorymap

POFCON = 0x40000510


def on(gate):
    """Drive the P-FET gate (a DigitalInOut output) low: rail on."""
    was = _bleio.adapter.enabled
    _bleio.adapter.enabled = False
    pofcon = memorymap.AddressRange(start=POFCON, length=4)
    saved = pofcon[0:4]
    pofcon[0:4] = bytes(4)
    gate.value = False
    time.sleep(0.05)
    pofcon[0:4] = saved
    _bleio.adapter.enabled = was
