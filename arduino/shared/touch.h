// touch.h -- one touch driver over the TS20 and the 2x MPR121 boards
//
// sketchingpad1 has one 20-channel TS20 at 0x6A; sketchingpad1_mpr
// replaces it with two 12-channel MPR121s at 0x5A and 0x5B.  The
// addresses are disjoint, so the board is identified at runtime and one
// binary runs on both -- no board #define, nothing to reflash when
// swapping boards on the bench.
//
// Everything here is in LED-index space: bit n of touchRead() is LED n,
// which is TP(n+1) on the silkscreen.  Each backend keeps its own
// mapping table privately, in touch_map.h.
//
// nRF52-specific, because touchArmWake() arms GPIO SENSE directly.  That
// is why the tables live in the other header, which is portable and
// host-testable.
//
// Knobs a sketch may #define before including this:
//   TOUCH_INT_PIN     shared INT/IRQ line                      (A3)
//   USE_AUTOCONFIG    MPR121 autoconfig on Stop -> Run          (1)
//   CONFIG2_VALUE     MPR121 CONFIG2, run tier                (0x20)
//   CONFIG2_LOWPOWER  MPR121 CONFIG2, low-power tier          (0x24)
//   TOUCH_SENSITIVITY TS20 threshold, 0 most sensitive           (15)
#pragma once

#include <Adafruit_MPR121.h>
#include <Arduino.h>
#include <TS20.h>
#include <Wire.h>

#include "touch_map.h"

#ifndef TOUCH_INT_PIN
#define TOUCH_INT_PIN (A3)
#endif

// Autoconfiguration searches each electrode's charge current and time on
// every Stop -> Run transition.  It depends on REXT being right (75k 1%),
// so turn it off if the chips ever come up with OVCF set.
#ifndef USE_AUTOCONFIG
#define USE_AUTOCONFIG (1)
#endif

// CONFIG2 bits 2-0 are ESI, the electrode sample interval, and most of the
// sensor's idle current.  What it costs is latency: the touch status
// update rate is ESI x SFI, so 0x24 responds in 64 ms rather than 4 ms.
//   0x20 = ESI 1 ms   393 uA/chip      0x25 = ESI 32 ms    17 uA/chip
//   0x24 = ESI 16 ms   29 uA/chip      0x27 = ESI 128 ms    8 uA/chip
#ifndef CONFIG2_VALUE
#define CONFIG2_VALUE (0x20)
#endif
#ifndef CONFIG2_LOWPOWER
#define CONFIG2_LOWPOWER (0x24)
#endif

#ifndef TOUCH_SENSITIVITY
#define TOUCH_SENSITIVITY (15)
#endif

static const uint8_t TOUCH_THRESHOLD = 12;
static const uint8_t RELEASE_THRESHOLD = 6;

// Not in the library's register enum.
#define MPR121_OOR_L (0x02)
#define MPR121_OOR_H (0x03)

// What begin() writes to the ECR: baseline tracking on, 12 electrodes.
static const uint8_t TOUCH_ECR_RUN_12 = 0x80 | 12;

// Two MPR121s wired-OR onto one line means a chip can re-assert in the gap
// between the two status reads, leaving the line low with no new edge
// behind it -- and nothing would ever read it again.  So the read chases
// the level down, bounded, because a chip stuck low would otherwise hang
// loop() forever.
static const uint8_t TOUCH_MAX_PASSES = 4;

enum TouchBackend { TOUCH_NONE, TOUCH_TS20, TOUCH_MPR };

static TouchBackend touch_backend = TOUCH_NONE;
static TS20 touch_ts20;
static Adafruit_MPR121 touch_mpr[2];
static const uint8_t TOUCH_MPR_ADDR[2] = {0x5A, 0x5B};
static const char *TOUCH_MPR_NAME[2] = {"U3", "U4"};
static bool touch_ovcf[2] = {false, false};
static bool touch_fault = false;
static uint8_t touch_passes = 0;

static inline bool touchI2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

