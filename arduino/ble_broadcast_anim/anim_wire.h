// SPDX-FileCopyrightText: 2026 Tod Kurt
// SPDX-License-Identifier: GPL-3.0-or-later
//
// anim_wire.h -- packing an animation into BLE advertising packets
//
// An animation is chunked across several advertising packets and
// reassembled by every receiver.  Advertising is unacknowledged, so the
// sender repeats the whole cycle and the receiver dedupes by chunk index.
//
// Depends on nothing but <stdint.h> and led_anim.h, so the protocol can be
// round-tripped on a host without a radio.
//
// Budget: BLEAdvertisingData::addData() guards _count + len + 2 <= 31, and
// the flags element has already spent 3, so an MSD element can carry at
// most 26 bytes.  AnimMsg is 25.
//
// No version field: scan_callback tests len == ANIM_MSG_LEN exactly, so a
// format change changes the length and old packets are ignored by
// construction.

#pragma once

#include <stdint.h>

#include "led_anim.h"

#define ANIM_EVENTS_PER_CHUNK 9
#define ANIM_MAX_CHUNKS 8 // ANIM_MAX_EVENTS / ANIM_EVENTS_PER_CHUNK
#define ANIM_RX_SLOTS 2
#define ANIM_RX_TIMEOUT_MS 6000

struct __attribute__((packed)) AnimMsg {
  uint16_t company_id;
  uint16_t device_id;
  uint8_t anim_id;
  uint8_t chunk_idx;
  // Carried in every packet, not just the first: a receiver that misses
  // chunk 0 still learns how many chunks to expect.
  uint8_t total_events;
  AnimEvent events[ANIM_EVENTS_PER_CHUNK];
};

static const uint8_t ANIM_MSG_LEN = sizeof(AnimMsg);

static inline uint8_t animChunkCount(uint8_t total_events) {
  return (uint8_t)((total_events + ANIM_EVENTS_PER_CHUNK - 1) /
                   ANIM_EVENTS_PER_CHUNK);
}

static inline void animFillChunk(AnimMsg *m, uint16_t company_id,
                                 uint16_t device_id, uint8_t anim_id,
                                 uint8_t chunk, const AnimEvent *ev,
                                 uint8_t total_events) {
  m->company_id = company_id;
  m->device_id = device_id;
  m->anim_id = anim_id;
  m->chunk_idx = chunk;
  m->total_events = total_events;
  for (uint8_t i = 0; i < ANIM_EVENTS_PER_CHUNK; i++) {
    uint16_t src = (uint16_t)chunk * ANIM_EVENTS_PER_CHUNK + i;
    // The last chunk is zero-padded: every packet must be the same length,
    // because BLEAdvertising::_start() freezes .len on its first call.
    m->events[i] = (src < total_events) ? ev[src] : animEventMake(0, 0, 0);
  }
}

class AnimReassembler {
 public:
  // True when this packet completed an animation, which is then available
  // from events()/count()/from()/animId() until the next completion.
  bool feed(const AnimMsg &m, uint32_t now_ms) {
    uint8_t total = m.total_events;
    if (total == 0 || total > ANIM_MAX_EVENTS) { return false; }
    uint8_t chunks = animChunkCount(total);
    if (m.chunk_idx >= chunks) { return false; }

    Slot *s = find(m.device_id, m.anim_id);
    if (s == nullptr) { s = claim(m.device_id, m.anim_id, total, now_ms); }
    if (s == nullptr) { return false; }
    if (s->total != total) { return false; } // stale id reused, ignore

    uint16_t bit = (uint16_t)1 << m.chunk_idx;
    if (s->got & bit) { return false; } // a repeat, which is expected

    for (uint8_t i = 0; i < ANIM_EVENTS_PER_CHUNK; i++) {
      uint16_t dst = (uint16_t)m.chunk_idx * ANIM_EVENTS_PER_CHUNK + i;
      if (dst < total) { s->ev[dst] = m.events[i]; }
    }
    s->got |= bit;

    uint16_t want = (uint16_t)((1u << chunks) - 1);
    if (s->got != want) { return false; }

    for (uint8_t i = 0; i < total; i++) { done_ev_[i] = s->ev[i]; }
    done_n_ = total;
    done_dev_ = s->dev;
    done_id_ = s->anim_id;
    s->used = false;
    return true;
  }

  const AnimEvent *events() const { return done_ev_; }
  uint8_t count() const { return done_n_; }
  uint16_t from() const { return done_dev_; }
  uint8_t animId() const { return done_id_; }

  // Drop partial transfers, so a sender that walked out of range does not
  // hold a slot forever.
  void expire(uint32_t now_ms) {
    for (uint8_t i = 0; i < ANIM_RX_SLOTS; i++) {
      if (slot_[i].used &&
          (uint32_t)(now_ms - slot_[i].first_ms) > ANIM_RX_TIMEOUT_MS) {
        slot_[i].used = false;
      }
    }
  }

 private:
  struct Slot {
    uint16_t dev;
    uint8_t anim_id;
    uint8_t total;
    uint16_t got;
    uint32_t first_ms;
    bool used;
    AnimEvent ev[ANIM_MAX_EVENTS];
  };

  Slot *find(uint16_t dev, uint8_t anim_id) {
    for (uint8_t i = 0; i < ANIM_RX_SLOTS; i++) {
      if (slot_[i].used && slot_[i].dev == dev && slot_[i].anim_id == anim_id) {
        return &slot_[i];
      }
    }
    return nullptr;
  }

  Slot *claim(uint16_t dev, uint8_t anim_id, uint8_t total, uint32_t now_ms) {
    Slot *s = nullptr;
    for (uint8_t i = 0; i < ANIM_RX_SLOTS; i++) {
      if (!slot_[i].used) { s = &slot_[i]; break; }
    }
    if (s == nullptr) {
      // All slots busy: evict the oldest rather than dropping the new
      // sender, so one stalled transfer cannot lock everyone else out.
      s = &slot_[0];
      for (uint8_t i = 1; i < ANIM_RX_SLOTS; i++) {
        if ((int32_t)(slot_[i].first_ms - s->first_ms) < 0) { s = &slot_[i]; }
      }
    }
    s->dev = dev;
    s->anim_id = anim_id;
    s->total = total;
    s->got = 0;
    s->first_ms = now_ms;
    s->used = true;
    return s;
  }

  Slot slot_[ANIM_RX_SLOTS] = {};
  AnimEvent done_ev_[ANIM_MAX_EVENTS] = {};
  uint8_t done_n_ = 0;
  uint16_t done_dev_ = 0;
  uint8_t done_id_ = 0;
};
