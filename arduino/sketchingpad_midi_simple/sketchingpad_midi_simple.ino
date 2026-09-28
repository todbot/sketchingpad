// SPDX-FileCopyrightText: 2026 Tod Kurt
// SPDX-License-Identifier: GPL-3.0-or-later
//
// sketchingpad_midi_simple.ino -- sketchingpad as a USB + BLE MIDI controller
//
// ble_midi_simple with USB MIDI alongside the radio.  Both transports run
// at once and neither is required: unplugged it is a BLE controller, with
// nothing paired it is a USB controller, and plugged into a host that is
// also paired it plays both at once.  Every message goes to whichever
// transports are up at that instant.
//
// Poll the sensor, send on a change, light the LEDs.  No interrupts, no
// power tiers, no sleep.  Runs on both boards -- touch.h probes I2C at
// startup, TS20 at 0x6A or MPR121s at 0x5A and 0x5B.
//
// The bottom two rows are 16 note pads, C major pentatonic from C2.  The
// top row of four is two control pairs: TP17/TP18 bend down/up, TP19/TP20
// modulation to half and full.  Both pairs are momentary and spring back
// when nothing in the pair is held -- bend to center, CC1 to 0.
//
// Messages go out through the FortySevenEffects MIDI library, which has
// to be installed; see the README.  It drives two transports the core
// already ships: BLEMidi and TinyUSB's Adafruit_USBD_MIDI.
//
// The two readiness tests are not equally strong.  BLE knows the host
// subscribed; USB only knows the host enumerated the device, since
// Adafruit_USBD_MIDI exposes no per-endpoint state.  A write to an
// unopened USB endpoint is dropped rather than blocking, so the weaker
// test is safe -- it just cannot promise anyone is listening.
//
// Two of its conventions are not the wire's and fail silently if they
// are confused: channels are 1-16, where 0 means OMNI and is dropped
// unsent, and sendPitchBend() is centered on 0, not on 8192.
//
// Everything here is in LED-index space: bit n of touchRead() is LED n,
// which is TP(n+1) on the silkscreen.  There is no led_to_pad table in
// this file; both tables live in touch_map.h, one per board rev.
//
// Board: Seeeduino:nrf52:xiaonRF52840

#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <Adafruit_TinyUSB.h>
#include <MIDI.h>
#include <bluefruit.h>

// sketchingpad1 / sketchingpad1_mpr -> XIAO nRF52840:
//   TOUCH_INT = D3 / A3  (open collector, R1 pulls up; both /IRQ on _mpr)
//   SDA       = D4       (Wire default)
//   SCL       = D5       (Wire default)
//   LED_DAT   = D6 / TX
//   LED_POWER = D7 / RX  (P-FET gate: LOW = rail on)
#define PIN_LED_DATA  (6)
#define PIN_LED_POWER (7)

#include "device_id_code.h"
#include "touch.h"

const int NUM_LEDS = TOUCH_NUM_LEDS;
const int LED_BRIGHTNESS = 40;

const uint32_t NOTE_COLOR = 0xff00ff;
const uint32_t BEND_COLOR = 0x00ffff;
const uint32_t MOD_COLOR = 0xff6000;

const uint8_t MIDI_CHANNEL = 1; // 1-16, as the library and DAWs count
const uint8_t MIDI_VELOCITY = 100;
const uint8_t CC_MODULATION = 1;
const uint8_t CC_ALL_NOTES_OFF = 123;

// Centered on 0, not 8192: sendPitchBend()'s int overload takes
// MIDI_PITCHBEND_MIN..MAX and offsets it itself, without clamping.
const int16_t BEND_MIN = -8192;
const int16_t BEND_CENTER = 0;
const int16_t BEND_MAX = 8191;

