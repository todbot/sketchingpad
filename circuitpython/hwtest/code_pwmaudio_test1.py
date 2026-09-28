# SPDX-FileCopyrightText: 2026 Tod Kurt
# SPDX-License-Identifier: GPL-3.0-or-later

import board, time, random
import synthio
import audiopwmio

# PWM_AUDIO is XIAO D0 on sketchingpad_proto2 (P0.02 -> R5)
audio = audiopwmio.PWMAudioOut(board.D0)

synth = synthio.Synthesizer(sample_rate=22050)
amp_env = synthio.Envelope(attack_time=0.01, release_time=0.4, sustain_level=0.5, attack_level=0.5)
audio.play(synth)

note_vals = (0,5,7,12) # ,12+5,12+7,12+9)
basen = 48

while True:
    print("hi")
    notes = [synthio.Note(synthio.midi_to_hz(basen+n), envelope=amp_env)  for n in note_vals]
    synth.press(notes)
    time.sleep(0.1)
    synth.release_all()
    time.sleep(0.3)
    basen = random.choice( (48,48-5,48+5,48+7))
    
