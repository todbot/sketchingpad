# SPDX-FileCopyrightText: 2026 Tod Kurt
# SPDX-License-Identifier: GPL-3.0-or-later
#
# touch_test.py -- touch.py against stub sensors
#
# No I2C, no board. What this can prove is that the driver reads the right
# register, keeps the right bits and puts them in the right place; what it
# cannot prove is that the real chips behave as the stubs do.

import contextlib
import io
import unittest

from test_util import FakeI2C, load_touch


def report_text(touch):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        touch.report()
    return buf.getvalue()


class Ts20Backend(unittest.TestCase):
    def setUp(self):
        self.touch = load_touch()
        self.assertTrue(self.touch.begin(FakeI2C([0x6A])))
        self.dev = self.touch._ts20

    def test_name(self):
        self.assertEqual(self.touch.name(), "TS20")

    def test_every_channel_lights_its_own_led(self):
        for led in range(20):
            self.dev.bits = 1 << self.touch.LED_TO_TS20[led]
            self.assertEqual(self.touch.read(), 1 << led)

    def test_read_is_always_one_pass(self):
        self.touch.read()
        self.assertEqual(self.touch.read_passes(), 1)

    def test_noise_flag_surfaces_as_fault(self):
        self.touch.read()
        self.assertFalse(self.touch.fault())
        self.dev.noise_detected = True
        self.touch.read()
        self.assertTrue(self.touch.fault())

    def test_source_names_the_channel(self):
        self.assertEqual(self.touch.source(0), "ch0")
        self.assertEqual(self.touch.source(16), "ch4")  # TP17 is not channel 16
        self.assertEqual(self.touch.source(99), "?")

    def test_low_power_reaches_the_driver_as_fast_mode(self):
        # The only observable effect: the CircuitPython driver takes
        # fast_mode at construction and offers no runtime setter.
        self.assertTrue(self.dev.fast_mode)
        other = load_touch()
        other.begin(FakeI2C([0x6A]), low_power=True)
        self.assertFalse(other._ts20.fast_mode)


class MprBackend(unittest.TestCase):
    def setUp(self):
        self.touch = load_touch()
        self.assertTrue(self.touch.begin(FakeI2C([0x5A, 0x5B])))
        self.u3, self.u4 = self.touch._mpr

    def press(self, bit):
        self.u3.status = self.u4.status = 0
        chip = self.u3 if bit < 12 else self.u4
        chip.status = 1 << (bit % 12)

    def test_name(self):
        self.assertEqual(self.touch.name(), "2x MPR121")

    def test_every_electrode_lights_its_own_led(self):
        for led in range(20):
            self.press(self.touch.LED_TO_MPR[led])
            self.assertEqual(self.touch.read(), 1 << led)

    def test_config2_run_value_written_to_both_chips(self):
        for chip in (self.u3, self.u4):
            self.assertEqual(chip.regs[0x5D], self.touch.CONFIG2_RUN)

    def test_low_power_writes_the_slower_config2(self):
        other = load_touch()
        other.begin(FakeI2C([0x5A, 0x5B]), low_power=True)
        for chip in other._mpr:
            self.assertEqual(chip.regs[0x5D], other.CONFIG2_LOWPOWER)

    def test_only_the_four_j2_electrodes_are_parked(self):
        for e in range(4, 8):
            self.assertEqual(self.u4[e].threshold, 0xFF)
        for e in (0, 1, 2, 3, 8, 9, 10, 11):
            self.assertEqual(self.u4[e].threshold, 12)
        for e in range(12):
            self.assertEqual(self.u3[e].threshold, 12)  # U3 is all real pads

    def test_ovcf_survives_the_read(self):
        # touched() masks to 12 bits and would drop bit 15, which is the
        # difference between an idle chip and one stuck in Stop Mode.
        self.touch.read()
        self.assertFalse(self.touch.fault())
        self.u3.status = 1 << 15
        self.touch.read()
        self.assertTrue(self.touch.fault())

    def test_ovcf_is_not_mistaken_for_a_touch(self):
        self.u3.status = 1 << 15
        self.assertEqual(self.touch.read(), 0)

    def test_held_low_irq_rereads_and_is_bounded(self):
        # Two chips wired-OR onto one line: one can re-assert between the
        # two status reads, leaving the line low with no edge behind it.
        self.touch._int.value = False
        self.touch.read()
        self.assertEqual(self.touch.read_passes(), self.touch.MAX_PASSES)

    def test_released_irq_reads_once(self):
        self.touch._int.value = True
        self.touch.read()
        self.assertEqual(self.touch.read_passes(), 1)

    def test_source_names_chip_and_electrode(self):
        self.assertEqual(self.touch.source(0), "U3 ELE6")
        self.assertEqual(self.touch.source(8), "U4 ELE8")

    def test_report_is_quiet_on_a_healthy_chip(self):
        # The CircuitPython driver leaves ECR 0x8F where the Arduino one
        # leaves 0x8C. Checking for 0x8C would warn on every healthy boot,
        # which is how a diagnostic gets trained out of usefulness.
        text = report_text(self.touch)
        self.assertIn("ECR=8f", text)
        self.assertNotIn("not in Run Mode", text)

    def test_report_complains_when_a_chip_left_run_mode(self):
        self.u3.regs[0x5E] = 0x00
        text = report_text(self.touch)
        self.assertIn("not in Run Mode", text)
        self.assertIn("U3", text)

    def test_report_names_out_of_range_electrodes(self):
        self.u3.regs[0x02] = 0x05
        self.assertIn("out-of-range", report_text(self.touch))


class Probe(unittest.TestCase):
    def test_no_sensor_found(self):
        touch = load_touch()
        self.assertFalse(touch.begin(FakeI2C([0x42])))
        self.assertEqual(touch.name(), "none")
        self.assertEqual(touch.read(), 0)

    def test_one_mpr_alone_is_not_enough(self):
        # Half a board is a wiring fault, not a supported configuration.
        touch = load_touch()
        self.assertFalse(touch.begin(FakeI2C([0x5A])))

    def test_ts20_wins_when_both_answer(self):
        touch = load_touch()
        self.assertTrue(touch.begin(FakeI2C([0x6A, 0x5A, 0x5B])))
        self.assertEqual(touch.name(), "TS20")


class WakeAndSleep(unittest.TestCase):
    def setUp(self):
        self.touch = load_touch()
        self.touch.begin(FakeI2C([0x5A, 0x5B]))

    def test_arm_wake_is_true_only_when_nothing_is_held(self):
        self.assertTrue(self.touch.arm_wake())
        self.touch._mpr[0].status = 1 << self.touch.LED_TO_MPR[0]
        self.assertFalse(self.touch.arm_wake())

    def test_release_int_frees_the_pin_for_a_pinalarm(self):
        pin = self.touch.int_pin()
        self.touch.release_int()
        self.assertTrue(pin.deinited)
        self.assertIsNone(self.touch.int_pin())

    def test_read_still_works_after_release_int(self):
        self.touch.release_int()
        self.touch.read()
        self.assertEqual(self.touch.read_passes(), 1)
