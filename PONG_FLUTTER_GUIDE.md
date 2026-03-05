# Pong Mode — Flutter Integration Guide

### Flutter Side

Add the `web_socket_channel` package to your `pubspec.yaml`:

```yaml
dependencies:
  web_socket_channel: ^3.0.1
```

---

## How It Works

1. A user sends `POST /pong` to the ESP32 to activate Pong mode (or use the existing web UI).
2. Two players connect to `ws://<ESP32_IP>:81` from their Flutter app.
3. The ESP32 assigns each player a number (1 = left paddle, 2 = right paddle).
4. Players send up/down/release commands; the ESP32 runs the game and renders on the LED matrix.
5. The ESP32 broadcasts game state to all connected clients at ~10 Hz.

---

## Activating Pong Mode

Before players can connect via WebSocket, Pong mode must be activated:

```dart
import 'package:http/http.dart' as http;

Future<void> activatePong(String espIp) async {
  final response = await http.post(Uri.parse('http://192.168.4.1//pong'));
  // response body: {"status":"ok","pong":true}
}
```

To deactivate, send the same `POST /pong` again (it toggles).

---

## WebSocket Protocol

### Connection

Connect to: `ws://<ESP32_IP>:81`

The IP is typically `192.168.4.1`.

### Messages: Server -> Client

#### Player Assignment (on connect)

```json
{"type": "assign", "player": 1}
```

`player` is `1` (left paddle) or `2` (right paddle).

#### Game State (broadcast at ~10 Hz)

```json
{
  "type": "state",
  "status": "playing",
  "score": [2, 3],
  "winner": 0
}
```

| Field    | Values                                      |
|----------|---------------------------------------------|
| `status` | `"waiting"`, `"playing"`, `"scored"`, `"gameover"` |
| `score`  | `[player1_score, player2_score]`            |
| `winner` | `0` (none), `1`, or `2`                     |

- `waiting` — fewer than 2 players connected
- `playing` — game is running
- `scored` — brief pause after a goal (1 second), then resumes
- `gameover` — a player reached 5 points; `winner` indicates who

#### Error (on connect if game is full or mode not active)

```json
{"type": "error", "message": "game full"}
```

### Messages: Client -> Server

#### Paddle Input

```json
{"type": "input", "action": "up"}
```

| Action     | Effect                          |
|------------|---------------------------------|
| `"up"`     | Move paddle up by 1 pixel       |
| `"down"`   | Move paddle down by 1 pixel     |

Each message moves the paddle by exactly 1 pixel. To hold a direction, send repeated messages on a timer (e.g. every 50ms) while the button is pressed, and stop the timer on release.

---

## Flutter Implementation

### Full Example: Pong Controller Widget

