// SPDX-FileCopyrightText: 2026 Tod Kurt
// SPDX-License-Identifier: GPL-3.0-or-later
//
// hwtest_touch.ino -- light up an LED for each touched pad, with a fading trail
//
// Replaces hwtest_ts20 and hwtest_mpr with one sketch that runs on both
// boards.  touch.h probes I2C at startup -- TS20 at 0x6A, MPR121s at 0x5A
// and 0x5B -- so swapping boards on the bench needs no reflash, and the
// banner says which one it found.
//
// Everything here is in LED-index space: bit n of touchRead() is LED n,
// which is TP(n+1) on the silkscreen.  There is no led_to_pad table in
// this file; both tables live in touch_map.h, one per board rev.
//
// Two modes, selected by USE_INT_PIN:
//
//   0 = POLL.  Reads the sensor every loop.
//   1 = INT.   Reads only when the INT line says something changed, and
//              cross-checks every interrupt against a 500 ms watchdog
//              read, printing "INT MISSED" if the touch state ever
//              changes with no edge to announce it.
//
// The two sensors' INT lines behave differently and this sketch is how
// that was settled: the TS20's is a ~1 ms pulse, the MPR121's a level held
// low until the status registers are read.  int=N->N prints the level
// either side of the read, so the two are told apart on sight.  passes=
// is the MPR121 case only: two chips wired-OR onto one line means one can
// re-assert between the two reads, so touchRead() chases the level down.
//
// Board: Seeeduino:nrf52:xiaonRF52840

#include <Adafruit_NeoPixel.h>
#include <Wire.h>

// sketchingpad1 / sketchingpad1_mpr -> XIAO nRF52840:
//   BUTTON    = D10/MOSI (SW1 to GND, needs internal pull-up)
//   TOUCH_INT = D3 / A3  (open collector, R1 pulls up; both /IRQ on _mpr)
//   SDA       = D4       (Wire default)
//   SCL       = D5       (Wire default)
//   LED_DAT   = D6 / TX
//   LED_POWER = D7 / RX  (P-FET gate: LOW = rail on)
#define PIN_LED_DATA  (6)
#define PIN_LED_POWER (7)
#define PIN_BUTTON    (10)

#define USE_INT_PIN (1)

// MPR121 only; see touch.h.  Turn off if the chips come up with OVCF set.
#define USE_AUTOCONFIG (1)

#include "touch.h"

// How often to read anyway in INT mode.  This is the diagnostic: if a poll
// finds a change that INT never announced, the INT line is not doing its job.
const uint32_t WATCHDOG_MS = 500;

const int NUM_LEDS = TOUCH_NUM_LEDS;
const int LED_BRIGHTNESS = 50;
const int DIM_BY = 5; // per-frame fade, same as hwtest_ts20

Adafruit_NeoPixel leds(NUM_LEDS, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);

// our own pixel buffer, so the fade does not fight setBrightness() rounding
uint8_t pix[NUM_LEDS][3];

uint32_t touches = 0;      // LED-indexed, from touchRead()
uint32_t last_touches = 0; // for edge-triggered printing
bool last_fault = false;
uint32_t last_report_ms = 0;

#if USE_INT_PIN
volatile uint32_t int_edges = 0;
volatile bool int_pending = false;
uint32_t edges_at_last_read = 0;
uint32_t last_read_ms = 0;
uint32_t chatter = 0;
uint32_t chatter_reported = 0;
uint32_t last_chatter_ms = 0;

void touch_isr() {
  int_edges++;
  int_pending = true;
}
#endif

void i2cScan() {
  Serial.print("i2c scan:");
  for (uint8_t a = 0x08; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { Serial.printf(" 0x%02x", a); }
  }
  Serial.println();
}

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
  Serial.begin(115200);

  pinMode(PIN_BUTTON, INPUT_PULLUP);  // little physical button up top

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
  memset(pix, 0, sizeof(pix));

  Wire.begin();
  delay(100); // the TS20 wants 100 ms after power up before it answers I2C
  i2cScan();

  if (!touchBegin()) {
    Serial.println("No touch sensor found, check wiring!");
    while (1) { delay(10); }
  }
  Serial.printf("sensor: %s\n", touchName());
  touchReport();

