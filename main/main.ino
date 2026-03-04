#include <FastLED.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>

#include "logo_base.h"
#include "logo_base_animated.h"
#include "plasma_v2.h"
#include "logo_anim_v4.h"

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

// ── Globals ─────────────────────────────────────────────────────────

CRGB leds[NUM_LEDS];
WebServer server(80);
bool fsReady = false;

// Built-in PROGMEM animation registry
struct BuiltinAnim {
  const char* name;
  const uint32_t (*frames)[WIDTH][HEIGHT];
  uint16_t frameCount;
};

static const BuiltinAnim builtins[] = {
  { "logo_anim_v4", logo_anim_v4, FRAME_COUNT_LOGO_ANIM_V4 },
  { "plasma_v2",    plasma_v2,    FRAME_COUNT_PLASMA_V2 },
  { "logo_base",    logo_base,    FRAME_COUNT_LOGO_BASE },
};
static const uint8_t BUILTIN_COUNT = sizeof(builtins) / sizeof(builtins[0]);

// Playback state
enum AnimSource : uint8_t { ANIM_BUILTIN, ANIM_LITTLEFS };

AnimSource    animSource       = ANIM_BUILTIN;
uint8_t       builtinIndex     = 0;            // default: logo_anim_v4
uint16_t      lfsFrameCount    = 0;
uint16_t      lfsFrameDelay    = 150;
uint16_t      currentFrame     = 0;
uint16_t      activeFrameDelay = 150;
unsigned long lastFrameTime    = 0;

// ── Hex decoding helper ─────────────────────────────────────────────

uint8_t hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + c - 'a';
  if (c >= 'A' && c <= 'F') return 10 + c - 'A';
  return 0;
}

int parseJsonInt(const String& json, const char* key) {
  String needle = "\"";
  needle += key;
  needle += "\"";
  int pos = json.indexOf(needle);
  if (pos < 0) return -1;
  pos = json.indexOf(':', pos);
  if (pos < 0) return -1;
  return json.substring(pos + 1).toInt();
}

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

void displayProgmemFrame(const uint32_t matrix[WIDTH][HEIGHT]) {
  for (uint8_t x = 0; x < WIDTH; x++) {
    for (uint8_t y = 0; y < HEIGHT; y++) {
      uint32_t color = pgm_read_dword(&matrix[x][y]);
      leds[XY(x, y)] = CRGB((color >> 16) & 0xFF,
                             (color >> 8)  & 0xFF,
                              color        & 0xFF);
    }
  }
}

