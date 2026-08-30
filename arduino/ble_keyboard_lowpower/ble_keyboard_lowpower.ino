// ble_keyboard_lowpower.ino -- sketchingpad as a low-power BLE HID keyboard
//
// Seeed XIAO nRF52840 on the sketchingpad_stripproto1 board (rev with the
// LED rail switch, the TS20 INT line, and SW1 wired).  Touching a pad types
// a character over BLE HID.
//
// Board: Seeeduino:nrf52:xiaonRF52840  (Adafruit-Bluefruit-derived core,
// S140 SoftDevice + FreeRTOS).  No extra libraries beyond TS20 and
// Adafruit NeoPixel are needed -- bluefruit.h ships with the core.
//
// Three power tiers:
//
//   ACTIVE  connected, slave latency 0, LED rail on.  Touch is reported
//           immediately.  Entered on any touch.
//   IDLE    after ACTIVE_TIMEOUT_MS untouched: connected, slave latency 20
//           (~630 ms effective), LED rail switched off at the P-FET.
//   DEEP    after DEEP_TIMEOUT_MS untouched, or on a button press:
//           SYSTEM_OFF, ~1-3 uA, woken by the TS20 INT line or SW1.
//
// Neither connected tier polls the TS20.  Its INT line drives a GPIO
// interrupt which releases a semaphore, so the CPU sleeps between touches
// instead of waking 15-100 times a second to ask over I2C.
//
// DEEP is a one-way door: waking from SYSTEM_OFF resets the chip, so the
// SoftDevice restarts and BLE has to reconnect (~1-3 s).  The touch that
// woke the board is gone by then -- treat it as a wake gesture, not a
// keystroke.  That is why DEEP is on a minutes-long timer and IDLE, which
// keeps the link up, does the routine power saving.

#include <Adafruit_NeoPixel.h>
#include <TS20.h>
#include <Wire.h>
#include <bluefruit.h>

// sketchingpad_stripproto1 -> XIAO nRF52840:
//   PWM_AUDIO = D0 / A0  (net at the header only, unused here)
//   BUTTON    = D2 / A2  (SW1 to GND, needs internal pull-up)
//   TS20 INT  = D3 / A3  (open collector, R16 pulls it up)
//   SDA       = D4       (Wire default)
//   SCL       = D5       (Wire default)
//   LED_DAT   = D6 / TX
//   LED_POWER = D7 / RX  (P-FET gate: LOW = rail on)
#define PIN_LED_DATA  (6)
#define PIN_LED_POWER (7)
#define PIN_BUTTON    (A2)
#define PIN_TS20_INT  (A3)

// Enable the nRF52840 DC/DC regulator.  Set to 0 if the XIAO turns out not
// to have the DC/DC inductors populated (that shows as an instant brownout).
#define USE_DCDC (1)

const int NUM_LEDS = 20;
const int LED_BRIGHTNESS = 40;
const int TS20_SENSITIVITY = 15; // 0 = most sensitive, 15 = least
const int8_t TX_POWER = -4;      // dBm, plenty for desk range

const uint32_t ACTIVE_TIMEOUT_MS = 10000;  // ACTIVE -> IDLE
const uint32_t DEEP_TIMEOUT_MS = 300000;   // IDLE   -> DEEP  (5 minutes)
const uint32_t TICK_MS = 250;              // bookkeeping only, not polling

// connection parameters (interval in 1.25 ms units, timeout in 10 ms units)
const uint16_t CONN_INTERVAL = 24;   // 30 ms
const uint16_t LATENCY_ACTIVE = 0;
const uint16_t LATENCY_IDLE = 20;    // 21 * 30 ms = 630 ms effective
const uint16_t SUPV_TIMEOUT = 400;   // 4000 ms, comfortably above the above

Adafruit_NeoPixel leds(NUM_LEDS, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);
TS20 ts20;
BLEDis bledis;
BLEHidAdafruit blehid;

