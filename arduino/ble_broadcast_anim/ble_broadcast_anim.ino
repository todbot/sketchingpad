// ble_broadcast_anim.ino -- record an LED animation by touch, broadcast it
//
// Touch pads to record a short animation; after a moment of silence it
// plays back locally and goes out over BLE advertising, and every other
// board in range plays the same thing.  No pairing, no connections, any
// number of participants, all running this same firmware.
//
// Pads 17-20 (the top row) are color swatches rather than animation
// events: touching one picks the color for everything recorded after it.
// Touching the swatch that is already selected switches to its second
// variant, so four pads reach all eight palette entries.
//
// The transport is arduino/ble_broadcast/; the animation model and the
// wire format live in led_anim.h and anim_wire.h, which depend on nothing
// and can be tested on a host.
//
// Board: Seeeduino:nrf52:xiaonRF52840

#include <Adafruit_NeoPixel.h>
#include <TS20.h>
#include <Wire.h>
#include <bluefruit.h>

#include "anim_wire.h"
#include "device_id_code.h"
#include "led_anim.h"

// sketchingpad -> XIAO nRF52840:
//   SDA = D4, SCL = D5 (Wire defaults)
#define PIN_LED_DATA  (6)
#define PIN_LED_POWER (7) // P-FET gate: LOW = rail on

// Manufacturer Specific Data company ID.  0xFFFF is the SIG's test value:
// fine for our own gear, "shall not be used in shipping end products".
#define COMPANY_ID (0xFFFF)

const int NUM_LEDS = ANIM_NUM_LEDS;
const int LED_BRIGHTNESS = 40;
const int TS20_SENSITIVITY = 15; // 0 = most sensitive, 15 = least

// Pads 17-20, zero-based.  These pick a color instead of recording.
const uint8_t SWATCH_FIRST_PAD = 16;

const uint32_t REC_SILENCE_MS = 1500;  // quiet this long ends a recording
const uint32_t LED_IDLE_MS = 30000;    // blank the rail after this
const uint32_t CHUNK_AIRTIME_MS = 200; // ~2 advertising events per chunk
const uint8_t SEND_PASSES = 3;         // whole cycle repeated this often

// Advertising interval, units of 0.625 ms. 160 = 100 ms.
const uint16_t ADV_INTERVAL = 160;
// Scan interval / window, units of 0.625 ms.  Unlike ble_broadcast this
// runs a 100% receive duty cycle: a missed chunk costs a whole animation,
// and reliability matters more than power here.  Halve the window to get
// ble_broadcast's power behaviour back.
const uint16_t SCAN_INTERVAL = 160;
const uint16_t SCAN_WINDOW = 160;

#define MAX_PEERS (16)
#define PEER_TIMEOUT_MS (60000)

Adafruit_NeoPixel leds(NUM_LEDS, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);
TS20 ts20;
LedAnim anim;
AnimReassembler reasm;

// Index is LED number, value is the pad wired to it.
const uint8_t led_to_pad[NUM_LEDS] = {
    18, 1, 3, 4, 6, 9, 19, 2, 5, 8,
    17, 0, 10, 11, 16, 15, 14, 13, 12, 7,
};
uint8_t pad_to_led[TS20_NUM_PADS]; // inverted from led_to_pad at startup

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
void advertise(const AnimMsg &m) {
  if (Bluefruit.Advertising.isRunning()) { Bluefruit.Advertising.stop(); }
  Bluefruit.Advertising.clearData();
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addData(BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA, &m,
                                ANIM_MSG_LEN);
  Bluefruit.Advertising.start(0); // 0 = advertise forever
}

void advertiseIdle() {
  AnimMsg m;
  animFillChunk(&m, COMPANY_ID, my_id, 0, 0, NULL, 0); // total_events = 0
  advertise(m);
}

// scan_callback runs on the SoftDevice event task, not in loop().  It must
// not touch the LEDs: NeoPixel show() blocks with interrupts disabled for
// over a millisecond across 20 pixels, which would make us miss the very
// advertisements we are trying to collect.  So it only parks the packet.
#define RX_RING 4
volatile AnimMsg rx_ring[RX_RING];
volatile uint8_t rx_head = 0, rx_tail = 0;

