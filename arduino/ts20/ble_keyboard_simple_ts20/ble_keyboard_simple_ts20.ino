// ble_keyboard_simple_ts20.ino -- sketchingpad as a BLE HID keyboard, plainly
//
// Touching a pad types a character over BLE and lights its LED.  That is
// all it does: poll the TS20, send a report when the pad state changes,
// update the LEDs.  No interrupts, no power tiers, no sleep.
//
// Start here when bringing up a board or changing the key map.  See
// ble_keyboard_lowpower_ts20 for the same thing with interrupt-driven scanning,
// connection-parameter management and SYSTEM_OFF sleep.
//
// Board: Seeeduino:nrf52:xiaonRF52840

#include <Adafruit_NeoPixel.h>
#include <TS20.h>
#include <Wire.h>
#include <bluefruit.h>

#include "device_id_code.h"

// sketchingpad_stripproto1 -> XIAO nRF52840:
//   SDA       = D4       (Wire default)
//   SCL       = D5       (Wire default)
//   LED_DAT   = D6 / TX
//   LED_POWER = D7 / RX
#define PIN_LED_DATA  (6)
#define PIN_LED_POWER (7)

const int NUM_LEDS = 20;
const int LED_BRIGHTNESS = 40;
const int TS20_SENSITIVITY = 15; // 0 = most sensitive, 15 = least
const uint32_t TOUCH_COLOR = 0xff00ff;

Adafruit_NeoPixel leds(NUM_LEDS, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);
TS20 ts20;
BLEDis bledis;
BLEHidAdafruit blehid;

// Index is LED number, value is the pad wired to it.
const uint8_t led_to_pad[NUM_LEDS] = {
    0, 1, 2, 3, 8, 9, 10, 11, 16, 17,
    18, 19, 12, 13, 14, 15, 4, 5, 6, 7,
};

// The TS20 channel order is NOT the pad numbering on the silkscreen.
// Physical layout on proto2, TP refdes above each column, channel below:
//
//            TP17 TP18 TP19 TP20              4  5  6  7
//   TP1  TP2  TP3  TP4   TP5  TP6  TP7  TP8   0  1  2  3   8  9 10 11
//   TP9 TP10 TP11 TP12  TP13 TP14 TP15 TP16  16 17 18 19  12 13 14 15
const uint8_t pad_to_key[TS20_NUM_PADS] = {
    HID_KEY_A, HID_KEY_S, HID_KEY_D, HID_KEY_F,       // TP1-4,   mid left
    HID_KEY_1, HID_KEY_2, HID_KEY_3, HID_KEY_4,       // TP17-20, top row
    HID_KEY_G, HID_KEY_H, HID_KEY_J, HID_KEY_K,       // TP5-8,   mid right
    HID_KEY_B, HID_KEY_N, HID_KEY_M, HID_KEY_COMMA,   // TP13-16, low right
    HID_KEY_Z, HID_KEY_X, HID_KEY_C, HID_KEY_V,       // TP9-12,  low left
};

// Which pads show the device ID code, left to right.  The bottom row is
// pads 9-16; these are its left five.  Change this line if you want the
// code somewhere else.
const uint8_t ID_PADS[DEVICE_ID_CODE_LEN] = {9, 10, 11, 12, 13};

uint32_t last_touches = 0;

// Light the device's ID code on ID_PADS.  Called at the end of setup(); it
// stays up until the first touch, since loop() only redraws on a change.
void showDeviceIdCode() {
  uint32_t colors[DEVICE_ID_CODE_LEN];
  deviceIdColors(colors);
  leds.clear();
  for (uint8_t i = 0; i < DEVICE_ID_CODE_LEN; i++) {
    for (int led = 0; led < NUM_LEDS; led++) {
      // ID_PADS is 1-based to match the layout above, led_to_pad is 0-based
      if (led_to_pad[led] == ID_PADS[i] - 1) { leds.setPixelColor(led, colors[i]); }
    }
  }
  leds.show();
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) { delay(10); } // bounded: may be on battery

  // The LED power rail hangs off a P-FET whose gate is pulled up to its source, 
  // so the rail is OFF until D7 is driven low.  Without this the LEDs don't light.
  pinMode(PIN_LED_POWER, OUTPUT);
  digitalWrite(PIN_LED_POWER, LOW);
  delay(2); // let ~2 uF of rail decoupling settle before clocking data in

  leds.begin();
  leds.setBrightness(LED_BRIGHTNESS);
  leds.clear();
  leds.show();

  delay(100); // wait 100 msec after power up, as per datasheet
  if (!ts20.begin()) {
    Serial.println("No TS20 found, check wiring!");
    while (1) { delay(100); }
  }
  // after begin(), which does Wire.begin() and forces the bus back to 100 kHz
  Wire.setClock(400000);
  ts20.setSensitivity(TS20_SENSITIVITY);

  Bluefruit.begin(1, 0); // one peripheral, no central
  Bluefruit.setName("sketchingpad");

  // Model carries this board's ID code, e.g. "sketchingpad-RMGGB", matching
  // the five LEDs lit at the end of setup().  static: BLEDis holds the
  // pointer until begin() copies the string into the GATT table.
  static char model[32];
  deviceIdName(model, sizeof(model), "sketchingpad");

  bledis.setManufacturer("todbot");
  bledis.setModel(model);
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

  Serial.printf("ble_keyboard_simple_ts20: pair with \"sketchingpad\", model %s\n",
                model);
}

void loop() {
  uint32_t touches = ts20.touched();

  if (touches != last_touches) {
    // Rebuild the whole report from the current pad state, so chords and
    // roll-offs are not dropped.  The boot keyboard report holds a modifier
    // plus six keycodes; a seventh simultaneous pad has nowhere to go.
    uint8_t keycodes[6] = {0, 0, 0, 0, 0, 0};
    uint8_t n = 0;
    for (uint8_t pad = 0; pad < TS20_NUM_PADS && n < 6; pad++) {
      if ((touches >> pad) & 1) { keycodes[n++] = pad_to_key[pad]; }
    }
    if (Bluefruit.connected()) { blehid.keyboardReport(0, keycodes); }

    for (int i = 0; i < NUM_LEDS; i++) {
      leds.setPixelColor(i, ((touches >> led_to_pad[i]) & 1) ? TOUCH_COLOR : 0);
    }
    leds.show();

    Serial.printf("touches %05lx -> %d key(s)\n", (unsigned long)touches, n);
    last_touches = touches;
  }

  delay(10);
}
