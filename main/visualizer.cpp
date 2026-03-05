#include "arduinoFFT.h"
#include <FastLED.h>

#include "visualizer.h"
#include "animation.h"

// ── Colour palette ──────────────────────────────────────────────────

const CRGB vizColorPalette[VIZ_COLOR_COUNT] = {
  CRGB(0,   120, 255),   // Ocean Blue (original)
  CRGB(255, 0,   100),   // Hot Pink
  CRGB(0,   230, 118),   // Emerald Green
  CRGB(160, 0,   255),   // Electric Purple
  CRGB(255, 170, 0),     // Amber Gold
};

const char* vizColorNames[VIZ_COLOR_COUNT] = {
  "Ocean Blue", "Hot Pink", "Emerald", "Purple", "Amber"
};

static uint8_t currentColorIndex = 0;

// ── Tunables ────────────────────────────────────────────────────────

static CRGB barColor             = vizColorPalette[0];
static const float NOISE_FLOOR   = 1500.0f;
static const float SENSITIVITY   = 20000.0f;
static const float SMOOTH_FACTOR = 0.70f;

// ── Haptic / beat detection tunables ────────────────────────────────
// Bass bins: at 9878 Hz / 256 samples ≈ 38.6 Hz per bin.
// Bins 2-7 cover ~77-270 Hz (kick drum / bass range).
static const uint16_t BASS_BIN_START     = 2;
static const uint16_t BASS_BIN_END       = 7;
static const float    BASS_NOISE_FLOOR   = 800.0f;
static const float    BASS_SENSITIVITY   = 8000.0f;

// Onset-based beat detection: trigger when current bass exceeds
// running average by BEAT_THRESHOLD multiplier.
static const float    BEAT_THRESHOLD     = 1.3f;
static const float    BEAT_AVG_DECAY     = 0.80f;
static const float    BEAT_MIN_LEVEL     = 0.12f;
static const uint32_t BEAT_COOLDOWN_MS   = 60;

// Vibration pulse shape: slam to max on beat, then decay fast.
static const float    VIB_DECAY          = 0.30f;
static const uint8_t  VIBRATOR_MAX_PWM   = 255;

// ── FFT buffers ─────────────────────────────────────────────────────

static double vReal[FFT_SAMPLES];
static double vImag[FFT_SAMPLES];

static ArduinoFFT<double> FFT =
    ArduinoFFT<double>(vReal, vImag, FFT_SAMPLES, SAMPLING_FREQ);

// ── Per-column state ────────────────────────────────────────────────

static float colHeight[WIDTH];

static uint16_t bandStart[WIDTH];
static uint16_t bandEnd[WIDTH];

static bool visualizerStarted = false;

// ── Haptic state ────────────────────────────────────────────────────

static float bassAverage  = 0.0f;
static float vibLevel     = 0.0f;
static uint32_t lastBeatMs = 0;

// ── Initialisation ──────────────────────────────────────────────────

void initVisualizer() {
  pinMode(MIC_PIN, INPUT);

  // PWM setup for haptic vibrator on GPIO 10
  ledcAttach(VIBRATOR_PIN, VIBRATOR_PWM_FREQ, VIBRATOR_PWM_RES);
  ledcWrite(VIBRATOR_PIN, 0);

  const int minBin = 3;
  const int maxBin = FFT_SAMPLES / 2 - 1;
  float logMin = log((float)minBin);
  float logMax = log((float)(maxBin + 1));

  for (int col = 0; col < WIDTH; col++) {
    float lo = exp(logMin + (logMax - logMin) * col       / (float)WIDTH);
    float hi = exp(logMin + (logMax - logMin) * (col + 1) / (float)WIDTH);

    bandStart[col] = (uint16_t)constrain((int)lo, minBin, maxBin);
    bandEnd[col]   = (uint16_t)constrain((int)hi, minBin, maxBin + 1);
    if (bandEnd[col] <= bandStart[col]) {
      bandEnd[col] = bandStart[col] + 1;
    }

    colHeight[col] = 0;
  }

  Serial.println("[VIZ] initVisualizer done (haptic on GPIO 10)");
}

// ── Main visualiser tick ────────────────────────────────────────────

