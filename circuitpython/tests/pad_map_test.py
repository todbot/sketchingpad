# SPDX-FileCopyrightText: 2026 Tod Kurt
# SPDX-License-Identifier: GPL-3.0-or-later
#
# pad_map_test.py -- the pad tables in touch.py and the note table in code.py
#
# The CircuitPython half of arduino/tests/touch_map_test.cpp, plus the one
# check that suite cannot make: these tables are a second copy of the ones
# in arduino/shared/touch_map.h, in another language, and nothing in the
# build makes them agree.

import os
import unittest

from test_util import ARDUINO_SHARED, SKETCH, c_array, load_touch, module_constants

TOUCH_MAP_H = os.path.join(ARDUINO_SHARED, "touch_map.h")


class PadTables(unittest.TestCase):
    def setUp(self):
        self.touch = load_touch()

    def test_ts20_table_is_a_permutation(self):
        self.assertEqual(sorted(self.touch.LED_TO_TS20), list(range(20)))

    def test_mpr_bits_distinct_and_in_range(self):
        table = self.touch.LED_TO_MPR
        self.assertEqual(len(set(table)), 20)
        for bit in table:
            self.assertIn(bit // 12, (0, 1))
            self.assertLess(bit % 12, 12)

    def test_each_sensor_bit_lights_only_its_own_led(self):
        # A table read backwards is still a permutation, so the structural
        # checks above pass it. This is what catches that.
        for table in (self.touch.LED_TO_TS20, self.touch.LED_TO_MPR):
            for led in range(20):
                self.assertEqual(self.touch._map(table, 1 << table[led]), 1 << led)

    def test_bits_ascend_by_one_within_every_group_of_four(self):
        # Two transposed entries survive every other check here. Both boards
        # route the pads in blocks of four, so this does not. Ported from
        # arduino/tests/touch_map_test.cpp, exception included.
        for n in range(19):
            if n % 4 == 3:
                continue  # group boundary: no relation
            self.assertEqual(self.touch.LED_TO_TS20[n + 1],
                             self.touch.LED_TO_TS20[n] + 1, "TS20 at %d" % n)
            if n == 17:
                continue  # the one documented wrap, pinned below
            self.assertEqual(self.touch.LED_TO_MPR[n + 1],
                             self.touch.LED_TO_MPR[n] + 1, "MPR at %d" % n)

    def test_top_row_wraps_u3_ele11_to_ele0_and_only_there(self):
        # TP17-20 is the one block split across U3's electrode numbering.
        # Pinned so the exception above cannot quietly cover a real error.
        self.assertEqual(self.touch.LED_TO_MPR[17], 11)
        self.assertEqual(self.touch.LED_TO_MPR[18], 0)

    def test_j2_electrodes_reach_no_led(self):
        # U4 ELE4-7 have no pad and cannot be switched off in the ECR, so if
        # they ever chatter they must still paint nothing.
        self.assertFalse(set(self.touch.LED_TO_MPR) & {16, 17, 18, 19})

    def test_tables_differ_and_top_row_is_top_of_neither_range(self):
        self.assertNotEqual(self.touch.LED_TO_TS20, self.touch.LED_TO_MPR)
        for table in (self.touch.LED_TO_TS20, self.touch.LED_TO_MPR):
            self.assertNotEqual(sorted(table[16:]), sorted(table)[-4:])


class MatchesTheArduinoTables(unittest.TestCase):
    """The reason this file exists.

    arduino/shared/touch_map.h is the source of truth and `make -C arduino
    check` keeps its C copies honest, but it only syncs .h files -- these
    Python ones are outside that mechanism entirely.
    """

    def setUp(self):
        self.touch = load_touch()

    def test_ts20_table_matches_touch_map_h(self):
        self.assertEqual(self.touch.LED_TO_TS20, c_array(TOUCH_MAP_H, "touch_led_to_ts20"))

    def test_mpr_table_matches_touch_map_h(self):
        self.assertEqual(self.touch.LED_TO_MPR, c_array(TOUCH_MAP_H, "touch_led_to_mpr"))

    def test_led_count_matches(self):
        text = open(TOUCH_MAP_H).read()
        self.assertIn("#define TOUCH_NUM_LEDS 20", text)
        self.assertEqual(self.touch.NUM_LEDS, 20)


class NoteTable(unittest.TestCase):
    """code.py's LED_TO_NOTE. Read with ast: code.py ends in `while True`."""

    @classmethod
    def setUpClass(cls):
        cls.const = module_constants(os.path.join(SKETCH, "code.py"))
        cls.notes = list(cls.const["LED_TO_NOTE"])

    def test_sixteen_notes_in_midi_range(self):
        self.assertEqual(len(self.notes), 16)
        self.assertEqual(self.const["NUM_NOTE_PADS"], 16)
        for note in self.notes:
            self.assertTrue(0 <= note <= 127)

    def test_both_rows_ascend_left_to_right(self):
        self.assertEqual(self.notes[:8], sorted(self.notes[:8]))
        self.assertEqual(self.notes[8:], sorted(self.notes[8:]))

    def test_low_row_is_lower_than_the_middle_row(self):
        self.assertLess(max(self.notes[8:]), min(self.notes[:8]))

    def test_is_major_pentatonic_throughout(self):
        # Ordered low row then middle row, the notes are one run up the
        # scale. A wrong entry here is audible but not obvious on the bench.
        ordered = self.notes[8:] + self.notes[:8]
        self.assertEqual(ordered, sorted(ordered))
        for note in ordered:
            self.assertIn(note % 12, (0, 2, 4, 7, 9))  # C D E G A

    def test_banner_prints_the_real_lowest_and_highest(self):
        # The startup banner hardcodes LED_TO_NOTE[8] and [7] as the range.
        self.assertEqual(min(self.notes), self.notes[8])
        self.assertEqual(max(self.notes), self.notes[7])


class ControlPads(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.const = module_constants(os.path.join(SKETCH, "code.py"))

    def test_four_control_pads_are_the_top_row_and_distinct(self):
        pads = [self.const[n] for n in
                ("PAD_BEND_DOWN", "PAD_BEND_UP", "PAD_MOD_MID", "PAD_MOD_MAX")]
        self.assertEqual(sorted(pads), [16, 17, 18, 19])
        self.assertEqual(len(set(pads)), 4)

    def test_control_pads_do_not_overlap_the_note_pads(self):
        for pad in ("PAD_BEND_DOWN", "PAD_BEND_UP", "PAD_MOD_MID", "PAD_MOD_MAX"):
            self.assertGreaterEqual(self.const[pad], self.const["NUM_NOTE_PADS"])

    def test_bend_range_is_centred_on_zero(self):
        # tmidi's pitch_bend property is the wire value minus 8192. Constants
        # carried over from a 0-16383 scheme would still look plausible.
        self.assertEqual(self.const["BEND_MIN"], -8192)
        self.assertEqual(self.const["BEND_CENTER"], 0)
        self.assertEqual(self.const["BEND_MAX"], 8191)
