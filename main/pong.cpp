#include "pong.h"
#include <FastLED.h>

// ── Global game state ───────────────────────────────────────────────

PongState pong;

// ── 3x5 digit font for score display ───────────────────────────────
// Each digit is 3 columns x 5 rows, stored column-major as a bitmask.

static const uint8_t DIGIT_FONT[10][3] = {
  {0x1F, 0x11, 0x1F},  // 0
  {0x00, 0x1F, 0x00},  // 1
  {0x1D, 0x15, 0x17},  // 2
  {0x15, 0x15, 0x1F},  // 3
  {0x07, 0x04, 0x1F},  // 4
  {0x17, 0x15, 0x1D},  // 5
  {0x1F, 0x15, 0x1D},  // 6
  {0x01, 0x01, 0x1F},  // 7
  {0x1F, 0x15, 0x1F},  // 8
  {0x17, 0x15, 0x1F},  // 9
};

// ── Helpers ─────────────────────────────────────────────────────────

static void drawPixel(uint8_t x, uint8_t y, CRGB color) {
  if (x < WIDTH && y < HEIGHT) {
    leds[XY(x, y)] = color;
  }
}

static void drawDigit(uint8_t baseX, uint8_t baseY, uint8_t digit, CRGB color) {
  if (digit > 9) return;
  for (uint8_t col = 0; col < 3; col++) {
    uint8_t bits = DIGIT_FONT[digit][col];
    for (uint8_t row = 0; row < 5; row++) {
      if (bits & (1 << row)) {
        drawPixel(baseX + col, baseY + (4 - row), color);
      }
    }
  }
}

static void serveBall() {
  pong.ballX = WIDTH / 2.0f;
  pong.ballY = HEIGHT / 2.0f;

  // Alternate serve direction based on total score
  float dir = ((pong.score1 + pong.score2) % 2 == 0) ? 1.0f : -1.0f;
  pong.ballVX = dir * 0.25f;
  pong.ballVY = (random(2) == 0) ? 0.15f : -0.15f;
}

// ── Player connection state ──────────────────────────────────────────

static bool player1Connected = false;
static bool player2Connected = false;

// ── Init / Reset ────────────────────────────────────────────────────

void initPong() {
  memset(&pong, 0, sizeof(pong));
  pong.status = PONG_WAITING;
  pong.paddle1Y = (HEIGHT - PONG_PADDLE_HEIGHT) / 2;
  pong.paddle2Y = (HEIGHT - PONG_PADDLE_HEIGHT) / 2;
  player1Connected = false;
  player2Connected = false;
}

void resetPongGame() {
  pong.score1 = 0;
  pong.score2 = 0;
  pong.winner = 0;
  pong.paddle1Y = (HEIGHT - PONG_PADDLE_HEIGHT) / 2;
  pong.paddle2Y = (HEIGHT - PONG_PADDLE_HEIGHT) / 2;
  pong.paddle1Dir = 0;
  pong.paddle2Dir = 0;
  pong.lastTick = millis();
  serveBall();

  if (pong.playerCount >= 2) {
    pong.status = PONG_PLAYING;
  } else {
    pong.status = PONG_WAITING;
  }
}

// ── Player management ───────────────────────────────────────────────

uint8_t pongAddPlayer() {
  if (!player1Connected) {
    player1Connected = true;
    pong.playerCount++;
    if (pong.playerCount >= 2 && pong.status == PONG_WAITING) {
      resetPongGame();
      pong.status = PONG_PLAYING;
    }
    return 1;
  }
  if (!player2Connected) {
    player2Connected = true;
    pong.playerCount++;
    if (pong.playerCount >= 2 && pong.status == PONG_WAITING) {
      resetPongGame();
      pong.status = PONG_PLAYING;
    }
    return 2;
  }
  return 0;
}

void pongRemovePlayer(uint8_t player) {
  if (player == 1 && player1Connected) {
    player1Connected = false;
    pong.playerCount--;
  } else if (player == 2 && player2Connected) {
    player2Connected = false;
    pong.playerCount--;
  }
  if (pong.playerCount < 2 && pong.status == PONG_PLAYING) {
    pong.status = PONG_WAITING;
  }
}

// ── Input handling ──────────────────────────────────────────────────
// Each "up"/"down" message immediately moves the paddle by 1 pixel.
// The Flutter app should send repeated messages while button is held.

static void clampPaddle(int8_t& paddleY) {
  int8_t maxY = static_cast<int8_t>(HEIGHT - PONG_PADDLE_HEIGHT);
  if (paddleY < 0) paddleY = 0;
  if (paddleY > maxY) paddleY = maxY;
}

void handlePongInput(uint8_t player, const char* action) {
  if (pong.status != PONG_PLAYING && pong.status != PONG_SCORED) return;

  int8_t delta = 0;
  if (strcmp(action, "up") == 0)        delta = 1;   // Y=0 is bottom, so "up" = +Y
  else if (strcmp(action, "down") == 0) delta = -1;
  else return;

  if (player == 1) {
    pong.paddle1Y += delta;
    clampPaddle(pong.paddle1Y);
  } else if (player == 2) {
    pong.paddle2Y += delta;
    clampPaddle(pong.paddle2Y);
  }
}

// ── Game update (~30fps) ────────────────────────────────────────────

