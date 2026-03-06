// High-level entry point. Detailed animation and web server
// logic live in separate modules for clarity.

#include <Arduino.h>
#include <FastLED.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>

#include "animation.h"
#include "visualizer.h"
#include "ncs_ring.h"
#include "web_server.h"
#include "pong.h"

// ── Setup ───────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(500);

  initAnimation();
  initVisualizer();
  initNcsRing();
  initPong();
  setupWebServer();
}

// ── Loop (non-blocking) ────────────────────────────────────────────

static unsigned long lastPongBroadcast = 0;

void loop() {
  server.handleClient();
  loopWebSocket();

  if (animSource == ANIM_PONG) {
    updatePong();
    renderPong();
    // Broadcast state to Flutter clients at ~10 Hz
    if (millis() - lastPongBroadcast >= 100) {
      lastPongBroadcast = millis();
      broadcastPongState();
    }
  } else if (animSource == ANIM_VISUALIZER) {
    runVisualizer();
  } else if (animSource == ANIM_NCS_RING) {
    runNcsRing();
  } else if (millis() - lastFrameTime >= activeFrameDelay) {
    lastFrameTime = millis();
    showNextFrame();
  }
}