#if USE_INT_PIN
  // CHANGE rather than FALLING: we are characterising the line, and want
  // the release edge too.
  attachInterrupt(digitalPinToInterrupt(touchIntPin()), touch_isr, CHANGE);
  Serial.printf("mode: INT on D%d (idle level %d), watchdog %lu ms\n",
                touchIntPin(), digitalRead(touchIntPin()),
                (unsigned long)WATCHDOG_MS);
#else
  Serial.println("mode: POLL");
#endif
}

void loop() {
  // fade everything toward black
  for (int n = 0; n < NUM_LEDS; n++) {
    for (int c = 0; c < 3; c++) {
      pix[n][c] = (pix[n][c] > DIM_BY) ? pix[n][c] - DIM_BY : 0;
    }
  }

  if (digitalRead(PIN_BUTTON) == LOW) {
      Serial.println("BUTTON press!");
      delay(20);
  }

#if USE_INT_PIN
  bool int_fired = false;
  int int_before = -1, int_after = -1;
  uint8_t passes = 0;

  bool due = (millis() - last_read_ms) >= WATCHDOG_MS;
  if (int_pending || due) {
    int_pending = false;
    last_read_ms = millis();

    uint32_t edges_now = int_edges; // volatile: sample once
    int_fired = (edges_now != edges_at_last_read);
    edges_at_last_read = edges_now;

    int_before = digitalRead(touchIntPin());
    touches = touchRead();
    passes = touchReadPasses();
    int_after = digitalRead(touchIntPin());

    if (int_after == LOW && touches == 0) {
      // Nothing is touched, yet somebody is still pulling the line down.
      Serial.println("INT STUCK LOW with no touches");
    }

    // An asserted line whose read shows nothing new.  int_before is what
    // filters out our own read's rising edge, which arrives with the line
    // already high.  What is left is an electrode toggling under the mask:
    // on the MPR121 board, the J2 four, whose IRQ is hardware even though
    // touch_map.h drops their bits.
    if (int_fired && int_before == LOW && touches == last_touches) {
      chatter++;
    }
  }
#else
  touches = touchRead();
#endif

  if (touches != last_touches) {
#if USE_INT_PIN
    Serial.printf("[%s] edges=%lu int=%d->%d passes=%d touches=%05lx",
                  int_fired ? "INT " : "poll",
                  (unsigned long)edges_at_last_read, int_before, int_after,
                  passes, (unsigned long)touches);
    if (!int_fired) {
      // The state changed but no edge arrived: either the INT net is open,
      // the line does not track touch, or one chip was read while the
      // other was still asserting.
      Serial.print("  <- INT MISSED");
    }
    Serial.println();
#else
    Serial.printf("touches=%05lx\n", (unsigned long)touches);
#endif
    for (int n = 0; n < NUM_LEDS; n++) {
      if ((touches >> n) & 1) {
        Serial.printf("  led %d (TP%d) <- %s\n", n, n + 1, touchSource(n));
      }
    }
    last_touches = touches;
  }

  uint32_t c = colorwheel(millis() / 10);
  for (int n = 0; n < NUM_LEDS; n++) {
    if ((touches >> n) & 1) {
      pix[n][0] = (c >> 16) & 0xff;
      pix[n][1] = (c >> 8) & 0xff;
      pix[n][2] = c & 0xff;
    }
  }

  bool fault = touchFault();
  if (fault != last_fault) {
    // TS20: noise.  MPR121: OVCF, the REXT reference current out of range,
    // on which the chip drops to Stop Mode and discards ECR writes.
    Serial.println(fault ? "FAULT" : "fault cleared");
    // The TS20's noise flag can chatter at loop rate, and a report is
    // several I2C reads, so this stays rate-limited like chatter: below.
    if (fault && (millis() - last_report_ms) >= 1000) {
      last_report_ms = millis();
      touchReport();
    }
    last_fault = fault;
  }

#if USE_INT_PIN
  if (chatter != chatter_reported && (millis() - last_chatter_ms) >= 1000) {
    last_chatter_ms = millis();
    Serial.printf("chatter: %lu edges with no change\n",
                  (unsigned long)(chatter - chatter_reported));
    chatter_reported = chatter;
  }
#endif

  for (int n = 0; n < NUM_LEDS; n++) {
    leds.setPixelColor(n, pix[n][0], pix[n][1], pix[n][2]);
  }
  leds.show();

  delay(10);
}
