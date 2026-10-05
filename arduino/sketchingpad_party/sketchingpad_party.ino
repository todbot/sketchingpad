// SPDX-FileCopyrightText: 2026 Tod Kurt
// SPDX-License-Identifier: GPL-3.0-or-later
//
// sketchingpad_party.ino -- ble_broadcast_anim with the power tiers
//
// Originally named ble_broadcast_anim_lp.
//
// Same game as ble_broadcast_anim: touch pads to record an animation, it
// plays locally and goes out over BLE advertising, every board in range
// plays it.  Same packets, so it plays with ble_broadcast_anim boards too.
//
// Three tiers, same shape as hwtest_touch_lowpower:
//
//   ACTIVE  touched, or playing something, in the last 10 s.  LED rail on,
//           scanning 100%.
//   IDLE    LED rail off at the P-FET.  Scanner sniffs 30 ms every 1 s and
//           goes back to 100% the moment it hears a chunk.
//   DEEP    5 min with no touch and no animation received, or a button
//           press.  SYSTEM_OFF, woken by touch INT or the button.  Deaf to
//           the radio until then.
//
// Sniffing only works because senders cycle the chunks for PREAMBLE_MS
// before the counted passes: long enough that every sniff window lands in
// it.  Cycling rather than holding one chunk means an ACTIVE receiver
// completes in one cycle instead of waiting out the preamble.
// An IDLE board can miss a ble_broadcast_anim sender, which has no
// preamble; an ACTIVE one will not.
//
// loop() blocks on one semaphore, given by the touch INT and by
// scan_callback, so a dark board wakes 4x/sec for the tier timers and
// does no I2C until something happens.
//
// Untested on hardware.  To check, with two or more boards:
//
//   - IDLE sniff: let a receiver go dark, record on a sender, and count
//     how often the receiver catches the animation.  Should be every time.
//   - DEEP: set DEEP_TIMEOUT_MS to 60000, confirm it sleeps after a
//     minute idle and that a pad or the button wakes it.
//   - Interop: plays both ways with a plain ble_broadcast_anim board.  An
//     IDLE receiver may miss that sender (no preamble); ACTIVE should not.
//
// Board: Seeeduino:nrf52:xiaonRF52840

// 1 = narrate to Serial, but only when USB is attached.
#define DEBUG (1)

// Set to 0 if this XIAO has no DC/DC inductors (shows as a brownout loop).
#define USE_DCDC (1)

#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <bluefruit.h>

#include "anim_wire.h"
#include "device_id_code.h"
#include "led_anim.h"
#include "touch.h"

// sketchingpad -> XIAO nRF52840:
//   BUTTON    = D10/MOSI (SW1 to GND, needs internal pull-up)
//   TOUCH_INT = D3 / A3  (open collector, pulled up on the board)
//   SDA = D4, SCL = D5 (Wire defaults)
#define PIN_LED_DATA  (6)
#define PIN_LED_POWER (7) // P-FET gate: LOW = rail on
#define PIN_BUTTON    (10)

// Manufacturer Specific Data company ID.  0xFFFF is the SIG's test value:
// fine for our own gear, "shall not be used in shipping end products".
#define COMPANY_ID (0xFFFF)

const int NUM_LEDS = ANIM_NUM_LEDS;
const int LED_BRIGHTNESS = 40;

// The top row, TP17-20, is LEDs 16-19.
const uint8_t SWATCH_FIRST_LED = 16;
const uint8_t SWATCH_COUNT = 4;

const uint32_t ACTIVE_TIMEOUT_MS = 10000;  // ACTIVE -> IDLE
const uint32_t DEEP_TIMEOUT_MS = 300000;   // IDLE   -> DEEP (5 minutes)
const uint32_t FRAME_MS = 10;              // while anything moves: 100 Hz
const uint32_t TICK_MS = 250;              // otherwise: tier timers only
const uint32_t RX_HOLD_MS = 3000;          // full scan after hearing a chunk

const uint32_t REC_SILENCE_MS = 1500;  // quiet this long ends a recording
const uint32_t CHUNK_AIRTIME_MS = 200; // ~10 advertising events per chunk
const uint8_t SEND_PASSES = 3;         // whole cycle repeated this often
// Must exceed SCAN_SNIFF_INTERVAL + SCAN_SNIFF_WINDOW, with margin.
const uint32_t PREAMBLE_MS = 1200;