```dart
import 'dart:async';
import 'dart:convert';
import 'package:flutter/material.dart';
import 'package:web_socket_channel/web_socket_channel.dart';

class PongController extends StatefulWidget {
  final String espIp;
  const PongController({super.key, required this.espIp});

  @override
  State<PongController> createState() => _PongControllerState();
}

class _PongControllerState extends State<PongController> {
  WebSocketChannel? _channel;
  Timer? _moveTimer;
  int? _player;
  String _status = 'connecting';
  List<int> _score = [0, 0];
  int _winner = 0;
  String? _error;

  static const _repeatInterval = Duration(milliseconds: 50);

  @override
  void initState() {
    super.initState();
    _connect();
  }

  void _connect() {
    final uri = Uri.parse('ws://${widget.espIp}:81');
    _channel = WebSocketChannel.connect(uri);
    _channel!.stream.listen(
      _onMessage,
      onError: (error) {
        setState(() => _error = 'Connection error: $error');
      },
      onDone: () {
        setState(() => _status = 'disconnected');
      },
    );
  }

  void _onMessage(dynamic raw) {
    final msg = jsonDecode(raw as String) as Map<String, dynamic>;
    final type = msg['type'] as String?;

    switch (type) {
      case 'assign':
        setState(() {
          _player = msg['player'] as int;
          _error = null;
        });
        break;
      case 'state':
        setState(() {
          _status = msg['status'] as String;
          _score = List<int>.from(msg['score'] as List);
          _winner = msg['winner'] as int;
        });
        break;
      case 'error':
        setState(() => _error = msg['message'] as String);
        break;
    }
  }

  void _sendAction(String action) {
    _channel?.sink.add(jsonEncode({
      'type': 'input',
      'action': action,
    }));
  }

  void _startMoving(String action) {
    _moveTimer?.cancel();
    _sendAction(action);
    _moveTimer = Timer.periodic(_repeatInterval, (_) {
      _sendAction(action);
    });
  }

  void _stopMoving() {
    _moveTimer?.cancel();
    _moveTimer = null;
  }

  @override
  void dispose() {
    _moveTimer?.cancel();
    _channel?.sink.close();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    if (_error != null) {
      return Scaffold(
        body: Center(child: Text('Error: $_error')),
      );
    }

    return Scaffold(
      appBar: AppBar(
        title: Text(_player != null
            ? 'Player $_player — ${_score[0]} : ${_score[1]}'
            : 'Connecting...'),
      ),
      body: _buildBody(),
    );
  }

  Widget _buildBody() {
    if (_status == 'waiting') {
      return const Center(
        child: Text('Waiting for other player...', style: TextStyle(fontSize: 24)),
      );
    }

    if (_status == 'gameover') {
      return Center(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Text(
              _winner == _player ? 'You Win!' : 'You Lose',
              style: const TextStyle(fontSize: 36, fontWeight: FontWeight.bold),
            ),
            const SizedBox(height: 16),
            Text('Final Score: ${_score[0]} - ${_score[1]}',
                style: const TextStyle(fontSize: 24)),
          ],
        ),
      );
    }

    // Playing / Scored — show paddle controls
    return Column(
      children: [
        Expanded(
          child: GestureDetector(
            onTapDown: (_) => _startMoving('up'),
            onTapUp: (_) => _stopMoving(),
            onTapCancel: () => _stopMoving(),
            child: Container(
              color: Colors.blue.shade100,
              child: const Center(
                child: Icon(Icons.arrow_upward, size: 80),
              ),
            ),
          ),
        ),
        Padding(
          padding: const EdgeInsets.all(16),
          child: Text(
            '${_score[0]}  :  ${_score[1]}',
            style: const TextStyle(fontSize: 48, fontWeight: FontWeight.bold),
          ),
        ),
        Expanded(
          child: GestureDetector(
            onTapDown: (_) => _startMoving('down'),
            onTapUp: (_) => _stopMoving(),
            onTapCancel: () => _stopMoving(),
            child: Container(
              color: Colors.red.shade100,
              child: const Center(
                child: Icon(Icons.arrow_downward, size: 80),
              ),
            ),
          ),
        ),
      ],
    );
  }
}
```

### Usage

```dart
// Navigate to the Pong controller screen
Navigator.push(
  context,
  MaterialPageRoute(
    builder: (_) => const PongController(espIp: '192.168.4.1'),
  ),
);
```

---

## Game Flow Summary

```
1. Flutter app sends POST /pong to ESP32 to activate Pong mode
2. Player 1 opens PongController -> connects to ws://192.168.4.1:81
3. ESP32 assigns player 1, sends {"type":"assign","player":1}
4. Player 2 opens PongController -> connects to ws://192.168.4.1:81
5. ESP32 assigns player 2, sends {"type":"assign","player":2}
6. ESP32 starts the game, broadcasts {"type":"state","status":"playing",...}
7. Players hold Up/Down buttons -> app sends {"type":"input","action":"up|down"} every 50ms
8. ESP32 moves paddles, updates ball, renders on LED matrix
9. ESP32 broadcasts score updates at ~10 Hz
10. First to 5 wins -> {"type":"state","status":"gameover","winner":1}
```

---

## Status Endpoint

You can check if Pong mode is active via `GET /status`:

```json
{
  "source": "pong",
  "pong_status": "playing",
  "pong_score": [2, 3],
  "pong_players": 2,
  "width": 32,
  "height": 16,
  "builtins": [...]
}
```
