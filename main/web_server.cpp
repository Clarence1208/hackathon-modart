#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>

#include "web_server.h"
#include "visualizer.h"
#include "pong.h"

// ── Globals ─────────────────────────────────────────────────────────

WebServer server(80);
WebSocketsServer wsServer(81);

// Map WebSocket client number -> assigned player (1 or 2), 0 = unassigned
static uint8_t wsPlayerSlot[4] = {0, 0, 0, 0};
#define WS_MAX_CLIENTS 4

// ── Helpers ─────────────────────────────────────────────────────────

static uint8_t hexVal(char c) {
  if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(10 + c - 'a');
  if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(10 + c - 'A');
  return 0;
}

static int parseJsonInt(const String& json, const char* key) {
  String needle = "\"";
  needle += key;
  needle += "\"";
  int pos = json.indexOf(needle);
  if (pos < 0) return -1;
  pos = json.indexOf(':', pos);
  if (pos < 0) return -1;
  return json.substring(pos + 1).toInt();
}

// ── WebSocket helpers ───────────────────────────────────────────────

static const char* parseJsonString(const String& json, const char* key, char* buf, size_t bufLen) {
  String needle = "\"";
  needle += key;
  needle += "\"";
  int pos = json.indexOf(needle);
  if (pos < 0) return nullptr;
  pos = json.indexOf('"', json.indexOf(':', pos) + 1);
  if (pos < 0) return nullptr;
  pos++;
  int end = json.indexOf('"', pos);
  if (end < 0) return nullptr;
  size_t len = static_cast<size_t>(end - pos);
  if (len >= bufLen) len = bufLen - 1;
  json.substring(pos, pos + len).toCharArray(buf, bufLen);
  return buf;
}

void broadcastPongState() {
  String msg = "{\"type\":\"state\",\"status\":\"";
  switch (pong.status) {
    case PONG_WAITING:  msg += "waiting";  break;
    case PONG_PLAYING:  msg += "playing";  break;
    case PONG_SCORED:   msg += "scored";   break;
    case PONG_GAMEOVER: msg += "gameover"; break;
  }
  msg += "\",\"score\":[";
  msg += pong.score1;
  msg += ",";
  msg += pong.score2;
  msg += "],\"winner\":";
  msg += pong.winner;
  msg += "}";
  wsServer.broadcastTXT(msg);
}

static void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      Serial.printf("[WS] Client %u connected\n", num);
      if (animSource != ANIM_PONG) {
        wsServer.sendTXT(num, "{\"type\":\"error\",\"message\":\"pong mode not active\"}");
        return;
      }
      uint8_t player = pongAddPlayer();
      if (player == 0) {
        wsServer.sendTXT(num, "{\"type\":\"error\",\"message\":\"game full\"}");
        return;
      }
      if (num < WS_MAX_CLIENTS) {
        wsPlayerSlot[num] = player;
      }
      Serial.printf("[WS] Assigned client %u -> player %u (total: %u)\n",
                    num, player, pong.playerCount);
      String assign = "{\"type\":\"assign\",\"player\":";
      assign += player;
      assign += "}";
      wsServer.sendTXT(num, assign);
      broadcastPongState();
      break;
    }
    case WStype_DISCONNECTED: {
      Serial.printf("[WS] Client %u disconnected\n", num);
      if (num < WS_MAX_CLIENTS && wsPlayerSlot[num] != 0) {
        pongRemovePlayer(wsPlayerSlot[num]);
        wsPlayerSlot[num] = 0;
        broadcastPongState();
      }
      break;
    }
    case WStype_TEXT: {
      if (num >= WS_MAX_CLIENTS || wsPlayerSlot[num] == 0) {
        Serial.printf("[WS] Ignoring text from client %u (slot=%u)\n", num,
                      num < WS_MAX_CLIENTS ? wsPlayerSlot[num] : 0);
        return;
      }
      String json = String((char*)payload);
      char actionBuf[16];
      if (parseJsonString(json, "action", actionBuf, sizeof(actionBuf))) {
        Serial.printf("[WS] Player %u: %s\n", wsPlayerSlot[num], actionBuf);
        handlePongInput(wsPlayerSlot[num], actionBuf);
      }
      break;
    }
    default:
      break;
  }
}

