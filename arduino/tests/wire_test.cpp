// SPDX-FileCopyrightText: 2026 Tod Kurt
// SPDX-License-Identifier: GPL-3.0-or-later
//
// wire_test.cpp -- AnimMsg packing, chunking and reassembly
//
// The reassembler is the part worth testing here: advertising is
// unacknowledged, so chunks arrive out of order, duplicated, or not at
// all, and every one of those paths is awkward to reproduce on hardware.

#include "anim_wire.h"
#include "test_util.h"

static const uint16_t DEV = 0xBEEF;

// A recognisable animation: every event distinct in all three fields.
static uint8_t makeAnim(AnimEvent *out, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) {
    out[i] = animEventMake((uint8_t)(i * 3 + 1), (uint8_t)(i % ANIM_NUM_LEDS),
                           (uint8_t)(i % 8));
  }
  return n;
}

static bool sameEvents(const AnimEvent *a, const AnimEvent *b, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) {
    if (a[i].dt != b[i].dt || a[i].led_color != b[i].led_color) { return false; }
  }
  return true;
}

// Deliver chunks in the given order; returns how many times a transfer
// completed, which must always be exactly 1.
static int deliver(AnimReassembler &rx, const AnimEvent *ev, uint8_t n,
                   uint8_t anim_id, const int *order, int count, bool dup) {
  int completions = 0;
  for (int i = 0; i < count; i++) {
    AnimMsg m;
    animFillChunk(&m, 0xFFFF, DEV, anim_id, (uint8_t)order[i], ev, n);
    if (rx.feed(m, 1000)) { completions++; }
    if (dup && rx.feed(m, 1000)) { completions++; }
  }
  return completions;
}

