// hwtest_touch_lowpower.ino -- hwtest_touch with the power techniques, no BLE
//
// Replaces hwtest_ts20_lowpower, and runs on the MPR121 board too:
// touch.h probes I2C at startup and the banner says which sensor it
// found.  Same visible behaviour as hwtest_touch -- touch a pad, its LED
// lights and fades.  The difference is everything underneath.  This is
// the sketch to measure against, because with no radio running the
// numbers are the firmware's own -- ble_keyboard_lowpower's floor is
// dominated by BLE.
//
// Three tiers, same shape as ble_keyboard_lowpower minus the connection
// management:
//
//   ACTIVE  any touch in the last 10 s.  LED rail on, frames at 100 Hz to
//           animate the fade.
//   IDLE    LED rail switched off at the P-FET, not merely blanked.  Still
//           interrupt-armed, so a touch is picked up immediately.
//   DEEP    1 minute untouched, or a button press.  SYSTEM_OFF, woken by
//           the touch INT line or the button.  Waking resets the chip.
//
// The DEEP path is the one place the two sensors genuinely differ: the
// TS20's INT is a ~1 ms pulse and the MPR121's is a level held low until
// its status registers are read, so the arm and the final read go in
// opposite orders.  touchArmWake() owns that; see touch.h.
//
// No BLE means no SoftDevice, which makes two things *simpler* than in
// ble_keyboard_lowpower: POWER is not a restricted peripheral, so the
// DC/DC regulator and SYSTEM_OFF are plain register writes rather than
// SVC calls that have to be error-checked.
//
// Board: Seeeduino:nrf52:xiaonRF52840

#include <Adafruit_NeoPixel.h>
#include <Wire.h>

// sketchingpad1 / sketchingpad1_mpr -> XIAO nRF52840:
//   BUTTON    = D10/MOSI (SW1 to GND, needs internal pull-up)
//   TOUCH_INT = D3 / A3  (open collector, R1 pulls up)
//   SDA       = D4       (Wire default)
//   SCL       = D5       (Wire default)
//   LED_DAT   = D6 / TX
//   LED_POWER = D7 / RX  (P-FET gate: LOW = rail on)
#define PIN_LED_DATA  (6)
#define PIN_LED_POWER (7)
#define PIN_BUTTON    (10)

// Enable the nRF52840 DC/DC regulator.  Set to 0 if this XIAO turns out
// not to have the DC/DC inductors populated (that shows as an instant
// brownout loop rather than a subtle fault).
#define USE_DCDC (1)

#include "touch.h"

const int NUM_LEDS = TOUCH_NUM_LEDS;
const int LED_BRIGHTNESS = 50;
const int DIM_BY = 5; // per-frame fade, same as hwtest_touch

const uint32_t ACTIVE_TIMEOUT_MS = 10000;  // ACTIVE -> IDLE
//const uint32_t DEEP_TIMEOUT_MS = 300000;   // IDLE   -> DEEP (5 minutes)
const uint32_t DEEP_TIMEOUT_MS = 60000;   // IDLE   -> DEEP (1 minute)
const uint32_t FRAME_MS = 10;              // while anything is lit: 100 Hz
const uint32_t TICK_MS = 250;              // while dark: tier timers only

Adafruit_NeoPixel leds(NUM_LEDS, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);

// our own pixel buffer, so the fade does not fight setBrightness() rounding
uint8_t pix[NUM_LEDS][3];

bool usb_attached = false;
#define LOG(...) do { if (usb_attached) { Serial.printf(__VA_ARGS__); } } while (0)

bool rail_on = false;
bool active = true;
uint32_t last_touch_ms = 0;
uint32_t touches = 0;      // LED-indexed, from touchRead()
uint32_t last_touches = 0;

// The ISR only releases this; all the work happens in loop().
SemaphoreHandle_t touch_sem;

void touch_isr() {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(touch_sem, &woken);
  portYIELD_FROM_ISR(woken);
}

// --------------------------------------------------------------- LED rail

// Sequencing matters in both directions.  Powering up: enable the rail and
// let ~2 uF of decoupling settle before clocking data in.  Powering down:
// drive the data line low first, or current sneaks into the strip through
// the SK6812 input protection diode and partially powers a rail we just
// switched off.
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
    memset(pix, 0, sizeof(pix)); // or the old trail paints one frame on return
  }
}

// ------------------------------------------------------------- deep sleep

void enterDeepSleep() {
  LOG("DEEP sleep\n");
  if (usb_attached) { Serial.flush(); }

  // Drive both LED pins explicitly: ledPower() early-returns if we arrived
  // from IDLE, and a floating data pin into a dead strip is exactly the
  // back-feed path we are trying to avoid.
  ledPower(false);
  digitalWrite(PIN_LED_DATA, LOW);
  digitalWrite(PIN_LED_POWER, HIGH);

  // SW1 is a held level, so arming SENSE_LOW while it is still down would
  // wake us the instant we slept.
  while (digitalRead(PIN_BUTTON) == LOW) { delay(10); }
  delay(50); // debounce the release

  nrf_gpio_cfg_sense_input(digitalPinToPinName(PIN_BUTTON),
                           NRF_GPIO_PIN_PULLUP, // SW1 shorts to GND
                           NRF_GPIO_PIN_SENSE_LOW);

  // Arms the touch line and closes the software half of its race, in
  // whichever order this sensor needs.
  if (!touchArmWake() || digitalRead(PIN_BUTTON) == LOW) {
    LOG("   aborted, activity during arm\n");
    return;
  }

  // No SoftDevice is running, so this is ours to write directly; with BLE
  // up it would have to be sd_power_system_off().
  NRF_POWER->SYSTEMOFF = 1;
  __DSB();

  // A debugger emulates SYSTEM_OFF rather than entering it, so execution
  // can continue here.  Parking keeps the behaviour close either way.
  while (1) { __WFE(); }
}