// Index is LED number, value is the pad wired to it (same table as hwtest_ts20)
const uint8_t led_to_pad[NUM_LEDS] = {
    18, 1, 3, 4, 6, 9, 19, 2, 5, 8,
    17, 0, 10, 11, 16, 15, 14, 13, 12, 7,
};

// Index is pad number.  Pad layout from planning.md, pads numbered 1-20:
//        17 18 19 20
//     1  2  3  4    5  6  7  8
//     9 10 11 12   13 14 15 16
const uint8_t pad_to_key[TS20_NUM_PADS] = {
    HID_KEY_A, HID_KEY_S, HID_KEY_D, HID_KEY_F,     // pads 1-4
    HID_KEY_G, HID_KEY_H, HID_KEY_J, HID_KEY_K,     // pads 5-8
    HID_KEY_Z, HID_KEY_X, HID_KEY_C, HID_KEY_V,     // pads 9-12
    HID_KEY_B, HID_KEY_N, HID_KEY_M, HID_KEY_COMMA, // pads 13-16
    HID_KEY_1, HID_KEY_2, HID_KEY_3, HID_KEY_4,     // pads 17-20
};

bool     usb_present = false;
uint16_t conn_hdl = BLE_CONN_HANDLE_INVALID;
uint32_t last_touches = 0;
uint32_t last_touch_ms = 0;
bool     active = true;
bool     rail_on = false;
uint32_t shown_touches = 0xFFFFFFFF;

SemaphoreHandle_t touch_sem;

#define LOG(...) do { if (usb_present) { Serial.printf(__VA_ARGS__); } } while (0)

// The TS20 INT line is a ~1 ms active-low pulse on any change -- measured on
// a scope, not assumed.  It says only *that* something changed, never what,
// so every edge has to be followed by an I2C read.
void ts20_isr() {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(touch_sem, &woken);
  portYIELD_FROM_ISR(woken);
}

// ------------------------------------------------------------- LED rail

// Sequencing matters in both directions.  Powering up: enable the rail and
// let ~2 uF of decoupling settle before clocking data in.  Powering down:
// drive the data line low first, or current sneaks into the strip through
// the SK6812 input protection diode and partially powers a rail we just
// switched off.
void ledPower(bool on) {
  if (on == rail_on) { return; }
  if (on) {
    digitalWrite(PIN_LED_POWER, LOW); // P-FET on
    delay(2);
    rail_on = true;
    leds.clear();
    leds.show();
    shown_touches = 0xFFFFFFFF;
  } else {
    leds.clear();
    leds.show();
    digitalWrite(PIN_LED_DATA, LOW);
    digitalWrite(PIN_LED_POWER, HIGH); // P-FET off (gate to source)
    rail_on = false;
  }
}

// ------------------------------------------------------------------- BLE

void connect_callback(uint16_t handle) {
  conn_hdl = handle;
  LOG("connected\n");
}

void disconnect_callback(uint16_t handle, uint8_t reason) {
  (void)handle;
  conn_hdl = BLE_CONN_HANDLE_INVALID;
  LOG("disconnected, reason 0x%02x\n", reason);
}

// Renegotiates the live connection.  This is BLEConnection, *not*
// Bluefruit.Periph.setConn*(), which only writes the preferred parameters
// used at connection setup and would silently save nothing here.  The
// central may refuse, so nothing downstream assumes it took.
void requestLatency(uint16_t latency) {
  if (conn_hdl == BLE_CONN_HANDLE_INVALID) { return; }
  BLEConnection *conn = Bluefruit.Connection(conn_hdl);
  if (conn) {
    conn->requestConnectionParameter(CONN_INTERVAL, latency, SUPV_TIMEOUT);
  }
}

void startAdv() {
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addAppearance(BLE_APPEARANCE_HID_KEYBOARD);
  Bluefruit.Advertising.addService(blehid);
  Bluefruit.Advertising.addName();

  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 800); // 20 ms fast, 500 ms slow
  Bluefruit.Advertising.setFastTimeout(15);
  Bluefruit.Advertising.start(0); // 0 = forever; DEEP is what ends it now
}