void loopWebSocket() {
  wsServer.loop();
}

// ── HTTP: upload animation (JSON) ───────────────────────────────────
// POST /animation
// Body: {"frameCount": N, "fps": F, "data": "RRGGBBRRGGBB..."}
// "data" is a flat hex string — all frames concatenated, column-major
// (x outer, y inner), 6 hex chars per pixel.

static void handleAnimationPost() {
  server.sendHeader("Access-Control-Allow-Origin", "*");

  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"missing body\"}");
    return;
  }

  const String& body = server.arg("plain");

  int fc = parseJsonInt(body, "frameCount");
  int fps = parseJsonInt(body, "fps");
  if (fc <= 0 || fps <= 0) {
    server.send(400, "application/json",
                "{\"error\":\"invalid frameCount or fps\"}");
    return;
  }

  int delayMs = 1000 / fps;

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

  size_t totalPixels = static_cast<size_t>(fc) * WIDTH * HEIGHT;
  size_t expectedHexLen = totalPixels * 6;

  int dataEnd = body.indexOf('"', dataPos);
  if (dataEnd < 0 || static_cast<size_t>(dataEnd - dataPos) < expectedHexLen) {
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
  hdr[0] = static_cast<uint8_t>(fc & 0xFF);
  hdr[1] = static_cast<uint8_t>((fc >> 8) & 0xFF);
  hdr[2] = static_cast<uint8_t>(delayMs & 0xFF);
  hdr[3] = static_cast<uint8_t>((delayMs >> 8) & 0xFF);
  f.write(hdr, ANIM_HDR_SIZE);

  static uint8_t buf[FRAME_BYTES];
  size_t bufPos = 0;
  size_t hexIdx = static_cast<size_t>(dataPos);

  for (size_t p = 0; p < totalPixels; p++) {
    buf[bufPos++] = static_cast<uint8_t>((hexVal(body[hexIdx])     << 4) | hexVal(body[hexIdx + 1]));
    buf[bufPos++] = static_cast<uint8_t>((hexVal(body[hexIdx + 2]) << 4) | hexVal(body[hexIdx + 3]));
    buf[bufPos++] = static_cast<uint8_t>((hexVal(body[hexIdx + 4]) << 4) | hexVal(body[hexIdx + 5]));
    hexIdx += 6;

    if (bufPos >= FRAME_BYTES) {
      f.write(buf, bufPos);
      bufPos = 0;
      yield();
    }
  }
  if (bufPos > 0) {
    f.write(buf, bufPos);
  }
  f.close();

  Serial.printf("JSON upload: %d frames, %d ms\n", fc, delayMs);

  if (loadLfsHeader()) {
    animSource = ANIM_LITTLEFS;
    currentFrame = 0;

    String resp = "{\"status\":\"ok\",\"frames\":";
    resp += lfsFrameCount;
    resp += ",\"fps\":";
    resp += (lfsFrameDelay > 0 ? 1000 / lfsFrameDelay : 0);
    resp += "}";
    server.send(200, "application/json", resp);
  } else {
    server.send(400, "application/json",
                "{\"status\":\"error\",\"message\":\"Save failed\"}");
  }
}

// ── HTTP: status ────────────────────────────────────────────────────

