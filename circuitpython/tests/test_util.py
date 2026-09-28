# SPDX-FileCopyrightText: 2026 Tod Kurt
# SPDX-License-Identifier: GPL-3.0-or-later
#
# test_util.py -- put the stubs on the path and load the module under test.
#
# touch.py is loaded by path rather than imported, and reloaded per test,
# because it keeps its sensor in module globals: a second begin() on an
# already-populated module would test the wrong thing.

import ast
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SKETCH = os.path.join(HERE, os.pardir, "sketchingpad_midi_simple")
ARDUINO_SHARED = os.path.join(HERE, os.pardir, os.pardir, "arduino", "shared")

if os.path.join(HERE, "stubs") not in sys.path:
    sys.path.insert(0, os.path.join(HERE, "stubs"))


def load_touch():
    """A freshly initialised touch module, stubs already on the path."""
    for name in ("touch", "ts20", "adafruit_mpr121", "board", "digitalio"):
        sys.modules.pop(name, None)
    spec = importlib.util.spec_from_file_location("touch", os.path.join(SKETCH, "touch.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def module_constants(path):
    """Top-level literal assignments of a file, without importing it.

    code.py ends in `while True:`, so it cannot be imported to be read.
    """
    tree = ast.parse(open(path).read())
    out = {}
    for node in tree.body:
        if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name):
            try:
                out[node.targets[0].id] = ast.literal_eval(node.value)
            except (ValueError, SyntaxError):
                pass
    return out


def c_array(header, name):
    """One `static const uint8_t name[TOUCH_NUM_LEDS] = {...}` from a header."""
    import re

    text = open(header).read()
    match = re.search(name + r"\[TOUCH_NUM_LEDS\]\s*=\s*\{(.*?)\};", text, re.S)
    assert match, "%s not found in %s" % (name, header)
    return tuple(int(x) for x in re.findall(r"\d+", match.group(1)))


class FakeI2C:
    """try_lock/scan/unlock, which is all touch.begin() asks of a bus."""

    def __init__(self, addresses):
        self.addresses = list(addresses)

    def try_lock(self):
        return True

    def unlock(self):
        pass

    def scan(self):
        return self.addresses