// ------------------------------------------------------------ deep sleep

void enterDeepSleep() {
  LOG("-> DEEP sleep\n");
  if (usb_present) { Serial.flush(); }

  ledPower(false);
  // ledPower() early-returns if the rail is already off, which it is when we
  // arrive here from IDLE -- so force the data pin low and the gate high
  // unconditionally, or D6 sits high into an unpowered strip and back-powers
  // it through the SK6812 input diode.
  digitalWrite(PIN_LED_DATA, LOW);
  digitalWrite(PIN_LED_POWER, HIGH);

  if (conn_hdl != BLE_CONN_HANDLE_INVALID) { Bluefruit.disconnect(conn_hdl); }
  Bluefruit.Advertising.stop();
  delay(100);

  // SW1 is a level, not a pulse: arming SENSE_LOW while it is still held
  // would wake us the instant we slept.
  while (digitalRead(PIN_BUTTON) == LOW) { delay(10); }
  delay(50); // debounce the release

  // Arm the wake sources BEFORE the final check.  Anything arriving after
  // this point asserts DETECT, and a SENSE condition already met when
  // SYSTEM_OFF is entered wakes the chip immediately -- so the hardware
  // covers the window from here on.
  nrf_gpio_cfg_sense_input(digitalPinToPinName(PIN_TS20_INT),
                           NRF_GPIO_PIN_NOPULL,   // R16 pulls this up already
                           NRF_GPIO_PIN_SENSE_LOW);
  nrf_gpio_cfg_sense_input(digitalPinToPinName(PIN_BUTTON),
                           NRF_GPIO_PIN_PULLUP,   // SW1 shorts to GND
                           NRF_GPIO_PIN_SENSE_LOW);

  // Now close the software half of the race.  A touch that landed before
  // SENSE was armed left no level behind -- INT is a 1 ms pulse -- but the
  // TS20 still holds the state in its output register, so read it.
  if (ts20.touched() != 0 || digitalRead(PIN_BUTTON) == LOW) {
    LOG("   aborted, activity during arm\n");
    return;
  }

  sd_power_system_off();

  // Only reached with a debugger attached, which blocks SYSTEM_OFF.
  while (1) { delay(1000); }
}

// ------------------------------------------------------------------ setup

