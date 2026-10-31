// SPDX-FileCopyrightText: 2026 Tod Kurt
// SPDX-License-Identifier: GPL-3.0-or-later
//
// sketchingpad_gamepad.ino -- sketchingpad as a USB + BLE HID gamepad
//
// sketchingpad_midi_simple's two transports with a gamepad instead of
// MIDI.  Both run at once and neither is required: unplugged it is a BLE
// gamepad, with nothing paired it is a USB gamepad.  A host that has both
// sees two controllers.
//
// Poll the sensor, send on a change, light the LEDs.  No interrupts, no
// power tiers, no sleep.  Runs on both boards -- touch.h probes I2C at
// startup, TS20 at 0x6A or MPR121s at 0x5A and 0x5B.
//
// Three 2x2 direction clusters, left stick, D-pad and right stick, then
// the face buttons, with shoulders and select/start on the top row.  The
// sticks are digital: each axis is centered or at full deflection.
//
// A gamepad report is the whole state, not an event, so there is nothing
// to get stuck.  Each transport keeps the last report it accepted and is
// sent the current one whenever they differ: a refused send retries next
// loop, and a transport coming back is caught up the same way.
//
// Everything here is in LED-index space: bit n of touchRead() is LED n,
// which is TP(n+1) on the silkscreen.  There is no led_to_pad table in
// this file; both tables live in touch_map.h, one per board rev.
//
// Board: Seeeduino:nrf52:xiaonRF52840

#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <Adafruit_TinyUSB.h>
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

const uint32_t STICK_COLOR = 0x00ff40;
const uint32_t DPAD_COLOR = 0x00ffff;
const uint32_t BUTTON_COLOR = 0xff00ff;

const int8_t AXIS_MAX = 127;

// The board, as the gamepad sees it.  L = left stick, D = D-pad,
// R = right stick:
//
//          TP17 TP18        TP19 TP20             LB  select  start  RB
//   TP1  TP2  TP3  TP4   TP5  TP6  TP7  TP8    L<  L^  D<  D^   R<  R^  X  Y
//   TP9 TP10 TP11 TP12  TP13 TP14 TP15 TP16    Lv  L>  Dv  D>   Rv  R>  A  B
//
// Each cluster is LEDs {left, up, down, right}, indexed by LED, which is
// TP(n+1), so these read straight off the silkscreen.
const uint8_t LSTICK_PADS[4] = {0, 1, 8, 9};   // TP1 TP2 TP9 TP10
const uint8_t DPAD_PADS[4] = {2, 3, 10, 11};   // TP3 TP4 TP11 TP12
const uint8_t RSTICK_PADS[4] = {4, 5, 12, 13}; // TP5 TP6 TP13 TP14

// The browser / XInput order, not TinyUSB's GAMEPAD_BUTTON_* names: those
// follow Linux, whose C and Z gaps make X read as Y and Y as LB.
enum : uint32_t {
  BTN_A = 1UL << 0,
  BTN_B = 1UL << 1,
  BTN_X = 1UL << 2,
  BTN_Y = 1UL << 3,
  BTN_LB = 1UL << 4,
  BTN_RB = 1UL << 5,
  BTN_SELECT = 1UL << 8,
  BTN_START = 1UL << 9,
  BTN_DPAD_UP = 1UL << 12,
  BTN_DPAD_DOWN = 1UL << 13,
  BTN_DPAD_LEFT = 1UL << 14,
  BTN_DPAD_RIGHT = 1UL << 15,
};

// Button mask per LED; 0 is a stick pad.  The D-pad is sent both as these
// buttons and as the hat: browsers ignore the hat on a generic HID
// gamepad, and other hosts read only the hat.
const uint32_t led_to_button[TOUCH_NUM_LEDS] = {
    0,      0,          BTN_DPAD_LEFT, BTN_DPAD_UP,    // TP1-4
    0,      0,          BTN_X,         BTN_Y,          // TP5-8
    0,      0,          BTN_DPAD_DOWN, BTN_DPAD_RIGHT, // TP9-12
    0,      0,          BTN_A,         BTN_B,          // TP13-16
    BTN_LB, BTN_SELECT, BTN_START,     BTN_RB,         // TP17-20
};