void runVisualizer() {

  // Flash all LEDs blue on first entry so we know the mode is active.
  if (!visualizerStarted) {
    visualizerStarted = true;
    Serial.println("[VIZ] === VISUALIZER MODE ACTIVE ===");

    FastLED.clear();
    for (int i = 0; i < NUM_LEDS; i++) leds[i] = barColor;
    FastLED.show();
    delay(400);
    FastLED.clear();
    FastLED.show();
    delay(200);
  }

  // ── 1. Acquire samples ──────────────────────────────────────────

  unsigned long period = (unsigned long)round(1000000.0 / SAMPLING_FREQ);

  for (int i = 0; i < FFT_SAMPLES; i++) {
    unsigned long t0 = micros();
    vReal[i] = analogRead(MIC_PIN);
    vImag[i] = 0;
    while (micros() - t0 < period) { /* busy-wait */ }
  }

  // Debug: print a few raw ADC readings
  static unsigned long lastDebug = 0;
  if (millis() - lastDebug > 1000) {
    lastDebug = millis();
    //Serial.printf(
    //  "[VIZ] Raw ADC: %d %d %d %d | ",
    //   (int)vReal[0], (int)vReal[1], (int)vReal[2], (int)vReal[3]
    //);

    // ── also show the max magnitude after FFT for tuning ──
  }

  // ── 2. Remove DC offset ────────────────────────────────────────

  double mean = 0;
  for (int i = 0; i < FFT_SAMPLES; i++) mean += vReal[i];
  mean /= FFT_SAMPLES;
  for (int i = 0; i < FFT_SAMPLES; i++) vReal[i] -= mean;

  // ── 3. FFT ─────────────────────────────────────────────────────

  FFT.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
  FFT.compute(FFT_FORWARD);
  FFT.complexToMagnitude();

  // Debug: find the overall max magnitude
  double maxMag = 0;
  for (int i = 3; i < FFT_SAMPLES / 2; i++) {
    if (vReal[i] > maxMag) maxMag = vReal[i];
  }

  if (millis() - lastDebug < 50) {
    Serial.printf("MaxMag: %.1f\n", maxMag);
  }

  // ── 4. Map bins → column heights ───────────────────────────────

  for (int col = 0; col < WIDTH; col++) {
    double peak = 0;
    for (uint16_t bin = bandStart[col]; bin < bandEnd[col]; bin++) {
      if (vReal[bin] > peak) peak = vReal[bin];
    }

    if (peak < NOISE_FLOOR) peak = 0;

    float target = constrain((float)peak / SENSITIVITY, 0.0f, 1.0f) * (float)HEIGHT;

    if (target > colHeight[col]) {
      colHeight[col] = colHeight[col] * 0.3f + target * 0.7f;
    } else {
      colHeight[col] = colHeight[col] * SMOOTH_FACTOR + target * (1.0f - SMOOTH_FACTOR);
    }

    if (colHeight[col] < 0.4f) colHeight[col] = 0;
  }

  // ── 5. Haptic beat detection (onset → punchy vibration pulse) ───

  {
    double bassPeak = 0;
    for (uint16_t bin = BASS_BIN_START; bin < BASS_BIN_END; bin++) {
      if (vReal[bin] > bassPeak) bassPeak = vReal[bin];
    }

    float bassLevel = 0.0f;
    if (bassPeak > BASS_NOISE_FLOOR) {
      bassLevel = constrain((float)(bassPeak - BASS_NOISE_FLOOR) / BASS_SENSITIVITY, 0.0f, 1.0f);
    }

    // Running average of bass level for onset comparison
    bassAverage = bassAverage * BEAT_AVG_DECAY + bassLevel * (1.0f - BEAT_AVG_DECAY);

    float onsetThreshold = max(bassAverage * BEAT_THRESHOLD, BEAT_MIN_LEVEL);
    uint32_t now = millis();

    if (bassLevel > onsetThreshold && (now - lastBeatMs) > BEAT_COOLDOWN_MS) {
      vibLevel = 1.0f;
      lastBeatMs = now;
    } else {
      vibLevel *= VIB_DECAY;
      if (vibLevel < 0.02f) vibLevel = 0.0f;
    }

    uint8_t pwm = (uint8_t)(vibLevel * (float)VIBRATOR_MAX_PWM);
    ledcWrite(VIBRATOR_PIN, pwm);
  }

  // ── 6. Render ──────────────────────────────────────────────────

  FastLED.clear();

  for (int col = 0; col < WIDTH; col++) {
    int barH = (int)(colHeight[col] + 0.5f);
    for (int row = 0; row < barH && row < HEIGHT; row++) {
      leds[XY(col, row)] = barColor;
    }
  }

  FastLED.show();

  delay(30);
}

void resetVisualizer() {
  visualizerStarted = false;
  for (int col = 0; col < WIDTH; col++) colHeight[col] = 0;
  bassAverage = 0.0f;
  vibLevel    = 0.0f;
  lastBeatMs  = 0;
  ledcWrite(VIBRATOR_PIN, 0);
}

bool setVisualizerColor(uint8_t index) {
  if (index >= VIZ_COLOR_COUNT) return false;

  currentColorIndex = index;
  barColor = vizColorPalette[index];

  FastLED.clear();
  for (int i = 0; i < NUM_LEDS; i++) leds[i] = barColor;
  FastLED.show();
  delay(1000);
  FastLED.clear();
  FastLED.show();

  return true;
}

uint8_t getVisualizerColorIndex() {
  return currentColorIndex;
}
