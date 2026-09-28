# SPDX-FileCopyrightText: 2026 Tod Kurt
# SPDX-License-Identifier: GPL-3.0-or-later
#
# adafruit_mpr121 -- stands in for the Adafruit CircuitPython driver.
#
# `status` is the full 16-bit touch status register, not the 12 bits
# touched() would return: OVCF lives in bit 15 and reading it is the whole
# reason touch.py bypasses touched().
#
# Register defaults are what the real driver's reset() leaves behind, ECR
# 0x8F included -- the Arduino library writes 0x8C for the same state, and
# a stub that returned 0x8C would hide that difference instead of pinning it.

MPR121_I2CADDR_DEFAULT = 0x5A
MPR121_TOUCHSTATUS_L = 0x00
MPR121_TOUCHTH_0 = 0x41
MPR121_RELEASETH_0 = 0x42
MPR121_CONFIG1 = 0x5C
MPR121_CONFIG2 = 0x5D
MPR121_ECR = 0x5E

_RESET_DEFAULTS = {MPR121_ECR: 0x8F, MPR121_CONFIG1: 0x10, MPR121_CONFIG2: 0x20}


class MPR121_Channel:
    def __init__(self):
        self.threshold = 12
        self.release_threshold = 6


class MPR121:
    def __init__(self, i2c, address=MPR121_I2CADDR_DEFAULT):
        self.address = address
        self.status = 0
        self.regs = dict(_RESET_DEFAULTS)
        self.channels = [MPR121_Channel() for _ in range(12)]

    def __getitem__(self, key):
        return self.channels[key]

    def touched(self):
        return self.status & 0x0FFF

    def _write_register_byte(self, register, value):
        self.regs[register] = value

    def _read_register_bytes(self, register, result, length=None):
        if length is None:
            length = len(result)
        if register == MPR121_TOUCHSTATUS_L:
            result[0] = self.status & 0xFF
            if length > 1:
                result[1] = (self.status >> 8) & 0xFF
            return
        result[0] = self.regs.get(register, 0)
