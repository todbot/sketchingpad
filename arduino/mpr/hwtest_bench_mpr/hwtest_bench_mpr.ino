// hwtest_bench_mpr.ino -- one or two MPR121s, touch to serial, no LEDs
//
// The smallest thing that proves an MPR121 works and that the pad mapping
// is right: printing every touch and release as the chip, the electrode,
// the TP number silkscreened on the board, and the LED index the firmware
// uses.  Nothing drives the LED rail, so this runs on a pair of breakouts
// with jumpers as happily as on sketchingpad1_mpr.
//
// It probes both addresses and uses whichever answer, so a half-populated
// board or a single breakout is a normal case, not an error.  U3 (0x5A)
// carries twelve pads, TP1-8 and TP17-20; U4 (0x5B) carries TP9-16 plus
// the four J2 electrodes, which have no pad and print as "no pad".
//
// Those four are left live on purpose.  hwtest_mpr masks them and parks
// their thresholds at 0xFF so they cannot chatter the shared IRQ line;
// here you want to see whether they would.  Untouched "no pad" lines
// appearing on their own is the answer.
//
// Two modes, selected by USE_INT_PIN:
//
//   0 = POLL.  Reads every present chip every POLL_MS.
//   1 = INT.   Reads only when /IRQ says something changed, and prints the
//              pin level either side of that read as int=N->N.
//
// That int=N->N field is the point of INT mode here.  It answered the
// question the datasheet and AN3895 both leave open: /IRQ is a level, held
// low until the touch status registers are read (measured on U3 2026-09-17
// with only one chip wired, int=0->1 on both touch and release).  One chip
// is what made that unambiguous -- with two on the shared line, a level
// still low after a read could be either of them.
//
// hwtest_mpr is the full version: the LEDs, the J2 mitigation, and both
// chips required rather than optional.
//
// Board: Seeeduino:nrf52:xiaonRF52840

#include <Adafruit_MPR121.h>
#include <Wire.h>

#define USE_INT_PIN (1)

// Both /IRQ pins land here on sketchingpad1_mpr; on breakouts, wire each
// chip's IRQ to it.  They are open collector, so tying them is correct.
#define PIN_TOUCH_INT (A3)

#define USE_AUTOCONFIG (1)

// See hwtest_mpr for what this costs and buys; 0x20 is what the library
// writes, ESI 1 ms.
#define CONFIG2_VALUE (0x20)

const uint8_t TOUCH_THRESHOLD = 12;
const uint8_t RELEASE_THRESHOLD = 6;

const uint32_t POLL_MS = 20;

// How often to read anyway in INT mode.  This is the diagnostic: if a read
// finds a change that /IRQ never announced, the line is not doing its job.
const uint32_t WATCHDOG_MS = 500;

// With two chips wired-OR onto one line, one can re-assert between the two
// reads, leaving the line low with no new edge behind it -- and nothing
// would read it again.  Bounded, because a chip stuck low would otherwise
// hang loop().
const uint8_t MAX_INT_PASSES = 4;

// Not in the library's register enum.
#define MPR121_OOR_L (0x02)
#define MPR121_OOR_H (0x03)

// What begin() writes to the ECR: baseline tracking on, 12 electrodes.
const uint8_t ECR_RUN_12 = 0x80 | 12;

const int NUM_LEDS = 20;
const int NUM_CHIPS = 2;
const uint8_t MPR_ADDR[NUM_CHIPS] = {0x5A, 0x5B};
const char *MPR_NAME[NUM_CHIPS] = {"U3", "U4"};

Adafruit_MPR121 mpr[NUM_CHIPS];
bool present[NUM_CHIPS];

// Index is LED number, value is the bit in the combined touch word, where
// bit = chip * 12 + ELEn.  Kept in the same form as hwtest_mpr's, rather
// than inverted by hand, so the two sketches cannot drift apart; setup()
// turns it into the electrode-first table this sketch prints from.
const uint8_t led_to_pad[NUM_LEDS] = {
    6, 7, 8, 9, 2, 3, 4, 5, 20, 21,
    22, 23, 12, 13, 14, 15, 10, 11, 0, 1,
};

int8_t ele_to_led[NUM_CHIPS][12]; // -1 where an electrode reaches no pad

uint32_t touches = 0;
uint32_t last_touches = 0;

#if USE_INT_PIN
volatile uint32_t int_edges = 0;
volatile bool int_pending = false;
uint32_t edges_at_last_read = 0;
uint32_t last_read_ms = 0;

void touch_isr() {
  int_edges++;
  int_pending = true;
}
#endif

uint32_t readTouches() {
  uint32_t t = 0;
  for (int i = 0; i < NUM_CHIPS; i++) {
    if (present[i]) { t |= (uint32_t)mpr[i].touched() << (12 * i); }
  }
  return t;
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
    Serial.printf("  ECR should be %02x -- not in Run Mode, check REXT"
                  " is 75k\n", ECR_RUN_12);
  }
}