static void handleStatus() {
  server.sendHeader("Access-Control-Allow-Origin", "*");

  String json = "{\"source\":\"";
  if (animSource == ANIM_PONG) {
    json += "pong\",\"pong_status\":\"";
    switch (pong.status) {
      case PONG_WAITING:  json += "waiting";  break;
      case PONG_PLAYING:  json += "playing";  break;
      case PONG_SCORED:   json += "scored";   break;
      case PONG_GAMEOVER: json += "gameover"; break;
    }
    json += "\",\"pong_score\":[";
    json += pong.score1;
    json += ",";
    json += pong.score2;
    json += "],\"pong_players\":";
    json += pong.playerCount;
  } else if (animSource == ANIM_STATIC) {
    json += "static\"";
  } else if (animSource == ANIM_VISUALIZER) {
    json += "visualizer\"";
  } else if (animSource == ANIM_LITTLEFS) {
    json += "custom\",\"frames\":";
    json += lfsFrameCount;
    json += ",\"fps\":";
    json += (lfsFrameDelay > 0 ? 1000 / lfsFrameDelay : 0);
  } else {
    json += "builtin\",\"name\":\"";
    json += builtins[builtinIndex].name;
    json += "\",\"frames\":";
    json += builtins[builtinIndex].frameCount;
    json += ",\"fps\":6";
  }
  json += ",\"brightness\":";
  json += FastLED.getBrightness();
  json += ",\"vizColor\":";
  json += getVisualizerColorIndex();
  json += ",\"vizColorName\":\"";
  json += vizColorNames[getVisualizerColorIndex()];
  json += "\",\"width\":";
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

static void handleBuiltin() {
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
    if (pos >= 0) {
      idx = body.substring(pos + 1).toInt();
    }
  }

  if (idx < 0 || idx >= BUILTIN_COUNT) {
    server.send(400, "application/json", "{\"error\":\"invalid index\"}");
    return;
  }

  builtinIndex = static_cast<uint8_t>(idx);
  animSource = ANIM_BUILTIN;
  currentFrame = 0;

  String json = "{\"status\":\"ok\",\"name\":\"";
  json += builtins[builtinIndex].name;
  json += "\"}";
  server.send(200, "application/json", json);
}

// ── HTTP: delete saved animation ────────────────────────────────────

static void handleDeleteAnimation() {
  server.sendHeader("Access-Control-Allow-Origin", "*");

  LittleFS.remove(ANIM_FILE);
  lfsFrameCount = 0;
  animSource = ANIM_BUILTIN;
  currentFrame = 0;

  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

// ── HTTP: toggle sound visualizer ────────────────────────────────────

static void handleVisualizer() {
  server.sendHeader("Access-Control-Allow-Origin", "*");

  if (animSource == ANIM_VISUALIZER) {
    resetVisualizer();
    animSource = ANIM_BUILTIN;
    currentFrame = 0;
    Serial.println("[WEB] Visualizer OFF");
    server.send(200, "application/json",
                "{\"status\":\"ok\",\"visualizer\":false}");
  } else {
    animSource = ANIM_VISUALIZER;
    currentFrame = 0;
    Serial.println("[WEB] Visualizer ON");
    server.send(200, "application/json",
                "{\"status\":\"ok\",\"visualizer\":true}");
  }
}

// ── HTTP: get/set global brightness ─────────────────────────────────
// GET  /brightness              → current brightness
// POST /brightness   Body: {"value": 0..255}

static void handleGetBrightness() {
  server.sendHeader("Access-Control-Allow-Origin", "*");

  String json = "{\"brightness\":";
  json += FastLED.getBrightness();
  json += "}";
  server.send(200, "application/json", json);
}

static void handleSetBrightness() {
  server.sendHeader("Access-Control-Allow-Origin", "*");

  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"missing body\"}");
    return;
  }

  int val = parseJsonInt(server.arg("plain"), "value");
  if (val < 0 || val > 255) {
    server.send(400, "application/json", "{\"error\":\"value must be 0-255\"}");
    return;
  }

  FastLED.setBrightness(static_cast<uint8_t>(val));
  FastLED.show();

  Serial.printf("[WEB] Brightness set to %d\n", val);

  String json = "{\"status\":\"ok\",\"brightness\":";
  json += val;
  json += "}";
  server.send(200, "application/json", json);
}

