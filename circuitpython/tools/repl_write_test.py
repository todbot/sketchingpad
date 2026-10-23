#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Tod Kurt
# SPDX-License-Identifier: GPL-3.0-or-later
#
# repl_write_test.py -- can a host write CIRCUITPY over the serial REPL?
#
# The question a web flasher for CircuitPython apps hangs on.  Does over
# pyserial what the page would do over Web Serial: raw REPL,
# storage.unsafe_disable_usb_drive(), write a file in base64 chunks,
# reset, then check the file survived and the host sees the drive again.
#
#   python3 repl_write_test.py [/dev/cu.usbmodemXXXX] [kbytes]
#
# Needs pyserial.  Close the Serial Monitor first.  Leaves the board
# running whatever code.py it had; the test file is deleted at the end.

import base64
import glob
import hashlib
import os
import sys
import time

import serial

TEST_FILE = "repl_test.bin"
CHUNK = 3072  # raw bytes per exec, 4096 once base64'd
VOLUME = "/Volumes/CIRCUITPY"


def find_port():
    ports = glob.glob("/dev/cu.usbmodem*")
    if len(ports) != 1:
        sys.exit("found %s: name the port" % (ports or "no board"))
    return ports[0]


def open_port(port, wait=10.0):
    # The port vanishes across a reset and comes back under the same name.
    global pending
    pending = b""
    end = time.monotonic() + wait
    while True:
        try:
            return serial.Serial(port, 115200, timeout=0.1)
        except serial.SerialException:
            if time.monotonic() > end:
                raise
            time.sleep(0.2)


pending = b""  # read past the last token, owed to the next read_until


def read_until(ser, token, wait=5.0):
    """Up to and including `token`."""
    global pending
    buf = pending
    end = time.monotonic() + wait
    while token not in buf:
        if time.monotonic() > end:
            raise TimeoutError("wanted %r, got %r" % (token, buf[-200:]))
        buf += ser.read(256)
    i = buf.index(token) + len(token)
    buf, pending = buf[:i], buf[i:]
    return buf


def raw_repl(ser):
    """Stop code.py and enter the raw REPL. Returns the banner text."""
    ser.write(b"\x03\x03")
    time.sleep(0.3)
    # Ctrl-B leaves a raw REPL an earlier run left behind; in any other
    # state it is the key that "Press any key to enter the REPL" wants.
    ser.write(b"\x02")
    time.sleep(0.3)
    ser.write(b"\r")
    banner = read_until(ser, b">>> ")
    ser.write(b"\x01")
    read_until(ser, b"raw REPL; CTRL-B to exit\r\n>")
    return banner.decode(errors="replace")


def run(ser, code, wait=5.0):
    """Exec in the raw REPL. Returns stdout, raises on a traceback."""
    ser.write(code.encode() + b"\x04")
    read_until(ser, b"OK", wait)
    out = read_until(ser, b"\x04", wait)[:-1]
    err = read_until(ser, b"\x04", wait)[:-1]
    read_until(ser, b">", wait)
    if err:
        raise RuntimeError(err.decode(errors="replace"))
    return out.decode(errors="replace").strip()


def reset(ser):
    # The board drops USB before it can answer, so no reply is expected.
    ser.write(b"import microcontroller; microcontroller.reset()\x04")
    ser.close()
    time.sleep(2.0)


def step(name):
    print("-- " + name)


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else find_port()
    kbytes = int(sys.argv[2]) if len(sys.argv) > 2 else 32
    payload = os.urandom(kbytes * 1024)
    digest = hashlib.sha256(payload).hexdigest()

    step("raw REPL on " + port)
    ser = open_port(port)
    banner = raw_repl(ser)
    print([ln for ln in banner.splitlines() if "CircuitPython" in ln][-1:])
    print(run(ser, "import os; print(os.uname())"))
    has = run(ser, "import storage; "
                   "print(hasattr(storage, 'unsafe_disable_usb_drive'))")
    print("unsafe_disable_usb_drive:", has)
    if has != "True":
        sys.exit("FAIL: this CircuitPython has no unsafe_disable_usb_drive")

    step("disable the drive and write")
    run(ser, "storage.unsafe_disable_usb_drive()")
    try:
        run(ser, "f = open('/%s', 'wb')" % TEST_FILE)
        print("writable without remount")
    except RuntimeError as e:
        print("open failed (%s), trying remount" % e.splitlines()[-1])
        run(ser, "storage.remount('/', False)")
        run(ser, "f = open('/%s', 'wb')" % TEST_FILE)
        print("writable after remount")

    run(ser, "import binascii; w = lambda s: f.write(binascii.a2b_base64(s))")
    t0 = time.monotonic()
    for i in range(0, len(payload), CHUNK):
        run(ser, "w(%r)" % base64.b64encode(payload[i:i + CHUNK]).decode())
    run(ser, "f.close()")
    secs = time.monotonic() - t0
    print("%d KB in %.1f s, %.1f KB/s" % (kbytes, secs, kbytes / secs))
    print("size on board:", run(ser, "print(os.stat('/%s')[6])" % TEST_FILE))

    step("reset")
    reset(ser)
    ser = open_port(port)
    raw_repl(ser)
    size = run(ser, "import os; print(os.stat('/%s')[6])" % TEST_FILE)
    print("survived reset, size", size)

    step("host side")
    end = time.monotonic() + 15
    path = os.path.join(VOLUME, TEST_FILE)
    while not os.path.exists(path) and time.monotonic() < end:
        time.sleep(0.5)
    if not os.path.exists(path):
        sys.exit("FAIL: %s never showed up on the host" % path)
    with open(path, "rb") as fh:
        ok = hashlib.sha256(fh.read()).hexdigest() == digest
    print("host sees it, sha256 %s" % ("matches" if ok else "DIFFERS"))
    os.remove(path)
    ser.write(b"\x02\x04")  # leave the raw REPL, soft reboot into code.py
    ser.close()
    print("PASS" if ok else "FAIL")


if __name__ == "__main__":
    main()
