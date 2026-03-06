#include "arduinoFFT.h"
#include <FastLED.h>
#include <math.h>

#include "ncs_ring.h"
#include "animation.h"
#include "visualizer.h"
#include "logo_base.h"

// ── Ring geometry ───────────────────────────────────────────────────
// The outer ring of the logo_base has 32 pixels traced clockwise,
// centered at (15.5, 6.5) with radius ~5.5.

static const float CX = 15.5f;
static const float CY = 6.5f;

#define RING_COUNT   32
#define NUM_BANDS    16
#define MAX_BAR_LEN  8

// Outer ring pixel x-coordinates, clockwise from top-left of top edge
static const int8_t ringX[RING_COUNT] = {
    13, 14, 15, 16, 17, 18,       // top edge       (y=1)
    19, 20,                         // upper-right diagonal
    21, 21, 21, 21, 21, 21,       // right edge      (x=21)
    20, 19,                         // lower-right diagonal
    18, 17, 16, 15, 14, 13,       // bottom edge     (y=12)
    12, 11,                         // lower-left diagonal
    10, 10, 10, 10, 10, 10,       // left edge       (x=10)
    11, 12                          // upper-left diagonal
};

static const int8_t ringY[RING_COUNT] = {
     1,  1,  1,  1,  1,  1,       // top edge
     2,  3,                         // upper-right
     4,  5,  6,  7,  8,  9,       // right edge
    10, 11,                         // lower-right
    12, 12, 12, 12, 12, 12,       // bottom edge
    11, 10,                         // lower-left
     9,  8,  7,  6,  5,  4,       // left edge
     3,  2                          // upper-left
};

// Precomputed bar extension pixels for each ring position
static int8_t barPX[RING_COUNT][MAX_BAR_LEN];
static int8_t barPY[RING_COUNT][MAX_BAR_LEN];
static uint8_t barMaxLen[RING_COUNT];

// ── FFT buffers ────────────────────────────────────────────────────

static double nReal[FFT_SAMPLES];
static double nImag[FFT_SAMPLES];

static ArduinoFFT<double> nFFT =
    ArduinoFFT<double>(nReal, nImag, FFT_SAMPLES, SAMPLING_FREQ);

// ── Frequency band state ───────────────────────────────────────────

static uint16_t bandLo[NUM_BANDS];
static uint16_t bandHi[NUM_BANDS];
static float    bandLevel[NUM_BANDS];   // normalized 0..1

// ── Tunables ────────────────────────────────────────────────────────

static const float NCS_NOISE_FLOOR = 1500.0f;
static const float NCS_SENSITIVITY = 18000.0f;
static const float ATTACK_SMOOTH   = 0.70f;    // fast attack
static const float DECAY_SMOOTH    = 0.75f;    // slow decay

// ── Haptic / beat state ────────────────────────────────────────────

static const uint16_t BASS_START   = 2;
static const uint16_t BASS_END     = 7;
static const float    BASS_FLOOR   = 800.0f;
static const float    BASS_SENS    = 8000.0f;
static const float    BEAT_THRESH  = 1.3f;
static const float    BEAT_AVG_DEC = 0.80f;
static const float    BEAT_MIN_LVL = 0.12f;
static const uint32_t BEAT_COOL_MS = 60;
static const float    VIB_DEC      = 0.30f;

static float    bassAvg   = 0.0f;
static float    vibLev    = 0.0f;
static uint32_t lastBt    = 0;
static float    glowLevel = 0.0f;   // beat flash on ring

static bool ncsStarted = false;

// ── Initialization ──────────────────────────────────────────────────