// ------------------------------------------------------------------ setup

uint32_t colorwheel(uint8_t pos) {
  if (pos < 85) {
    return Adafruit_NeoPixel::Color(255 - pos * 3, pos * 3, 0);
  } else if (pos < 170) {
    pos -= 85;
    return Adafruit_NeoPixel::Color(0, 255 - pos * 3, pos * 3);
  }
  pos -= 170;
  return Adafruit_NeoPixel::Color(pos * 3, 0, 255 - pos * 3);
}

void setup() {
  // Never touch Serial unless USB is actually attached: on battery the
  // CDC stack is pure overhead, and while(!Serial) would hang forever.
  usb_attached = (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk);
  if (usb_attached) {
    Serial.begin(115200);
    // note this adds up to 2000 millis of startup time after deep sleep
    while (!Serial && millis() < 2000) { delay(10); }
  }

#if USE_DCDC
  NRF_POWER->DCDCEN = 1;
#endif

  pinMode(PIN_LED_POWER, OUTPUT);
  digitalWrite(PIN_LED_POWER, HIGH); // rail off until ledPower() says so
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  leds.begin();
  leds.setBrightness(LED_BRIGHTNESS);
  ledPower(true);
  memset(pix, 0, sizeof(pix));

  if (!touchBegin()) {
    LOG("No touch sensor found, check wiring!\n");
    while (1) { delay(100); }
  }
  // Set once here, not per tier: this soft-resets the sensor and restarts
  // calibration, which would swallow a touch in progress.
  touchSetLowPower(true);

  touch_sem = xSemaphoreCreateBinary();

  // FALLING, not CHANGE: one event per state change is enough, since we
  // read the sensor either way.  hwtest_touch uses CHANGE because it is
  // characterising the line; here we just want the wakeup.
  attachInterrupt(digitalPinToInterrupt(touchIntPin()), touch_isr, FALLING);

  last_touch_ms = millis();
  if (usb_attached) {
    LOG("hwtest_touch_lowpower: sensor %s, DCDCEN=%lu, touch a pad\n",
        touchName(), (unsigned long)NRF_POWER->DCDCEN);
    touchReport();
  }
}

// ------------------------------------------------------------------- loop

void loop() {
  // Anything lit means the fade still needs frames; a dark board only
  // needs to wake often enough to service the tier timers.  Either way
  // this blocks -- delay()/xSemaphoreTake drop to the FreeRTOS idle task
  // and WFE, so the CPU is asleep between events rather than polling I2C.
  bool lit = false;
  for (int n = 0; n < NUM_LEDS && !lit; n++) {
    if (pix[n][0] || pix[n][1] || pix[n][2]) { lit = true; }
  }
  uint32_t wait = (lit && rail_on) ? FRAME_MS : TICK_MS;
  bool by_int = (xSemaphoreTake(touch_sem, pdMS_TO_TICKS(wait)) == pdTRUE);

  uint32_t now = millis();

  // Every INT edge has to be followed by a read: the line only says that
  // something changed, never what it changed to.  Re-reading on the plain
  // timeout as well, but only while a pad is still held, catches a missed
  // release -- which is the one that shows, as an LED stuck on -- without
  // putting any I2C traffic on an idle board.
  if (by_int || touches) {
    touches = touchRead();
    if (touches) { last_touch_ms = now; }
  }

  if (touches != last_touches) {
    LOG("touches=%05lx\n", (unsigned long)touches);
    last_touches = touches;
  }

  // Tier bookkeeping runs every pass, not just on the timeout.  If INT
  // chatters with nothing actually touched, last_touch_ms would otherwise
  // go stale and pin the board in ACTIVE forever.
  if (touches && !active) {
    active = true;
    ledPower(true);
    LOG("-> ACTIVE\n");
  } else if (active && (now - last_touch_ms) >= ACTIVE_TIMEOUT_MS) {
    active = false;
    ledPower(false);
    LOG("-> IDLE\n");
  }

  if (!active && (now - last_touch_ms) >= DEEP_TIMEOUT_MS) {
    enterDeepSleep();
    last_touch_ms = millis(); // aborted: do not retry immediately
  }

  if (digitalRead(PIN_BUTTON) == LOW) {
    enterDeepSleep();
    last_touch_ms = millis();
  }

  if (!rail_on) { return; } // nothing to draw while the rail is down

  for (int n = 0; n < NUM_LEDS; n++) {
    for (int c = 0; c < 3; c++) {
      pix[n][c] = (pix[n][c] > DIM_BY) ? pix[n][c] - DIM_BY : 0;
    }
  }
  uint32_t col = colorwheel(now / 10);
  for (int n = 0; n < NUM_LEDS; n++) {
    if ((touches >> n) & 1) {
      pix[n][0] = (col >> 16) & 0xff;
      pix[n][1] = (col >> 8) & 0xff;
      pix[n][2] = col & 0xff;
    }
  }
  for (int n = 0; n < NUM_LEDS; n++) {
    leds.setPixelColor(n, pix[n][0], pix[n][1], pix[n][2]);
  }
  leds.show();
}
