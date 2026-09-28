// SPDX-FileCopyrightText: 2026 Tod Kurt
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ble_broadcast.ino -- symmetric BLE broadcast messaging, no connections
//
// Every node advertises a small payload and simultaneously scans for
// everyone else's.  No pairing, no connections, no connection-slot limit,
// any number of participants, symmetric in both directions.
//
// Type a line into the serial monitor and it appears on every other node
// in range.  Depends only on the core's bluefruit.h -- no extra hardware,
// so it runs on a bare XIAO nRF52840.
//
// Board: Seeeduino:nrf52:xiaonRF52840
//
// See arduino/notes-ble-multiuser.md for why this rather than BLE Mesh
// (the Arduino core has no mesh stack, only GAP/GATT), and for the
// advertising-payload budget arithmetic.

#include <bluefruit.h>

#include "device_id_code.h"

// Manufacturer Specific Data company ID.  0xFFFF is the SIG's test value:
// fine for our own gear, "shall not be used in shipping end products".
#define COMPANY_ID (0xFFFF)

// Fixed user payload size.  Keep this a compile-time constant -- see the
// note in updateAdvPayload() about why the length must not vary.
#define PAYLOAD_LEN (8)

// 0 = derive a stable ID from the chip's factory device ID, so two boards
// running identical firmware still differ.  Override for a fixed address.
// See device_id_code.h; the same hash drives the color code printed at
// startup, so the hex ID and the letters always name the same board.
#define DEVICE_ID (0)

// Refuse to send faster than this.  Advertising is unacknowledged, so a
// message needs to stay on air across several advertising intervals to
// have a fair chance of being seen.
#define MIN_SEND_INTERVAL_MS (500)

// Try non-connectable advertising (nothing here accepts connections).
// Bluefruit hardcodes a peripheral conn_cfg_tag in its adv start, which
// the SoftDevice may reject for non-connectable adv -- so we check and
// fall back rather than assuming. Set to 0 to skip the attempt.
#define TRY_NONCONNECTABLE (1)

#define MAX_PEERS (16)
#define PEER_TIMEOUT_MS (60000)

// Advertising interval, units of 0.625 ms. 160 = 100 ms.
const uint16_t ADV_INTERVAL = 160;
// Scan interval / window, units of 0.625 ms. 160/80 = 100 ms every 50 ms,
// i.e. a 50% receive duty cycle. Shrink the window to save power.
const uint16_t SCAN_INTERVAL = 160;
const uint16_t SCAN_WINDOW = 80;

// on-air layout of the Manufacturer Specific Data field
struct __attribute__((packed)) Msg {
  uint16_t company_id;
  uint16_t device_id;
  uint8_t  seq;
  uint8_t  payload[PAYLOAD_LEN];
};
static const uint8_t MSG_LEN = sizeof(Msg);

uint16_t my_id;
uint8_t  my_seq = 0;
uint32_t last_send_ms = 0;
bool     adv_nonconnectable = false;

struct Peer {
  uint16_t id;
  uint8_t  seq;
  uint32_t last_ms;
  bool     used;
};
Peer peers[MAX_PEERS];

// ---------------------------------------------------------------- receive

// Returns true if this is a message we have not seen before.  Repeats of
// the same (device, seq) are how the sender compensates for advertising
// being unacknowledged, so dropping them here is the point.
bool isNewMessage(uint16_t id, uint8_t seq) {
  uint32_t now = millis();
  int free_slot = -1;

  for (int i = 0; i < MAX_PEERS; i++) {
    if (peers[i].used && (now - peers[i].last_ms) > PEER_TIMEOUT_MS) {
      peers[i].used = false; // expire, so a rebooted peer is not stuck
    }
    if (peers[i].used && peers[i].id == id) {
      if (peers[i].seq == seq) {
        peers[i].last_ms = now;
        return false; // a repeat of one we already handled
      }
      peers[i].seq = seq;
      peers[i].last_ms = now;
      return true;
    }
    if (!peers[i].used && free_slot < 0) { free_slot = i; }
  }

  if (free_slot < 0) { return true; } // table full: report rather than drop
  peers[free_slot] = { id, seq, now, true };
  return true;
}

