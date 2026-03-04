#pragma once

#include <Arduino.h>

// ── Microphone / FFT config ─────────────────────────────────────────

#define MIC_PIN           A1
#define FFT_SAMPLES       256
#define SAMPLING_FREQ     9878.0

// ── Haptic vibrator config ──────────────────────────────────────────

#define VIBRATOR_PIN      10
#define VIBRATOR_PWM_FREQ 1000
#define VIBRATOR_PWM_RES  8        // 0-255

// Call once from setup() to configure the ADC pin and build the
// logarithmic frequency-band table.
void initVisualizer();

// Sample the mic, run FFT, and push one frame of the spectrum
// analyzer to the LED matrix.  Call this from loop() when in
// visualizer mode — it handles its own timing.
void runVisualizer();

// Reset state when leaving visualizer mode.
void resetVisualizer();