// ── HTTP: get/set visualizer color ──────────────────────────────────
// GET  /color              → current color index & name
// POST /color   Body: {"index": 0..4}

static void handleGetColor() {
  server.sendHeader("Access-Control-Allow-Origin", "*");

  uint8_t idx = getVisualizerColorIndex();
  String json = "{\"index\":";
  json += idx;
  json += ",\"color\":\"";
  json += vizColorNames[idx];
  json += "\"}";
  server.send(200, "application/json", json);
}

static void handleSetColor() {
  server.sendHeader("Access-Control-Allow-Origin", "*");

  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"missing body\"}");
    return;
  }

  int idx = parseJsonInt(server.arg("plain"), "index");
  if (idx < 0 || idx >= VIZ_COLOR_COUNT) {
    server.send(400, "application/json", "{\"error\":\"index must be 0-4\"}");
    return;
  }

  if (!setVisualizerColor(static_cast<uint8_t>(idx))) {
    server.send(500, "application/json", "{\"error\":\"failed to set color\"}");
    return;
  }

  Serial.printf("[WEB] Visualizer color set to %s\n", vizColorNames[idx]);

  String json = "{\"status\":\"ok\",\"color\":\"";
  json += vizColorNames[idx];
  json += "\",\"index\":";
  json += idx;
  json += "}";
  server.send(200, "application/json", json);
}

// ── HTTP: static display (no animation, no file save) ───────────────
// POST /static
// Body: {"data": "RRGGBBRRGGBB..."}
// "data" is a flat hex string — one frame, column-major
// (x outer, y inner), 6 hex chars per pixel.

static void handleStaticPost() {
  server.sendHeader("Access-Control-Allow-Origin", "*");

  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"missing body\"}");
    return;
  }

  const String& body = server.arg("plain");

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

  size_t expectedHexLen = static_cast<size_t>(WIDTH) * HEIGHT * 6;

  int dataEnd = body.indexOf('"', dataPos);
  if (dataEnd < 0 || static_cast<size_t>(dataEnd - dataPos) < expectedHexLen) {
    server.send(400, "application/json", "{\"error\":\"data too short\"}");
    return;
  }

  size_t hexIdx = static_cast<size_t>(dataPos);
  for (uint8_t x = 0; x < WIDTH; x++) {
    for (uint8_t y = 0; y < HEIGHT; y++) {
      uint8_t r = static_cast<uint8_t>((hexVal(body[hexIdx])     << 4) | hexVal(body[hexIdx + 1]));
      uint8_t g = static_cast<uint8_t>((hexVal(body[hexIdx + 2]) << 4) | hexVal(body[hexIdx + 3]));
      uint8_t b = static_cast<uint8_t>((hexVal(body[hexIdx + 4]) << 4) | hexVal(body[hexIdx + 5]));
      leds[XY(x, y)] = CRGB(r, g, b);
      hexIdx += 6;
    }
  }

  animSource = ANIM_STATIC;
  FastLED.show();

  Serial.println("[WEB] Static frame displayed");
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

// ── HTTP: toggle Pong mode ───────────────────────────────────────────