void scan_callback(ble_gap_evt_adv_report_t *report) {
  uint8_t buf[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
  uint8_t len = Bluefruit.Scanner.parseReportByType(
      report, BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA, buf, sizeof(buf));

  // The exact-length test doubles as the protocol version check: any
  // format change changes ANIM_MSG_LEN, so old packets fall out here.
  if (len == ANIM_MSG_LEN) {
    AnimMsg m;
    memcpy(&m, buf, ANIM_MSG_LEN);
    if (m.device_id != my_id && m.total_events > 0) {
      uint8_t next = (rx_head + 1) % RX_RING;
      if (next != rx_tail) { // full: drop, a repeat pass will bring it back
        memcpy((void *)&rx_ring[rx_head], &m, ANIM_MSG_LEN);
        rx_head = next;
      }
    }
  }

  Bluefruit.Scanner.resume(); // required, the scanner stops on each report
}

// ----------------------------------------------------------------- sender

AnimEvent send_ev[ANIM_MAX_EVENTS];
uint8_t send_n = 0;
uint8_t send_anim_id = 0;
uint8_t send_chunk = 0;
uint8_t send_pass = 0;
uint32_t send_next_ms = 0;
bool sending = false;

void startSend(const AnimEvent *ev, uint8_t n) {
  if (n == 0) { return; }
  send_n = (n > ANIM_MAX_EVENTS) ? ANIM_MAX_EVENTS : n;
  for (uint8_t i = 0; i < send_n; i++) { send_ev[i] = ev[i]; }
  send_anim_id++;
  send_chunk = 0;
  send_pass = 0;
  send_next_ms = millis();
  sending = true;
  Serial.printf("sending anim %u: %u events, %u chunks x %u passes\n",
                send_anim_id, send_n, animChunkCount(send_n), SEND_PASSES);
}

void pumpSender(uint32_t now) {
  if (!sending || (int32_t)(now - send_next_ms) < 0) { return; }

  AnimMsg m;
  animFillChunk(&m, COMPANY_ID, my_id, send_anim_id, send_chunk, send_ev,
                send_n);
  advertise(m);
  send_next_ms = now + CHUNK_AIRTIME_MS;

  send_chunk++;
  if (send_chunk >= animChunkCount(send_n)) {
    send_chunk = 0;
    send_pass++;
    if (send_pass >= SEND_PASSES) {
      sending = false;
      advertiseIdle();
      Serial.println("send done");
    }
  }
}

// --------------------------------------------------------------- recorder

AnimEvent rec_ev[ANIM_MAX_EVENTS];
uint8_t rec_n = 0;
bool recording = false;
uint32_t rec_last_ms = 0;

void recPad(uint8_t pad, uint32_t now) {
  if (rec_n >= ANIM_MAX_EVENTS) { return; }
  uint32_t gap = recording ? (now - rec_last_ms) / ANIM_DT_MS : 0;
  if (gap > 255) { gap = 255; }
  if (!recording) {
    recording = true;
    rec_n = 0;
    Serial.println("recording...");
  }
  rec_ev[rec_n++] = animEventMake((uint8_t)gap, pad_to_led[pad], cur_color);
  rec_last_ms = now;
}

void recFinish() {
  recording = false;
  Serial.printf("recorded %u events\n", rec_n);
  startSend(rec_ev, rec_n);
  // We never see our own advertisement -- scan_callback filters on
  // device_id -- so local playback has to be kicked off from here.
  anim.load(rec_ev, rec_n);
  anim.start(millis());
}

// ------------------------------------------------------------------ setup

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) { delay(10); }

  for (uint8_t i = 0; i < NUM_LEDS; i++) { pad_to_led[led_to_pad[i]] = i; }

  pinMode(PIN_LED_POWER, OUTPUT);
  digitalWrite(PIN_LED_POWER, HIGH); // rail off until ledPower() says so
  leds.begin();
  leds.setBrightness(LED_BRIGHTNESS);
  ledPower(true);

  delay(100); // wait 100 msec after power up, as per datasheet
  if (!ts20.begin()) {
    Serial.println("No TS20 found, check wiring!");
    while (1) { delay(100); }
  }
  // after begin(), which does Wire.begin() and forces the bus back to 100 kHz
  Wire.setClock(400000);
  ts20.setSensitivity(TS20_SENSITIVITY);

  my_id = deviceIdShort();
  memset(peers, 0, sizeof(peers));

  Bluefruit.begin(1, 1); // 1 peripheral (advertise) + 1 central (scan)
  Bluefruit.setTxPower(0);
  Bluefruit.autoConnLed(false);
  Bluefruit.Advertising.setInterval(ADV_INTERVAL, ADV_INTERVAL);
  Bluefruit.Advertising.restartOnDisconnect(true);
  advertiseIdle();

  Bluefruit.Scanner.setRxCallback(scan_callback);
  Bluefruit.Scanner.filterMSD(COMPANY_ID);
  Bluefruit.Scanner.useActiveScan(false); // adv packet holds everything
  Bluefruit.Scanner.setInterval(SCAN_INTERVAL, SCAN_WINDOW);
  Bluefruit.Scanner.start(0); // 0 = scan forever

  char code[DEVICE_ID_CODE_LEN + 1];
  deviceIdCode(code);
  Serial.printf("ble_broadcast_anim: device %04x (%s), %u byte packets\n",
                my_id, code, ANIM_MSG_LEN);
  Serial.println("touch pads 1-16 to record, 17-20 pick a color");

  last_activity_ms = millis();
}

