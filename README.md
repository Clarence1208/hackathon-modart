# ModArt LED Controller

An Arduino firmware for the **Adafruit ESP32-S2 Feather** that drives a 32×16 WS2812B LED matrix with audio-reactive visualizers, built-in animations, an interactive Pong game, and a Wi-Fi control API.

---

## Table of Contents

- [Hardware](#hardware)
  - [Bill of Materials](#bill-of-materials)
  - [Pin Assignments](#pin-assignments)
  - [Wiring Guide](#wiring-guide)
- [Features Overview](#features-overview)
- [Modes of Operation](#modes-of-operation)
  - [Built-in Animations](#built-in-animations)
  - [Custom LittleFS Animations](#custom-littlefs-animations)
  - [Sound Visualizer (Bar Mode)](#sound-visualizer-bar-mode)
  - [NCS Ring Visualizer](#ncs-ring-visualizer)
  - [Pong Game](#pong-game)
  - [Static Frame](#static-frame)
- [Web Control API](#web-control-api)
  - [Connecting](#connecting)
  - [HTTP Endpoints](#http-endpoints)
  - [WebSocket (Pong Controls)](#websocket-pong-controls)
- [Animation File Format](#animation-file-format)
  - [JSON Upload Format](#json-upload-format)
  - [Binary Storage Format (LittleFS)](#binary-storage-format-littlefs)
- [Python Utility: `h_to_json.py`](#python-utility-h_to_jsonpy)
- [Haptic Feedback](#haptic-feedback)
- [Library Dependencies](#library-dependencies)
- [Build & Flash Instructions](#build--flash-instructions)
  - [Partition Scheme](#partition-scheme)
  - [Flashing](#flashing)
- [Project Structure](#project-structure)

---

## Hardware

### Bill of Materials

| Component | Description |
|-----------|-------------|
| **Adafruit ESP32-S2 Feather** | Main microcontroller (Wi-Fi, ADC, PWM) |
| **32×16 WS2812B LED matrix** | Addressable RGB LED panel (512 LEDs, two 8×16 panels) |
| **Microphone** | Analog microphone on pin A1 (e.g. MAX4466, MAX9814, or similar electret with amplifier) |
| **Haptic vibrator / motor** | Vibration motor driven by PWM on GPIO 10 |

### Pin Assignments

| Signal | GPIO / Pin | Notes |
|--------|-----------|-------|
| LED matrix data | GPIO 13 | WS2812B signal line |
| Microphone audio | A1 | Analog input for FFT sampling |
| Vibrator PWM | GPIO 10 | 1 kHz PWM, 8-bit resolution (0–255) |
| Wi-Fi | Built-in | Soft Access Point |

### Wiring Guide

```
ESP32-S2 Feather
  │
  ├── GPIO 13 ──────────────────► LED Matrix DIN
  │                                (5 V logic level shifter recommended)
  │
  ├── A1 (ADC) ─────────────────► Microphone OUT / AOUT
  │
  ├── GPIO 10 ──── (transistor) ─► Vibrator motor +
  │
  ├── GND ─────────────────────► LED Matrix GND / Mic GND / Vibrator GND
  │
  └── 5 V ──────────────────────► LED Matrix VCC (separate power supply recommended for full brightness)
```

> **Power note:** A 32×16 WS2812B matrix at full white can draw up to ~9 A at 5 V. Use a dedicated 5 V supply rated for at least 4 A and connect its GND to the Feather GND.

---

## Features Overview

- Multi-mode LED matrix driver for a 32×16 WS2812B panel
- Four built-in PROGMEM animations (logo, plasma, etc.)
- Upload and store custom animations via HTTP JSON API
- Real-time 256-point FFT audio visualizer (bar graph mode)
- NCS ring visualizer — music-reactive ring rendered around the logo
- Beat detection driving a haptic vibrator on bass hits
- Two-player Pong game controlled via WebSocket
- Wi-Fi Soft AP (`Resonance`) with a REST + WebSocket API
- Brightness and color palette adjustable at runtime

---

## Modes of Operation

### Built-in Animations

The firmware ships with several animations stored in Flash (PROGMEM):

| Index | Name | Description |
|-------|------|-------------|
| 0 | `logo_anim_v4` | Animated ModArt logo (multi-frame) — **default on boot** |
| 1 | `plasma_v2` | Plasma colour-flow effect (multi-frame) |
| 2 | `logo_base` | Static logo frame |

Switch between them via `POST /builtin`.

### Custom LittleFS Animations

Animations can be uploaded at runtime via `POST /animation`. They are saved to the ESP32's LittleFS filesystem as `/anim.bin` and automatically played back on the next boot if present.

- Delete a saved animation with `DELETE /animation`.
- See the [Animation File Format](#animation-file-format) section for the upload payload.

### Sound Visualizer (Bar Mode)

Activated by `POST /visualizer`. The firmware:

1. Continuously samples the microphone at **9,878 Hz**.
2. Runs a **256-point FFT** using the `arduinoFFT` library.
3. Maps the spectrum into **32 frequency bands** (logarithmic scale, bass on the left, treble on the right) displayed as vertical bar columns on the LED matrix.
4. Applies **smooth attack / decay** to each bar for fluid motion.
5. Detects **bass beats** (bins 2–7, ~77–270 Hz) using an onset-based threshold (`1.3×` running average) with a 60 ms cooldown.
6. Triggers the **haptic vibrator** on every detected beat (PWM ramps to 255 then decays at ×0.30 per frame).



**Tunables (in `visualizer.cpp`):**

| Parameter | Default | Description |
|-----------|---------|-------------|
| `NOISE_FLOOR` | 1500 | ADC noise floor (below = silence) |
| `SENSITIVITY` | 20,000 | Full-scale amplitude |
| `SMOOTH_FACTOR` | 0.70 | Bar height IIR smoothing (0 = instant, 1 = frozen) |
| `BEAT_THRESHOLD` | 1.3 | Ratio above average to trigger a beat |
| `BEAT_COOLDOWN_MS` | 60 | Minimum ms between beats |
| `VIB_DECAY` | 0.30 | Vibration PWM decay per frame |

**Available colour palettes** (select via `POST /color`):

| Index | Name | RGB |
|-------|------|-----|
| 0 | Ocean Blue | `(0, 120, 255)` |
| 1 | Hot Pink | `(255, 0, 100)` |
| 2 | Emerald Green | `(0, 230, 118)` |
| 3 | Electric Purple | `(160, 0, 255)` |
| 4 | Amber Gold | `(255, 170, 0)` |

### NCS Ring Visualizer

Activated by `POST /ncs_ring`. Renders **16 frequency bands** as radial bars arranged in a circular ring around the logo:

- Bars extend outward from the ring perimeter.
- The inner logo area pulses with overall audio energy.
- Uses additive pixel blending for a glowing halo effect.
- Same beat detection and haptic vibrator logic as bar mode.

### Pong Game

Activated by `POST /pong`. A two-player Pong game rendered on the 32×16 matrix:

| Attribute | Value |
|-----------|-------|
| Player 1 paddle column | 1 |
| Player 2 paddle column | 30 |
| Paddle height | 4 pixels |
| Win score | 5 points |
| Frame rate | ~30 fps (33 ms tick) |
| Player 1 colour | Blue |
| Player 2 colour | Red |

Players connect via WebSocket (port 81) and send `{"action": "up"}`, `{"action": "down"}`, or `{"action": "release"}` messages. Ball speed increases with each rally hit.

**Game state machine:**

```
PONG_WAITING  ──(2 players connected)──► PONG_PLAYING
PONG_PLAYING  ──(goal scored)──────────► PONG_SCORED
PONG_SCORED   ──(1 s pause)────────────► PONG_PLAYING / PONG_GAMEOVER
PONG_GAMEOVER ──(winner flashes score)──► (reset on next /pong call)
```

### Static Frame

Send `POST /static` with a pixel payload to freeze a single frame on the matrix without any animation loop.

---

## Web Control API

### Connecting

On boot the ESP32 creates a Wi-Fi access point:

| Setting | Value |
|---------|-------|
| SSID | `Resonance` |
| Password | *(open — no password)* |
| IP address | `192.168.4.1` (default ESP32 AP address) |

Connect any device to the `Resonance` network and open `http://192.168.4.1/` in a browser to access the built-in control dashboard.

### HTTP Endpoints

All endpoints are on **port 80**.  
CORS header `Access-Control-Allow-Origin: *` is included on all responses.

#### `GET /`
Returns the built-in HTML control dashboard.

---

#### `GET /status`
Returns a JSON object with the current device state.

**Response example (builtin mode):**
```json
{
  "source": "builtin",
  "name": "logo_anim_v4",
  "frames": 24,
  "fps": 6,
  "brightness": 80,
  "vizColor": 0,
  "vizColorName": "Ocean Blue",
  "width": 32,
  "height": 16,
  "builtins": [
    {"index": 0, "name": "logo_anim_v4", "frames": 24},
    {"index": 1, "name": "plasma_v2",    "frames": 30},
    {"index": 2, "name": "logo_base",    "frames": 1}
  ]
}
```

---

#### `POST /animation`
Upload a custom animation. The device saves it to LittleFS and starts playing it immediately.

**Request body (JSON):**
```json
{
  "frameCount": 2,
  "fps": 6,
  "data": "RRGGBBRRGGBB..."
}
```

| Field | Type | Description |
|-------|------|-------------|
| `frameCount` | integer | Number of animation frames |
| `fps` | integer | Playback speed (frames per second) |
| `data` | string | Flat hex string — all pixels for all frames concatenated. Column-major layout (x outer, y inner), 6 hex characters per pixel (`RRGGBB`). Total length = `frameCount × 32 × 16 × 6` characters. |

**Response (success):**
```json
{"status": "ok", "frames": 2, "fps": 6}
```

---

#### `DELETE /animation`
Deletes the saved LittleFS animation and reverts to the default built-in animation.

---

#### `POST /builtin`
Switch to a built-in PROGMEM animation.

**Request body (JSON):**
```json
{"index": 0}
```

| Field | Type | Description |
|-------|------|-------------|
| `index` | integer | Built-in animation index (0–N) |

---

#### `POST /visualizer`
Toggle the audio bar visualizer on/off.

---

#### `POST /ncs_ring`
Toggle the NCS ring visualizer on/off.

---

#### `GET /brightness`
Returns the current LED brightness.

**Response:**
```json
{"brightness": 80}
```

---

#### `POST /brightness`
Set LED brightness (0–255).

**Request body (JSON):**
```json
{"brightness": 120}
```

---

#### `GET /color`
Returns the current visualizer color index and name.

**Response:**
```json
{"color": 0, "name": "Ocean Blue"}
```

---

#### `POST /color`
Set the visualizer bar color.

**Request body (JSON):**
```json
{"color": 2}
```

| Value | Color |
|-------|-------|
| 0 | Ocean Blue |
| 1 | Hot Pink |
| 2 | Emerald Green |
| 3 | Electric Purple |
| 4 | Amber Gold |

---

#### `POST /static`
Display a static (single-frame, no animation loop) image.

**Request body (JSON):**
```json
{
  "data": "RRGGBBRRGGBB..."
}
```

`data` is a hex string of exactly `32 × 16 × 6 = 3072` characters.

---

#### `POST /pong`
Toggle Pong game mode on/off.

---

### WebSocket (Pong Controls)

The WebSocket server listens on **port 81** (`ws://192.168.4.1:81`).

**Connection flow:**
1. Client connects → server checks that Pong mode is active.
2. Server assigns the client a player slot and replies:
   ```json
   {"type": "assign", "player": 1}
   ```
3. If the game is full, the server replies with an error and closes the connection:
   ```json
   {"type": "error", "message": "game full"}
   ```

**Client → Server messages:**
```json
{"action": "up"}      // Move paddle up
{"action": "down"}    // Move paddle down
{"action": "release"} // Stop paddle movement
```

**Server → Client broadcasts** (~10 Hz while a game is running):
```json
{
  "type": "state",
  "status": "playing",
  "score": [2, 1],
  "winner": 0
}
```

| `status` | Meaning |
|----------|---------|
| `waiting` | Waiting for 2 players to connect |
| `playing` | Active rally in progress |
| `scored` | Brief pause after a goal |
| `gameover` | A player has reached 5 points; `winner` is `1` or `2` |

---

## Animation File Format

### JSON Upload Format

Used with `POST /animation`. The `data` field is a flat hexadecimal string:

```
Frame 0: pixel(x=0,y=0)  pixel(x=0,y=1)  … pixel(x=0,y=15)   ← column 0
         pixel(x=1,y=0)  … pixel(x=1,y=15)                    ← column 1
         …
         pixel(x=31,y=0) … pixel(x=31,y=15)                   ← column 31
Frame 1: (same layout)
…
```

Each pixel is encoded as 6 hex characters: `RRGGBB`.

Total string length: `frameCount × 32 × 16 × 6` characters.

### Binary Storage Format (LittleFS)

The firmware converts the JSON payload to a compact binary file (`/anim.bin`):

```
Byte 0–1 : frameCount   (uint16_t, little-endian)
Byte 2–3 : frameDelayMs (uint16_t, little-endian) = 1000 / fps
Byte 4+  : pixel data   (frameCount × 32 × 16 × 3 bytes)
           Layout: column-major (x outer, y inner), 3 bytes per pixel [R, G, B]
```

Frame size = `32 × 16 × 3 = 1536 bytes`.

---

## Python Utility: `h_to_json.py`

Converts LED Matrix Studio `.h` animation header files to the JSON format accepted by `POST /animation`.

### Usage

```bash
python h_to_json.py input.h [output.json] [--fps 6]
```

| Argument | Default | Description |
|----------|---------|-------------|
| `input` | *(required)* | Path to the `.h` animation file |
| `output` | same name as input with `.json` extension | Output JSON file path |
| `--fps` | `6` | Animation playback speed in frames per second |

### Example

```bash
# Convert my_anim.h to my_anim.json at 12 fps
python h_to_json.py my_anim.h --fps 12

# Then upload to the device
curl -X POST http://192.168.4.1/animation \
     -H "Content-Type: application/json" \
     -d @my_anim.json
```

The `.h` file must contain 32-bit hex colour values (`0xRRGGBB`) in column-major order (x outer, y inner) for a 32×16 grid.

---

## Haptic Feedback

The vibrator motor on **GPIO 10** provides tactile feedback synchronized to audio beats:

- **Trigger condition:** Bass energy (FFT bins 2–7, ~77–270 Hz) exceeds 1.3× its running average and is above a minimum level, with at least 60 ms between triggers.
- **Pulse shape:** PWM duty instantly set to 255 on a beat, then multiplied by 0.30 each visualizer frame for a sharp-attack / fast-decay feel.
- **Active during:** Both the standard bar visualizer and the NCS ring visualizer.
- **Silent during:** All animation and Pong modes.

---

## Library Dependencies

Install these libraries through the **Arduino IDE Library Manager** or `arduino-cli lib install`:

| Library | Purpose |
|---------|---------|
| [FastLED](https://github.com/FastLED/FastLED) | WS2812B LED matrix driver |
| [arduinoFFT](https://github.com/kosme/arduinoFFT) | 256-point FFT for audio analysis |
| [WebSockets (Links2004)](https://github.com/Links2004/arduinoWebSockets) | WebSocket server for Pong |
| ESP32 Arduino Core ≥ 2.x | `WiFi.h`, `WebServer.h`, `LittleFS.h`, ADC, PWM |

---

## Build & Flash Instructions

### Partition Scheme

LittleFS is required to store custom animations. In the **Arduino IDE**:

1. Select **Tools → Board → Adafruit Feather ESP32-S2**.
2. Select **Tools → Partition Scheme → Default 4MB with spiffs** (or any scheme that includes a SPIFFS/LittleFS partition).

> Without a LittleFS partition the firmware still runs but custom animation upload (`POST /animation`) will return a 500 error.

### Flashing

#### Using Arduino IDE

1. Open `main/main.ino` in the Arduino IDE.
2. Install all [Library Dependencies](#library-dependencies).
3. Set the board and partition scheme as above.
4. Click **Upload**.

#### Using `arduino-cli`

```bash
# Install the ESP32 platform
arduino-cli core install esp32:esp32

# Install libraries
arduino-cli lib install "FastLED" "arduinoFFT" "WebSockets"

# Compile and upload (adjust port as needed)
arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s2 main/
arduino-cli upload  --fqbn esp32:esp32:adafruit_feather_esp32s2 \
                    --port /dev/ttyUSB0 main/
```

After flashing, open the Serial Monitor at **115 200 baud** to see boot messages and runtime debug output.

---

## Project Structure

```
hackathon-modart/
├── main/
│   ├── main.ino           # Entry point: setup() and loop()
│   ├── animation.h        # LED matrix config, animation structures & XY mapping
│   ├── animation.cpp      # FastLED init, LittleFS load, frame playback
│   ├── visualizer.h       # Microphone / FFT / vibrator config & API
│   ├── visualizer.cpp     # 256-pt FFT bar visualizer + beat detection
│   ├── ncs_ring.h         # NCS ring visualizer API
│   ├── ncs_ring.cpp       # Circular FFT ring renderer
│   ├── pong.h             # Pong game config, state struct & API
│   ├── pong.cpp           # Pong physics, rendering & player management
│   ├── web_server.h       # HTTP & WebSocket server API
│   ├── web_server.cpp     # REST endpoints + WebSocket Pong handler
│   ├── logo_base.h        # Static 32×16 logo frame (PROGMEM)
│   ├── logo_anim_v4.h     # Animated logo frames (PROGMEM)
│   └── plasma_v2.h        # Plasma animation frames (PROGMEM)
├── h_to_json.py           # CLI tool: convert .h animation files → JSON
├── yes.h                  # Example single-frame animation header
└── yes.json               # Example single-frame animation JSON
```
