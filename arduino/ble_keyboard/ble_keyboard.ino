// SPDX-FileCopyrightText: 2026 Tod Kurt
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ble_keyboard.ino -- ble_keyboard_simple with the power tiers
//
// Originally named ble_keyboard_lowpower.
//
// Touching a pad types a character over BLE HID and lights its LED, as in
// ble_keyboard_simple.  Replaces ble_keyboard_lowpower_ts20 and runs on
// the MPR121 board too: touch.h probes I2C at startup and the banner says
// which sensor it found.  Same key map as ble_keyboard_simple.
//
// Three tiers:
//
//   ACTIVE  touched in the last 10 s.  Slave latency 0, LED rail on.
//   IDLE    still connected, slave latency 20 (~630 ms effective), LED
//           rail off at the P-FET.  A touch is still picked up at once.
//   DEEP    5 min untouched, or a button press.  SYSTEM_OFF, woken by
//           touch INT or the button.
//
// DEEP is a one-way door: waking resets the chip and BLE has to
// reconnect (~1-3 s), so the waking touch is a gesture, not a keystroke.
// That is why IDLE, which keeps the link, does the routine saving.
//
// Board: Seeeduino:nrf52:xiaonRF52840

// 1 = narrate to Serial, but only when USB is attached.
#define DEBUG (1)

// Set to 0 if this XIAO has no DC/DC inductors (shows as a brownout loop).
#define USE_DCDC (1)

#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <bluefruit.h>

#include "device_id_code.h"
#include "touch.h"

// sketchingpad1 / sketchingpad1_mpr -> XIAO nRF52840:
//   BUTTON    = D10/MOSI (SW1 to GND, needs internal pull-up)
//   TOUCH_INT = D3 / A3  (open collector, pulled up on the board)
//   SDA = D4, SCL = D5 (Wire defaults)
#define PIN_LED_DATA  (6)
#define PIN_LED_POWER (7) // P-FET gate: LOW = rail on
#define PIN_BUTTON    (10)

const int NUM_LEDS = TOUCH_NUM_LEDS;
const int LED_BRIGHTNESS = 40;
const uint32_t TOUCH_COLOR = 0xff00ff;
const int8_t TX_POWER = -4; // dBm, plenty for desk range

const uint32_t ACTIVE_TIMEOUT_MS = 10000;  // ACTIVE -> IDLE
const uint32_t DEEP_TIMEOUT_MS = 300000;   // IDLE   -> DEEP (5 minutes)
const uint32_t TICK_MS = 250;              // tier timers only, no I2C

// interval in 1.25 ms units, timeout in 10 ms units
const uint16_t CONN_INTERVAL = 24;   // 30 ms
const uint16_t LATENCY_ACTIVE = 0;
const uint16_t LATENCY_IDLE = 20;    // 21 * 30 ms = 630 ms effective
const uint16_t SUPV_TIMEOUT = 400;   // 4 s, well above 630 ms

Adafruit_NeoPixel leds(NUM_LEDS, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);
BLEDis bledis;
BLEHidAdafruit blehid;

// Same as ble_keyboard_simple: indexed by LED, which is TP(n+1).
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

// Which LEDs show the device ID code at boot: TP9-13.
const uint8_t ID_LEDS[DEVICE_ID_CODE_LEN] = {8, 9, 10, 11, 12};

bool usb_attached = false;
uint16_t conn_hdl = BLE_CONN_HANDLE_INVALID;
bool active = true;
bool rail_on = false;
uint32_t last_touch_ms = 0;
uint32_t touches = 0;      // LED-indexed, from touchRead()
uint32_t last_touches = 0;

SemaphoreHandle_t touch_sem;

// if, not #if: the arguments still compile with DEBUG 0, so that build
// cannot grow unused-variable warnings nobody sees.
#define dbg(...)                                         \
  do {                                                   \
    if (DEBUG && usb_attached) {                         \
      Serial.printf("%7lu ", (unsigned long)millis());   \
      Serial.printf(__VA_ARGS__);                        \
    }                                                    \
  } while (0)

void touch_isr() {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(touch_sem, &woken);
  portYIELD_FROM_ISR(woken);
}

// --------------------------------------------------------------- LED rail

// Up: let ~2 uF of decoupling settle before clocking data in.  Down: data
// low first, or current sneaks into the strip through the SK6812 input
// protection diode.
void ledPower(bool on) {
  if (on == rail_on) { return; }
  if (on) {
    digitalWrite(PIN_LED_POWER, LOW);
    delay(2);
    rail_on = true;
    leds.clear();
    leds.show();
  } else {
    leds.clear();
    leds.show();
    digitalWrite(PIN_LED_DATA, LOW);
    digitalWrite(PIN_LED_POWER, HIGH);
    rail_on = false;
  }
}

