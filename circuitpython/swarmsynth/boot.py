# SPDX-FileCopyrightText: 2026 Tod Kurt
# SPDX-License-Identifier: GPL-3.0-or-later
#
# boot.py -- turn on the USB MIDI device, for swarmsynth
#
# usb_midi.ports is empty unless USB MIDI is enabled here, and boot.py only
# runs from the CIRCUITPY root, not from a subfolder.  Copy both this and
# code.py to the root, then hard reset: a soft reload will not re-run it.

import usb_midi

usb_midi.enable()