void printMap(int i) {
  Serial.printf("%s @0x%02x map:\n", MPR_NAME[i], MPR_ADDR[i]);
  for (uint8_t e = 0; e < 12; e++) {
    if (ele_to_led[i][e] < 0) {
      Serial.printf("  ELE%-2d  no pad\n", e);
    } else {
      Serial.printf("  ELE%-2d  TP%-2d  led %d\n", e, ele_to_led[i][e] + 1, ele_to_led[i][e]);
    }
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 5000) { delay(10); }

  Serial.println("hwtest_bench_mpr");

  Wire.begin();
  // Left at 100 kHz, unlike hwtest_mpr: bench wiring is longer and its
  // pull-ups are whatever came with the breakouts.
  int found = 0;
  for (int i = 0; i < NUM_CHIPS; i++) {
    present[i] = mpr[i].begin(MPR_ADDR[i], &Wire, TOUCH_THRESHOLD,
                              RELEASE_THRESHOLD, USE_AUTOCONFIG);
    if (present[i]) {
      mpr[i].writeRegister(MPR121_CONFIG2, CONFIG2_VALUE);
      found++;
    } else {
      Serial.printf("%s @0x%02x: absent\n", MPR_NAME[i], MPR_ADDR[i]);
    }
  }
  if (found == 0) {
    Serial.println("no MPR121 on the bus, check wiring!");
    while (1) { delay(10); }
  }

  // Those writes cycled each chip through Stop Mode, and every Stop -> Run
  // transition restarts autoconfig's per-channel search.  Read the OOR
  // flags too soon and a clean 000 only means "still searching".
  delay(100);

  for (int i = 0; i < NUM_CHIPS; i++) {
    for (uint8_t e = 0; e < 12; e++) {
      ele_to_led[i][e] = -1;
      for (int n = 0; n < NUM_LEDS; n++) {
        if (led_to_pad[n] == i * 12 + e) {
          ele_to_led[i][e] = n;
          break;
        }
      }
    }
  }

  for (int i = 0; i < NUM_CHIPS; i++) {
    if (present[i]) {
      reportChip(i);
      printMap(i);
    }
  }

#if USE_INT_PIN
  // INPUT, not INPUT_PULLUP: R1 already pulls this up on the board.  Use
  // INPUT_PULLUP on breakouts that have no pull-up of their own.
  pinMode(PIN_TOUCH_INT, INPUT);
  // CHANGE, not FALLING: we are characterising the line, so we want the
  // release edge too if there is one.
  attachInterrupt(digitalPinToInterrupt(PIN_TOUCH_INT), touch_isr, CHANGE);
  Serial.printf("mode: INT on D%d (idle level %d), watchdog %lu ms\n",
                PIN_TOUCH_INT, digitalRead(PIN_TOUCH_INT),
                (unsigned long)WATCHDOG_MS);
#else
  Serial.println("mode: POLL");
#endif

  Serial.printf("%d chip%s, touch a pad\n", found, found == 1 ? "" : "s");
}

void loop() {
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
      // With one chip wired that is the chip; with two, suspect the one
      // whose IRQ you have not actually connected to PIN_TOUCH_INT.
      Serial.println("INT STUCK LOW with no touches");
    }
  }
#else
  touches = readTouches();
#endif

  if (touches != last_touches) {
#if USE_INT_PIN
    Serial.printf("[%s] edges=%lu int=%d->%d passes=%d\n",
                  int_fired ? "INT " : "poll",
                  (unsigned long)edges_at_last_read, int_before, int_after,
                  passes);
    if (!int_fired) {
      // The state changed but no edge arrived: either that chip's IRQ is
      // not wired, or the line does not track touch.
      Serial.println("  <- INT MISSED");
    }
#endif
    for (int i = 0; i < NUM_CHIPS; i++) {
      if (!present[i]) { continue; }
      for (uint8_t e = 0; e < 12; e++) {
        uint8_t bit = i * 12 + e;
        bool now = (touches >> bit) & 1;
        if (now == ((last_touches >> bit) & 1)) { continue; }
        const char *what = now ? "touched" : "released";
        if (ele_to_led[i][e] < 0) {
          Serial.printf("%-9s %s ELE%-2d  no pad\n", what, MPR_NAME[i], e);
        } else {
          Serial.printf("%-9s %s ELE%-2d  TP%-2d  led %d\n", what,
                        MPR_NAME[i], e, ele_to_led[i][e] + 1,
                        ele_to_led[i][e]);
        }
      }
    }
    last_touches = touches;
  }

#if USE_INT_PIN
  delay(1); // the ISR only sets a flag; this keeps checking it cheap
#else
  delay(POLL_MS);
#endif
}
