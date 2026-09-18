// hwtest_mpr.ino -- light up an LED for each touched pad, with a fading trail
//
// The MPR121 version of hwtest_ts20, for the sketchingpad1_mpr board, which
// replaces the TS20 with two MPR121s: U3 at 0x5A (ADDR to GND)
// and U4 at 0x5B (ADDR to VDD).  Visible behaviour is identical to
// hwtest_ts20 -- touch a pad, its LED lights and fades.
//
// Three things are different and this sketch exists to settle them:
//
//   1. The two /IRQ pins are open-collector and wired together onto the pin
//      TS20_INT used (XIAO A3).  The line says only that *something*
//      changed, never which chip, so every edge reads both.  See the
//      re-assert loop in loop() for the race that creates.
//
//   2. /IRQ is a level, held low until the touch status registers are
//      read, which is what makes a SYSTEM_OFF SENSE_LOW wake viable.
//      Neither the datasheet nor AN3895 says so; measured on U3 with
//      hwtest_bench_mpr, 2026-09-17.  int=N->N still prints it here,
//      where two chips share the line and either could be asserting.
//
//   3. CONFIG2 sets the sample interval, which is most of the sensor's idle
//      current, and costs latency in exchange.  See CONFIG2_VALUE below.
//
// Two modes, selected by USE_INT_PIN, as in hwtest_ts20:
//
//   0 = POLL.  Reads both chips every loop.
//   1 = INT.   Reads only when the shared IRQ says something changed, and
//              cross-checks every interrupt against a 500 ms watchdog read,
//              printing "INT MISSED" if the touch state ever changes with
//              no edge to announce it.
//
// Board: Seeeduino:nrf52:xiaonRF52840

#include <Adafruit_MPR121.h>
#include <Adafruit_NeoPixel.h>
#include <Wire.h>

// sketchingpad1_mpr -> XIAO nRF52840:
//   TOUCH_INT = D3 / A3  (both /IRQ pins, open collector, R1 pulls up)
//   SDA       = D4       (Wire default)
//   SCL       = D5       (Wire default)
//   LED_DAT   = D6 / TX
//   LED_POWER = D7 / RX  (P-FET gate: LOW = rail on)
#define PIN_LED_DATA  (6)
#define PIN_LED_POWER (7)
#define PIN_TOUCH_INT (A3)

#define USE_INT_PIN (1)

// Autoconfiguration searches each electrode's charge current and time on
// every Stop -> Run transition.  It depends on REXT being right (75k 1%),
// so turn it off if the chips ever come up with OVCF set.
#define USE_AUTOCONFIG (1)

// CDT 0.5us, SFI 4 samples, ESI 1 ms -- what Adafruit_MPR121::begin() writes.
// ESI is the bottom three bits and dominates idle current, at the cost of
// latency: the touch status update rate is ESI x SFI, so 0x24 responds in
// 64 ms rather than 4 ms.  Sweep this against a current meter.
//   0x20 = ESI 1 ms   393 uA/chip      0x25 = ESI 32 ms    17 uA/chip
//   0x24 = ESI 16 ms   29 uA/chip      0x27 = ESI 128 ms    8 uA/chip
#define CONFIG2_VALUE (0x20)

const uint8_t TOUCH_THRESHOLD = 12;
const uint8_t RELEASE_THRESHOLD = 6;

// Not in the library's register enum.
#define MPR121_OOR_L (0x02)
#define MPR121_OOR_H (0x03)

// What begin() writes to the ECR: baseline tracking on, 12 electrodes.
const uint8_t ECR_RUN_12 = 0x80 | 12;

// How often to read anyway in INT mode.  This is the diagnostic: if a poll
// finds a change that INT never announced, the INT line is not doing its job.
const uint32_t WATCHDOG_MS = 500;

// A chip re-asserting between the two status reads leaves the shared line
// low with no new edge behind it, so the read has to chase the level down.
// Bounded, because a chip stuck low would otherwise hang loop() forever.
const uint8_t MAX_INT_PASSES = 4;

const int NUM_LEDS = 20;
const int LED_BRIGHTNESS = 50;
const int DIM_BY = 5; // per-frame fade, same as hwtest_ts20

Adafruit_NeoPixel leds(NUM_LEDS, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);