void showTouches() {
  if (!rail_on) { return; }
  for (int i = 0; i < NUM_LEDS; i++) {
    leds.setPixelColor(i, ((touches >> i) & 1) ? TOUCH_COLOR : 0);
  }
  leds.show();
}

// Stays up until the first touch repaints.
void showDeviceIdCode() {
  uint32_t colors[DEVICE_ID_CODE_LEN];
  deviceIdColors(colors);
  leds.clear();
  for (uint8_t i = 0; i < DEVICE_ID_CODE_LEN; i++) {
    leds.setPixelColor(ID_LEDS[i], colors[i]);
  }
  leds.show();
}

// -------------------------------------------------------------------- BLE

void connect_callback(uint16_t handle) {
  conn_hdl = handle;
  dbg("connected\n");
}

void disconnect_callback(uint16_t handle, uint8_t reason) {
  (void)handle;
  conn_hdl = BLE_CONN_HANDLE_INVALID;
  dbg("disconnected, reason 0x%02x\n", reason);
}

// BLEConnection, *not* Bluefruit.Periph.setConn*(): that only sets the
// preferred parameters for connection setup and saves nothing on a live
// link.  The central may refuse, so nothing assumes it took.
void requestLatency(uint16_t latency) {
  if (conn_hdl == BLE_CONN_HANDLE_INVALID) { return; }
  BLEConnection *conn = Bluefruit.Connection(conn_hdl);
  if (conn) {
    conn->requestConnectionParameter(CONN_INTERVAL, latency, SUPV_TIMEOUT);
  }
}

void sendKeys() {
  // Rebuild the whole report from the pad state, so chords and roll-offs
  // are not dropped.  The boot report holds six keycodes; a seventh
  // simultaneous pad has nowhere to go.
  uint8_t keycodes[6] = {0, 0, 0, 0, 0, 0};
  uint8_t n = 0;
  for (uint8_t led = 0; led < NUM_LEDS && n < 6; led++) {
    if ((touches >> led) & 1) { keycodes[n++] = led_to_key[led]; }
  }
  if (conn_hdl != BLE_CONN_HANDLE_INVALID) {
    blehid.keyboardReport(0, keycodes);
  }
  dbg("touches %05lx -> %d key(s)%s\n", (unsigned long)touches, n,
      conn_hdl == BLE_CONN_HANDLE_INVALID ? ", not connected" : "");
}

// ------------------------------------------------------------- deep sleep

void enterDeepSleep() {
  dbg("-> DEEP\n");
  if (usb_attached) { Serial.flush(); }

  // ledPower() early-returns from IDLE, so drive both pins regardless: a
  // high data pin into a dead strip is the back-feed path.
  ledPower(false);
  digitalWrite(PIN_LED_DATA, LOW);
  digitalWrite(PIN_LED_POWER, HIGH);

  if (conn_hdl != BLE_CONN_HANDLE_INVALID) { Bluefruit.disconnect(conn_hdl); }
  Bluefruit.Advertising.stop();
  delay(100);

  // SW1 is a held level: arming SENSE_LOW while it is down wakes us at once.
  while (digitalRead(PIN_BUTTON) == LOW) { delay(10); }
  delay(50); // debounce the release

  nrf_gpio_cfg_sense_input(digitalPinToPinName(PIN_BUTTON),
                           NRF_GPIO_PIN_PULLUP, // SW1 shorts to GND
                           NRF_GPIO_PIN_SENSE_LOW);

  if (!touchArmWake() || digitalRead(PIN_BUTTON) == LOW) {
    dbg("   aborted, activity during arm\n");
    Bluefruit.Advertising.start(0);
    return;
  }

  sd_power_system_off();

  // Only reached with a debugger attached, which blocks SYSTEM_OFF.
  while (1) { delay(1000); }
}

// ------------------------------------------------------------------ setup