void displayRgbFrame(const uint8_t* rgb) {
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
    if (f) f.close();
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

// ── HTTP: upload animation (JSON) ───────────────────────────────────
// POST /animation
// Body: {"frameCount": N, "delay": M, "data": "RRGGBBRRGGBB..."}
// "data" is a flat hex string — all frames concatenated, column-major
// (x outer, y inner), 6 hex chars per pixel.

void handleAnimationPost() {
  server.sendHeader("Access-Control-Allow-Origin", "*");

  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"missing body\"}");
    return;
  }

  const String& body = server.arg("plain");

  int fc = parseJsonInt(body, "frameCount");
  int delayMs = parseJsonInt(body, "delay");
  if (fc <= 0 || delayMs <= 0) {
    server.send(400, "application/json",
                "{\"error\":\"invalid frameCount or delay\"}");
    return;
  }

  int dataPos = body.indexOf("\"data\"");
  if (dataPos < 0) {
    server.send(400, "application/json", "{\"error\":\"missing data field\"}");
    return;
  }
  dataPos = body.indexOf('"', body.indexOf(':', dataPos) + 1);
  if (dataPos < 0) {
    server.send(400, "application/json", "{\"error\":\"malformed data field\"}");
    return;
  }
  dataPos += 1;

  size_t totalPixels = (size_t)fc * WIDTH * HEIGHT;
  size_t expectedHexLen = totalPixels * 6;

  int dataEnd = body.indexOf('"', dataPos);
  if (dataEnd < 0 || (size_t)(dataEnd - dataPos) < expectedHexLen) {
    server.send(400, "application/json", "{\"error\":\"data too short\"}");
    return;
  }

  Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());

  if (!fsReady) {
    server.send(500, "application/json",
                "{\"error\":\"LittleFS not mounted — select partition scheme with spiffs in Arduino IDE\"}");
    return;
  }

  File f = LittleFS.open(ANIM_FILE, "w");
  if (!f) {
    server.send(500, "application/json", "{\"error\":\"filesystem write failed\"}");
    return;
  }

  uint8_t hdr[ANIM_HDR_SIZE];
  hdr[0] = fc & 0xFF;
  hdr[1] = (fc >> 8) & 0xFF;
  hdr[2] = delayMs & 0xFF;
  hdr[3] = (delayMs >> 8) & 0xFF;
  f.write(hdr, ANIM_HDR_SIZE);

  static uint8_t buf[FRAME_BYTES];
  size_t bufPos = 0;
  size_t hexIdx = (size_t)dataPos;

  for (size_t p = 0; p < totalPixels; p++) {
    buf[bufPos++] = (hexVal(body[hexIdx])     << 4) | hexVal(body[hexIdx + 1]);
    buf[bufPos++] = (hexVal(body[hexIdx + 2]) << 4) | hexVal(body[hexIdx + 3]);
    buf[bufPos++] = (hexVal(body[hexIdx + 4]) << 4) | hexVal(body[hexIdx + 5]);
    hexIdx += 6;

    if (bufPos >= FRAME_BYTES) {
      f.write(buf, bufPos);
      bufPos = 0;
      yield();
    }
  }
  if (bufPos > 0) f.write(buf, bufPos);
  f.close();

  Serial.printf("JSON upload: %d frames, %d ms\n", fc, delayMs);

  if (loadLfsHeader()) {
    animSource = ANIM_LITTLEFS;
    currentFrame = 0;

    String resp = "{\"status\":\"ok\",\"frames\":";
    resp += lfsFrameCount;
    resp += ",\"delay\":";
    resp += lfsFrameDelay;
    resp += "}";
    server.send(200, "application/json", resp);
  } else {
    server.send(400, "application/json",
                "{\"status\":\"error\",\"message\":\"Save failed\"}");
  }
}

// ── HTTP: status ────────────────────────────────────────────────────

void handleStatus() {
  server.sendHeader("Access-Control-Allow-Origin", "*");

  String json = "{\"source\":\"";
  if (animSource == ANIM_LITTLEFS) {
    json += "custom\",\"frames\":";
    json += lfsFrameCount;
    json += ",\"delay\":";
    json += lfsFrameDelay;
  } else {
    json += "builtin\",\"name\":\"";
    json += builtins[builtinIndex].name;
    json += "\",\"frames\":";
    json += builtins[builtinIndex].frameCount;
    json += ",\"delay\":150";
  }
  json += ",\"width\":";
  json += WIDTH;
  json += ",\"height\":";
  json += HEIGHT;
  json += ",\"builtins\":[";
  for (uint8_t i = 0; i < BUILTIN_COUNT; i++) {
    if (i) json += ",";
    json += "{\"index\":";
    json += i;
    json += ",\"name\":\"";
    json += builtins[i].name;
    json += "\",\"frames\":";
    json += builtins[i].frameCount;
    json += "}";
  }
  json += "]}";

  server.send(200, "application/json", json);
}

// ── HTTP: switch to built-in animation ──────────────────────────────

void handleBuiltin() {
  server.sendHeader("Access-Control-Allow-Origin", "*");

  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"missing body\"}");
    return;
  }

  String body = server.arg("plain");
  int idx = -1;
  int pos = body.indexOf("\"index\"");
  if (pos >= 0) {
    pos = body.indexOf(':', pos);
    if (pos >= 0) idx = body.substring(pos + 1).toInt();
  }

  if (idx < 0 || idx >= BUILTIN_COUNT) {
    server.send(400, "application/json", "{\"error\":\"invalid index\"}");
    return;
  }

  builtinIndex = (uint8_t)idx;
  animSource = ANIM_BUILTIN;
  currentFrame = 0;

  String json = "{\"status\":\"ok\",\"name\":\"";
  json += builtins[builtinIndex].name;
  json += "\"}";
  server.send(200, "application/json", json);
}

// ── HTTP: delete saved animation ────────────────────────────────────

