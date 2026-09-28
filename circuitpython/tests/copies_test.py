# SPDX-FileCopyrightText: 2026 Tod Kurt
# SPDX-License-Identifier: GPL-3.0-or-later
#
# copies_test.py -- touch.py is copied per sketch, like the Arduino headers
#
# Each CIRCUITPY root gets one flat pile of files, so a sketch that imports
# touch.py needs its own copy next to code.py. `make -C arduino check` does
# this job for the C side but only syncs .h files; these copies are outside
# it entirely.

import hashlib
import os
import unittest

from test_util import HERE

CIRCUITPYTHON = os.path.abspath(os.path.join(HERE, os.pardir))


def copies_of(filename):
    found = []
    for root, dirs, files in os.walk(CIRCUITPYTHON):
        dirs[:] = [d for d in dirs if d not in ("tests", "__pycache__")]
        if filename in files:
            found.append(os.path.join(root, filename))
    return sorted(found)


class TouchCopies(unittest.TestCase):
    def setUp(self):
        self.copies = copies_of("touch.py")

    def test_there_is_more_than_one_to_compare(self):
        # If this ever drops to one the suite below is vacuous, so say so
        # rather than passing silently.
        self.assertGreaterEqual(len(self.copies), 2, "only one touch.py: %s" % self.copies)

    def test_every_copy_is_identical(self):
        digests = {}
        for path in self.copies:
            with open(path, "rb") as fh:
                digests.setdefault(hashlib.sha256(fh.read()).hexdigest(), []).append(
                    os.path.relpath(path, CIRCUITPYTHON)
                )
        self.assertEqual(
            len(digests), 1,
            "touch.py copies have drifted:\n" + "\n".join(
                "  %s  %s" % (d[:12], ", ".join(paths)) for d, paths in digests.items()))


class SketchesUsingTouch(unittest.TestCase):
    """A sketch that imports touch must have it sitting next to code.py."""

    def test_importers_have_a_local_copy(self):
        for root, dirs, files in os.walk(CIRCUITPYTHON):
            dirs[:] = [d for d in dirs if d not in ("tests", "__pycache__")]
            if "code.py" not in files:
                continue
            source = open(os.path.join(root, "code.py")).read()
            if "import touch" not in source:
                continue
            self.assertIn("touch.py", files,
                          "%s imports touch but has no copy" % os.path.relpath(root, CIRCUITPYTHON))

    def test_no_sketch_still_drives_a_sensor_directly(self):
        # The point of touch.py is that no sketch reaches past it. A stray
        # `import ts20` would also quietly reintroduce a per-board pad table.
        for root, dirs, files in os.walk(CIRCUITPYTHON):
            dirs[:] = [d for d in dirs if d not in ("tests", "__pycache__", "hwtest", "hwtest-orig")]
            if "code.py" not in files:
                continue
            source = open(os.path.join(root, "code.py")).read()
            where = os.path.relpath(root, CIRCUITPYTHON)
            self.assertNotIn("import ts20", source, "%s imports ts20 directly" % where)
            self.assertNotIn("import adafruit_mpr121", source,
                             "%s imports adafruit_mpr121 directly" % where)