void initNcsRing() {
    // Precompute bar extension pixel coords via DDA ray-cast
    for (int i = 0; i < RING_COUNT; i++) {
        float dx = (float)ringX[i] - CX;
        float dy = (float)ringY[i] - CY;
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 0.001f) len = 1.0f;
        dx /= len;
        dy /= len;

        float px = ringX[i] + 0.5f;
        float py = ringY[i] + 0.5f;
        int prevX = ringX[i];
        int prevY = ringY[i];
        uint8_t count = 0;

        for (int s = 0; s < MAX_BAR_LEN * 5 && count < MAX_BAR_LEN; s++) {
            px += dx * 0.35f;
            py += dy * 0.35f;
            int ix = (int)roundf(px);
            int iy = (int)roundf(py);

            if (ix < 0 || ix >= WIDTH || iy < 0 || iy >= HEIGHT) break;
            if (ix == prevX && iy == prevY) continue;
            if (ix == ringX[i] && iy == ringY[i]) continue;

            barPX[i][count] = (int8_t)ix;
            barPY[i][count] = (int8_t)iy;
            count++;
            prevX = ix;
            prevY = iy;
        }
        barMaxLen[i] = count;
    }

    // Build log-spaced frequency band table for 16 bands
    const int minBin = 3;
    const int maxBin = FFT_SAMPLES / 2 - 1;
    float logMin = logf((float)minBin);
    float logMax = logf((float)(maxBin + 1));

    for (int b = 0; b < NUM_BANDS; b++) {
        float lo = expf(logMin + (logMax - logMin) *  b      / (float)NUM_BANDS);
        float hi = expf(logMin + (logMax - logMin) * (b + 1) / (float)NUM_BANDS);

        bandLo[b] = (uint16_t)constrain((int)lo, minBin, maxBin);
        bandHi[b] = (uint16_t)constrain((int)hi, minBin, maxBin + 1);
        if (bandHi[b] <= bandLo[b]) bandHi[b] = bandLo[b] + 1;

        bandLevel[b] = 0.0f;
    }

    Serial.println("[NCS] initNcsRing done");
}

// ── Main render tick ────────────────────────────────────────────────