// The 16 note pads, indexed by LED, which is TP(n+1), so the table reads
// in silkscreen order.  C major pentatonic (C D E G A) climbing left to
// right and bottom to top, so the low row is the low octaves:
//
//   TP1  TP2  TP3  TP4   TP5  TP6  TP7  TP8    G3 A3 C4 D4  E4 G4 A4 C5
//   TP9 TP10 TP11 TP12  TP13 TP14 TP15 TP16    C2 D2 E2 G2  A2 C3 D3 E3
const uint8_t NUM_NOTE_PADS = 16;
const uint8_t led_to_note[NUM_NOTE_PADS] = {
    55, 57, 60, 62, 64, 67, 69, 72, // TP1-8,  mid row, G3 up to C5
    36, 38, 40, 43, 45, 48, 50, 52, // TP9-16, low row, C2 up to E3
};

// The top row of four, TP17-TP20.  Swap a pair to flip its direction.
const uint8_t PAD_BEND_DOWN = 16; // TP17
const uint8_t PAD_BEND_UP = 17;   // TP18
const uint8_t PAD_MOD_MID = 18;   // TP19
const uint8_t PAD_MOD_MAX = 19;   // TP20

// Which LEDs show the device ID code, left to right: the bottom row's left
// five, TP9-13.  Change this line to put it somewhere else.
const uint8_t ID_LEDS[DEVICE_ID_CODE_LEN] = {8, 9, 10, 11, 12};

Adafruit_NeoPixel leds(NUM_LEDS, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);
BLEDis bledis;
// MIDI_CREATE_BLE_INSTANCE() is not used: it hardcodes the instance name
// to MIDI, and there are two of them here.
Adafruit_USBD_MIDI usb_midi;
MIDI_CREATE_INSTANCE(Adafruit_USBD_MIDI, usb_midi, MIDI_USB);

BLEMidi blemidi;
MIDI_CREATE_INSTANCE(BLEMidi, blemidi, MIDI_BLE);

uint32_t last_touches = 0;
int16_t last_bend = BEND_CENTER;
uint8_t last_mod = 0;

// Shadowed so a transport coming up can be spotted as an edge.  The pad
// code never reads these; the four senders below do the gating.
bool usb_ready = false;
bool ble_ready = false;
bool was_usb_ready = false;
bool was_ble_ready = false;

void midiNoteOn(uint8_t note) {
  if (usb_ready) { MIDI_USB.sendNoteOn(note, MIDI_VELOCITY, MIDI_CHANNEL); }
  if (ble_ready) { MIDI_BLE.sendNoteOn(note, MIDI_VELOCITY, MIDI_CHANNEL); }
}

void midiNoteOff(uint8_t note) {
  if (usb_ready) { MIDI_USB.sendNoteOff(note, 0, MIDI_CHANNEL); }
  if (ble_ready) { MIDI_BLE.sendNoteOff(note, 0, MIDI_CHANNEL); }
}

void midiControlChange(uint8_t cc, uint8_t value) {
  if (usb_ready) { MIDI_USB.sendControlChange(cc, value, MIDI_CHANNEL); }
  if (ble_ready) { MIDI_BLE.sendControlChange(cc, value, MIDI_CHANNEL); }
}

void midiPitchBend(int16_t value) {
  if (usb_ready) { MIDI_USB.sendPitchBend(value, MIDI_CHANNEL); }
  if (ble_ready) { MIDI_BLE.sendPitchBend(value, MIDI_CHANNEL); }
}

/// Bring one transport up to the state the pads are already in: silence
/// whatever it was left holding, then re-send what is held now.
///
/// ble_midi_simple could replay by zeroing last_touches, because it had
/// one transport.  Here that would re-send every held note to the
/// transport that was already playing them, so the catch-up is aimed at
/// the one that just arrived and last_touches is left alone.
template <typename T> void resyncHost(T &midi, uint32_t touches) {
  midi.sendControlChange(CC_ALL_NOTES_OFF, 0, MIDI_CHANNEL);
  midi.sendControlChange(CC_MODULATION, modFor(touches), MIDI_CHANNEL);
  midi.sendPitchBend(bendFor(touches), MIDI_CHANNEL);
  for (uint8_t led = 0; led < NUM_NOTE_PADS; led++) {
    if ((touches >> led) & 1) {
      midi.sendNoteOn(led_to_note[led], MIDI_VELOCITY, MIDI_CHANNEL);
    }
  }
}