void handleDeleteAnimation() {
  server.sendHeader("Access-Control-Allow-Origin", "*");

  LittleFS.remove(ANIM_FILE);
  lfsFrameCount = 0;
  animSource = ANIM_BUILTIN;
  currentFrame = 0;

  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

// ── HTTP: test page ─────────────────────────────────────────────────

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
  body{font-family:sans-serif;max-width:480px;margin:40px auto;padding:0 16px}
  h2{color:#333}
  .card{background:#f5f5f5;border-radius:8px;padding:16px;margin:12px 0}
  button{background:#2196F3;color:#fff;border:none;padding:10px 20px;
         border-radius:4px;cursor:pointer;margin:4px}
  button:hover{background:#1976D2}
  input[type=file]{margin:8px 0}
  #status,#uploadResult{white-space:pre-wrap;font-size:13px}
</style></head><body>
<h2>ModArt LED Controller</h2>
<div class="card">
  <h3>Upload Animation (JSON)</h3>
  <input type="file" id="animFile" accept=".json"><br>
  <button onclick="uploadAnim()">Upload</button>
  <pre id="uploadResult"></pre>
</div>
<div class="card">
  <h3>Status</h3>
  <pre id="status">loading...</pre>
  <button onclick="loadStatus()">Refresh</button>
  <button onclick="deleteAnim()">Delete Custom</button>
</div>
<div class="card">
  <h3>Built-in Animations</h3>
  <div id="builtins"></div>
</div>
<script>
function loadStatus(){
  fetch('/status').then(r=>r.json()).then(d=>{
    document.getElementById('status').textContent=JSON.stringify(d,null,2);
    let h='';
    (d.builtins||[]).forEach(b=>{
      h+='<button onclick="switchBuiltin('+b.index+')">'+b.name+'</button> ';
    });
    document.getElementById('builtins').innerHTML=h;
  });
}
function uploadAnim(){
  var f=document.getElementById('animFile').files[0];
  if(!f){document.getElementById('uploadResult').textContent='Pick a .json file first';return;}
  var r=new FileReader();
  r.onload=function(e){
    document.getElementById('uploadResult').textContent='Uploading...';
    fetch('/animation',{method:'POST',
      headers:{'Content-Type':'application/json'},
      body:e.target.result
    }).then(r=>r.json()).then(d=>{
      document.getElementById('uploadResult').textContent=JSON.stringify(d,null,2);
      loadStatus();
    }).catch(err=>{
      document.getElementById('uploadResult').textContent='Error: '+err;
    });
  };
  r.readAsText(f);
}
function switchBuiltin(i){
  fetch('/builtin',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({index:i})}).then(()=>loadStatus());
}
function deleteAnim(){
  fetch('/animation',{method:'DELETE'}).then(()=>loadStatus());
}
loadStatus();
</script></body></html>
)rawliteral";

void handleRoot() {
  server.send(200, "text/html", FPSTR(INDEX_HTML));
}

// ── CORS preflight ──────────────────────────────────────────────────

void sendCorsOk() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(204);
}

// ── Setup ───────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(500);

  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear();
  FastLED.show();

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed — check partition scheme (need Default with spiffs)");
  } else {
    fsReady = true;
    Serial.printf("LittleFS OK — total: %u, used: %u\n", LittleFS.totalBytes(), LittleFS.usedBytes());
  }

  if (loadLfsHeader()) {
    animSource = ANIM_LITTLEFS;
    Serial.printf("Saved animation: %u frames, %u ms\n", lfsFrameCount, lfsFrameDelay);
  } else {
    Serial.println("No saved animation — using builtin");
  }

  uint8_t mac[6];
  WiFi.macAddress(mac);
  char ssid[16];
  snprintf(ssid, sizeof(ssid), "ModArt-%02X%02X", mac[4], mac[5]);
  WiFi.softAP(ssid);
  Serial.printf("AP: %s  IP: %s\n", ssid, WiFi.softAPIP().toString().c_str());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/animation", HTTP_POST, handleAnimationPost);
  server.on("/animation", HTTP_DELETE, handleDeleteAnimation);
  server.on("/animation", HTTP_OPTIONS, sendCorsOk);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/builtin", HTTP_POST, handleBuiltin);
  server.on("/builtin", HTTP_OPTIONS, sendCorsOk);

  server.begin();
  Serial.println("HTTP server ready");
}

// ── Loop (non-blocking) ────────────────────────────────────────────

void loop() {
  server.handleClient();

  if (millis() - lastFrameTime >= activeFrameDelay) {
    lastFrameTime = millis();
    showNextFrame();
  }
}