void runNcsRing() {
    CRGB vizColor = vizColorPalette[getVisualizerColorIndex()];

    // Flash on first entry so the user knows the mode is active
    if (!ncsStarted) {
        ncsStarted = true;
        Serial.println("[NCS] === NCS RING MODE ACTIVE ===");
        FastLED.clear();
        for (int i = 0; i < NUM_LEDS; i++) leds[i] = vizColor;
        FastLED.show();
        delay(400);
        FastLED.clear();
        FastLED.show();
        delay(200);
    }

    // ── 1. Sample mic ──────────────────────────────────────────────

    unsigned long period = (unsigned long)roundf(1000000.0f / SAMPLING_FREQ);

    for (int i = 0; i < FFT_SAMPLES; i++) {
        unsigned long t0 = micros();
        nReal[i] = analogRead(MIC_PIN);
        nImag[i] = 0;
        while (micros() - t0 < period) { /* busy-wait */ }
    }

    // ── 2. DC removal ──────────────────────────────────────────────

    double mean = 0;
    for (int i = 0; i < FFT_SAMPLES; i++) mean += nReal[i];
    mean /= FFT_SAMPLES;
    for (int i = 0; i < FFT_SAMPLES; i++) nReal[i] -= mean;

    // ── 3. FFT ─────────────────────────────────────────────────────

    nFFT.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
    nFFT.compute(FFT_FORWARD);
    nFFT.complexToMagnitude();

    // ── 4. Map FFT bins → 16 band levels (0..1) ───────────────────

    for (int b = 0; b < NUM_BANDS; b++) {
        double peak = 0;
        for (uint16_t bin = bandLo[b]; bin < bandHi[b]; bin++) {
            if (nReal[bin] > peak) peak = nReal[bin];
        }

        if (peak < NCS_NOISE_FLOOR) peak = 0;

        float target = constrain((float)peak / NCS_SENSITIVITY, 0.0f, 1.0f);

        if (target > bandLevel[b]) {
            bandLevel[b] = bandLevel[b] * (1.0f - ATTACK_SMOOTH)
                         + target * ATTACK_SMOOTH;
        } else {
            bandLevel[b] = bandLevel[b] * DECAY_SMOOTH
                         + target * (1.0f - DECAY_SMOOTH);
        }

        if (bandLevel[b] < 0.04f) bandLevel[b] = 0;
    }

    // ── 5. Beat detection + haptic ──────────────────────────────────

    {
        double bassPeak = 0;
        for (uint16_t bin = BASS_START; bin < BASS_END; bin++) {
            if (nReal[bin] > bassPeak) bassPeak = nReal[bin];
        }

        float bl = 0.0f;
        if (bassPeak > BASS_FLOOR) {
            bl = constrain(
                (float)(bassPeak - BASS_FLOOR) / BASS_SENS, 0.0f, 1.0f);
        }

        bassAvg = bassAvg * BEAT_AVG_DEC + bl * (1.0f - BEAT_AVG_DEC);

        float thresh = max(bassAvg * BEAT_THRESH, BEAT_MIN_LVL);
        uint32_t now = millis();

        if (bl > thresh && (now - lastBt) > BEAT_COOL_MS) {
            vibLev    = 1.0f;
            glowLevel = 1.0f;
            lastBt    = now;
        } else {
            vibLev *= VIB_DEC;
            if (vibLev < 0.02f) vibLev = 0.0f;

            glowLevel *= 0.82f;
            if (glowLevel < 0.04f) glowLevel = 0.0f;
        }

        ledcWrite(VIBRATOR_PIN, (uint8_t)(vibLev * 255.0f));
    }

    // ── 6. Render ──────────────────────────────────────────────────

    FastLED.clear();

    // Average energy for inner-logo pulse
    float avgEnergy = 0;
    for (int b = 0; b < NUM_BANDS; b++) avgEnergy += bandLevel[b];
    avgEnergy /= NUM_BANDS;

    // 6a. Draw full logo base (dimmed, inner pixels pulse with energy)
    for (uint8_t x = 0; x < MATRIX_WIDTH_LOGO_BASE; x++) {
        for (uint8_t y = 0; y < MATRIX_HEIGHT_LOGO_BASE; y++) {
            uint32_t c = pgm_read_dword(&logo_base[0][x][y]);
            if (c == 0) continue;

            CRGB base(
                (uint8_t)((c >> 16) & 0xFF),
                (uint8_t)((c >> 8)  & 0xFF),
                (uint8_t)( c        & 0xFF)
            );

            float dist = sqrtf(
                ((float)x - CX) * ((float)x - CX) +
                ((float)y - CY) * ((float)y - CY)
            );

            if (dist < 4.5f) {
                // Inner logo detail: pulse with overall energy
                uint8_t bright = (uint8_t)(100 + avgEnergy * 100 + glowLevel * 55);
                base.nscale8(bright);
            } else {
                base.fadeToBlackBy(150);
            }

            leds[XY(x, y)] = base;
        }
    }

    // 6b. Draw ring pixels (viz color, brightness from band + beat glow)
    for (int i = 0; i < RING_COUNT; i++) {
        // Mirror: right half (0..15) and left half (16..31) share bands
        int band = (i < NUM_BANDS) ? i : (RING_COUNT - 1 - i);

        float level = bandLevel[band];

        uint8_t ringBright = (uint8_t)(
            80.0f + level * 140.0f + glowLevel * 35.0f
        );

        CRGB rc = vizColor;
        rc.nscale8(ringBright);
        leds[XY(ringX[i], ringY[i])] = rc;

        // 6c. Draw bar extension outward from ring
        if (barMaxLen[i] == 0) continue;

        int barLen = (int)(level * (float)barMaxLen[i] + 0.5f);
        if (barLen > (int)barMaxLen[i]) barLen = (int)barMaxLen[i];

        for (int s = 0; s < barLen; s++) {
            float t = (float)s / (float)barMaxLen[i];
            uint8_t fade = (uint8_t)(255.0f * (1.0f - t * 0.7f));
            if (fade < 40) fade = 40;

            CRGB bc = vizColor;
            bc.nscale8(fade);

            uint16_t idx = XY(barPX[i][s], barPY[i][s]);
            // Additive blend so overlapping bars glow brighter
            CRGB existing = leds[idx];
            leds[idx] = CRGB(
                min(255, (int)existing.r + (int)bc.r),
                min(255, (int)existing.g + (int)bc.g),
                min(255, (int)existing.b + (int)bc.b)
            );
        }
    }

    FastLED.show();
    delay(30);
}

// ── Reset ───────────────────────────────────────────────────────────

void resetNcsRing() {
    ncsStarted = false;
    for (int b = 0; b < NUM_BANDS; b++) bandLevel[b] = 0;
    bassAvg   = 0.0f;
    vibLev    = 0.0f;
    lastBt    = 0;
    glowLevel = 0.0f;
    ledcWrite(VIBRATOR_PIN, 0);
}