/// Probes 0x6A then 0x5A/0x5B and brings up whichever sensor answers.
static inline bool touchBegin() {
  Wire.begin();
  delay(100); // the TS20 datasheet wants 100 ms after power up before I2C

  // INPUT, not INPUT_PULLUP: R1/R16 already pulls this up on every board.
  // Set unconditionally -- touchRead() reads the level even in poll mode.
  pinMode(TOUCH_INT_PIN, INPUT);

  if (touchI2cPresent(TS20_ADDR_GND)) {
    if (!touch_ts20.begin()) { return false; }
    Wire.setClock(400000); // begin() forces the bus back to 100 kHz
    touch_ts20.setSensitivity(TOUCH_SENSITIVITY);
    touch_backend = TOUCH_TS20;
    return true;
  }

  if (!touchI2cPresent(TOUCH_MPR_ADDR[0]) ||
      !touchI2cPresent(TOUCH_MPR_ADDR[1])) {
    return false;
  }
  for (int i = 0; i < 2; i++) {
    if (!touch_mpr[i].begin(TOUCH_MPR_ADDR[i], &Wire, TOUCH_THRESHOLD,
                            RELEASE_THRESHOLD, USE_AUTOCONFIG)) {
      return false;
    }
  }
  Wire.setClock(400000);
  for (int i = 0; i < 2; i++) {
    touch_mpr[i].writeRegister(MPR121_CONFIG2, CONFIG2_VALUE);
  }

  // Park the four J2 electrodes at an unreachable threshold, so they
  // cannot chatter the shared IRQ.  Masking them in touch_map.h is not
  // enough -- the interrupt is hardware.
  for (uint8_t e = 4; e <= 7; e++) {
    touch_mpr[1].writeRegister(MPR121_TOUCHTH_0 + 2 * e, 0xff);
  }

  // Each write above cycles the chip through Stop Mode, and every Stop ->
  // Run transition restarts autoconfig's per-channel search.  Read the OOR
  // flags too soon and a clean 000 only means "still searching".
  delay(100);
  touch_backend = TOUCH_MPR;
  return true;
}

static inline const char *touchName() {
  switch (touch_backend) {
  case TOUCH_TS20: return "TS20";
  case TOUCH_MPR: return "2x MPR121";
  default: return "none";
  }
}

static inline uint8_t touchIntPin() { return TOUCH_INT_PIN; }

/// Bit n = LED n = TP(n+1).  Every INT edge must be followed by one of
/// these: the line says only that something changed, never what to.
static inline uint32_t touchRead() {
  touch_passes = 0;
  if (touch_backend == TOUCH_TS20) {
    touch_passes = 1;
    uint32_t channels = touch_ts20.touched();
    touch_fault = touch_ts20.noiseDetected(); // a second I2C read
    return touchMapTS20(channels);
  }
  if (touch_backend != TOUCH_MPR) { return 0; }

  uint32_t electrodes = 0;
  do {
    electrodes = 0;
    for (int i = 0; i < 2; i++) {
      // Not touched(), which masks to 12 bits and so discards OVCF -- the
      // difference between a chip that is idle and one that has faulted to
      // Stop Mode and is ignoring writes.
      uint16_t raw = touch_mpr[i].readRegister16(MPR121_TOUCHSTATUS_L);
      touch_ovcf[i] = (raw >> 15) & 1;
      electrodes |= (uint32_t)(raw & 0x0fff) << (12 * i);
    }
    touch_passes++;
  } while (digitalRead(TOUCH_INT_PIN) == LOW && touch_passes < TOUCH_MAX_PASSES);

  touch_fault = touch_ovcf[0] || touch_ovcf[1];
  return touchMapMPR(electrodes);
}

/// Status reads the last touchRead() took.  >1 means a chip re-asserted
/// mid-read; always 1 on the TS20.
static inline uint8_t touchReadPasses() { return touch_passes; }

/// TS20 noise flag, or either MPR121's OVCF.  Updated by touchRead().
static inline bool touchFault() { return touch_fault; }

/// Trade latency for idle current.  Set once in setup(), not per tier:
/// both backends soft-reset on this write, which restarts calibration and
/// would swallow a touch in progress.
static inline void touchSetLowPower(bool low) {
  if (touch_backend == TOUCH_TS20) {
    touch_ts20.setFastMode(!low);
  } else if (touch_backend == TOUCH_MPR) {
    uint8_t cfg = low ? CONFIG2_LOWPOWER : CONFIG2_VALUE;
    for (int i = 0; i < 2; i++) {
      touch_mpr[i].writeRegister(MPR121_CONFIG2, cfg);
    }
    delay(100); // that write restarted autoconfig; OOR read too soon reads 000
  }
}

