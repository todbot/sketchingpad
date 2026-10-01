// SPDX-FileCopyrightText: 2026 Tod Kurt
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ble_broadcast_anim.ino -- record an LED animation by touch, broadcast it
//
// Touch pads to record a short animation; after a moment of silence it
// plays back locally and goes out over BLE advertising, and every other
// board in range plays the same thing.  No pairing, no connections, any
// number of participants, all running this same firmware.
//
// The top row of pads (TP17-20) are color swatches rather than animation
// events: touching one picks the color for everything recorded after it.
// Touching the swatch that is already selected switches to its second
// variant, so four pads reach all eight palette entries.
//
// Runs on either board: touch.h probes I2C at startup, so the TS20 and
// the 2x MPR121 respin are both ordinary cases and the banner says which
// it found.  Everything below is in LED-index space, which is what makes
// the swatch range a plain 16-19 rather than a slice of a channel order.
//
// The transport is arduino/ble_broadcast/; the animation model and the
// wire format live in led_anim.h and anim_wire.h, which depend on nothing
// and can be tested on a host.
//
// Board: Seeeduino:nrf52:xiaonRF52840

// 1 = narrate everything to Serial.  0 = silent and no 3 s wait for USB at
// boot, which is what a battery-powered board wants.
#define DEBUG (1)

#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <bluefruit.h>

#include "anim_wire.h"
#include "device_id_code.h"
#include "led_anim.h"
#include "touch.h"

// sketchingpad -> XIAO nRF52840:
//   SDA = D4, SCL = D5 (Wire defaults)
#define PIN_LED_DATA  (6)
#define PIN_LED_POWER (7) // P-FET gate: LOW = rail on

// Manufacturer Specific Data company ID.  0xFFFF is the SIG's test value:
// fine for our own gear, "shall not be used in shipping end products".
#define COMPANY_ID (0xFFFF)

const int NUM_LEDS = ANIM_NUM_LEDS;
const int LED_BRIGHTNESS = 40;

// The top row, TP17-20, is LEDs 16-19.
const uint8_t SWATCH_FIRST_LED = 16;
const uint8_t SWATCH_COUNT = 4;

const uint32_t REC_SILENCE_MS = 1500;  // quiet this long ends a recording
const uint32_t LED_IDLE_MS = 30000;    // blank the rail after this
const uint32_t CHUNK_AIRTIME_MS = 200; // ~10 advertising events per chunk
const uint8_t SEND_PASSES = 3;         // whole cycle repeated this often

// Advertising intervals, units of 0.625 ms.  Idle is 100 ms.  Chunks go
// out at 20 ms, the minimum: a receiver measured hearing only ~40% of
// adverts, so a chunk needs many events in its airtime to land.
const uint16_t ADV_INTERVAL = 160;
const uint16_t ADV_INTERVAL_SEND = 32;
// Scan interval / window, units of 0.625 ms.  Unlike ble_broadcast this
// runs a 100% receive duty cycle: a missed chunk costs a whole animation,
// and reliability matters more than power here.  Halve the window to get
// ble_broadcast's power behaviour back.
const uint16_t SCAN_INTERVAL = 160;
const uint16_t SCAN_WINDOW = 160;

// if, not #if: the arguments still compile with DEBUG 0, so that build
// cannot grow unused-variable warnings nobody sees.
#define dbg(...)                                       \
  do {                                                 \
    if (DEBUG) {                                       \
      Serial.printf("%7lu ", (unsigned long)millis()); \
      Serial.printf(__VA_ARGS__);                      \
    }                                                  \
  } while (0)

#define MAX_PEERS (16)
#define PEER_TIMEOUT_MS (60000)

Adafruit_NeoPixel leds(NUM_LEDS, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);
LedAnim anim;
AnimReassembler reasm;

uint16_t my_id;
bool rail_on = false;
uint32_t last_activity_ms = 0;
uint8_t cur_color = 6; // magenta, distinct from the swatch defaults

// ------------------------------------------------------------------ peers

// One entry per device we have heard from, holding the last animation we
// actually played.  This is what stops a repeat pass replaying an
// animation we already ran -- ble_broadcast's isNewMessage() cannot do the
// job, because every chunk of one animation shares an anim_id and the
// repeats it would suppress are exactly the ones reliability depends on.
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
  }
  dbg("rail %s\n", on ? "on" : "off");
}