// ------------------------------------------------------------------- loop

uint32_t last_touches = 0;

void loop() {
  uint32_t now = millis();

  // ---- touch input
  uint32_t touches = ts20.touched();
  uint32_t newly = touches & ~last_touches;
  last_touches = touches;

  bool dirty = false;
  for (uint8_t pad = 0; pad < TS20_NUM_PADS; pad++) {
    if (!((newly >> pad) & 1)) { continue; }
    last_activity_ms = now;
    if (pad >= SWATCH_FIRST_PAD) {
      // Touching the selected swatch again reaches its second variant, so
      // four pads cover all eight palette entries.
      uint8_t base = pad - SWATCH_FIRST_PAD;
      cur_color = (cur_color == base) ? (uint8_t)(base + 4) : base;
      // Swatches record nothing, but they do keep a recording alive, so
      // changing color mid-animation cannot end it.
      if (recording) { rec_last_ms = now; }
      Serial.printf("color %u\n", cur_color);
    } else {
      recPad(pad, now);
    }
    anim.poke(pad_to_led[pad], cur_color);
    dirty = true;
  }
  if (dirty) { blit(); } // once per pass: show() is ~600 us for 20 pixels

  if (recording && (now - rec_last_ms) > REC_SILENCE_MS) { recFinish(); }

  // ---- received chunks
  while (rx_tail != rx_head) {
    AnimMsg m;
    memcpy(&m, (const void *)&rx_ring[rx_tail], ANIM_MSG_LEN);
    rx_tail = (rx_tail + 1) % RX_RING;

    if (reasm.feed(m, now)) {
      if (!alreadyPlayed(reasm.from(), reasm.animId())) {
        Serial.printf("[%04x] anim %u complete, %u events\n", reasm.from(),
                      reasm.animId(), reasm.count());
        anim.load(reasm.events(), reasm.count());
        anim.start(now);
        last_activity_ms = now;
      }
    }
  }
  reasm.expire(now);

  // ---- playback and housekeeping
  if (anim.update(now)) {
    blit();
    last_activity_ms = now;
  }
  pumpSender(now);

  if (rail_on && !anim.playing() && !recording &&
      (now - last_activity_ms) > LED_IDLE_MS) {
    ledPower(false);
  }

  delay(5); // vTaskDelay: yields to the RTOS idle task
}