void setup() {
  // Read VBUS before Bluefruit.begin() enables the SoftDevice -- POWER is a
  // restricted peripheral once it is running.
  usb_present = (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
  if (usb_present) {
    Serial.begin(115200);
    delay(200); // never block on !Serial
  }

  // Hold the rail off until we mean it.  At reset this pin is an input and
  // the 100k gate pull-up already holds the P-FET off; this just makes it
  // explicit before we drive anything.
  pinMode(PIN_LED_POWER, OUTPUT);
  digitalWrite(PIN_LED_POWER, HIGH);

  pinMode(PIN_BUTTON, INPUT_PULLUP);

  leds.begin();
  leds.setBrightness(LED_BRIGHTNESS);
  digitalWrite(PIN_LED_DATA, LOW);
  ledPower(true);

  delay(100); // wait 100 msec after power up, as per datasheet
  // Auto mode instead of fast mode: lower continuous current on the sensor.
  // Set before begin() so the single startup reset() writes it -- calling it
  // later would soft-reset the chip and restart calibration.
  ts20.setFastMode(false);
  if (!ts20.begin()) {
    LOG("No TS20 found, check wiring!\n");
    while (1) { delay(100); }
  }
  // after begin(), which does Wire.begin() and forces the bus back to 100 kHz
  Wire.setClock(400000);
  ts20.setSensitivity(TS20_SENSITIVITY);

  touch_sem = xSemaphoreCreateBinary();
  pinMode(PIN_TS20_INT, INPUT); // R16 pulls it up on the board
  attachInterrupt(digitalPinToInterrupt(PIN_TS20_INT), ts20_isr, FALLING);

  Bluefruit.begin(1, 0); // one peripheral, no central
  Bluefruit.setTxPower(TX_POWER);
  Bluefruit.autoConnLed(false);
  Bluefruit.setName("sketchingpad");
  Bluefruit.Periph.setConnectCallback(connect_callback);
  Bluefruit.Periph.setDisconnectCallback(disconnect_callback);

#if USE_DCDC
  // The SVC return code is the confirmation -- reading NRF_POWER->DCDCEN back
  // is not reliable with the SoftDevice enabled.
  uint32_t err = sd_power_dcdc_mode_set(NRF_POWER_DCDC_ENABLE);
  LOG("sd_power_dcdc_mode_set: %lu (0 = ok)\n", (unsigned long)err);
#endif

  bledis.setManufacturer("todbot");
  bledis.setModel("sketchingpad");
  bledis.begin();

  blehid.begin();
  // BLEHidAdafruit::begin() asks for 11.25-15 ms; widen the top end so the
  // central can pick something cheaper.
  Bluefruit.Periph.setConnInterval(9, CONN_INTERVAL);
  Bluefruit.Periph.setConnSupervisionTimeoutMS(SUPV_TIMEOUT * 10);

  startAdv();

  last_touch_ms = millis();
  active = true;
  LOG("ble_keyboard_lowpower ready, advertising as \"sketchingpad\"\n");
}

// ------------------------------------------------------------------- loop

void loop() {
  // Blocks until the TS20 INT fires or TICK_MS elapses.  The timeout only
  // services the tier timers; it does no I2C, so an untouched board costs
  // four wakeups a second and nothing on the bus.
  bool by_int = (xSemaphoreTake(touch_sem, pdMS_TO_TICKS(TICK_MS)) == pdTRUE);
  uint32_t now = millis();

  if (digitalRead(PIN_BUTTON) == LOW) {
    enterDeepSleep();
    last_touch_ms = millis();
    return;
  }

  if (by_int) {
    uint32_t touches = ts20.touched();
    if (touches) { last_touch_ms = now; }

    if (!active) {
      active = true;
      requestLatency(LATENCY_ACTIVE);
      ledPower(true);
      LOG("-> ACTIVE\n");
    }

    if (touches != last_touches) {
      // Rebuild the whole report from the current pad state rather than only
      // handling all-up, so chords and roll-offs are not dropped.  The boot
      // keyboard report holds a modifier plus six keycodes; a seventh
      // simultaneous pad has nowhere to go.
      uint8_t keycodes[6] = {0, 0, 0, 0, 0, 0};
      uint8_t n = 0;
      for (uint8_t pad = 0; pad < TS20_NUM_PADS && n < 6; pad++) {
        if ((touches >> pad) & 1) { keycodes[n++] = pad_to_key[pad]; }
      }
      if (conn_hdl != BLE_CONN_HANDLE_INVALID) {
        blehid.keyboardReport(0, keycodes);
      }
      LOG("touches %05lx -> %d key(s)\n", (unsigned long)touches, n);
      last_touches = touches;
    }

    if (rail_on && touches != shown_touches) {
      for (int i = 0; i < NUM_LEDS; i++) {
        leds.setPixelColor(i, ((touches >> led_to_pad[i]) & 1) ? 0xff00ff : 0);
      }
      leds.show();
      shown_touches = touches;
    }
  }

  // Tier bookkeeping runs on every pass, not just on the timeout. If the INT
  // line chatters with no pad actually touched, last_touch_ms goes stale and
  // these still demote us; an early return here would pin us in ACTIVE.
  if (active && (now - last_touch_ms) >= ACTIVE_TIMEOUT_MS) {
    active = false;
    requestLatency(LATENCY_IDLE);
    ledPower(false);
    LOG("-> IDLE\n");
  }

  if (!active && (now - last_touch_ms) >= DEEP_TIMEOUT_MS) {
    enterDeepSleep();
    last_touch_ms = millis();
  }
}
