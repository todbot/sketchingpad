#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Tod Kurt
# SPDX-License-Identifier: GPL-3.0-or-later
#
# uf2_to_hex.py -- a CircuitPython UF2 as Intel hex, for adafruit-nrfutil
#
#   python3 uf2_to_hex.py in.uf2 out.hex
#
# circuitpython.org ships the XIAO only as UF2, and getting to the UF2
# drive takes the reset button.  As hex it packages into a DFU zip that
# goes over serial like the Arduino sketches.  Needs intelhex
# (pip3 install intelhex).

import struct
import sys

from intelhex import IntelHex

UF2_MAGIC = (0x0A324655, 0x9E5D5157)
FAMILY_NRF52840 = 0xADA52840
FLAG_FAMILY = 0x2000
# The app start for S140 7.3.0, which the XIAO bootloader carries.  An
# image built for another SoftDevice would overwrite it.
APP_START = 0x27000


def main():
    src, dst = sys.argv[1:3]
    data = open(src, "rb").read()
    ih = IntelHex()
    for i in range(0, len(data), 512):
        m0, m1, flags, addr, n, _, _, fam = struct.unpack_from("<8I", data, i)
        if (m0, m1) != UF2_MAGIC:
            sys.exit("%s: not a UF2 at block %d" % (src, i // 512))
        if not flags & FLAG_FAMILY or fam != FAMILY_NRF52840:
            sys.exit("%s: not an nRF52840 UF2" % src)
        ih.puts(addr, data[i + 32:i + 32 + n])
    if ih.minaddr() != APP_START:
        sys.exit("%s: starts at %#x, not %#x" % (src, ih.minaddr(), APP_START))
    ih.write_hex_file(dst)
    print("%s: %#x-%#x" % (dst, ih.minaddr(), ih.maxaddr() + 1))


if __name__ == "__main__":
    main()