// Units of 0.625 ms; 32 = 20 ms, the SoftDevice minimum.
const uint16_t ADV_INTERVAL_SEND = 32;
const uint16_t SCAN_FULL_INTERVAL = 160;  // 100 ms
const uint16_t SCAN_FULL_WINDOW = 160;
const uint16_t SCAN_SNIFF_INTERVAL = 1600; // 1 s
// 30 ms: adverts are 20 ms + up to 10 ms of random delay apart, so any
// 30 ms window holds at least one.
const uint16_t SCAN_SNIFF_WINDOW = 48;

#define MAX_PEERS (64)
#define PEER_TIMEOUT_MS (60000)

bool usb_attached = false;

// if, not #if: the arguments still compile with DEBUG 0, so that build
// cannot grow unused-variable warnings nobody sees.
#define dbg(...)                                         \
  do {                                                   \
    if (DEBUG && usb_attached) {                         \
      Serial.printf("%7lu ", (unsigned long)millis());   \
      Serial.printf(__VA_ARGS__);                        \
    }                                                    \
  } while (0)

Adafruit_NeoPixel leds(NUM_LEDS, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);
LedAnim anim;
AnimReassembler reasm;

uint16_t my_id;
bool rail_on = false;
bool active = true;
uint32_t last_activity_ms = 0; // touch, playback, or an animation received
uint32_t last_rx_ms = 0;
uint8_t cur_color = 6; // magenta, distinct from the swatch defaults

SemaphoreHandle_t wake_sem;

void touch_isr() {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(wake_sem, &woken);
  portYIELD_FROM_ISR(woken);
}

// ------------------------------------------------------------------ peers

// Holds the last animation played per peer, so repeat passes are not
// replayed.  ble_broadcast's isNewMessage() would drop the repeats instead.
struct Peer {
  uint16_t id;
  uint8_t last_anim;
  uint32_t last_ms;
  bool used;
};
Peer peers[MAX_PEERS];

bool alreadyPlayed(uint16_t id, uint8_t anim_id) {
  uint32_t now = millis();
  int free_slot = -1;
  for (int i = 0; i < MAX_PEERS; i++) {
    if (peers[i].used && (now - peers[i].last_ms) > PEER_TIMEOUT_MS) {
      peers[i].used = false; // expire, so a rebooted peer is not stuck
    }
    if (peers[i].used && peers[i].id == id) {
      if (peers[i].last_anim == anim_id) {
        peers[i].last_ms = now;
        return true;
      }
      peers[i].last_anim = anim_id;
      peers[i].last_ms = now;
      return false;
    }
    if (!peers[i].used && free_slot < 0) { free_slot = i; }
  }
  if (free_slot < 0) { return false; } // table full: play rather than drop
  peers[free_slot] = {id, anim_id, now, true};
  return false;
}

// --------------------------------------------------------------- LED rail

// Up: enable the rail and let the decoupling settle before clocking data.
// Down: data line low first, or the strip back-feeds through the SK6812
// input protection diode.
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
  dbg("rail %s\n", on ? "on" : "off");
}

void blit() {
  if (!rail_on) { ledPower(true); }
  for (int i = 0; i < NUM_LEDS; i++) { leds.setPixelColor(i, anim.pixel(i)); }
  leds.show();
}

bool anyLit() {
  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    if (anim.pixel(i)) { return true; }
  }
  return false;
}

// ------------------------------------------------------------------ radio

// Advertising runs only while sending: every advert a peer hears pauses
// its scanner until scan_callback resumes it.
//
// Every packet is the same length.  BLEAdvertising::_start() builds its
// ble_gap_adv_data_t as a *static* local, so .len is captured on the first
// call and never updated.
void advertise(const AnimMsg &m) {
  if (Bluefruit.Advertising.isRunning()) { Bluefruit.Advertising.stop(); }
  Bluefruit.Advertising.clearData();
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addData(BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA, &m,
                                ANIM_MSG_LEN);
  if (!Bluefruit.Advertising.start(0)) {
    dbg("advertise failed: anim %u chunk %u\n", m.anim_id, m.chunk_idx);
  }
}