// Called from the SoftDevice event task, not from loop().  Only this
// function touches peers[], so no locking is needed.
void scan_callback(ble_gap_evt_adv_report_t *report) {
  uint8_t buf[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
  uint8_t len = Bluefruit.Scanner.parseReportByType(
      report, BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA, buf, sizeof(buf));

  // filterMSD() already matched the company ID; this guards against a
  // truncated or foreign packet that happens to share it.
  if (len == MSG_LEN) {
    Msg msg;
    memcpy(&msg, buf, MSG_LEN);

    if (msg.device_id != my_id && isNewMessage(msg.device_id, msg.seq)) {
      Serial.printf("[%04x seq %3u rssi %4d] ", msg.device_id, msg.seq,
                    report->rssi);
      for (int i = 0; i < PAYLOAD_LEN; i++) {
        char c = (char)msg.payload[i];
        if (c == 0) { break; }
        Serial.print(isprint((unsigned char)c) ? c : '.');
      }
      Serial.println();
    }
  }

  Bluefruit.Scanner.resume(); // required, the scanner stops on each report
}

// ------------------------------------------------------------------- send

void updateAdvPayload(const uint8_t *payload, uint8_t len) {
  Msg msg;
  msg.company_id = COMPANY_ID;
  msg.device_id = my_id;
  msg.seq = my_seq;
  memset(msg.payload, 0, PAYLOAD_LEN);
  memcpy(msg.payload, payload, min(len, (uint8_t)PAYLOAD_LEN));

  // The advertising payload length must stay constant for the life of the
  // sketch.  BLEAdvertising::_start() builds its ble_gap_adv_data_t as a
  // *static* local, so .len is captured on the first call and never
  // updated; the data pointer is stable but a changed length would be
  // ignored.  Always sending a full-size Msg sidesteps that entirely.
  if (Bluefruit.Advertising.isRunning()) { Bluefruit.Advertising.stop(); }
  Bluefruit.Advertising.clearData();
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addData(BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA,
                                &msg, MSG_LEN);
  Bluefruit.Advertising.start(0); // 0 = advertise forever
}

// Returns false if called before MIN_SEND_INTERVAL_MS has elapsed, so a
// message always gets several advertising intervals on air.
bool send(const uint8_t *payload, uint8_t len) {
  uint32_t now = millis();
  if (last_send_ms != 0 && (now - last_send_ms) < MIN_SEND_INTERVAL_MS) {
    return false;
  }
  my_seq++;
  updateAdvPayload(payload, len);
  last_send_ms = now;
  return true;
}

// ------------------------------------------------------------------ setup

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) { delay(10); }

  my_id = DEVICE_ID ? DEVICE_ID : deviceIdShort();
  memset(peers, 0, sizeof(peers));

  Bluefruit.begin(1, 1); // 1 peripheral (advertise) + 1 central (scan)
  Bluefruit.setTxPower(0);
  Bluefruit.autoConnLed(false);

  Bluefruit.Advertising.setInterval(ADV_INTERVAL, ADV_INTERVAL);
  Bluefruit.Advertising.restartOnDisconnect(true);

#if TRY_NONCONNECTABLE
  Bluefruit.Advertising.setType(
      BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED);
#endif

  uint8_t empty[PAYLOAD_LEN] = {0};
  updateAdvPayload(empty, PAYLOAD_LEN);

  adv_nonconnectable = Bluefruit.Advertising.isRunning();
#if TRY_NONCONNECTABLE
  if (!adv_nonconnectable) {
    // The SoftDevice refused non-connectable adv, so use the connectable
    // default.  Harmless here -- nothing advertises a service to connect to.
    // (Advertising.start() swallows the SVC error code, so all we can
    // report is that it was refused, not which error it returned.)
    Serial.println("non-connectable adv refused, falling back to connectable");
    Bluefruit.Advertising.setType(
        BLE_GAP_ADV_TYPE_CONNECTABLE_SCANNABLE_UNDIRECTED);
    // Safe to retry only because the payload length never changes: _start()
    // already committed its static ble_gap_adv_data_t on the failed attempt.
    updateAdvPayload(empty, PAYLOAD_LEN);
  }
#endif

  Bluefruit.Scanner.setRxCallback(scan_callback);
  Bluefruit.Scanner.filterMSD(COMPANY_ID);
  Bluefruit.Scanner.useActiveScan(false); // adv packet holds everything
  Bluefruit.Scanner.setInterval(SCAN_INTERVAL, SCAN_WINDOW);
  Bluefruit.Scanner.start(0); // 0 = scan forever

  // The letter code is what identifies this board by eye -- it is the same
  // code ble_keyboard_simple_ts20 puts in its BLE model name and lights on the
  // LEDs, so a peer seen on air can be matched to a board on the bench.
  char code[DEVICE_ID_CODE_LEN + 1];
  deviceIdCode(code);
  Serial.printf("ble_broadcast: device %04x (%s), %u byte payload, adv %s\n",
                my_id, code, PAYLOAD_LEN,
                adv_nonconnectable ? "non-connectable" : "connectable");
  Serial.println("type a line to broadcast it");
}

// ------------------------------------------------------------------- loop

char line[PAYLOAD_LEN + 1];
uint8_t line_len = 0;
uint32_t last_millis = 0;
uint16_t myi = 1234;

void loop() {
  // auto mode: every N seconds send a message autoincremented "myi" as str
  if( millis() - last_millis > 3000 ) {  // every 3 seconds send a msg
    last_millis = millis();
    Serial.println("sending");
    line_len = 5;
    snprintf(line, line_len, "%d", myi % 9999 );
    if (send((uint8_t *)line, line_len)) {
      Serial.printf("sent seq %u: %.*s\n", my_seq, line_len, line);
    }
    myi++;
    line_len = 0;
  }

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (line_len > 0) {
        if (send((uint8_t *)line, line_len)) {
          Serial.printf("sent seq %u: %.*s\n", my_seq, line_len, line);
        } else {
          Serial.printf("too soon, wait %u ms between sends\n",
                        MIN_SEND_INTERVAL_MS);
        }
        line_len = 0;
      }
    } else if (line_len < PAYLOAD_LEN) {
      line[line_len++] = c;
    }
  }

  delay(10); // vTaskDelay: yields to the RTOS idle task
}