static void handlePong() {
  server.sendHeader("Access-Control-Allow-Origin", "*");

  if (animSource == ANIM_PONG) {
    for (uint8_t i = 0; i < WS_MAX_CLIENTS; i++) {
      if (wsPlayerSlot[i] != 0) {
        pongRemovePlayer(wsPlayerSlot[i]);
        wsPlayerSlot[i] = 0;
      }
    }
    animSource = ANIM_BUILTIN;
    currentFrame = 0;
    Serial.println("[WEB] Pong OFF");
    server.send(200, "application/json",
                "{\"status\":\"ok\",\"pong\":false}");
  } else {
    animSource = ANIM_PONG;
    initPong();
    currentFrame = 0;
    Serial.println("[WEB] Pong ON — waiting for players on ws://port 81");
    server.send(200, "application/json",
                "{\"status\":\"ok\",\"pong\":true}");
  }
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
  <h3>Brightness</h3>
  <input type="range" id="bright" min="0" max="255" value="80" style="width:100%">
  <span id="brightVal">80</span>
  <button onclick="setBright()">Set</button>
</div>
<div class="card">
  <h3>Sound Visualizer</h3>
  <button onclick="toggleViz()">Toggle Visualizer</button>
  <div style="margin-top:8px"><b>Color:</b></div>
  <button style="background:#0078FF" onclick="setColor(0)">Ocean Blue</button>
  <button style="background:#FF0064" onclick="setColor(1)">Hot Pink</button>
  <button style="background:#00E676" onclick="setColor(2)">Emerald</button>
  <button style="background:#A000FF" onclick="setColor(3)">Purple</button>
  <button style="background:#FFAA00" onclick="setColor(4)">Amber</button>
</div>
<div class="card">
  <h3>Built-in Animations</h3>
  <div id="builtins"></div>
</div>
<script>
function loadStatus(){
  fetch('/status').then(r=>r.json()).then(d=>{
    document.getElementById('status').textContent=JSON.stringify(d,null,2);
    if(d.brightness!==undefined){
      document.getElementById('bright').value=d.brightness;
      document.getElementById('brightVal').textContent=d.brightness;
    }
    let h='';
    (d.builtins||[]).forEach(b=>{
      h+='<button onclick="switchBuiltin('+b.index+')">'+b.name+'</button> ';
    });
    document.getElementById('builtins').innerHTML=h;
  });
}
document.getElementById('bright').addEventListener('input',function(){
  document.getElementById('brightVal').textContent=this.value;
});
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
function toggleViz(){
  fetch('/visualizer',{method:'POST'}).then(()=>loadStatus());
}
function setBright(){
  var v=document.getElementById('bright').value;
  fetch('/brightness',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({value:parseInt(v)})}).then(()=>loadStatus());
}
function setColor(i){
  fetch('/color',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({index:i})}).then(()=>loadStatus());
}
loadStatus();
</script></body></html>
)rawliteral";

static void handleRoot() {
  server.send(200, "text/html", FPSTR(INDEX_HTML));
}

// ── CORS preflight ──────────────────────────────────────────────────

static void sendCorsOk() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(204);
}

// ── Setup ───────────────────────────────────────────────────────────

void setupWebServer() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char ssid[10] = "Resonance";
  //snprintf(ssid, sizeof(ssid), "Resonance");
  WiFi.softAP(ssid);
  Serial.printf("AP: %s  IP: %s\n", ssid, WiFi.softAPIP().toString().c_str());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/animation", HTTP_POST, handleAnimationPost);
  server.on("/animation", HTTP_DELETE, handleDeleteAnimation);
  server.on("/animation", HTTP_OPTIONS, sendCorsOk);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/builtin", HTTP_POST, handleBuiltin);
  server.on("/builtin", HTTP_OPTIONS, sendCorsOk);
  server.on("/visualizer", HTTP_POST, handleVisualizer);
  server.on("/visualizer", HTTP_OPTIONS, sendCorsOk);
  server.on("/color", HTTP_GET, handleGetColor);
  server.on("/color", HTTP_POST, handleSetColor);
  server.on("/color", HTTP_OPTIONS, sendCorsOk);
  server.on("/brightness", HTTP_GET, handleGetBrightness);
  server.on("/brightness", HTTP_POST, handleSetBrightness);
  server.on("/brightness", HTTP_OPTIONS, sendCorsOk);
  server.on("/static", HTTP_POST, handleStaticPost);
  server.on("/static", HTTP_OPTIONS, sendCorsOk);
  server.on("/pong", HTTP_POST, handlePong);
  server.on("/pong", HTTP_OPTIONS, sendCorsOk);

  server.begin();
  Serial.println("HTTP server ready");

  wsServer.begin();
  wsServer.onEvent(onWebSocketEvent);
  Serial.println("WebSocket server ready on port 81");
}