const uint8_t MPR_ADDR[2] = {0x5A, 0x5B};
const char *MPR_NAME[2] = {"U3", "U4"};
Adafruit_MPR121 mpr[2];

// U4's ELE4-7 are TOUCH21-24, brought out to the J2 header rather than to
// pads.  ECR's ELE_EN enables ELE0..ELE(n-1) contiguously, so they cannot be
// switched off without losing ELE8-11, which are real pads.  Unpopulated they
// float, and every spurious touch pulls the *shared* IRQ down.
const uint32_t UNUSED_MASK = 0x0f0000UL;

// Index is LED number, value is the bit in the combined touch word, where
// bit = chip * 12 + ELEn: U3 (0x5A) is bits 0-11, U4 (0x5B) bits 12-23.
// Re-derived from the sketchingpad1_mpr netlist -- the MPR121 electrode
// order is not the pad order, and it is not the TS20 order either.
//
// Physical layout, TP refdes above each column, combined bit below:
//
//            TP17 TP18 TP19 TP20             10 11  0  1
//   TP1  TP2  TP3  TP4   TP5  TP6  TP7  TP8   6  7  8  9   2  3  4  5
//   TP9 TP10 TP11 TP12  TP13 TP14 TP15 TP16  20 21 22 23  12 13 14 15
const uint8_t led_to_pad[NUM_LEDS] = {
    6, 7, 8, 9, 2, 3, 4, 5, 20, 21,
    22, 23, 12, 13, 14, 15, 10, 11, 0, 1,
};

// our own pixel buffer, so the fade does not fight setBrightness() rounding
uint8_t pix[NUM_LEDS][3];

uint32_t touches = 0;      // last combined value read from both chips
uint32_t last_touches = 0; // for edge-triggered printing
bool ovcf[2] = {false, false};
bool fault = false;
bool last_fault = false;

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

// Reads the touch status of both chips in one transaction each.  Not
// mpr.touched(), which masks the result to 12 bits and so discards OVCF --
// the flag that says the REXT reference current is out of range, and the
// difference between a chip that is idle and one that has faulted to Stop
// Mode and is ignoring writes.
uint32_t readTouches() {
  uint32_t t = 0;
  for (int i = 0; i < 2; i++) {
    uint16_t raw = mpr[i].readRegister16(MPR121_TOUCHSTATUS_L);
    ovcf[i] = (raw >> 15) & 1;
    t |= (uint32_t)(raw & 0x0fff) << (12 * i);
  }
  fault = ovcf[0] || ovcf[1];
  return t & ~UNUSED_MASK;
}

void i2cScan() {
  Serial.print("i2c scan:");
  for (uint8_t a = 0x08; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { Serial.printf(" 0x%02x", a); }
  }
  Serial.println();
}

