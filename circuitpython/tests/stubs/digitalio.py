# SPDX-FileCopyrightText: 2026 Tod Kurt
# SPDX-License-Identifier: GPL-3.0-or-later
#
# digitalio -- enough of it for the INT line.
#
# `value` starts True because the real line is pulled up by R1/R16 and only
# a sensor pulls it down; a stub that started False would make every test
# take the IRQ-still-asserted path.

class DigitalInOut:
    def __init__(self, pin):
        self.pin = pin
        self.value = True
        self.deinited = False

    def switch_to_input(self, **kwargs):
        pass

    def switch_to_output(self, value=False, **kwargs):
        self.value = value

    def deinit(self):
        self.deinited = True