// For the diagnostic prints.
const char *const led_name[TOUCH_NUM_LEDS] = {
    "lstick left", "lstick up",    "dpad left",   "dpad up",
    "rstick left", "rstick up",    "X",           "Y",
    "lstick down", "lstick right", "dpad down",   "dpad right",
    "rstick down", "rstick right", "A",           "B",
    "LB",          "select",       "start",       "RB",
};

// Which LEDs show the device ID code, left to right: the bottom row's left
// five, TP9-13.  Change this line to put it somewhere else.
const uint8_t ID_LEDS[DEVICE_ID_CODE_LEN] = {8, 9, 10, 11, 12};

// No report ID: BLEHidGamepad's ID 1 rides in its characteristic's
// report reference, not in the payload, so one struct serves both.
uint8_t const desc_hid_report[] = {TUD_HID_REPORT_DESC_GAMEPAD()};

Adafruit_NeoPixel leds(NUM_LEDS, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);
// Never call usb_hid.setStringDescriptor() (or isValid()): this file
// compiles against the core's TinyUSB 1.7.0 Adafruit_USBD_Interface.h,
// whose inline version writes a pointer over 3.7.7's _instance, and
// begin() then returns true without adding the interface.
Adafruit_USBD_HID usb_hid(desc_hid_report, sizeof(desc_hid_report),
                          HID_ITF_PROTOCOL_NONE, 2, false);
BLEDis bledis;
BLEHidGamepad blegamepad;

uint32_t last_touches = 0;

// What each transport last accepted; synced false means send regardless.
hid_gamepad_report_t usb_sent;
hid_gamepad_report_t ble_sent;
bool usb_synced = false;
bool ble_synced = false;
bool was_usb_up = false;
bool was_ble_up = false;

static inline bool padOn(uint32_t touches, uint8_t led) {
  return (touches >> led) & 1;
}

/// -1, 0 or +1 on each axis, y down as HID has it.  Opposite pads held
/// together cancel rather than one of them winning on read order.
void clusterDir(uint32_t touches, const uint8_t pads[4], int &dx, int &dy) {
  dx = padOn(touches, pads[3]) - padOn(touches, pads[0]);
  dy = padOn(touches, pads[2]) - padOn(touches, pads[1]);
}

uint8_t hatFor(uint32_t touches) {
  int dx, dy;
  clusterDir(touches, DPAD_PADS, dx, dy);
  static const uint8_t hats[3][3] = {
      {GAMEPAD_HAT_UP_LEFT, GAMEPAD_HAT_UP, GAMEPAD_HAT_UP_RIGHT},
      {GAMEPAD_HAT_LEFT, GAMEPAD_HAT_CENTERED, GAMEPAD_HAT_RIGHT},
      {GAMEPAD_HAT_DOWN_LEFT, GAMEPAD_HAT_DOWN, GAMEPAD_HAT_DOWN_RIGHT},
  };
  return hats[dy + 1][dx + 1];
}

uint32_t buttonsFor(uint32_t touches) {
  uint32_t mask = 0;
  for (uint8_t led = 0; led < NUM_LEDS; led++) {
    if (padOn(touches, led)) { mask |= led_to_button[led]; }
  }
  return mask;
}

/// Right stick on Z/Rz, as TUD_HID_REPORT_DESC_GAMEPAD labels them.
hid_gamepad_report_t buildReport(uint32_t touches) {
  hid_gamepad_report_t rep = {};
  int dx, dy;
  clusterDir(touches, LSTICK_PADS, dx, dy);
  rep.x = dx * AXIS_MAX;
  rep.y = dy * AXIS_MAX;
  clusterDir(touches, RSTICK_PADS, dx, dy);
  rep.z = dx * AXIS_MAX;
  rep.rz = dy * AXIS_MAX;
  rep.hat = hatFor(touches);
  rep.buttons = buttonsFor(touches);
  return rep;
}

static inline bool inCluster(const uint8_t pads[4], uint8_t led) {
  for (uint8_t i = 0; i < 4; i++) {
    if (pads[i] == led) { return true; }
  }
  return false;
}

bool needsSend(bool synced, const hid_gamepad_report_t &sent,
               const hid_gamepad_report_t &rep) {
  return !synced || memcmp(&sent, &rep, sizeof(rep)) != 0;
}

