#pragma once

#include <Arduino.h>
#include <FastLED.h>

// ── Microphone / FFT config ─────────────────────────────────────────

#define MIC_PIN           A1
#define FFT_SAMPLES       256
#define SAMPLING_FREQ     9878.0

// ── Haptic vibrator config ──────────────────────────────────────────

#define VIBRATOR_PIN      10
#define VIBRATOR_PWM_FREQ 1000
#define VIBRATOR_PWM_RES  8        // 0-255

// ── Visualizer color palette ────────────────────────────────────────

#define VIZ_COLOR_COUNT 5

extern const CRGB vizColorPalette[VIZ_COLOR_COUNT];
extern const char* vizColorNames[VIZ_COLOR_COUNT];

// Call once from setup() to configure the ADC pin and build the
// logarithmic frequency-band table.
void initVisualizer();

// Sample the mic, run FFT, and push one frame of the spectrum
// analyzer to the LED matrix.  Call this from loop() when in
// visualizer mode — it handles its own timing.
void runVisualizer();

// Reset state when leaving visualizer mode.
void resetVisualizer();

// Switch visualizer bar color. Flashes the new color for 1 second.
// Returns false if index is out of range.
bool setVisualizerColor(uint8_t index);

// Current color index (0 .. VIZ_COLOR_COUNT-1).
uint8_t getVisualizerColorIndex();
