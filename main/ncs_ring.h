#pragma once

#include <Arduino.h>
#include <FastLED.h>

// Call once from setup() — precomputes ring geometry and band tables.
void initNcsRing();

// Sample the mic, run FFT, and render one frame of the NCS ring
// visualizer on the logo.  Call from loop() when in NCS ring mode.
void runNcsRing();

// Reset state when leaving NCS ring mode.
void resetNcsRing();
