# SPDX-FileCopyrightText: 2026 Tod Kurt
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ts20 -- stands in for todbot's CircuitPython_TS20 driver.
#
# Tests drive `bits` and `noise_detected` directly. `fast_mode` is recorded
# rather than acted on, because that is the only thing touch.py's
# low_power argument can be observed to do.

NUM_PADS = 20
DEFAULT_SENSITIVITY = 15


class TS20:
    def __init__(self, i2c, address=0x6A, *, sensitivity=DEFAULT_SENSITIVITY,
                 fast_mode=True, **kwargs):
        self.address = address
        self.fast_mode = fast_mode
        self.sensitivity = [sensitivity] * NUM_PADS
        self.bits = 0
        self.noise_detected = False

    def touched(self):
        return self.bits

    def sensitivity_percent(self, pad):
        return 0.2 * (self.sensitivity[pad] + 1)