// OFF while sending: a 100% scan takes the radio from our own advertiser.
// Not an enum: the IDE's auto-prototype for scanSet() lands above it.
const uint8_t SCAN_OFF = 0, SCAN_SNIFF = 1, SCAN_FULL = 2;
volatile uint8_t scan_mode = SCAN_OFF;

void scanSet(uint8_t mode) {
  if (mode == scan_mode) { return; }
  if (scan_mode != SCAN_OFF) { Bluefruit.Scanner.stop(); }
  scan_mode = SCAN_OFF;
  if (mode == SCAN_OFF) {
    dbg("scan off\n");
    return;
  }
  if (mode == SCAN_FULL) {
    Bluefruit.Scanner.setInterval(SCAN_FULL_INTERVAL, SCAN_FULL_WINDOW);
  } else {
    Bluefruit.Scanner.setInterval(SCAN_SNIFF_INTERVAL, SCAN_SNIFF_WINDOW);
  }
  // Left OFF on failure, so the next pass retries.
  if (!Bluefruit.Scanner.start(0)) {
    dbg("scan start failed\n");
    return;
  }
  scan_mode = mode;
  dbg("scan %s\n", mode == SCAN_FULL ? "full" : "sniff");
}

// Runs on the callback task, not in loop(): park the packet and wake loop.
// No LEDs here, show() would hold off the very reports we are collecting.
#define RX_RING 16
volatile AnimMsg rx_ring[RX_RING];
volatile uint8_t rx_head = 0, rx_tail = 0;

