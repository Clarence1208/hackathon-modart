#pragma once

#include <Arduino.h>
#include <FastLED.h>

// ── LED matrix config ───────────────────────────────────────────────

#define LED_PIN         13
#define WIDTH           32
#define HEIGHT          16
#define NUM_LEDS        (WIDTH * HEIGHT)
#define BRIGHTNESS      40
#define LED_TYPE        WS2812B
#define COLOR_ORDER     GRB
#define PANEL_HEIGHT    8
#define PANEL_LED_COUNT 256

// ── Animation file format ───────────────────────────────────────────
// Bytes 0-1 : frameCount  (uint16_t LE)
// Bytes 2-3 : frameDelayMs (uint16_t LE)
// Bytes 4+  : pixel data — frameCount * WIDTH * HEIGHT * 3 bytes
//             Column-major (x outer, y inner), 3 bytes per pixel (R,G,B)

#define FRAME_BYTES   (WIDTH * HEIGHT * 3)   // 1536
#define ANIM_FILE     "/anim.bin"
#define ANIM_HDR_SIZE 4

// Global LED buffer
extern CRGB leds[NUM_LEDS];

// Built-in PROGMEM animation registry
struct BuiltinAnim {
  const char* name;
  const uint32_t (*frames)[WIDTH][HEIGHT];
  uint16_t frameCount;
};

extern const BuiltinAnim builtins[];
extern const uint8_t     BUILTIN_COUNT;

// Playback state
enum AnimSource : uint8_t { ANIM_BUILTIN, ANIM_LITTLEFS, ANIM_VISUALIZER, ANIM_STATIC };

extern AnimSource    animSource;
extern uint8_t       builtinIndex;      // default: logo_anim_v4
extern uint16_t      lfsFrameCount;
extern uint16_t      lfsFrameDelay;
extern uint16_t      currentFrame;
extern uint16_t      activeFrameDelay;
extern unsigned long lastFrameTime;

// Filesystem state
extern bool fsReady;

// ── XY mapping ──────────────────────────────────────────────────────

uint16_t XY(uint8_t x, uint8_t y);

// ── Animation lifecycle ─────────────────────────────────────────────

// Configure FastLED, mount LittleFS, and load any saved animation header.
void initAnimation();

// Reload the header for the LittleFS-backed animation file, if present.
bool loadLfsHeader();

// Advance and display the next frame from the active animation source.
void showNextFrame();

