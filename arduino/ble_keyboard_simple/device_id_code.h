// device_id_code.h -- nRF5x unique device ID as a short color code
//
// Every nRF52 has a 64-bit factory-programmed ID in FICR.  This hashes it
// down to five letters drawn from R/Y/G/B, e.g. "RYGGB", so a board can be
// told apart from its siblings by name or by lighting five RGB LEDs.
//
// To use it in another sketch, copy this one file next to the .ino and
// #include "device_id_code.h".  It is header-only and depends on nothing
// but the nRF52 core -- no NeoPixel, no BLE -- so how the code gets shown
// is up to the sketch.
//
//   char code[DEVICE_ID_CODE_LEN + 1];
//   deviceIdCode(code);                    // "RYGGB"
//
//   char name[32];
//   deviceIdName(name, sizeof(name), "sketchingpad");  // "sketchingpad-RYGGB"
//
//   uint32_t colors[DEVICE_ID_CODE_LEN];
//   deviceIdColors(colors);                // 0xRRGGBB per letter
//
// Five letters is 4^5 = 1024 codes.  That is an identifier hint, not a
// guarantee: collisions get likely somewhere past a dozen boards.

#pragma once

#include <Arduino.h>

#define DEVICE_ID_CODE_LEN 5

// 2-bit value 0..3 -> letter.  Keep this table and the color table below
// in the same order.
static const char DEVICE_ID_CODE_LETTERS[4] = {'R', 'Y', 'G', 'B'};

// Yellow leans amber on purpose: at low brightness a full-green yellow is
// hard to tell from plain green on a side-view SK6812.
static const uint32_t DEVICE_ID_CODE_COLORS[4] = {
    0xff0000, // R
    0xff6000, // Y
    0x00ff00, // G
    0x0000ff, // B
};

// The 64-bit unique ID, FICR DEVICEID[0..1] (same registers the core's
// hwinfo example reads as 0x10000060 / 0x10000064).
static inline uint64_t deviceIdRaw(void) {
  return ((uint64_t)NRF_FICR->DEVICEID[1] << 32) | NRF_FICR->DEVICEID[0];
}

// FNV-1a over the eight ID bytes.  The ID's own bits are not well mixed,
// so hash before slicing or nearby chips get near-identical codes.
static inline uint32_t deviceIdHash(void) {
  uint64_t id = deviceIdRaw();
  uint32_t h = 2166136261UL;
  for (uint8_t i = 0; i < 8; i++) {
    h ^= (uint8_t)(id >> (i * 8));
    h *= 16777619UL;
  }
  return h;
}

// Fill out[] with DEVICE_ID_CODE_LEN letters plus a NUL.  out[0] is the
// first/leftmost position and takes the hash's low 2 bits.
static inline void deviceIdCode(char *out) {
  uint32_t h = deviceIdHash();
  for (uint8_t i = 0; i < DEVICE_ID_CODE_LEN; i++) {
    out[i] = DEVICE_ID_CODE_LETTERS[(h >> (i * 2)) & 3];
  }
  out[DEVICE_ID_CODE_LEN] = '\0';
}

// The same code as 0xRRGGBB colors, one per position, for showing on LEDs.
static inline void deviceIdColors(uint32_t *out) {
  uint32_t h = deviceIdHash();
  for (uint8_t i = 0; i < DEVICE_ID_CODE_LEN; i++) {
    out[i] = DEVICE_ID_CODE_COLORS[(h >> (i * 2)) & 3];
  }
}

// "<prefix>-RYGGB".  out needs strlen(prefix) + DEVICE_ID_CODE_LEN + 2 bytes.
static inline void deviceIdName(char *out, size_t out_len, const char *prefix) {
  char code[DEVICE_ID_CODE_LEN + 1];
  deviceIdCode(code);
  snprintf(out, out_len, "%s-%s", prefix, code);
}
