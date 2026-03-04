#include "arduinoFFT.h"
#include <FastLED.h>

#include "visualizer.h"
#include "animation.h"

// ── Tunables ────────────────────────────────────────────────────────

static const CRGB BAR_COLOR      = CRGB(0, 120, 255);
static const float NOISE_FLOOR   = 1500.0f;   // ADC is noisy — need a high floor to stay dark in silence
static const float SENSITIVITY   = 20000.0f;  // MaxMag peaks around 15k-27k, scale to fill 16 rows
static const float SMOOTH_FACTOR = 0.70f;

// ── Haptic / beat detection tunables ────────────────────────────────
// Bass bins: at 9878 Hz / 256 samples ≈ 38.6 Hz per bin.
// Bins 2-6 cover ~77-231 Hz (kick drum / bass range).
static const uint16_t BASS_BIN_START      = 2;
static const uint16_t BASS_BIN_END        = 7;
static const float    BASS_NOISE_FLOOR    = 2000.0f;
static const float    BASS_BEAT_THRESHOLD = 6000.0f;  // raw magnitude above which we consider it a beat
static const unsigned long PULSE_ON_MS    = 90;       // vibration ON duration per beat
static const unsigned long PULSE_COOLDOWN_MS = 120;   // minimum gap between two pulses
static const uint8_t  VIBRATOR_PWM_ON     = 255;

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

static unsigned long pulseStartMs  = 0;
static bool          pulseActive   = false;

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
    for (int i = 0; i < NUM_LEDS; i++) leds[i] = BAR_COLOR;
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

  // ── 5. Haptic beat pulse (bass peak → short vibration burst) ────

  {
    double bassPeak = 0;
    for (uint16_t bin = BASS_BIN_START; bin < BASS_BIN_END; bin++) {
      if (vReal[bin] > bassPeak) bassPeak = vReal[bin];
    }

    unsigned long now = millis();

    if (!pulseActive && bassPeak > BASS_BEAT_THRESHOLD
        && (now - pulseStartMs) > (PULSE_ON_MS + PULSE_COOLDOWN_MS)) {
      pulseActive  = true;
      pulseStartMs = now;
      ledcWrite(VIBRATOR_PIN, VIBRATOR_PWM_ON);
    }

    if (pulseActive && (now - pulseStartMs) >= PULSE_ON_MS) {
      pulseActive = false;
      ledcWrite(VIBRATOR_PIN, 0);
    }
  }

  // ── 6. Render ──────────────────────────────────────────────────

  FastLED.clear();

  for (int col = 0; col < WIDTH; col++) {
    int barH = (int)(colHeight[col] + 0.5f);
    for (int row = 0; row < barH && row < HEIGHT; row++) {
      leds[XY(col, row)] = BAR_COLOR;
    }
  }

  FastLED.show();

  delay(40);
}

void resetVisualizer() {
  visualizerStarted = false;
  for (int col = 0; col < WIDTH; col++) colHeight[col] = 0;
  pulseActive = false;
  pulseStartMs = 0;
  ledcWrite(VIBRATOR_PIN, 0);
}
