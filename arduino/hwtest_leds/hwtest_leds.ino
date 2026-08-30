// hwtest_leds.ino -- chase a single pixel around the sketchingpad LED ring
//
// Arduino port of circuitpython/hwtest/code_ledtest.py, for a
// Seeed XIAO nRF52840 plugged into the sketchingpad_stripproto1 board.
//
// Board: Seeeduino:nrf52:xiaonRF52840

#include <Adafruit_NeoPixel.h>

// sketchingpad_stripproto1: LED_DAT is J1 pin 7 -> XIAO D6.
// (Flip these if the module is seated in the other orientation.)
#define PIN_LED_DATA (6)

const int NUM_LEDS = 20;
const int LED_BRIGHTNESS = 50; // out of 255, ~0.2 like the CircuitPython test

Adafruit_NeoPixel leds(NUM_LEDS, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);

// same 0-255 hue ramp as CircuitPython's rainbowio.colorwheel()
uint32_t colorwheel(uint8_t pos) {
  if (pos < 85) {
    return Adafruit_NeoPixel::Color(255 - pos * 3, pos * 3, 0);
  } else if (pos < 170) {
    pos -= 85;
    return Adafruit_NeoPixel::Color(0, 255 - pos * 3, pos * 3);
  }
  pos -= 170;
  return Adafruit_NeoPixel::Color(pos * 3, 0, 255 - pos * 3);
}

void setup() {
  Serial.begin(115200);

  leds.begin();
  leds.setBrightness(LED_BRIGHTNESS);
  leds.clear();
  leds.show();

  Serial.println("hwtest_leds: sketchingpad LED chase");
}

int i = 0;

void loop() {
  uint32_t c = colorwheel(millis() / 10); // matches time.monotonic()*100

  leds.clear();
  leds.setPixelColor(i, c);
  leds.show();

  Serial.printf("i: %d  c: %06lx\n", i, (unsigned long)c);

  i = (i + 1) % NUM_LEDS;
  delay(300);
}