void blit() {
  if (!rail_on) { ledPower(true); }
  for (int i = 0; i < NUM_LEDS; i++) { leds.setPixelColor(i, anim.pixel(i)); }
  leds.show();
}

// ------------------------------------------------------------------ radio

// Every packet is the same length.  BLEAdvertising::_start() builds its
// ble_gap_adv_data_t as a *static* local, so .len is captured on the first
// call and never updated; the data pointer is stable but a changed length
// would be ignored.
void advertise(const AnimMsg &m, uint16_t interval) {
  if (Bluefruit.Advertising.isRunning()) { Bluefruit.Advertising.stop(); }
  Bluefruit.Advertising.setInterval(interval, interval);
  Bluefruit.Advertising.clearData();
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addData(BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA, &m,
                                ANIM_MSG_LEN);
  if (!Bluefruit.Advertising.start(0)) { // 0 = advertise forever
    dbg("advertise failed: anim %u chunk %u\n", m.anim_id, m.chunk_idx);
  }
}

void advertiseIdle() {
  AnimMsg m;
  animFillChunk(&m, COMPANY_ID, my_id, 0, 0, NULL, 0); // total_events = 0
  advertise(m, ADV_INTERVAL);
}

// scan_callback runs on the SoftDevice event task, not in loop().  It must
// not touch the LEDs: NeoPixel show() blocks with interrupts disabled for
// over a millisecond across 20 pixels, which would make us miss the very
// advertisements we are trying to collect.  So it only parks the packet.
#define RX_RING 4
volatile AnimMsg rx_ring[RX_RING];
volatile uint8_t rx_head = 0, rx_tail = 0;

// The scanner is stopped while we send: at a 100% scan duty cycle it
// takes the radio from our own advertiser, and two boards measured
// getting each chunk out for only ~70 ms of its 200 ms.
volatile bool sending = false;

// Debug link stats: every packet heard from another board, idle adverts
// included, so link quality shows up without anyone touching a pad.
volatile uint16_t heard_n = 0;
volatile int32_t heard_rssi_sum = 0;
volatile int8_t heard_rssi_min = 0, heard_rssi_max = -128;

