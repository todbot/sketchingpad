// hwtest_ts20.ino -- light up an LED for each touched pad, with a fading trail
//
// Arduino port of circuitpython/hwtest/code_test_ts20.py, for a
// Seeed XIAO nRF52840 plugged into the sketchingpad_stripproto1 board.
//
// Sensitivity is 0 (most sensitive) to 15 (least) and can be changed
// per pad at runtime, see below.
//
// Two modes, selected by USE_INT_PIN below:
//
//   0 = POLL.  Reads the TS20 over I2C every loop, as the CircuitPython
//       test does.  Works on the board as built.
//
//   1 = INT.   Reads the TS20 only when its INT pin says something changed.
//       U1 pin 19 (INT, open collector) shares a net with its 10k pull-up
//       R16 and XIAO A3.  This mode cross-checks every interrupt against
//       a 500 ms watchdog read and prints "INT MISSED" if the touch state
//       ever changes without an edge -- the TS20 library has no INT
//       support, so its behaviour had to be settled empirically.
//
// Note INT mode does not save power here -- the loop still runs at 100 Hz
// to animate the LED fade.  What it removes is the I2C traffic.  The power
// win lands in ble_keyboard_lowpower, which can then sleep between touches.
//
// Board: Seeeduino:nrf52:xiaonRF52840

#include <Adafruit_NeoPixel.h>
#include <TS20.h>
#include <Wire.h>

// sketchingpad_stripproto1 -> XIAO nRF52840:
//   SDA     = D4  (Wire default)
//   SCL     = D5  (Wire default)
//   LED_DAT = D6  (also TX)
#define PIN_LED_DATA (6)

#define USE_INT_PIN (1)

// TS20_INT is routed to XIAO A3 (= D3).
#define PIN_TS20_INT (A3)

// How often to read anyway in INT mode.  This is the diagnostic: if a poll
// finds a change that INT never announced, the INT line is not doing its job.
const uint32_t WATCHDOG_MS = 500;

const int NUM_LEDS = 20;
const int LED_BRIGHTNESS = 50;
const int DIM_BY = 5; // per-frame fade, same as the CircuitPython test

Adafruit_NeoPixel leds(NUM_LEDS, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);
TS20 ts20;

// Index is LED number, value is the pad wired to it.  This is the table
// the CircuitPython test calls "touch_to_led" and searches with .index();
// walking the LEDs instead of the pads gets the same mapping for free.
const uint8_t led_to_pad[NUM_LEDS] = {
    18, 1, 3, 4, 6, 9, 19, 2, 5, 8,
    17, 0, 10, 11, 16, 15, 14, 13, 12, 7,
};

// our own pixel buffer, so the fade does not fight setBrightness() rounding
uint8_t pix[NUM_LEDS][3];

uint32_t touches = 0;      // last value read from the TS20
uint32_t last_touches = 0; // for edge-triggered printing
bool noisy = false;
bool last_noisy = false;

#if USE_INT_PIN
volatile uint32_t int_edges = 0;
volatile bool int_pending = false;
uint32_t edges_at_last_read = 0;
uint32_t last_read_ms = 0;

void ts20_isr() {
  int_edges++;
  int_pending = true;
}
#endif

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

  leds.begin();
  leds.setBrightness(LED_BRIGHTNESS);
  leds.clear();
  leds.show();
  memset(pix, 0, sizeof(pix));

  delay(100); // wait 100 msec after power up, as per datasheet
  if (!ts20.begin()) {
    Serial.println("No TS20 found, check wiring!");
    while (1) { delay(10); }
  }
  // after begin(), which does Wire.begin() and forces the bus back to 100 kHz
  Wire.setClock(400000);
  ts20.setSensitivity(15); // 0 = most sensitive, 15 = least

  // tweak individual pads that read too hot or too cold, e.g.:
  // ts20.setSensitivity(12, 3);
  // ts20.setSensitivity(15, 7);

#if USE_INT_PIN
  // INPUT, not INPUT_PULLUP: R16 already pulls this up to +3.3V on the
  // board.  Use INPUT_PULLUP if you are testing without that resistor.
  pinMode(PIN_TS20_INT, INPUT);
  // CHANGE rather than FALLING: we do not know yet whether INT is a level
  // held low for the duration of a touch or a pulse, and CHANGE catches
  // releases too, so the LEDs clear promptly either way.
  attachInterrupt(digitalPinToInterrupt(PIN_TS20_INT), ts20_isr, CHANGE);
  Serial.printf("mode: INT on D%d (idle level %d), watchdog %lu ms\n",
                PIN_TS20_INT, digitalRead(PIN_TS20_INT),
                (unsigned long)WATCHDOG_MS);
#else
  Serial.println("mode: POLL");
#endif

  Serial.printf("pad 0 needs a %.2f%% capacitance change\n",
                ts20.sensitivityPercent(0));
}

void loop() {
  // fade everything toward black
  for (int n = 0; n < NUM_LEDS; n++) {
    for (int c = 0; c < 3; c++) {
      pix[n][c] = (pix[n][c] > DIM_BY) ? pix[n][c] - DIM_BY : 0;
    }
  }

  bool int_fired = false;

#if USE_INT_PIN
  bool due = (millis() - last_read_ms) >= WATCHDOG_MS;
  if (int_pending || due) {
    int_pending = false;
    last_read_ms = millis();

    uint32_t edges_now = int_edges; // volatile: sample once
    int_fired = (edges_now != edges_at_last_read);
    edges_at_last_read = edges_now;

    touches = ts20.touched();
    noisy = ts20.noiseDetected(); // a second I2C read: only on a read event
  }
#else
  touches = ts20.touched();
  noisy = ts20.noiseDetected();
#endif

  if (touches != last_touches) {
#if USE_INT_PIN
    Serial.printf("[%s] edges=%lu level=%d touches=%05lx",
                  int_fired ? "INT " : "poll", (unsigned long)edges_at_last_read,
                  digitalRead(PIN_TS20_INT), (unsigned long)touches);
    if (!int_fired) {
      // The state changed but no edge arrived: either INT needs configuring
      // after all, the bodge wire is off, or it does not track touch.
      Serial.print("  <- INT MISSED");
    }
    Serial.println();
#else
    Serial.printf("touches=%05lx\n", (unsigned long)touches);
#endif
    for (int n = 0; n < NUM_LEDS; n++) {
      uint8_t pad = led_to_pad[n];
      if ((touches >> pad) & 1) { Serial.printf("  pad %d -> led %d\n", pad, n); }
    }
    last_touches = touches;
  }

  uint32_t c = colorwheel(millis() / 10);
  for (int n = 0; n < NUM_LEDS; n++) {
    if ((touches >> led_to_pad[n]) & 1) {
      pix[n][0] = (c >> 16) & 0xff;
      pix[n][1] = (c >> 8) & 0xff;
      pix[n][2] = c & 0xff;
    }
  }

  if (noisy != last_noisy) {
    Serial.println(noisy ? "NOISY" : "noise cleared");
    last_noisy = noisy;
  }

  for (int n = 0; n < NUM_LEDS; n++) {
    leds.setPixelColor(n, pix[n][0], pix[n][1], pix[n][2]);
  }
  leds.show();

  delay(10);
}