static inline void touchArmSense() {
  nrf_gpio_cfg_sense_input(digitalPinToPinName(TOUCH_INT_PIN),
                           NRF_GPIO_PIN_NOPULL, // the board pulls this up
                           NRF_GPIO_PIN_SENSE_LOW);
}

/// Arms the INT line as a SYSTEM_OFF wake source.  False means a touch
/// arrived during the arm: do not sleep.
///
/// The two orderings are opposite, which is why this cannot be a flag the
/// sketch branches on.  MPR121: read first, because the read is what
/// releases the held-low line -- arming SENSE_LOW while it is still low
/// latches DETECT and wakes the board the instant it sleeps.  TS20: arm
/// first, because its ~1 ms pulse is long gone, so the final read is what
/// closes the software race, the chip still holding the state in its
/// output register.
static inline bool touchArmWake() {
  if (touch_backend == TOUCH_MPR) {
    uint32_t touches = touchRead();
    touchArmSense();
    return touches == 0;
  }
  touchArmSense();
  return touchRead() == 0;
}

/// Where LED n comes from, for diagnostic prints.  Not stable across
/// calls: one static buffer.
static inline const char *touchSource(uint8_t led) {
  static char buf[16];
  if (led >= TOUCH_NUM_LEDS) { return "?"; }
  if (touch_backend == TOUCH_TS20) {
    snprintf(buf, sizeof(buf), "ch%d", touch_led_to_ts20[led]);
  } else {
    uint8_t bit = touch_led_to_mpr[led];
    snprintf(buf, sizeof(buf), "%s ELE%d", TOUCH_MPR_NAME[bit / 12], bit % 12);
  }
  return buf;
}

/// Whatever this sensor can say about its own health, printed to Serial.
/// Callers must have brought Serial up.
static inline void touchReport() {
  if (touch_backend == TOUCH_TS20) {
    Serial.printf("TS20: sensitivity %d, pad 0 needs a %.2f%% capacitance"
                  " change, noise=%d\n",
                  TOUCH_SENSITIVITY, touch_ts20.sensitivityPercent(0),
                  touch_ts20.noiseDetected());
    return;
  }
  if (touch_backend != TOUCH_MPR) { return; }

  Serial.printf("config2=%02x touch/release=%d/%d autoconfig=%d\n",
                CONFIG2_VALUE, TOUCH_THRESHOLD, RELEASE_THRESHOLD,
                USE_AUTOCONFIG);
  for (int i = 0; i < 2; i++) {
    uint8_t ecr = touch_mpr[i].readRegister8(MPR121_ECR);
    uint8_t status_h = touch_mpr[i].readRegister8(MPR121_TOUCHSTATUS_H);
    uint8_t oor_h = touch_mpr[i].readRegister8(MPR121_OOR_H);
    uint16_t oor = ((oor_h & 0x0f) << 8) | touch_mpr[i].readRegister8(MPR121_OOR_L);

    Serial.printf("%s @0x%02x: ECR=%02x OVCF=%d OOR=%03x ACFF=%d ARFF=%d\n",
                  TOUCH_MPR_NAME[i], TOUCH_MPR_ADDR[i], ecr, (status_h >> 7) & 1,
                  oor, (oor_h >> 7) & 1, (oor_h >> 6) & 1);

    // begin() returns true without checking that its ECR write took, and a
    // chip that has tripped OVCF discards the write and stays in Stop Mode.
    // That failure otherwise looks exactly like nobody touching anything.
    if (ecr != TOUCH_ECR_RUN_12) {
      Serial.printf("  ECR should be %02x -- %s is not in Run Mode\n",
                    TOUCH_ECR_RUN_12, TOUCH_MPR_NAME[i]);
    }
    if (oor) {
      Serial.println("  out-of-range electrodes: autoconfig found no working"
                     " charge setting for these");
    }
  }
}
