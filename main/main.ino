#include <FastLED.h>
#include "logo_base_animated.h"
#include "plasma_flow.h"
#include "plasma_v2.h"
#include "logo_anim_v2.h"
#include "logo_anim_v3.h"

#define LED_PIN     13
#define WIDTH       32
#define HEIGHT      16
#define NUM_LEDS    (WIDTH * HEIGHT)
#define BRIGHTNESS  40
#define LED_TYPE    WS2812B
#define COLOR_ORDER GRB
#define PANEL_HEIGHT 8
#define PANEL_LED_COUNT 256

CRGB leds[NUM_LEDS];

uint16_t XY(uint8_t x, uint8_t y) {
  uint16_t returnValue;
  
  if (y < PANEL_HEIGHT) {
    // Bas de l'affichage = panneau du bas (1er dans la chaîne)
    if (x % 2 == 0) {
      returnValue = (x * PANEL_HEIGHT) + PANEL_HEIGHT - 1 - y;
    } else {
      returnValue = (x * PANEL_HEIGHT) + y ;
    }
  } else {
    // Haut de l'affichage = panneau du haut (2e dans la chaîne)
     if (x % 2 == 0) {
      returnValue = (31 - x) * PANEL_HEIGHT + PANEL_LED_COUNT + HEIGHT - y - 1;
     } else {
      returnValue = (30 - x) * PANEL_HEIGHT + PANEL_LED_COUNT + y;
     }
  }

  // Serial.println("x:");
  // Serial.println(x);
  // Serial.println("y:");
  // Serial.println(y);
  // Serial.println("return value:");
  // Serial.println(returnValue);

  return returnValue;
}

void displayMatrixFrame(const uint32_t matrix[WIDTH][HEIGHT]) {

  for (uint8_t x = 0; x < WIDTH; x++) {
    for (uint8_t y = 0; y < HEIGHT; y++) {

      // Lire la couleur depuis la PROGMEM (frame passée en paramètre)
      uint32_t color = pgm_read_dword(&(matrix[x][y]));

      // Extraire R, G, B (format 0xRRGGBB)
      uint8_t r = (color >> 16) & 0xFF;
      uint8_t g = (color >> 8)  & 0xFF;
      uint8_t b =  color        & 0xFF;

      leds[XY(x, y)] = CRGB(r, g, b);
    }
  }
}

// Play any animation table: frames[frameCount][WIDTH][HEIGHT] in PROGMEM
void playAnimation(const uint32_t (*frames)[WIDTH][HEIGHT], uint16_t frameCount, uint16_t frameDelayMs) {
  for (uint16_t f = 0; f < frameCount; f++) {
    FastLED.clear();
    displayMatrixFrame(frames[f]);
    FastLED.show();
    delay(frameDelayMs);
  }
}

void setup() {
  delay(1000);
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear();
  FastLED.show();
}

void loop() {
  playAnimation(logo_anim_v3, FRAME_COUNT_LOGO_ANIM_V3, 150);

  //playAnimation(plasma_v2, FRAME_COUNT_PLASMA_FLOW, 150);

}