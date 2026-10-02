// SPDX-FileCopyrightText: 2026 Tod Kurt
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ble_keyboard_simple.ino -- sketchingpad as a BLE HID keyboard, plainly
//
// Touching a pad types a character over BLE and lights its LED.  That is
// all it does: poll the sensor, send a report when the pad state changes,
// update the LEDs.  No interrupts, no power tiers, no sleep.
//
// Replaces ble_keyboard_simple_ts20 and runs on the MPR121 board too:
// touch.h probes I2C at startup -- TS20 at 0x6A, MPR121s at 0x5A and 0x5B
// -- so swapping boards on the bench needs no reflash, and the banner says
// which one it found.
//
// Everything here is in LED-index space: bit n of touchRead() is LED n,
// which is TP(n+1) on the silkscreen.  There is no led_to_pad table in
// this file; both tables live in touch_map.h, one per board rev.  That is
// also what lets the key map below be read straight off the silkscreen
// instead of off the TS20's channel order.
//
// Start here when bringing up a board or changing the key map.  See
// ble_keyboard_lowpower for the same thing with interrupt-driven
// scanning, connection-parameter management and SYSTEM_OFF sleep.
//
// Board: Seeeduino:nrf52:xiaonRF52840

#include <Adafruit_NeoPixel.h>
#include <Wire.h>
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
const uint32_t TOUCH_COLOR = 0xff00ff;

Adafruit_NeoPixel leds(NUM_LEDS, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);
BLEDis bledis;
BLEHidAdafruit blehid;

// Indexed by LED, which is TP(n+1), so the table reads in silkscreen order
// and is the same on both boards:
//
//            TP17 TP18 TP19 TP20               1  2  3  4
//   TP1  TP2  TP3  TP4   TP5  TP6  TP7  TP8    a  s  d  f   g  h  j  k
//   TP9 TP10 TP11 TP12  TP13 TP14 TP15 TP16    z  x  c  v   b  n  m  ,
const uint8_t led_to_key[TOUCH_NUM_LEDS] = {
    HID_KEY_A, HID_KEY_S, HID_KEY_D, HID_KEY_F,      // TP1-4,   mid left
    HID_KEY_G, HID_KEY_H, HID_KEY_J, HID_KEY_K,      // TP5-8,   mid right
    HID_KEY_Z, HID_KEY_X, HID_KEY_C, HID_KEY_V,      // TP9-12,  low left
    HID_KEY_B, HID_KEY_N, HID_KEY_M, HID_KEY_COMMA,  // TP13-16, low right
    HID_KEY_1, HID_KEY_2, HID_KEY_3, HID_KEY_4,      // TP17-20, top row
};

// Which LEDs show the device ID code, left to right: the bottom row's left
// five, TP9-13.  Change this line to put it somewhere else.
const uint8_t ID_LEDS[DEVICE_ID_CODE_LEN] = {8, 9, 10, 11, 12};

uint32_t last_touches = 0;

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
  Serial.begin(115200);
  while (!Serial && millis() < 3000) { delay(10); } // bounded: may be on battery

  // The LED power rail hangs off a P-FET whose gate is pulled up to its
  // source, so the rail is OFF until D7 is driven low.  Without this the
  // LEDs never light.
  pinMode(PIN_LED_POWER, OUTPUT);
  digitalWrite(PIN_LED_POWER, LOW);
  delay(2); // let ~2 uF of rail decoupling settle before clocking data in

  leds.begin();
  leds.setBrightness(LED_BRIGHTNESS);
  leds.clear();
  leds.show();

  // Does Wire.begin(), the TS20's 100 ms power-up wait, the 400 kHz clock
  // and the per-sensor configuration.
  if (!touchBegin()) {
    Serial.println("No touch sensor found, check wiring!");
    while (1) { delay(100); }
  }

  Bluefruit.begin(1, 0); // one peripheral, no central

  // The advertised name carries this board's ID code, e.g.
  // "sketchingpad-RMGGB", matching the five LEDs lit at the end of setup().
  // In the name rather than the DIS model string because a central shows
  // the name while scanning, and the model only after connecting.
  //
  // This fills the advertising packet exactly: 31 = flags 3 + appearance 4
  // + HID UUID 4 + name (2 + 18).  Anything further added to the adv
  // packet has to go in the scan response instead.
  static char adv_name[32];
  deviceIdName(adv_name, sizeof(adv_name), "sketchingpad");
  Bluefruit.setName(adv_name);

  bledis.setManufacturer("todbot");
  bledis.setModel("sketchingpad");
  bledis.begin();

  blehid.begin();

  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addAppearance(BLE_APPEARANCE_HID_KEYBOARD);
  Bluefruit.Advertising.addService(blehid);
  Bluefruit.Advertising.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244);
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.start(0); // 0 = advertise forever

  showDeviceIdCode();

  // Seed from a real read rather than 0.  A sensor that reports anything at
  // all on the first pass would otherwise trip loop()'s repaint immediately
  // and wipe the ID code before it could be seen.
  last_touches = touchRead();

  Serial.printf("ble_keyboard_simple: sensor %s, pair with \"%s\"\n",
                touchName(), adv_name);
  Serial.printf("id code on LEDs %d-%d, first read %05lx\n", ID_LEDS[0],
                ID_LEDS[DEVICE_ID_CODE_LEN - 1], (unsigned long)last_touches);
  touchReport();
}

void loop() {
  uint32_t touches = touchRead();

  if (touches != last_touches) {
    // Rebuild the whole report from the current pad state, so chords and
    // roll-offs are not dropped.  The boot keyboard report holds a modifier
    // plus six keycodes; a seventh simultaneous pad has nowhere to go.
    uint8_t keycodes[6] = {0, 0, 0, 0, 0, 0};
    uint8_t n = 0;
    for (uint8_t led = 0; led < NUM_LEDS && n < 6; led++) {
      if ((touches >> led) & 1) { keycodes[n++] = led_to_key[led]; }
    }
    if (Bluefruit.connected()) { blehid.keyboardReport(0, keycodes); }

    for (int i = 0; i < NUM_LEDS; i++) {
      leds.setPixelColor(i, ((touches >> i) & 1) ? TOUCH_COLOR : 0);
    }
    leds.show();

    Serial.printf("touches %05lx -> %d key(s)\n", (unsigned long)touches, n);
    // Which sensor channel each one came from: this is how a key map is
    // checked against the silkscreen on a board nobody has flashed before.
    for (uint8_t led = 0; led < NUM_LEDS; led++) {
      if ((touches >> led) & 1) {
        Serial.printf("  led %d (TP%d) <- %s, key %02x\n", led, led + 1,
                      touchSource(led), led_to_key[led]);
      }
    }
    last_touches = touches;
  }

  delay(10);
}