void scan_callback(ble_gap_evt_adv_report_t *report) {
  uint8_t buf[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
  uint8_t len = Bluefruit.Scanner.parseReportByType(
      report, BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA, buf, sizeof(buf));

  // The exact-length test doubles as the protocol version check: any
  // format change changes ANIM_MSG_LEN, so old packets fall out here.
  if (len == ANIM_MSG_LEN) {
    AnimMsg m;
    memcpy(&m, buf, ANIM_MSG_LEN);
    if (DEBUG && m.device_id != my_id) {
      heard_n++;
      heard_rssi_sum += report->rssi;
      if (report->rssi < heard_rssi_min) { heard_rssi_min = report->rssi; }
      if (report->rssi > heard_rssi_max) { heard_rssi_max = report->rssi; }
    }
    if (m.device_id != my_id && m.total_events > 0) {
      uint8_t next = (rx_head + 1) % RX_RING;
      if (next != rx_tail) { // full: drop, a repeat pass will bring it back
        memcpy((void *)&rx_ring[rx_head], &m, ANIM_MSG_LEN);
        rx_head = next;
      }
    }
  }

  // Required, the scanner stops on each report.  Not while sending, or a
  // report already queued when the send began would restart it.
  if (!sending) { Bluefruit.Scanner.resume(); }
}

// ----------------------------------------------------------------- sender

AnimEvent send_ev[ANIM_MAX_EVENTS];
uint8_t send_n = 0;
uint8_t send_anim_id = 0;
uint8_t send_chunk = 0;
uint8_t send_pass = 0;
uint32_t send_next_ms = 0;

void startSend(const AnimEvent *ev, uint8_t n) {
  if (n == 0) { return; }
  send_n = (n > ANIM_MAX_EVENTS) ? ANIM_MAX_EVENTS : n;
  for (uint8_t i = 0; i < send_n; i++) { send_ev[i] = ev[i]; }
  send_anim_id++;
  send_chunk = 0;
  send_pass = 0;
  send_next_ms = millis();
  sending = true;
  Bluefruit.Scanner.stop();
  dbg("sending anim %u: %u events, %u chunks x %u passes\n", send_anim_id,
      send_n, animChunkCount(send_n), SEND_PASSES);
}

void pumpSender(uint32_t now) {
  if (!sending || (int32_t)(now - send_next_ms) < 0) { return; }

  // Checked here rather than after the last advertise(), so the final
  // chunk gets its airtime before the idle advert replaces it.
  if (send_pass >= SEND_PASSES) {
    sending = false;
    advertiseIdle();
    if (!Bluefruit.Scanner.start(0)) { dbg("scanner restart failed\n"); }
    dbg("send done\n");
    return;
  }

  AnimMsg m;
  animFillChunk(&m, COMPANY_ID, my_id, send_anim_id, send_chunk, send_ev,
                send_n);
  advertise(m, ADV_INTERVAL_SEND);
  dbg("send anim %u chunk %u pass %u\n", send_anim_id, send_chunk, send_pass);
  send_next_ms = now + CHUNK_AIRTIME_MS;

  send_chunk++;
  if (send_chunk >= animChunkCount(send_n)) {
    send_chunk = 0;
    send_pass++;
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
  dbg("rec #%u: dt %u led %u color %u\n", rec_n - 1, (unsigned)gap, led,
      cur_color);
  rec_last_ms = now;
}

void recFinish() {
  recording = false;
  uint32_t span = 0;
  for (uint8_t i = 0; i < rec_n; i++) { span += rec_ev[i].dt * ANIM_DT_MS; }
  dbg("recorded %u events, %lu ms\n", rec_n, (unsigned long)span);
  for (uint8_t i = 0; i < rec_n; i++) {
    dbg("  #%u: dt %u led %u color %u\n", i, rec_ev[i].dt,
        animEventLed(rec_ev[i]), animEventColor(rec_ev[i]));
  }
  startSend(rec_ev, rec_n);
  // We never see our own advertisement -- scan_callback filters on
  // device_id -- so local playback has to be kicked off from here.
  anim.load(rec_ev, rec_n);
  anim.start(millis());
  dbg("local play: playing %d rail %d LED_POWER pin %d sending %d\n",
      anim.playing(), rail_on, digitalRead(PIN_LED_POWER), sending);
}

// ------------------------------------------------------------------ setup

void setup() {
  Serial.begin(115200);
  if (DEBUG) {
    while (!Serial && millis() < 3000) { delay(10); }
  }
  uint32_t reset_reason = NRF_POWER->RESETREAS;
  NRF_POWER->RESETREAS = reset_reason; // write-1-to-clear, else it accumulates

  pinMode(PIN_LED_POWER, OUTPUT);
  digitalWrite(PIN_LED_POWER, HIGH); // rail off until ledPower() says so
  leds.begin();
  leds.setBrightness(LED_BRIGHTNESS);
  ledPower(true);

  if (!touchBegin()) {
    Serial.println("No touch sensor found, check wiring!");
    while (1) { delay(100); }
  }

  my_id = deviceIdShort();
  memset(peers, 0, sizeof(peers));

  Bluefruit.begin(1, 1); // 1 peripheral (advertise) + 1 central (scan)
  Bluefruit.setTxPower(0);
  Bluefruit.autoConnLed(false);
  Bluefruit.Advertising.restartOnDisconnect(true);
  advertiseIdle();

  Bluefruit.Scanner.setRxCallback(scan_callback);
  Bluefruit.Scanner.filterMSD(COMPANY_ID);
  Bluefruit.Scanner.useActiveScan(false); // adv packet holds everything
  Bluefruit.Scanner.setInterval(SCAN_INTERVAL, SCAN_WINDOW);
  Bluefruit.Scanner.start(0); // 0 = scan forever

  char code[DEVICE_ID_CODE_LEN + 1];
  deviceIdCode(code);
  dbg("ble_broadcast_anim: device %04x (%s), sensor %s, %u byte packets\n",
      my_id, code, touchName(), ANIM_MSG_LEN);
  // 0 = power-on, 1 = reset pin, 2 = watchdog, 4 = soft reset (a crash
  // handler or upload), 8 = CPU lockup, 0x10000 = woke from System OFF.
  dbg("reset reason %08lx\n", (unsigned long)reset_reason);
  if (DEBUG) { touchReport(); }
  dbg("touch pads to record; the top row picks a color\n");

  last_activity_ms = millis();
}

// ------------------------------------------------------------------- loop

uint32_t last_touches = 0;
bool last_fault = false;

// Debug-only playback stats, so a dark board says whether frames were
// computed at all or computed and lost on the way to the strip.
bool was_playing = false;
uint32_t play_start_ms = 0;
uint32_t play_frames = 0;
uint8_t play_max_lit = 0;
uint32_t heard_report_ms = 0;

void loop() {
  uint32_t now = millis();

  // ---- touch input
  // Polled at the sender's rate, not the sensor's: both backends update
  // slower than this loop runs (MPR121 ESI x SFI is 4 ms at CONFIG2 0x20),
  // so most passes re-read an unchanged word.  Edge detection below is
  // what makes that harmless.
  uint32_t touches = touchRead();
  uint32_t newly = touches & ~last_touches;
  last_touches = touches;
  if (touchFault() != last_fault) {
    last_fault = touchFault();
    dbg("touch fault %d\n", last_fault);
  }

  bool dirty = false;
  for (uint8_t led = 0; led < NUM_LEDS; led++) {
    if (!((newly >> led) & 1)) { continue; }
    last_activity_ms = now;
    dbg("touch led %u (%s)\n", led, touchSource(led));
    if (led >= SWATCH_FIRST_LED && led < SWATCH_FIRST_LED + SWATCH_COUNT) {
      // Touching the selected swatch again reaches its second variant, so
      // four pads cover all eight palette entries.
      uint8_t base = led - SWATCH_FIRST_LED;
      cur_color = (cur_color == base) ? (uint8_t)(base + 4) : base;
      // Swatches record nothing, but they do keep a recording alive, so
      // changing color mid-animation cannot end it.
      if (recording) { rec_last_ms = now; }
      dbg("color %u\n", cur_color);
    } else {
      recLed(led, now);
    }
    anim.poke(led, cur_color);
    dirty = true;
  }
  if (dirty) { blit(); } // once per pass: show() is ~600 us for 20 pixels

  if (recording && (now - rec_last_ms) > REC_SILENCE_MS) { recFinish(); }

  // Another board's idle adverts arrive ~10/s, the advertising interval.
  if (DEBUG && (now - heard_report_ms) >= 5000) {
    uint16_t n = heard_n;
    if (n) {
      dbg("heard %u adv in %lu ms, rssi %d/%ld/%d min/avg/max\n", n,
          (unsigned long)(now - heard_report_ms), heard_rssi_min,
          (long)(heard_rssi_sum / n), heard_rssi_max);
    } else {
      dbg("heard 0 adv in %lu ms\n", (unsigned long)(now - heard_report_ms));
    }
    heard_n = 0;
    heard_rssi_sum = 0;
    heard_rssi_min = 0;
    heard_rssi_max = -128;
    heard_report_ms = now;
  }

  // ---- received chunks
  while (rx_tail != rx_head) {
    AnimMsg m;
    memcpy(&m, (const void *)&rx_ring[rx_tail], ANIM_MSG_LEN);
    rx_tail = (rx_tail + 1) % RX_RING;
    dbg("rx [%04x] anim %u chunk %u total %u\n", m.device_id, m.anim_id,
        m.chunk_idx, m.total_events);

    if (reasm.feed(m, now)) {
      if (!alreadyPlayed(reasm.from(), reasm.animId())) {
        dbg("[%04x] anim %u complete, %u events\n", reasm.from(),
            reasm.animId(), reasm.count());
        anim.load(reasm.events(), reasm.count());
        anim.start(now);
        last_activity_ms = now;
      } else {
        dbg("[%04x] anim %u already played\n", reasm.from(), reasm.animId());
      }
    }
  }
  reasm.expire(now);

  // ---- playback and housekeeping
  bool frame = anim.update(now);
  if (frame) {
    blit();
    last_activity_ms = now;
  }
  if (DEBUG) {
    if (anim.playing() && !was_playing) {
      play_start_ms = now;
      play_frames = 0;
      play_max_lit = 0;
      dbg("play start\n");
    }
    if (anim.playing() && frame) {
      play_frames++;
      uint8_t lit = 0;
      for (uint8_t i = 0; i < NUM_LEDS; i++) { lit += (anim.pixel(i) != 0); }
      if (lit > play_max_lit) { play_max_lit = lit; }
    }
    if (!anim.playing() && was_playing) {
      dbg("play end: %lu ms, %lu frames, max %u lit\n",
          (unsigned long)(now - play_start_ms), (unsigned long)play_frames,
          play_max_lit);
    }
    was_playing = anim.playing();
  }
  pumpSender(now);

  if (rail_on && !anim.playing() && !recording &&
      (now - last_activity_ms) > LED_IDLE_MS) {
    ledPower(false);
  }

  delay(5); // vTaskDelay: yields to the RTOS idle task
}