uint32_t colorFor(uint8_t led) {
  if (inCluster(DPAD_PADS, led)) { return DPAD_COLOR; }
  return led_to_button[led] ? BUTTON_COLOR : STICK_COLOR;
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
  bool usb_hid_ok = usb_hid.begin();

  // Detach/attach to let the host re-enumerate with the HID interface.
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
  // This fills the advertising packet exactly: 31 = flags 3 + appearance 4
  // + HID UUID 4 + name (2 + 18), so no addTxPower() here.
  static char adv_name[32];
  deviceIdName(adv_name, sizeof(adv_name), "sketchingpad");
  Bluefruit.setName(adv_name);

  bledis.setManufacturer("todbot");
  bledis.setModel("sketchingpad");
  bledis.begin();

  blegamepad.begin();

  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addAppearance(BLE_APPEARANCE_HID_GAMEPAD);
  Bluefruit.Advertising.addService(blegamepad);
  Bluefruit.Advertising.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244);
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.start(0); // 0 = advertise forever

  showDeviceIdCode();

  // Seed from a real read so we don't wipe the ID code on startup.
  last_touches = touchRead();

  Serial.printf("sketchingpad_gamepad: sensor %s, pair with \"%s\"\n",
                touchName(), adv_name);
  // begin() can return true having added nothing; the instance count is a
  // static, so it is the one figure the header mismatch cannot fake.
  Serial.printf("usb hid begin %s, instances %d, %s, mounted now %d\n",
                usb_hid_ok ? "ok" : "FAILED",
                Adafruit_USBD_HID::getInstanceCount(),
                usb_was_mounted ? "mounted before setup, bounced"
                                : "not mounted at setup",
                TinyUSBDevice.mounted());
  Serial.printf("id code on LEDs %d-%d, first read %05lx\n", ID_LEDS[0],
                ID_LEDS[DEVICE_ID_CODE_LEN - 1], (unsigned long)last_touches);
  touchReport();
}

void loop() {
  uint32_t touches = touchRead();
  hid_gamepad_report_t rep = buildReport(touches);

  // Up is mounted()/connected(); ready() only says the endpoint is free
  // this instant, so a busy one is retried rather than counted as gone.
  bool usb_up = TinyUSBDevice.mounted();
  bool ble_up = Bluefruit.connected();
  if (usb_up != was_usb_up) {
    Serial.println(usb_up ? "usb mounted" : "usb unmounted");
  }
  if (ble_up != was_ble_up) {
    Serial.println(ble_up ? "ble connected" : "ble gone");
  }
  was_usb_up = usb_up;
  was_ble_up = ble_up;

  if (!usb_up) {
    usb_synced = false;
  } else if (usb_hid.ready() && needsSend(usb_synced, usb_sent, rep)) {
    if (usb_hid.sendReport(0, &rep, sizeof(rep))) {
      if (!usb_synced) { Serial.println("usb hid: report accepted"); }
      usb_sent = rep;
      usb_synced = true;
    }
  }

  // report() is refused until the host has paired and subscribed, so a
  // fresh connection just keeps retrying until it lands.
  if (!ble_up) {
    ble_synced = false;
  } else if (needsSend(ble_synced, ble_sent, rep)) {
    if (blegamepad.report(&rep)) {
      if (!ble_synced) { Serial.println("ble hid: report accepted"); }
      ble_sent = rep;
      ble_synced = true;
    }
  }

  if (touches != last_touches) {
    uint32_t changed = touches ^ last_touches;

    // Sync state lags this report by a loop; "pending" that never clears
    // is a transport that is up but not taking reports.
    Serial.printf("touches %05lx -> L %d,%d R %d,%d hat %d buttons %04lx"
                  " [usb %s, ble %s]\n",
                  (unsigned long)touches, rep.x, rep.y, rep.z, rep.rz, rep.hat,
                  (unsigned long)rep.buttons,
                  usb_up ? (usb_synced ? "ok" : "pending") : "down",
                  ble_up ? (ble_synced ? "ok" : "pending") : "down");
    for (uint8_t led = 0; led < NUM_LEDS; led++) {
      if (!padOn(changed, led)) { continue; }
      Serial.printf("  led %2d (TP%2d) <- %-9s %s %s\n", led, led + 1,
                    touchSource(led), padOn(touches, led) ? "down" : "up  ",
                    led_name[led]);
    }

    for (int i = 0; i < NUM_LEDS; i++) {
      leds.setPixelColor(i, padOn(touches, i) ? colorFor(i) : 0);
    }
    leds.show();

    last_touches = touches;
  }

  delay(10);
}