void scan_callback(ble_gap_evt_adv_report_t *report) {
  uint8_t buf[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
  uint8_t len = Bluefruit.Scanner.parseReportByType(
      report, BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA, buf, sizeof(buf));

  // The exact-length test doubles as the protocol version check.
  if (len == ANIM_MSG_LEN) {
    AnimMsg m;
    memcpy(&m, buf, ANIM_MSG_LEN);
    if (m.device_id != my_id && m.total_events > 0) {
      uint8_t next = (rx_head + 1) % RX_RING;
      if (next != rx_tail) { // full: drop, a repeat pass will bring it back
        memcpy((void *)&rx_ring[rx_head], &m, ANIM_MSG_LEN);
        rx_head = next;
        xSemaphoreGive(wake_sem);
      }
    }
  }

  // Required, the scanner stops on each report.  Not once scanSet() has
  // turned it off, or a report queued before that would restart it.
  if (scan_mode != SCAN_OFF) { Bluefruit.Scanner.resume(); }
}

// ----------------------------------------------------------------- sender

AnimEvent send_ev[ANIM_MAX_EVENTS];
uint8_t send_n = 0;
uint8_t send_anim_id = 0;
uint8_t send_chunk = 0;
uint8_t send_pass = 0;
uint32_t send_start_ms = 0;
uint32_t send_next_ms = 0;
bool sending = false;

void startSend(const AnimEvent *ev, uint8_t n) {
  if (n == 0) { return; }
  send_n = (n > ANIM_MAX_EVENTS) ? ANIM_MAX_EVENTS : n;
  for (uint8_t i = 0; i < send_n; i++) { send_ev[i] = ev[i]; }
  send_anim_id++;
  send_chunk = 0;
  send_pass = 0;
  send_start_ms = send_next_ms = millis();
  sending = true;
  dbg("sending anim %u: %u events, %u chunks x %u passes\n", send_anim_id,
      send_n, animChunkCount(send_n), SEND_PASSES);
}

void pumpSender(uint32_t now) {
  if (!sending || (int32_t)(now - send_next_ms) < 0) { return; }

  // Checked a deadline later, so the final chunk gets its airtime.
  if (send_pass >= SEND_PASSES) {
    sending = false;
    Bluefruit.Advertising.stop();
    dbg("send done\n");
    return;
  }

  AnimMsg m;
  animFillChunk(&m, COMPANY_ID, my_id, send_anim_id, send_chunk, send_ev,
                send_n);
  advertise(m);
  send_next_ms = now + CHUNK_AIRTIME_MS;
  dbg("send anim %u chunk %u pass %u\n", send_anim_id, send_chunk, send_pass);

  send_chunk++;
  if (send_chunk >= animChunkCount(send_n)) {
    send_chunk = 0;
    if (now - send_start_ms >= PREAMBLE_MS) { send_pass++; }
  }
}

// --------------------------------------------------------------- recorder

AnimEvent rec_ev[ANIM_MAX_EVENTS];
uint8_t rec_n = 0;
bool recording = false;
uint32_t rec_last_ms = 0;

void recLed(uint8_t led, uint32_t now) {
  if (rec_n >= ANIM_MAX_EVENTS) { return; }
  uint32_t gap = recording ? (now - rec_last_ms) / ANIM_DT_MS : 0;
  if (gap > 255) { gap = 255; }
  if (!recording) {
    recording = true;
    rec_n = 0;
    dbg("recording...\n");
  }
  rec_ev[rec_n++] = animEventMake((uint8_t)gap, led, cur_color);
  rec_last_ms = now;
}

void recFinish() {
  recording = false;
  dbg("recorded %u events\n", rec_n);
  startSend(rec_ev, rec_n);
  // We never hear our own adverts, so local playback starts here.
  anim.load(rec_ev, rec_n);
  anim.start(millis());
}

// ------------------------------------------------------------------ tiers

void setActive(bool on) {
  if (on == active) { return; }
  active = on;
  if (!on) { ledPower(false); } // blit() brings it back when needed
  dbg("-> %s\n", on ? "ACTIVE" : "IDLE");
}

void enterDeepSleep() {
  dbg("-> DEEP\n");
  if (usb_attached) { Serial.flush(); }

  scanSet(SCAN_OFF);
  Bluefruit.Advertising.stop();
  sending = false;

  // ledPower() early-returns from IDLE, so drive both pins regardless: a
  // high data pin into a dead strip is the back-feed path.
  ledPower(false);
  digitalWrite(PIN_LED_DATA, LOW);
  digitalWrite(PIN_LED_POWER, HIGH);

  // SW1 is a held level: arming SENSE_LOW while it is down wakes us at once.
  while (digitalRead(PIN_BUTTON) == LOW) { delay(10); }
  delay(50); // debounce the release

  nrf_gpio_cfg_sense_input(digitalPinToPinName(PIN_BUTTON),
                           NRF_GPIO_PIN_PULLUP, // SW1 shorts to GND
                           NRF_GPIO_PIN_SENSE_LOW);

  if (!touchArmWake() || digitalRead(PIN_BUTTON) == LOW) {
    dbg("   aborted, activity during arm\n");
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

  if (!touchBegin()) {
    if (usb_attached) { Serial.println("No touch sensor found, check wiring!"); }
    while (1) { delay(100); }
  }
  // Set once, not per tier: it soft-resets the sensor and restarts
  // calibration, which would swallow a touch in progress.
  touchSetLowPower(true);

  wake_sem = xSemaphoreCreateBinary();
  attachInterrupt(digitalPinToInterrupt(touchIntPin()), touch_isr, FALLING);

  my_id = deviceIdShort();
  memset(peers, 0, sizeof(peers));

  Bluefruit.begin(1, 1); // 1 peripheral (advertise) + 1 central (scan)
  // TX runs only during sends, so -4 dBm would save little and cost range.
  Bluefruit.setTxPower(0);
  Bluefruit.autoConnLed(false);
#if USE_DCDC
  // The return code is the confirmation: DCDCEN does not read back
  // reliably with the SoftDevice up.
  uint32_t dcdc_err = sd_power_dcdc_mode_set(NRF_POWER_DCDC_ENABLE);
  dbg("sd_power_dcdc_mode_set: %lu (0 = ok)\n", (unsigned long)dcdc_err);
#endif
  Bluefruit.Advertising.setType(
      BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED);
  Bluefruit.Advertising.setInterval(ADV_INTERVAL_SEND, ADV_INTERVAL_SEND);

  Bluefruit.Scanner.setRxCallback(scan_callback);
  Bluefruit.Scanner.filterMSD(COMPANY_ID);
  Bluefruit.Scanner.useActiveScan(false); // adv packet holds everything
  scanSet(SCAN_FULL);

  char code[DEVICE_ID_CODE_LEN + 1];
  deviceIdCode(code);
  dbg("sketchingpad_party: device %04x (%s), sensor %s\n", my_id, code,
      touchName());
  // 0 = power-on, 1 = reset pin, 4 = soft reset, 8 = lockup,
  // 0x10000 = woke from System OFF.
  dbg("reset reason %08lx\n", (unsigned long)reset_reason);
  if (DEBUG && usb_attached) { touchReport(); }

  // One fading LED at boot, so a wake from DEEP is visible.
  anim.poke(0, 6);
  blit();
  last_activity_ms = millis();

  // An MPR121 holds INT low until read, and a finger still on the pad that
  // woke us from DEEP leaves it low with no FALLING edge to come.
  xSemaphoreGive(wake_sem);
}

// ------------------------------------------------------------------- loop

uint32_t touches = 0;
uint32_t last_touches = 0;

void loop() {
  bool moving = anim.playing() || recording || sending || (rail_on && anyLit());
  bool woken = (xSemaphoreTake(wake_sem,
                               pdMS_TO_TICKS(moving ? FRAME_MS : TICK_MS)) ==
                pdTRUE);
  uint32_t now = millis();

  if (digitalRead(PIN_BUTTON) == LOW) {
    enterDeepSleep();
    last_activity_ms = millis(); // aborted: do not retry at once
    return;
  }

  // ---- touch input
  // Every INT edge needs a read; re-reading while a pad is held catches a
  // missed release without bus traffic on an idle board.
  if (woken || touches) { touches = touchRead(); }
  uint32_t newly = touches & ~last_touches;
  last_touches = touches;

  bool dirty = false;
  for (uint8_t led = 0; led < NUM_LEDS; led++) {
    if (!((newly >> led) & 1)) { continue; }
    last_activity_ms = now;
    setActive(true);
    if (led >= SWATCH_FIRST_LED && led < SWATCH_FIRST_LED + SWATCH_COUNT) {
      // Touching the selected swatch again reaches its second variant.
      uint8_t base = led - SWATCH_FIRST_LED;
      cur_color = (cur_color == base) ? (uint8_t)(base + 4) : base;
      if (recording) { rec_last_ms = now; } // a swatch keeps recording alive
      dbg("color %u\n", cur_color);
    } else {
      recLed(led, now);
    }
    anim.poke(led, cur_color);
    dirty = true;
  }
  if (dirty) { blit(); }

  if (recording && (now - rec_last_ms) > REC_SILENCE_MS) { recFinish(); }

  // ---- received chunks
  while (rx_tail != rx_head) {
    AnimMsg m;
    memcpy(&m, (const void *)&rx_ring[rx_tail], ANIM_MSG_LEN);
    rx_tail = (rx_tail + 1) % RX_RING;
    last_rx_ms = now;
    dbg("rx [%04x] anim %u chunk %u\n", m.device_id, m.anim_id, m.chunk_idx);

    if (reasm.feed(m, now) && !alreadyPlayed(reasm.from(), reasm.animId())) {
      dbg("[%04x] anim %u complete, %u events\n", reasm.from(),
          reasm.animId(), reasm.count());
      anim.load(reasm.events(), reasm.count());
      anim.start(now);
      last_activity_ms = now;
      setActive(true);
    }
  }
  reasm.expire(now);

  // ---- playback and sending
  if (anim.update(now)) {
    blit();
    last_activity_ms = now;
  }
  pumpSender(now);

  // ---- tiers
  bool rx_hold = (now - last_rx_ms) < RX_HOLD_MS;
  if (active && !moving && (now - last_activity_ms) >= ACTIVE_TIMEOUT_MS) {
    setActive(false);
  }
  if (sending) {
    scanSet(SCAN_OFF);
  } else {
    scanSet((active || rx_hold) ? SCAN_FULL : SCAN_SNIFF);
  }

  if (!active && !sending && !rx_hold &&
      (now - last_activity_ms) >= DEEP_TIMEOUT_MS) {
    enterDeepSleep();
    last_activity_ms = millis();
  }
}