void reportChip(int i) {
  uint8_t ecr = mpr[i].readRegister8(MPR121_ECR);
  uint8_t status_h = mpr[i].readRegister8(MPR121_TOUCHSTATUS_H);
  uint8_t oor_h = mpr[i].readRegister8(MPR121_OOR_H);
  uint16_t oor = ((oor_h & 0x0f) << 8) | mpr[i].readRegister8(MPR121_OOR_L);

  Serial.printf("%s @0x%02x: ECR=%02x OVCF=%d OOR=%03x ACFF=%d ARFF=%d\n",
                MPR_NAME[i], MPR_ADDR[i], ecr, (status_h >> 7) & 1, oor,
                (oor_h >> 7) & 1, (oor_h >> 6) & 1);

  // begin() returns true without checking that its ECR write took, and a
  // chip that has tripped OVCF discards the write and stays in Stop Mode.
  // That failure otherwise looks exactly like nobody touching anything.
  if (ecr != ECR_RUN_12) {
    Serial.printf("  ECR should be %02x -- %s is not in Run Mode\n",
                  ECR_RUN_12, MPR_NAME[i]);
  }
  if (oor) {
    Serial.println("  out-of-range electrodes: autoconfig found no working"
                   " charge setting for these");
  }
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
  i2cScan();

  for (int i = 0; i < 2; i++) {
    if (!mpr[i].begin(MPR_ADDR[i], &Wire, TOUCH_THRESHOLD, RELEASE_THRESHOLD,
                      USE_AUTOCONFIG)) {
      Serial.printf("No MPR121 (%s) at 0x%02x, check wiring!\n", MPR_NAME[i],
                    MPR_ADDR[i]);
      while (1) { delay(10); }
    }
  }
  Wire.setClock(400000);

  for (int i = 0; i < 2; i++) {
    mpr[i].writeRegister(MPR121_CONFIG2, CONFIG2_VALUE);
  }

  // Park the four J2 electrodes at an unreachable threshold, so they cannot
  // chatter the shared IRQ line.  Masking them in software is not enough --
  // the interrupt is hardware.
  for (uint8_t e = 4; e <= 7; e++) {
    mpr[1].writeRegister(MPR121_TOUCHTH_0 + 2 * e, 0xff);
  }

  // Each write above cycles the chip through Stop Mode, and every Stop ->
  // Run transition restarts autoconfig's per-channel search.  Read the OOR
  // flags too soon and a clean 000 only means "still searching".
  delay(100);
  for (int i = 0; i < 2; i++) { reportChip(i); }

#if USE_INT_PIN
  // INPUT, not INPUT_PULLUP: R1 already pulls this up to +3.3V on the board.
  pinMode(PIN_TOUCH_INT, INPUT);
  // CHANGE rather than FALLING, as in hwtest_ts20: we are characterising the
  // line, and want the release edge too if there is one.
  attachInterrupt(digitalPinToInterrupt(PIN_TOUCH_INT), touch_isr, CHANGE);
  Serial.printf("mode: INT on D%d (idle level %d), watchdog %lu ms\n",
                PIN_TOUCH_INT, digitalRead(PIN_TOUCH_INT),
                (unsigned long)WATCHDOG_MS);
#else
  Serial.println("mode: POLL");
#endif

  Serial.printf("config2=%02x touch/release=%d/%d autoconfig=%d\n",
                CONFIG2_VALUE, TOUCH_THRESHOLD, RELEASE_THRESHOLD,
                USE_AUTOCONFIG);
}

void loop() {
  // fade everything toward black
  for (int n = 0; n < NUM_LEDS; n++) {
    for (int c = 0; c < 3; c++) {
      pix[n][c] = (pix[n][c] > DIM_BY) ? pix[n][c] - DIM_BY : 0;
    }
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

    int_before = digitalRead(PIN_TOUCH_INT);
    do {
      touches = readTouches();
      passes++;
    } while (digitalRead(PIN_TOUCH_INT) == LOW && passes < MAX_INT_PASSES);
    int_after = digitalRead(PIN_TOUCH_INT);

    if (int_after == LOW && touches == 0) {
      // Nothing is touched, yet somebody is still pulling the line down.
      Serial.println("INT STUCK LOW with no touches");
    }

    // An asserted line whose read shows nothing new.  int_before is what
    // filters out our own read's rising edge, which arrives with the line
    // already high.  What is left is an electrode toggling under the mask:
    // the J2 four, whose IRQ is hardware even though their bits are not.
    if (int_fired && int_before == LOW && touches == last_touches) {
      chatter++;
    }
  }
#else
  touches = readTouches();
#endif

  if (touches != last_touches) {
#if USE_INT_PIN
    Serial.printf("[%s] edges=%lu int=%d->%d passes=%d touches=%06lx",
                  int_fired ? "INT " : "poll",
                  (unsigned long)edges_at_last_read, int_before, int_after,
                  passes, (unsigned long)touches);
    if (!int_fired) {
      // The state changed but no edge arrived: either the IRQ net is open,
      // the line does not track touch, or one chip was read while the other
      // was still asserting.
      Serial.print("  <- INT MISSED");
    }
    Serial.println();
#else
    Serial.printf("touches=%06lx\n", (unsigned long)touches);
#endif
    for (int n = 0; n < NUM_LEDS; n++) {
      uint8_t pad = led_to_pad[n];
      if ((touches >> pad) & 1) {
        Serial.printf("  led %d (TP%d) <- %s ELE%d\n", n, n + 1,
                      MPR_NAME[pad / 12], pad % 12);
      }
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

  if (fault != last_fault) {
    if (fault) {
      Serial.printf("OVCF U3=%d U4=%d -- REXT over current, chip stopped\n",
                    ovcf[0], ovcf[1]);
    } else {
      Serial.println("OVCF cleared");
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