/// Both pads of the pair held is treated as neither: the two directions
/// cancel rather than one of them winning on read order.
int16_t bendFor(uint32_t touches) {
  bool down = (touches >> PAD_BEND_DOWN) & 1;
  bool up = (touches >> PAD_BEND_UP) & 1;
  if (down == up) { return BEND_CENTER; }
  return down ? BEND_MIN : BEND_MAX;
}

uint8_t modFor(uint32_t touches) {
  if ((touches >> PAD_MOD_MAX) & 1) { return 127; }
  if ((touches >> PAD_MOD_MID) & 1) { return 64; }
  return 0;
}

uint32_t colorFor(uint8_t led) {
  if (led < NUM_NOTE_PADS) { return NOTE_COLOR; }
  return (led == PAD_BEND_DOWN || led == PAD_BEND_UP) ? BEND_COLOR : MOD_COLOR;
}

/// What each pad is, for the diagnostic prints.
const char *padName(uint8_t led) {
  static char buf[24];
  if (led < NUM_NOTE_PADS) {
    snprintf(buf, sizeof(buf), "note %d", led_to_note[led]);
  } else if (led == PAD_BEND_DOWN) {
    snprintf(buf, sizeof(buf), "bend down");
  } else if (led == PAD_BEND_UP) {
    snprintf(buf, sizeof(buf), "bend up");
  } else if (led == PAD_MOD_MID) {
    snprintf(buf, sizeof(buf), "mod 64");
  } else {
    snprintf(buf, sizeof(buf), "mod 127");
  }
  return buf;
}

// Light the device's ID code on ID_LEDS.  Called at the end of setup(); it
// stays up until the first touch, since loop() only redraws on a change.
void showDeviceIdCode() {
  uint32_t colors[DEVICE_ID_CODE_LEN];
  deviceIdColors(colors);
  leds.clear();
  for (uint8_t i = 0; i < DEVICE_ID_CODE_LEN; i++) {
    leds.setPixelColor(ID_LEDS[i], colors[i]);
  }
  leds.show();
}

void setup() {
  usb_midi.setStringDescriptor("sketchingpad");

  // Required on the Seeed core, despite the constructor already
  // defaulting to one cable.  That core's platform.txt puts its bundled
  // TinyUSB 1.7.0 headers on every include path. If you have a later 
  // version of Adafruit_TinyUSB (3.7.7 latest), you get old header with new driver.
  // Re-asserting it here works because the store goes through a library
  // method after the damage; a constructor argument would not.  
  // This hack works, but the Seeed nrf52 Arduino core needs to be patched.
  usb_midi.setCables(1);

  MIDI_USB.begin(MIDI_CHANNEL); // also does usb_midi.begin()

  // Detach/attach to let the host re-enumerate with the updated USB descriptors.
  bool usb_was_mounted = TinyUSBDevice.mounted();
  if (usb_was_mounted) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }

  Serial.begin(115200);
  while (!Serial && millis() < 3000) { delay(10); } // bounded: may be on battery

  // Enable the power rail to the Neopixels
  pinMode(PIN_LED_POWER, OUTPUT);
  digitalWrite(PIN_LED_POWER, LOW);
  delay(2); // let ~2 uF of rail decoupling settle before clocking data in

  leds.begin();
  leds.setBrightness(LED_BRIGHTNESS);
  leds.clear();
  leds.show();

  // Does Wire.begin(), the TS20's 100 ms power-up wait, and per-sensor config
  if (!touchBegin()) {
    Serial.println("No touch sensor found, check wiring!");
    while (1) { delay(100); }
  }

  Bluefruit.begin(1, 0); // one peripheral, no central

  // The advertised name carries this board's ID code, e.g.
  // "sketchingpad-RMGGB", matching the five LEDs lit at the end of setup().
  static char adv_name[32];
  deviceIdName(adv_name, sizeof(adv_name), "sketchingpad");
  Bluefruit.setName(adv_name);

  bledis.setManufacturer("todbot");
  bledis.setModel("sketchingpad");
  bledis.begin();

  MIDI_BLE.begin(MIDI_CHANNEL); // also does blemidi.begin(), down the chain

  // Unlike the HID keyboard, the name cannot ride in the advertising
  // packet: MIDI's service UUID is 128-bit, so flags 3 + UUID 18 = 21 of
  // 31, and an 18-character name needs 20.  It goes in the scan response,
  // which is why a scanner may show the name a beat later than it shows
  // the device.  There is no MIDI appearance value to advertise either.
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addService(blemidi);
  Bluefruit.ScanResponse.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244);
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.start(0); // 0 = advertise forever

  showDeviceIdCode();

  // Seed from a real read so we don't wipe the ID code on startup. 
  last_touches = touchRead();
  last_bend = bendFor(last_touches);
  last_mod = modFor(last_touches);

  Serial.printf("sketchingpad_midi_simple: sensor %s, pair with \"%s\"\n",
                touchName(), adv_name);
  Serial.printf("MIDI channel %d, velocity %d, notes %d-%d\n", MIDI_CHANNEL,
                MIDI_VELOCITY, led_to_note[8], led_to_note[7]);
  // Every USB send is gated on mounted() and drops silently, so say so.
  Serial.printf("usb: %s, mounted now %d\n",
                usb_was_mounted ? "mounted before setup, bounced"
                                : "not mounted at setup",
                TinyUSBDevice.mounted());
  Serial.printf("id code on LEDs %d-%d, first read %05lx\n", ID_LEDS[0],
                ID_LEDS[DEVICE_ID_CODE_LEN - 1], (unsigned long)last_touches);
  touchReport();
}

