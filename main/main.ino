// High-level entry point. Detailed animation and web server
// logic live in separate modules for clarity.

#include <Arduino.h>
#include <FastLED.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>

#include "animation.h"
#include "web_server.h"

// ── Setup ───────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(500);

  // Initialize LED matrix + filesystem + animation state.
  initAnimation();

  // Start Wi-Fi access point and HTTP API.
  setupWebServer();
}

// ── Loop (non-blocking) ────────────────────────────────────────────

void loop() {
  server.handleClient();

  if (millis() - lastFrameTime >= activeFrameDelay) {
    lastFrameTime = millis();
    showNextFrame();
  }
}
