#include <Arduino.h>
#include <LittleFS.h>
#include <FastLED.h>

#include "animation.h"
#include "logo_base.h"
#include "plasma_v2.h"
#include "logo_anim_v4.h"

// ── Globals ─────────────────────────────────────────────────────────

CRGB leds[NUM_LEDS];

bool fsReady = false;

AnimSource    animSource       = ANIM_BUILTIN;
uint8_t       builtinIndex     = 0;      // default: logo_anim_v4
uint16_t      lfsFrameCount    = 0;
uint16_t      lfsFrameDelay    = 150;
uint16_t      currentFrame     = 0;
uint16_t      activeFrameDelay = 150;
unsigned long lastFrameTime    = 0;

// Built-in PROGMEM animation registry
const BuiltinAnim builtins[] = {
  { "logo_anim_v4", logo_anim_v4, FRAME_COUNT_LOGO_ANIM_V4 },
  { "plasma_v2",    plasma_v2,    FRAME_COUNT_PLASMA_V2 },
  { "logo_base",    logo_base,    FRAME_COUNT_LOGO_BASE },
};

const uint8_t BUILTIN_COUNT = sizeof(builtins) / sizeof(builtins[0]);

// ── XY mapping (dual-panel zigzag) ─────────────────────────────────

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

// ── Display helpers ─────────────────────────────────────────────────

static void displayProgmemFrame(const uint32_t matrix[WIDTH][HEIGHT]) {
  for (uint8_t x = 0; x < WIDTH; x++) {
    for (uint8_t y = 0; y < HEIGHT; y++) {
      uint32_t color = pgm_read_dword(&matrix[x][y]);
      leds[XY(x, y)] = CRGB(
        (color >> 16) & 0xFF,
        (color >> 8)  & 0xFF,
         color        & 0xFF
      );
    }
  }
}

static void displayRgbFrame(const uint8_t* rgb) {
  for (uint8_t x = 0; x < WIDTH; x++) {
    for (uint8_t y = 0; y < HEIGHT; y++) {
      size_t i = ((size_t)x * HEIGHT + y) * 3;
      leds[XY(x, y)] = CRGB(rgb[i], rgb[i + 1], rgb[i + 2]);
    }
  }
}

// ── LittleFS animation header ───────────────────────────────────────

bool loadLfsHeader() {
  File f = LittleFS.open(ANIM_FILE, "r");
  if (!f || f.size() < ANIM_HDR_SIZE) {
    if (f) {
      f.close();
    }
    return false;
  }

  uint8_t hdr[ANIM_HDR_SIZE];
  if (f.read(hdr, ANIM_HDR_SIZE) != ANIM_HDR_SIZE) {
    f.close();
    return false;
  }

  uint16_t fc = (uint16_t)hdr[0] | ((uint16_t)hdr[1] << 8);
  uint16_t fd = (uint16_t)hdr[2] | ((uint16_t)hdr[3] << 8);
  size_t expected = ANIM_HDR_SIZE + (size_t)fc * FRAME_BYTES;

  if (fc == 0 || fd == 0 || (size_t)f.size() < expected) {
    f.close();
    return false;
  }

  lfsFrameCount = fc;
  lfsFrameDelay = fd;
  f.close();
  return true;
}

// ── Non-blocking frame advance ──────────────────────────────────────

void showNextFrame() {
  FastLED.clear();

  if (animSource == ANIM_LITTLEFS) {
    File f = LittleFS.open(ANIM_FILE, "r");
    if (!f) {
      animSource = ANIM_BUILTIN;
      currentFrame = 0;
      return;
    }

    size_t offset = ANIM_HDR_SIZE + (size_t)currentFrame * FRAME_BYTES;
    if (!f.seek(offset)) {
      f.close();
      animSource = ANIM_BUILTIN;
      currentFrame = 0;
      return;
    }

    uint8_t buf[FRAME_BYTES];
    if (f.read(buf, FRAME_BYTES) != FRAME_BYTES) {
      f.close();
      animSource = ANIM_BUILTIN;
      currentFrame = 0;
      return;
    }
    f.close();

    displayRgbFrame(buf);
    currentFrame = (currentFrame + 1) % lfsFrameCount;
    activeFrameDelay = lfsFrameDelay;

  } else {
    const BuiltinAnim& anim = builtins[builtinIndex];
    displayProgmemFrame(anim.frames[currentFrame]);
    currentFrame = (currentFrame + 1) % anim.frameCount;
    activeFrameDelay = 150;
  }

  FastLED.show();
}

// ── Initialization ──────────────────────────────────────────────────

void initAnimation() {
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear();
  FastLED.show();

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed — check partition scheme (need Default with spiffs)");
  } else {
    fsReady = true;
    Serial.printf(
      "LittleFS OK — total: %u, used: %u\n",
      LittleFS.totalBytes(),
      LittleFS.usedBytes()
    );
  }

  if (loadLfsHeader()) {
    animSource = ANIM_LITTLEFS;
    Serial.printf(
      "Saved animation: %u frames, %u ms\n",
      lfsFrameCount,
      lfsFrameDelay
    );
  } else {
    Serial.println("No saved animation — using builtin");
  }
}