int main() {
  printf("wire_test\n");

  // --- the budget. BLEAdvertisingData::addData() guards
  // _count + len + 2 <= 31, and addFlags() has already spent 3.
  printf("  sizeof(AnimEvent)=%zu sizeof(AnimMsg)=%zu\n", sizeof(AnimEvent),
         sizeof(AnimMsg));
  CHECK(sizeof(AnimEvent) == 2, "AnimEvent is 2 bytes");
  CHECK(sizeof(AnimMsg) == 25, "AnimMsg is 25 bytes");
  CHECK(3 + (int)sizeof(AnimMsg) + 2 <= 31, "flags + MSD fits a 31-byte advert");
  CHECK(ANIM_MAX_EVENTS == ANIM_EVENTS_PER_CHUNK * ANIM_MAX_CHUNKS,
        "max events matches chunks x per-chunk");
  CHECK(ANIM_MAX_CHUNKS <= 16, "chunk count fits the uint16_t got mask");

  // --- field packing, exhaustively
  bool packs = true;
  for (int led = 0; led < 32; led++) {
    for (int col = 0; col < 8; col++) {
      AnimEvent e = animEventMake(42, (uint8_t)led, (uint8_t)col);
      if (e.dt != 42 || animEventLed(e) != led || animEventColor(e) != col) {
        packs = false;
      }
    }
  }
  CHECK(packs, "all 32 LED slots x 8 colors round-trip through 2 bytes");

  // --- chunk arithmetic
  CHECK(animChunkCount(1) == 1, "1 event is 1 chunk");
  CHECK(animChunkCount(9) == 1, "9 events is 1 chunk");
  CHECK(animChunkCount(10) == 2, "10 events is 2 chunks");
  CHECK(animChunkCount(30) == 4, "30 events is 4 chunks");
  CHECK(animChunkCount(ANIM_MAX_EVENTS) == ANIM_MAX_CHUNKS,
        "the maximum is exactly ANIM_MAX_CHUNKS");

  AnimEvent anim[ANIM_MAX_EVENTS];
  const uint8_t N = makeAnim(anim, 30);
  const int chunks = animChunkCount(N);

  // --- in order
  {
    AnimReassembler rx;
    int order[8];
    for (int i = 0; i < chunks; i++) { order[i] = i; }
    CHECK(deliver(rx, anim, N, 1, order, chunks, false) == 1,
          "in-order delivery completes exactly once");
    CHECK(rx.count() == N && rx.from() == DEV && rx.animId() == 1,
          "sender, id and count survive reassembly");
    CHECK(sameEvents(rx.events(), anim, N), "all 30 events come back byte-identical");
  }

  // --- reverse order, every packet duplicated
  {
    AnimReassembler rx;
    int order[8];
    for (int i = 0; i < chunks; i++) { order[i] = chunks - 1 - i; }
    CHECK(deliver(rx, anim, N, 2, order, chunks, true) == 1,
          "reverse order with duplicates still completes exactly once");
    CHECK(sameEvents(rx.events(), anim, N), "events intact despite duplicates");
  }

  // --- lose each chunk in turn, then deliver it late
  {
    bool ok = true;
    for (int drop = 0; drop < chunks; drop++) {
      AnimReassembler rx;
      for (int c = 0; c < chunks; c++) {
        if (c == drop) { continue; }
        AnimMsg m;
        animFillChunk(&m, 0xFFFF, DEV, 3, (uint8_t)c, anim, N);
        if (rx.feed(m, 1000)) { ok = false; } // must not complete yet
      }
      AnimMsg late;
      animFillChunk(&late, 0xFFFF, DEV, 3, (uint8_t)drop, anim, N);
      if (!rx.feed(late, 1000)) { ok = false; }
      if (!sameEvents(rx.events(), anim, N)) { ok = false; }
    }
    CHECK(ok, "losing any single chunk blocks until it is redelivered");
  }

  // --- two senders interleaved, which is what a room full of these does
  {
    AnimReassembler rx;
    int done_a = 0, done_b = 0;
    for (int c = 0; c < chunks; c++) {
      AnimMsg a, b;
      animFillChunk(&a, 0xFFFF, 0x1111, 5, (uint8_t)c, anim, N);
      animFillChunk(&b, 0xFFFF, 0x2222, 5, (uint8_t)c, anim, N);
      if (rx.feed(a, 1000)) { done_a++; }
      if (rx.feed(b, 1000)) { done_b++; }
    }
    CHECK(done_a == 1 && done_b == 1, "two interleaved senders both complete");
  }

  // --- a third sender evicts the oldest partial rather than being dropped
  {
    AnimReassembler rx;
    AnimMsg a, b, c;
    animFillChunk(&a, 0xFFFF, 0x1111, 6, 0, anim, N);
    animFillChunk(&b, 0xFFFF, 0x2222, 6, 0, anim, N);
    animFillChunk(&c, 0xFFFF, 0x3333, 6, 0, anim, N);
    rx.feed(a, 1000);
    rx.feed(b, 2000);
    rx.feed(c, 3000); // evicts 0x1111
    int done = 0;
    for (int i = 1; i < chunks; i++) {
      AnimMsg m;
      animFillChunk(&m, 0xFFFF, 0x3333, 6, (uint8_t)i, anim, N);
      if (rx.feed(m, 3000)) { done++; }
    }
    CHECK(done == 1, "a third sender is not locked out by two stalled partials");
  }

  // --- a partial expires and does not wedge the slot
  {
    AnimReassembler rx;
    AnimMsg first;
    animFillChunk(&first, 0xFFFF, DEV, 7, 0, anim, N);
    rx.feed(first, 1000);
    rx.expire(1000 + ANIM_RX_TIMEOUT_MS + 1);

    int done = 0;
    for (int c = 0; c < chunks; c++) {
      AnimMsg m;
      animFillChunk(&m, 0xFFFF, DEV, 7, (uint8_t)c, anim, N);
      if (rx.feed(m, 99999)) { done++; }
    }
    CHECK(done == 1, "an expired partial frees its slot for a fresh transfer");
  }

  // --- expire must not touch a transfer still in progress
  {
    AnimReassembler rx;
    AnimMsg m0;
    animFillChunk(&m0, 0xFFFF, DEV, 8, 0, anim, N);
    rx.feed(m0, 1000);
    rx.expire(1000 + ANIM_RX_TIMEOUT_MS - 1); // still inside the window
    int done = 0;
    for (int c = 1; c < chunks; c++) {
      AnimMsg m;
      animFillChunk(&m, 0xFFFF, DEV, 8, (uint8_t)c, anim, N);
      if (rx.feed(m, 1000)) { done++; }
    }
    CHECK(done == 1, "expire leaves an in-window partial alone");
  }

  // --- malformed packets
  {
    AnimReassembler rx;
    AnimMsg m;
    animFillChunk(&m, 0xFFFF, DEV, 9, 0, anim, N);
    uint8_t good_total = m.total_events;

    m.total_events = 0;
    CHECK(!rx.feed(m, 1), "rejects total_events == 0");
    m.total_events = ANIM_MAX_EVENTS + 1;
    CHECK(!rx.feed(m, 1), "rejects total_events past the maximum");
    m.total_events = good_total;
    m.chunk_idx = ANIM_MAX_CHUNKS;
    CHECK(!rx.feed(m, 1), "rejects chunk_idx past the chunk count");
    m.chunk_idx = (uint8_t)chunks; // valid index range, wrong for this total
    CHECK(!rx.feed(m, 1), "rejects a chunk_idx beyond this animation's chunks");
  }

  // --- an oversized total_events must be refused outright.  The
  // single-packet check above can pass for the wrong reason (chunk_idx
  // catches it first); delivering every chunk is what actually exercises
  // the guard, and without it dst runs past the end of the slot buffer.
  {
    AnimEvent big[ANIM_MAX_EVENTS];
    makeAnim(big, ANIM_MAX_EVENTS);
    AnimReassembler rx;
    int done = 0;
    int over_chunks = animChunkCount(ANIM_MAX_EVENTS + 1);
    for (int c = 0; c < over_chunks; c++) {
      AnimMsg m;
      animFillChunk(&m, 0xFFFF, DEV, 13, (uint8_t)c, big, ANIM_MAX_EVENTS);
      m.total_events = ANIM_MAX_EVENTS + 1; // claims more than can be held
      if (rx.feed(m, 1)) { done++; }
    }
    CHECK(done == 0, "an over-long animation never completes, at any chunk");
  }

  // --- the extremes
  {
    AnimEvent one[1] = {animEventMake(0, 3, 5)};
    AnimReassembler rx;
    AnimMsg m;
    animFillChunk(&m, 0xFFFF, DEV, 10, 0, one, 1);
    CHECK(rx.feed(m, 1) && rx.count() == 1 && sameEvents(rx.events(), one, 1),
          "a single-event animation is one chunk and round-trips");
  }
  {
    AnimEvent big[ANIM_MAX_EVENTS];
    makeAnim(big, ANIM_MAX_EVENTS);
    AnimReassembler rx;
    int done = 0;
    for (int c = 0; c < ANIM_MAX_CHUNKS; c++) {
      AnimMsg m;
      animFillChunk(&m, 0xFFFF, DEV, 11, (uint8_t)c, big, ANIM_MAX_EVENTS);
      if (rx.feed(m, 1)) { done++; }
    }
    CHECK(done == 1 && rx.count() == ANIM_MAX_EVENTS &&
              sameEvents(rx.events(), big, ANIM_MAX_EVENTS),
          "the 72-event maximum round-trips");
  }

  // --- the last chunk is zero-padded, since every packet is one length
  {
    AnimMsg m;
    animFillChunk(&m, 0xFFFF, DEV, 12, 3, anim, N); // 30 events -> 3 in chunk 3
    bool padded = true;
    for (int i = 3; i < ANIM_EVENTS_PER_CHUNK; i++) {
      if (m.events[i].dt != 0 || m.events[i].led_color != 0) { padded = false; }
    }
    CHECK(padded, "the short final chunk is zero-padded to full length");
  }

  return test_report("wire_test");
}