void updatePong() {
  unsigned long now = millis();
  if (now - pong.lastTick < PONG_TICK_MS) return;
  pong.lastTick = now;

  if (pong.status == PONG_WAITING || pong.status == PONG_GAMEOVER) return;

  if (pong.status == PONG_SCORED) {
    if (now - pong.scoreTime >= PONG_SERVE_DELAY_MS) {
      serveBall();
      pong.status = PONG_PLAYING;
    }
    return;
  }

  // Move ball
  pong.ballX += pong.ballVX;
  pong.ballY += pong.ballVY;

  // Wall bounce (top/bottom)
  if (pong.ballY <= 0.0f) {
    pong.ballY = -pong.ballY;
    pong.ballVY = -pong.ballVY;
  }
  if (pong.ballY >= HEIGHT - 1.0f) {
    pong.ballY = 2.0f * (HEIGHT - 1.0f) - pong.ballY;
    pong.ballVY = -pong.ballVY;
  }

  // Paddle 1 collision (left side)
  int bx = static_cast<int>(pong.ballX + 0.5f);
  int by = static_cast<int>(pong.ballY + 0.5f);

  if (bx <= PONG_PADDLE_X1 && pong.ballVX < 0) {
    if (by >= pong.paddle1Y && by < pong.paddle1Y + PONG_PADDLE_HEIGHT) {
      pong.ballX = PONG_PADDLE_X1 + 1.0f;
      pong.ballVX = -pong.ballVX;
      pong.ballVX *= 1.02f;
      float hitPos = (pong.ballY - pong.paddle1Y) / (float)PONG_PADDLE_HEIGHT;
      pong.ballVY = (hitPos - 0.5f) * 0.5f;
    }
  }

  // Paddle 2 collision (right side)
  if (bx >= PONG_PADDLE_X2 && pong.ballVX > 0) {
    if (by >= pong.paddle2Y && by < pong.paddle2Y + PONG_PADDLE_HEIGHT) {
      pong.ballX = PONG_PADDLE_X2 - 1.0f;
      pong.ballVX = -pong.ballVX;
      pong.ballVX *= 1.02f;
      float hitPos = (pong.ballY - pong.paddle2Y) / (float)PONG_PADDLE_HEIGHT;
      pong.ballVY = (hitPos - 0.5f) * 0.5f;
    }
  }

  // Cap ball speed
  float maxSpeed = 0.6f;
  if (pong.ballVX > maxSpeed)  pong.ballVX = maxSpeed;
  if (pong.ballVX < -maxSpeed) pong.ballVX = -maxSpeed;
  if (pong.ballVY > maxSpeed)  pong.ballVY = maxSpeed;
  if (pong.ballVY < -maxSpeed) pong.ballVY = -maxSpeed;

  // Goal detection
  if (pong.ballX < 0) {
    pong.score2++;
    pong.scoreTime = now;
    if (pong.score2 >= PONG_WIN_SCORE) {
      pong.status = PONG_GAMEOVER;
      pong.winner = 2;
    } else {
      pong.status = PONG_SCORED;
    }
    return;
  }
  if (pong.ballX >= WIDTH) {
    pong.score1++;
    pong.scoreTime = now;
    if (pong.score1 >= PONG_WIN_SCORE) {
      pong.status = PONG_GAMEOVER;
      pong.winner = 1;
    } else {
      pong.status = PONG_SCORED;
    }
    return;
  }
}

// ── Rendering ───────────────────────────────────────────────────────

void renderPong() {
  FastLED.clear();

  CRGB colorP1     = CRGB(0, 80, 255);   // blue
  CRGB colorP2     = CRGB(255, 40, 0);    // red
  CRGB colorBall   = CRGB(255, 255, 255); // white
  CRGB colorCenter = CRGB(30, 30, 30);    // dim center line
  CRGB colorScore  = CRGB(120, 120, 120); // score digits

  // Center dashed line
  for (uint8_t y = 0; y < HEIGHT; y += 2) {
    drawPixel(WIDTH / 2, y, colorCenter);
  }

  // Paddles
  for (uint8_t i = 0; i < PONG_PADDLE_HEIGHT; i++) {
    drawPixel(PONG_PADDLE_X1, pong.paddle1Y + i, colorP1);
    drawPixel(PONG_PADDLE_X2, pong.paddle2Y + i, colorP2);
  }

  // Ball (draw in all active states)
  if (pong.status == PONG_PLAYING || pong.status == PONG_SCORED) {
    uint8_t bx = static_cast<uint8_t>(constrain((int)(pong.ballX + 0.5f), 0, WIDTH - 1));
    uint8_t by = static_cast<uint8_t>(constrain((int)(pong.ballY + 0.5f), 0, HEIGHT - 1));
    drawPixel(bx, by, colorBall);
  }

  // Score display at top of screen (y=0 is bottom, so top = HEIGHT - 6)
  drawDigit(WIDTH / 2 - 5, HEIGHT - 6, pong.score1, colorScore);
  drawDigit(WIDTH / 2 + 3, HEIGHT - 6, pong.score2, colorScore);

  // "Waiting" indicator: blink paddles if not enough players
  if (pong.status == PONG_WAITING) {
    if ((millis() / 500) % 2 == 0) {
      for (uint8_t i = 0; i < PONG_PADDLE_HEIGHT; i++) {
        drawPixel(PONG_PADDLE_X1, pong.paddle1Y + i, CRGB::Black);
        drawPixel(PONG_PADDLE_X2, pong.paddle2Y + i, CRGB::Black);
      }
    }
  }

  // Game over: flash winner score
  if (pong.status == PONG_GAMEOVER) {
    if ((millis() / 300) % 2 == 0) {
      CRGB winColor = (pong.winner == 1) ? colorP1 : colorP2;
      uint8_t winX = (pong.winner == 1) ? (WIDTH / 2 - 5) : (WIDTH / 2 + 2);
      uint8_t winScore = (pong.winner == 1) ? pong.score1 : pong.score2;
      drawDigit(winX, HEIGHT - 6, winScore, winColor);
    }
  }

  FastLED.show();
}
