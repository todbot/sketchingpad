// led_anim.h -- a tiny non-blocking LED animation player
//
// An animation is a list of (delta time, LED, color) events.  Load one,
// start() it, then call update() as often as you like from loop(); it
// fires events whose deadline has passed and fades the rest.  Nothing
// blocks and nothing calls delay().
//
// Depends on nothing but <stdint.h> -- no NeoPixel, no BLE -- so it drops
// into any sketch and can be exercised on a host.  The player owns its own
// frame buffer; the sketch blits pixel(i) out to the strip when update()
// says the frame changed.
//
//   LedAnim anim;
//   anim.load(events, n);
//   anim.start(millis());
//   ...
//   if (anim.update(millis())) {
//     for (int i = 0; i < ANIM_NUM_LEDS; i++) leds.setPixelColor(i, anim.pixel(i));
//     leds.show();
//   }

#pragma once

#include <stdint.h>

#define ANIM_NUM_LEDS 20
#define ANIM_MAX_EVENTS 72 // ANIM_EVENTS_PER_CHUNK * ANIM_MAX_CHUNKS
#define ANIM_FRAME_MS 10   // decay tick, 100 Hz
#define ANIM_DECAY 5       // subtracted per channel per frame
#define ANIM_DT_MS 10      // one dt unit

// dt is the gap *before* this event: from the previous event, or from
// start() for the first one.  255 units = 2.55 s is the longest gap
// expressible; a recorder wanting more must insert a filler event.
struct AnimEvent {
  uint8_t dt;
  uint8_t led_color; // led in bits 7..3, palette index in bits 2..0
};

static inline uint8_t animEventLed(AnimEvent e) { return e.led_color >> 3; }
static inline uint8_t animEventColor(AnimEvent e) { return e.led_color & 0x07; }

static inline AnimEvent animEventMake(uint8_t dt, uint8_t led, uint8_t color) {
  AnimEvent e;
  e.dt = dt;
  e.led_color = (uint8_t)((led << 3) | (color & 0x07));
  return e;
}

// Blue is pulled toward cyan and yellow toward amber: this board's backlit
// SK6812s show blue poorly (thin 3.3 V headroom, see docs/issues.md), and a
// full-green yellow is hard to tell from green at low brightness.
static const uint32_t ANIM_PALETTE[8] = {
    0xff0000, // 0 red
    0xff5000, // 1 orange
    0xffb000, // 2 amber
    0x00ff00, // 3 green
    0x00ffb0, // 4 cyan
    0x0060ff, // 5 blue
    0xff00ff, // 6 magenta
    0xffffff, // 7 white
};

class LedAnim {
 public:
  void load(const AnimEvent *ev, uint8_t n) {
    n_ = (n > ANIM_MAX_EVENTS) ? ANIM_MAX_EVENTS : n;
    for (uint8_t i = 0; i < n_; i++) { ev_[i] = ev[i]; }
  }

  void start(uint32_t now_ms) {
    clear();
    playing_ = (n_ > 0);
    next_ = 0;
    last_frame_ = now_ms;
    if (playing_) { due_ = now_ms + (uint32_t)ev_[0].dt * ANIM_DT_MS; }
  }

  void stop() {
    playing_ = false;
    clear();
  }

  bool playing() const { return playing_; }

  uint32_t pixel(uint8_t i) const {
    if (i >= ANIM_NUM_LEDS) { return 0; }
    return ((uint32_t)pix_[i][0] << 16) | ((uint32_t)pix_[i][1] << 8) | pix_[i][2];
  }

  // Light a LED right now, outside any animation, so live touch feedback
  // gets the same trail as playback.  Does not start or disturb playback.
  void poke(uint8_t led, uint8_t color) {
    setPix(led, ANIM_PALETTE[color & 0x07]);
    lit_ = true;
  }

  // Returns true if the frame changed and is worth pushing to the strip.
  bool update(uint32_t now_ms) {
    bool changed = false;

    // Signed comparison so the deadline survives millis() wrapping.
    while (playing_ && next_ < n_ && (int32_t)(now_ms - due_) >= 0) {
      AnimEvent e = ev_[next_];
      setPix(animEventLed(e), ANIM_PALETTE[animEventColor(e)]);
      changed = true;
      lit_ = true;
      next_++;
      if (next_ < n_) { due_ += (uint32_t)ev_[next_].dt * ANIM_DT_MS; }
    }

    if (lit_ && (uint32_t)(now_ms - last_frame_) >= ANIM_FRAME_MS) {
      last_frame_ = now_ms;
      bool any = false;
      for (uint8_t i = 0; i < ANIM_NUM_LEDS; i++) {
        for (uint8_t c = 0; c < 3; c++) {
          uint8_t v = pix_[i][c];
          if (v) {
            pix_[i][c] = (v > ANIM_DECAY) ? (uint8_t)(v - ANIM_DECAY) : 0;
            changed = true;
            if (pix_[i][c]) { any = true; }
          }
        }
      }
      lit_ = any;
      // Playback ends only once the last event has faded out, not when the
      // last event fires -- otherwise the trail is cut off mid-fade.
      if (playing_ && next_ >= n_ && !any) { playing_ = false; }
    }

    return changed;
  }

 private:
  void clear() {
    for (uint8_t i = 0; i < ANIM_NUM_LEDS; i++) {
      pix_[i][0] = pix_[i][1] = pix_[i][2] = 0;
    }
    lit_ = false;
  }

  void setPix(uint8_t led, uint32_t rgb) {
    if (led >= ANIM_NUM_LEDS) { return; }
    pix_[led][0] = (uint8_t)(rgb >> 16);
    pix_[led][1] = (uint8_t)(rgb >> 8);
    pix_[led][2] = (uint8_t)rgb;
  }

  AnimEvent ev_[ANIM_MAX_EVENTS];
  uint8_t n_ = 0;
  uint8_t next_ = 0;
  uint32_t due_ = 0;
  uint32_t last_frame_ = 0;
  bool playing_ = false;
  bool lit_ = false;
  // Our own buffer, so the fade does not fight setBrightness() rounding.
  uint8_t pix_[ANIM_NUM_LEDS][3] = {};
};