void setup() {
  // POWER is a restricted peripheral once Bluefruit.begin() starts the
  // SoftDevice, so read it first.
  usb_attached = (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk);
  uint32_t reset_reason = NRF_POWER->RESETREAS;
  NRF_POWER->RESETREAS = reset_reason; // write-1-to-clear, else it accumulates

  if (usb_attached) {
    Serial.begin(115200);
    // adds up to 2 s to a wake from DEEP, on USB only
    if (DEBUG) {
      while (!Serial && millis() < 2000) { delay(10); }
    }
  }

  pinMode(PIN_LED_POWER, OUTPUT);
  digitalWrite(PIN_LED_POWER, HIGH); // rail off until ledPower() says so
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  leds.begin();
  leds.setBrightness(LED_BRIGHTNESS);
  ledPower(true);

  if (!touchBegin()) {
    if (usb_attached) {
      Serial.println("No touch sensor found, check wiring!");
    }
    while (1) { delay(100); }
  }
  // Set once, not per tier: it soft-resets the sensor and restarts
  // calibration, which would swallow a touch in progress.
  touchSetLowPower(true);

  touch_sem = xSemaphoreCreateBinary();
  attachInterrupt(digitalPinToInterrupt(touchIntPin()), touch_isr, FALLING);

  Bluefruit.begin(1, 0); // one peripheral, no central
  Bluefruit.setTxPower(TX_POWER);
  Bluefruit.autoConnLed(false);
#if USE_DCDC
  // The return code is the confirmation: DCDCEN does not read back
  // reliably with the SoftDevice up.
  uint32_t dcdc_err = sd_power_dcdc_mode_set(NRF_POWER_DCDC_ENABLE);
  dbg("sd_power_dcdc_mode_set: %lu (0 = ok)\n", (unsigned long)dcdc_err);
#endif
  Bluefruit.Periph.setConnectCallback(connect_callback);
  Bluefruit.Periph.setDisconnectCallback(disconnect_callback);

  // Fills the adv packet exactly: flags 3 + appearance 4 + HID UUID 4 +
  // name 2+18.  Anything more goes in the scan response.
  static char adv_name[32];
  deviceIdName(adv_name, sizeof(adv_name), "sketchingpad");
  Bluefruit.setName(adv_name);

  bledis.setManufacturer("todbot");
  bledis.setModel("sketchingpad");
  bledis.begin();

  blehid.begin();
  // BLEHidAdafruit::begin() asks for 11.25-15 ms; widen the top end so the
  // central can pick something cheaper.
  Bluefruit.Periph.setConnInterval(9, CONN_INTERVAL);
  Bluefruit.Periph.setConnSupervisionTimeoutMS(SUPV_TIMEOUT * 10);

  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addAppearance(BLE_APPEARANCE_HID_KEYBOARD);
  Bluefruit.Advertising.addService(blehid);
  Bluefruit.Advertising.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 800); // 20 ms fast, 500 ms slow
  Bluefruit.Advertising.setFastTimeout(15);
  Bluefruit.Advertising.start(0); // forever; DEEP is what ends it

  showDeviceIdCode();

  dbg("ble_keyboard: sensor %s, pair with \"%s\"\n", touchName(),
      adv_name);
  // 0 = power-on, 1 = reset pin, 4 = soft reset, 8 = lockup,
  // 0x10000 = woke from System OFF.
  dbg("reset reason %08lx\n", (unsigned long)reset_reason);
  if (DEBUG && usb_attached) { touchReport(); }

  last_touch_ms = millis();

  // An MPR121 holds INT low until read, and a finger still on the pad that
  // woke us from DEEP leaves it low with no FALLING edge to come.
  xSemaphoreGive(touch_sem);
}

// ------------------------------------------------------------------- loop

void loop() {
  bool woken = (xSemaphoreTake(touch_sem, pdMS_TO_TICKS(TICK_MS)) == pdTRUE);
  uint32_t now = millis();

  if (digitalRead(PIN_BUTTON) == LOW) {
    enterDeepSleep();
    last_touch_ms = millis(); // aborted: do not retry at once
    return;
  }

  // Re-reading on the timeout while a pad is held catches a missed
  // release, which would otherwise be a key stuck down.
  if (woken || touches) {
    touches = touchRead();
    if (touches) { last_touch_ms = now; }
  }

  if (touches && !active) {
    active = true;
    requestLatency(LATENCY_ACTIVE);
    ledPower(true);
    dbg("-> ACTIVE\n");
  }

  if (touches != last_touches) {
    sendKeys();
    showTouches();
    last_touches = touches;
  }

  // Every pass, not just on timeout: if INT chatters with nothing touched,
  // this still demotes us.
  if (active && (now - last_touch_ms) >= ACTIVE_TIMEOUT_MS) {
    active = false;
    requestLatency(LATENCY_IDLE);
    ledPower(false);
    dbg("-> IDLE\n");
  }

  if (!active && (now - last_touch_ms) >= DEEP_TIMEOUT_MS) {
    enterDeepSleep();
    last_touch_ms = millis();
  }
}