void loop() {
  uint32_t touches = touchRead();

  // mounted() is the whole of what USB offers; notifyEnabled() rather
  // than connected() for BLE, since a host can hold a connection without
  // ever subscribing and the notification would go nowhere.
  usb_ready = TinyUSBDevice.mounted();
  ble_ready = Bluefruit.connected() && blemidi.notifyEnabled();

  // Whatever a transport was left holding when it went away could not be
  // silenced while it was gone, so each one is caught up as it returns.
  if (usb_ready && !was_usb_ready) {
    resyncHost(MIDI_USB, touches);
    Serial.println("usb mounted, host state resynced");
  }
  if (ble_ready && !was_ble_ready) {
    resyncHost(MIDI_BLE, touches);
    Serial.println("ble subscribed, host state resynced");
  }
  if (!usb_ready && was_usb_ready) { Serial.println("usb unmounted"); }
  if (!ble_ready && was_ble_ready) { Serial.println("ble gone"); }
  was_usb_ready = usb_ready;
  was_ble_ready = ble_ready;

  if (touches != last_touches) {
    uint32_t changed = touches ^ last_touches;

    Serial.printf("touches %05lx\n", (unsigned long)touches);
    for (uint8_t led = 0; led < NUM_LEDS; led++) {
      if (!((changed >> led) & 1)) { continue; }
      bool on = (touches >> led) & 1;
      Serial.printf("  led %2d (TP%2d) <- %-9s %s %s\n", led, led + 1,
                    touchSource(led), on ? "down" : "up  ", padName(led));
      // A MIDI note has no six-at-once cap to work around, so every note
      // pad that changed sends, unlike the keyboard's six-keycode report.
      if (led < NUM_NOTE_PADS) {
        if (on) {
          midiNoteOn(led_to_note[led]);
        } else {
          midiNoteOff(led_to_note[led]);
        }
      }
    }

    // Rebuilt from the whole pad word rather than from the pad that moved,
    // so a roll-off inside a pair lands on the value the other pad still
    // asks for instead of on the spring-back.
    int16_t bend = bendFor(touches);
    uint8_t mod = modFor(touches);
    if (bend != last_bend) {
      midiPitchBend(bend);
      Serial.printf("  bend %d\n", bend);
      last_bend = bend;
    }
    if (mod != last_mod) {
      midiControlChange(CC_MODULATION, mod);
      Serial.printf("  cc%d %d\n", CC_MODULATION, mod);
      last_mod = mod;
    }

    for (int i = 0; i < NUM_LEDS; i++) {
      leds.setPixelColor(i, ((touches >> i) & 1) ? colorFor(i) : 0);
    }
    leds.show();

    last_touches = touches;
  }

  delay(10);
}
