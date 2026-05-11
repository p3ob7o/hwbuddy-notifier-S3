// M5Stick S3 — Hardware Buddy firmware
// BLE protocol: anthropics/claude-desktop-buddy/REFERENCE.md
//
// Idle UX: screen off until Claude Desktop sends a permission prompt.
// On prompt: the character slides up from below, then the prompt + two
// options appear. The side button (B) cycles Approve/Deny; the front
// button (A) confirms. After the decision, brief feedback then sleep.

#include <M5Unified.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <AnimatedGIF.h>
#include <esp_mac.h>
#include <sys/time.h>

#include "character_gif.h"
#include "melody.h"

// ---- Nordic UART Service ----
static const char* NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static const char* NUS_RX_CHAR = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
static const char* NUS_TX_CHAR = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

// ---- UI state ----
enum class UIMode {
  IdleDisconnected,   // not paired yet, screen on with hint
  IdleSleeping,       // paired, no prompt, screen off
  IdleGlancing,       // user tapped a button while idle, brief status peek
  Pairing,            // BLE pairing in progress; passkey shown on screen
  PromptRevealing,    // animating character reveal
  PromptActive,       // prompt visible, waiting for cycle/confirm
  DecisionFeedback,   // brief "Approved!" / "Denied" flash
};

enum class Choice { Deny = 0, Approve = 1 };

struct Heartbeat {
  bool fresh = false;
  uint32_t lastMs = 0;
  int total = 0, running = 0, waiting = 0;
  String msg;
  uint32_t tokens = 0;
  bool promptActive = false;
  String promptId, promptTool, promptHint;
};

static Heartbeat hb;
static UIMode mode = UIMode::IdleDisconnected;
static uint32_t modeEnteredMs = 0;
static Choice activeChoice = Choice::Approve;
static bool lastDecisionWasApprove = false;

// ---- BLE ----
static NimBLECharacteristic* txChar = nullptr;
static volatile bool bleConnected = false;
static volatile bool bleBonded = false;
static volatile uint32_t pendingPasskey = 0;  // 6-digit code being shown
static uint16_t bleMtu = 23;
static String rxBuffer;

// ---- Persistent ----
static Preferences prefs;
static String deviceName = "Claude";
static String ownerName;
static uint32_t statApprove = 0, statDeny = 0;

// ---- IMU ----
// Two behaviors:
//   - Face-up (screen pointing at the ceiling/user) silences the melody:
//     the user can already see the screen, so a chime is redundant.
//   - A vigorous shake while a prompt is active is treated as Deny — a
//     dismissive "no" gesture.
static uint32_t imuLastSampleMs = 0;
static volatile bool faceUp = false;
static int shakeStreak = 0;
static uint32_t shakeCooldownMs = 0;

static void tickIMU() {
  uint32_t now = millis();
  if (now - imuLastSampleMs < 20) return;  // sample at ~50 Hz
  imuLastSampleMs = now;
  M5.Imu.update();
  float ax, ay, az;
  if (!M5.Imu.getAccel(&ax, &ay, &az)) return;
  faceUp = (az > 0.7f);  // +Z opposes gravity when face up
  float mag2 = ax * ax + ay * ay + az * az;
  // At rest mag2 ≈ 1.0; a real shake spikes well above 4.0 (i.e. >2 g).
  if (mag2 > 4.0f) {
    if (shakeStreak < 100) shakeStreak++;
  } else if (shakeStreak > 0) {
    shakeStreak--;
  }
}

static bool consumeShake() {
  uint32_t now = millis();
  if (shakeStreak < 3 || now < shakeCooldownMs) return false;
  shakeStreak = 0;
  shakeCooldownMs = now + 1000;  // 1s lockout so one shake = one action
  return true;
}

// ---- Melody (alert chime) ----
// Non-blocking sequencer: the main loop ticks it; M5.Speaker.tone() is
// fire-and-forget, so we just schedule the next note when the current
// one's duration has elapsed. This lets the GIF keep animating while
// the melody plays.
static size_t melodyIdx = 0;
static uint32_t melodyNextMs = 0;
static bool melodyPlaying = false;

static void startMelody() {
  melodyIdx = 0;
  melodyNextMs = millis();
  melodyPlaying = true;
}

static void stopMelody() {
  melodyPlaying = false;
  M5.Speaker.stop();
}

static void tickMelody() {
  if (!melodyPlaying) return;
  uint32_t now = millis();
  if (now < melodyNextMs) return;
  if (melodyIdx >= PROMPT_MELODY_LEN) {
    melodyPlaying = false;
    return;
  }
  const Note& n = PROMPT_MELODY[melodyIdx];
  if (n.freq > 0.0f) M5.Speaker.tone(n.freq, n.ms);
  melodyNextMs = now + n.ms;
  melodyIdx++;
}

// ---- GIF playback (character) ----
static AnimatedGIF gif;
static bool gifOpen = false;
static uint32_t gifNextFrameMs = 0;

// Character is bottom-anchored: the bottom edge of the GIF sits at y=239
// (bottom of the 240-tall panel). The top half of the screen is for chrome.
static constexpr int CHAR_REST_Y = 240 - CHAR_H;

static void GIFDraw(GIFDRAW *pDraw) {
  // Draw directly to the display. The character sits at CHAR_REST_Y and
  // is centered horizontally; map sprite-local coords to panel coords.
  const int xOrigin = (M5.Display.width() - CHAR_W) / 2;
  const int yOrigin = CHAR_REST_Y;
  int y = yOrigin + pDraw->iY + pDraw->y;
  int x = xOrigin + pDraw->iX;
  int w = pDraw->iWidth;
  uint8_t* s = pDraw->pPixels;
  uint8_t* pal24 = pDraw->pPalette24;

  // Pack each palette entry into a "swap565" — RGB565 with bytes already
  // swapped to panel SPI order — and stream the line via pushImage. This
  // is the same fast-path lgfx itself uses for bulk pixel pushes, and it
  // sidesteps any color-conversion oddities seen with drawPixel/color888.
  static uint16_t lineBuf[CHAR_W];
  if (pDraw->ucHasTransparency) {
    uint8_t tk = pDraw->ucTransparent;
    for (int i = 0; i < w; i++) {
      uint8_t idx = s[i];
      if (idx == tk) continue;
      uint8_t* c = &pal24[idx * 3];
      uint16_t v = lgfx::swap565(c[0], c[1], c[2]);
      M5.Display.pushImage(x + i, y, 1, 1, &v);
    }
  } else {
    int n = (w > CHAR_W) ? CHAR_W : w;
    for (int i = 0; i < n; i++) {
      uint8_t* c = &pal24[s[i] * 3];
      lineBuf[i] = lgfx::swap565(c[0], c[1], c[2]);
    }
    M5.Display.pushImage(x, y, n, 1, lineBuf);
  }
}

static void startGifPlayback() {
  if (gifOpen) gif.close();
  // Clear the slot the GIF will draw into — the library only paints
  // pixels it changes, so any leftover content shows through.
  int xOrigin = (M5.Display.width() - CHAR_W) / 2;
  M5.Display.fillRect(xOrigin, CHAR_REST_Y, CHAR_W, CHAR_H, TFT_BLACK);
  gifOpen = gif.open((uint8_t*)char_gif, CHAR_GIF_LEN, GIFDraw);
  gifNextFrameMs = millis();
}

static void stopGifPlayback() {
  if (gifOpen) {
    gif.close();
    gifOpen = false;
  }
}

// ---- Helpers ----
static String macSuffix() {
  uint8_t mac[6] = {0};
  esp_efuse_mac_get_default(mac);
  char buf[6];
  snprintf(buf, sizeof(buf), "%02X%02X", mac[4], mac[5]);
  return String(buf);
}

static void enterMode(UIMode m) {
  mode = m;
  modeEnteredMs = millis();
}

static void screenSleep() {
  M5.Display.setBrightness(0);
  M5.Display.sleep();
}

static void screenWake() {
  M5.Display.wakeup();
  M5.Display.setBrightness(120);
}

// ---- BLE TX ----
static void notifyLine(const String& line) {
  if (!bleConnected || !txChar) return;
  String out = line + "\n";
  size_t len = out.length();
  size_t chunk = (bleMtu > 3) ? (bleMtu - 3) : 20;
  for (size_t i = 0; i < len; i += chunk) {
    size_t n = (len - i > chunk) ? chunk : (len - i);
    txChar->setValue((const uint8_t*)out.c_str() + i, n);
    txChar->notify();
    delay(2);
  }
}

static void sendAck(const char* cmd, bool ok = true, uint32_t n = 0, const char* err = nullptr) {
  JsonDocument d;
  d["ack"] = cmd; d["ok"] = ok; d["n"] = n;
  if (err) d["error"] = err;
  String s; serializeJson(d, s);
  notifyLine(s);
}

static void sendStatus() {
  JsonDocument d;
  d["ack"] = "status"; d["ok"] = true;
  JsonObject data = d["data"].to<JsonObject>();
  data["name"] = deviceName;
  data["sec"] = (bool)bleBonded;
  JsonObject bat = data["bat"].to<JsonObject>();
  int32_t lvl = M5.Power.getBatteryLevel();
  int32_t mv = M5.Power.getBatteryVoltage();
  if (lvl >= 0) bat["pct"] = lvl;
  if (mv > 0)   bat["mV"]  = mv;
  bat["usb"] = M5.Power.isCharging();
  JsonObject sys = data["sys"].to<JsonObject>();
  sys["up"] = (uint32_t)(millis() / 1000);
  sys["heap"] = (uint32_t)ESP.getFreeHeap();
  JsonObject stats = data["stats"].to<JsonObject>();
  stats["appr"] = statApprove;
  stats["deny"] = statDeny;
  String s; serializeJson(d, s);
  notifyLine(s);
}

static void sendPermission(const char* decision) {
  if (!hb.promptActive) return;
  JsonDocument d;
  d["cmd"] = "permission";
  d["id"] = hb.promptId;
  d["decision"] = decision;
  String s; serializeJson(d, s);
  notifyLine(s);
}

// ---- Drawing ----
static void drawPasskeyScreen(uint32_t pin) {
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(TFT_BLACK);
  d.setFont(&fonts::Font0);
  d.setTextSize(1);
  d.setTextDatum(top_center);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("Pair this device", d.width() / 2, 14);
  d.setTextColor(TFT_DARKGREY, TFT_BLACK);
  d.drawString("Enter in Hardware", d.width() / 2, 28);
  d.drawString("Buddy panel:",       d.width() / 2, 40);

  d.setFont(&fonts::Font4);
  d.setTextDatum(middle_center);
  d.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
  char buf[8];
  snprintf(buf, sizeof(buf), "%06u", (unsigned)pin);
  d.drawString(buf, d.width() / 2, d.height() / 2 + 10);

  d.setFont(&fonts::Font0);
  d.setTextDatum(top_center);
  d.setTextColor(TFT_DARKGREY, TFT_BLACK);
  d.drawString("Pair once; reconnects", d.width() / 2, d.height() - 24);
  d.drawString("are automatic.",        d.width() / 2, d.height() - 12);
  d.endWrite();
}

static void drawIdleDisconnected() {
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(TFT_BLACK);
  d.setTextDatum(top_left);
  d.setFont(&fonts::Font0);
  d.setTextSize(1);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString(deviceName.c_str(), 4, 4);
  d.fillCircle(d.width() - 8, 8, 4, TFT_RED);
  d.drawFastHLine(0, 16, d.width(), TFT_DARKGREY);
  d.setTextColor(TFT_YELLOW);
  d.drawString("Pairing...", 4, 28);
  d.setTextColor(TFT_DARKGREY);
  d.drawString("Open Hardware Buddy", 4, 50);
  d.drawString("in Claude Desktop", 4, 62);
  d.drawString("(Developer menu)", 4, 74);
  d.endWrite();
}

static void drawIdleGlance() {
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(TFT_BLACK);
  d.setTextDatum(top_left);
  d.setFont(&fonts::Font0);
  d.setTextSize(1);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString(deviceName.c_str(), 4, 4);
  d.fillCircle(d.width() - 8, 8, 4, TFT_GREEN);
  d.drawFastHLine(0, 16, d.width(), TFT_DARKGREY);
  int y = 26;
  d.setTextColor(TFT_GREEN);
  d.drawString("Connected", 4, y); y += 14;
  if (hb.fresh) {
    d.setTextColor(TFT_CYAN);
    char counts[40];
    snprintf(counts, sizeof(counts), "T:%d  R:%d  W:%d", hb.total, hb.running, hb.waiting);
    d.drawString(counts, 4, y); y += 14;
    if (hb.msg.length()) {
      d.setTextColor(TFT_WHITE);
      d.drawString(hb.msg.substring(0, 22).c_str(), 4, y); y += 14;
    }
    if (hb.tokens > 0) {
      d.setTextColor(TFT_DARKGREY);
      char tk[32];
      snprintf(tk, sizeof(tk), "tok %lu", (unsigned long)hb.tokens);
      d.drawString(tk, 4, y); y += 14;
    }
  } else {
    d.setTextColor(TFT_DARKGREY);
    d.drawString("Idle", 4, y);
  }
  // Stats footer
  d.setTextColor(TFT_DARKGREY);
  char st[32];
  snprintf(st, sizeof(st), "appr %lu  deny %lu",
           (unsigned long)statApprove, (unsigned long)statDeny);
  d.drawString(st, 4, d.height() - 12);
  d.endWrite();
}

// Chrome (text + pills + footer) lives in the top CHAR_REST_Y pixels,
// above the character animation. The GIF slot (y >= CHAR_REST_Y) is
// painted by tickGifPlayback and we never touch it from here.
static void drawPromptChrome(bool fullClear) {
  auto& d = M5.Display;
  d.startWrite();
  if (fullClear) {
    d.fillScreen(TFT_BLACK);
  } else {
    // Clear only the chrome area; leave the GIF below alone.
    d.fillRect(0, 0, d.width(), CHAR_REST_Y, TFT_BLACK);
  }
  {
    d.setTextDatum(top_left);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    int yText = 4;
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.drawString(hb.promptTool.substring(0, 22).c_str(), 4, yText);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.drawString(hb.promptHint.substring(0, 22).c_str(), 4, yText + 12);

    // Two option pills, side by side
    const int pillY = 36, pillH = 28;
    const int gap = 4;
    int pillW = (d.width() - 3 * gap) / 2;
    int x0 = gap;                       // Approve
    int x1 = gap + pillW + gap;         // Deny
    bool apprActive = (activeChoice == Choice::Approve);
    bool denyActive = (activeChoice == Choice::Deny);

    // Approve pill (green)
    if (apprActive) {
      d.fillRoundRect(x0, pillY, pillW, pillH, 6, TFT_DARKGREEN);
      d.drawRoundRect(x0, pillY, pillW, pillH, 6, TFT_GREENYELLOW);
      d.setTextColor(TFT_WHITE, TFT_DARKGREEN);
    } else {
      d.drawRoundRect(x0, pillY, pillW, pillH, 6, TFT_DARKGREY);
      d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    }
    d.setTextDatum(middle_center);
    d.drawString("Approve", x0 + pillW / 2, pillY + pillH / 2);

    // Deny pill (red)
    if (denyActive) {
      d.fillRoundRect(x1, pillY, pillW, pillH, 6, TFT_MAROON);
      d.drawRoundRect(x1, pillY, pillW, pillH, 6, TFT_RED);
      d.setTextColor(TFT_WHITE, TFT_MAROON);
    } else {
      d.drawRoundRect(x1, pillY, pillW, pillH, 6, TFT_DARKGREY);
      d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    }
    d.drawString("Deny", x1 + pillW / 2, pillY + pillH / 2);

    // Footer hint, placed ~1/3 of the way down between the pills and the
    // GIF. Gives the buttons a bit of breathing room above and leaves the
    // bigger gap toward the character.
    d.setTextDatum(top_left);
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    int gapTop = pillY + pillH;
    int gapH   = CHAR_REST_Y - gapTop;
    d.drawString("B=cycle  A=confirm", 6, gapTop + gapH / 3);
  }
  d.endWrite();
}

// Advances the GIF by one frame (if its inter-frame delay has elapsed).
// The frame is drawn directly into the display by GIFDraw().
static void tickGifPlayback() {
  if (!gifOpen) return;
  uint32_t now = millis();
  if (now < gifNextFrameMs) return;
  int frameDelay = 0;
  int more = gif.playFrame(false, &frameDelay);
  gifNextFrameMs = now + (frameDelay > 0 ? frameDelay : 80);
  if (more == 0) gif.reset();
}

static void drawDecisionFeedback() {
  auto& d = M5.Display;
  uint16_t bg = lastDecisionWasApprove ? TFT_DARKGREEN : TFT_MAROON;
  uint16_t fg = lastDecisionWasApprove ? TFT_GREENYELLOW : TFT_RED;
  d.startWrite();
  d.fillScreen(bg);
  d.setTextDatum(middle_center);
  d.setFont(&fonts::Font4);
  d.setTextColor(TFT_WHITE, bg);
  d.drawString(lastDecisionWasApprove ? "Approved" : "Denied",
               d.width() / 2, d.height() / 2 - 12);
  d.setFont(&fonts::Font0);
  d.setTextColor(fg, bg);
  d.drawString(hb.promptTool.substring(0, 22).c_str(),
               d.width() / 2, d.height() / 2 + 14);
  d.endWrite();
}

// ---- Mode transitions ----
static void onPromptArrived() {
  if (mode == UIMode::PromptActive) {
    // Updated prompt while one is already showing — refresh chrome but
    // keep the GIF running (user is already focused).
    drawPromptChrome(/*fullClear=*/false);
    return;
  }
  screenWake();
  activeChoice = Choice::Approve;  // friendlier default; tap A to accept
  startGifPlayback();
  drawPromptChrome(/*fullClear=*/true);
  // Skip the audio chime if the user is already looking at the screen.
  if (!faceUp) startMelody();
  enterMode(UIMode::PromptActive);
}

static void onPromptCleared() {
  // Heartbeat without prompt arrived — desktop may have resolved it
  // independently. Drop back to idle if we were showing it.
  if (mode == UIMode::PromptRevealing || mode == UIMode::PromptActive) {
    stopMelody();
    stopGifPlayback();
    enterMode(UIMode::IdleSleeping);
    screenSleep();
  }
}

static void confirmActiveChoice() {
  bool approve = (activeChoice == Choice::Approve);
  lastDecisionWasApprove = approve;
  sendPermission(approve ? "once" : "deny");
  if (approve) statApprove++; else statDeny++;
  prefs.putUInt("appr", statApprove);
  prefs.putUInt("deny", statDeny);
  hb.promptActive = false;
  stopMelody();
  stopGifPlayback();
  drawDecisionFeedback();
  enterMode(UIMode::DecisionFeedback);
}

// ---- Inbound JSON ----
static void handleHeartbeat(JsonDocument& d) {
  hb.fresh = true;
  hb.lastMs = millis();
  hb.total = d["total"] | 0;
  hb.running = d["running"] | 0;
  hb.waiting = d["waiting"] | 0;
  hb.msg = (const char*)(d["msg"] | "");
  hb.tokens = d["tokens"] | 0;

  bool wasActive = hb.promptActive;
  if (d["prompt"].is<JsonObject>()) {
    JsonObject p = d["prompt"];
    hb.promptActive = true;
    hb.promptId = (const char*)(p["id"] | "");
    hb.promptTool = (const char*)(p["tool"] | "");
    hb.promptHint = (const char*)(p["hint"] | "");
    onPromptArrived();
  } else {
    hb.promptActive = false;
    if (wasActive) onPromptCleared();
  }
}

static void handleLine(const String& line) {
  if (line.length() == 0) return;
  JsonDocument d;
  if (deserializeJson(d, line)) return;

  if (d["time"].is<JsonArray>()) {
    int64_t epoch = d["time"][0] | 0;
    if (epoch > 0) {
      struct timeval tv = { (time_t)epoch, 0 };
      settimeofday(&tv, nullptr);
    }
    return;
  }
  if (d["evt"].is<const char*>()) return;

  const char* cmd = d["cmd"] | "";
  if      (strcmp(cmd, "status") == 0) sendStatus();
  else if (strcmp(cmd, "name") == 0) {
    deviceName = (const char*)(d["name"] | deviceName.c_str());
    prefs.putString("name", deviceName);
    sendAck("name");
  } else if (strcmp(cmd, "owner") == 0) {
    ownerName = (const char*)(d["name"] | "");
    prefs.putString("owner", ownerName);
    sendAck("owner");
  } else if (strcmp(cmd, "unpair") == 0) {
    NimBLEDevice::deleteAllBonds();
    sendAck("unpair");
  } else if (strcmp(cmd, "char_begin") == 0 || strcmp(cmd, "char_end") == 0
          || strcmp(cmd, "file") == 0       || strcmp(cmd, "chunk") == 0
          || strcmp(cmd, "file_end") == 0) {
    return;  // folder push not supported (yet)
  } else if (cmd[0] == '\0') {
    handleHeartbeat(d);
  }
}

// ---- BLE callbacks ----
class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    std::string v = c->getValue();
    rxBuffer += String(v.c_str());
    int nl;
    while ((nl = rxBuffer.indexOf('\n')) >= 0) {
      String line = rxBuffer.substring(0, nl);
      rxBuffer.remove(0, nl + 1);
      line.trim();
      handleLine(line);
    }
    if (rxBuffer.length() > 8192) rxBuffer = "";
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo&) override {
    bleConnected = true;
    bleMtu = 23;
    // From idle-disconnected we drop to sleep; the user shouldn't need to
    // see "connected" once we've paired once. If pairing is needed,
    // onPassKeyDisplay() will wake us back up momentarily.
    if (mode == UIMode::IdleDisconnected) {
      enterMode(UIMode::IdleSleeping);
      screenSleep();
    }
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    bleConnected = false;
    bleBonded = false;
    hb.fresh = false;
    hb.promptActive = false;
    enterMode(UIMode::IdleDisconnected);
    screenWake();
    drawIdleDisconnected();
    NimBLEDevice::startAdvertising();
  }
  void onMTUChange(uint16_t MTU, NimBLEConnInfo&) override { bleMtu = MTU; }

  // Called when the peer initiates pairing and we have IO=DisplayOnly.
  // We generate a 6-digit passkey, show it, and return it — the peer
  // (Claude Desktop) will prompt the user to type the same code.
  uint32_t onPassKeyDisplay() override {
    uint32_t pin = (esp_random() % 900000u) + 100000u;
    pendingPasskey = pin;
    screenWake();
    drawPasskeyScreen(pin);
    enterMode(UIMode::Pairing);
    Serial.printf("[ble] pairing passkey: %06u\n", (unsigned)pin);
    return pin;
  }

  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    bool ok = info.isEncrypted();
    bleBonded = ok && info.isBonded();
    pendingPasskey = 0;
    Serial.printf("[ble] auth complete: enc=%d bonded=%d\n",
                  info.isEncrypted(), info.isBonded());
    if (mode == UIMode::Pairing) {
      enterMode(UIMode::IdleSleeping);
      screenSleep();
    }
  }
};

  d.drawString("bd ol be bg r2 c2", xStart + 12, yE + cellH + 6);
}

// ---- Setup / loop ----
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(0);
  M5.Display.setBrightness(120);

  // Speaker for the prompt-arrival chime. Volume 0..255.
  M5.Speaker.begin();
  M5.Speaker.setVolume(255);


  prefs.begin("buddy", false);
  String defaultName = String("Claude-") + macSuffix();
  deviceName = prefs.getString("name", defaultName);
  ownerName = prefs.getString("owner", "");
  statApprove = prefs.getUInt("appr", 0);
  statDeny = prefs.getUInt("deny", 0);

  gif.begin(GIF_PALETTE_RGB888);

  Serial.begin(115200);
  delay(50);
  Serial.printf("[boot] %s ready (heap=%u)\n", deviceName.c_str(), ESP.getFreeHeap());

  NimBLEDevice::init(deviceName.c_str());
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  // LE Secure Connections with bonding + MITM protection, DisplayOnly IO.
  // Pairs via "Passkey Entry: Peripheral Displays" — we generate a
  // 6-digit code, show it, the desktop user types it in. After bonding
  // the LTK is persisted in NVS and reconnects are silent.
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);

  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  NimBLEService* svc = server->createService(NUS_SERVICE);
  NimBLECharacteristic* rx = svc->createCharacteristic(
      NUS_RX_CHAR,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
        | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN);
  rx->setCallbacks(new RxCallbacks());
  txChar = svc->createCharacteristic(
      NUS_TX_CHAR,
      NIMBLE_PROPERTY::NOTIFY
        | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN);

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setName(deviceName.c_str());
  adv->addServiceUUID(NUS_SERVICE);
  adv->enableScanResponse(true);
  NimBLEDevice::startAdvertising();

  enterMode(UIMode::IdleDisconnected);
  drawIdleDisconnected();
}

void loop() {
  M5.update();
  uint32_t now = millis();

  // Heartbeat freshness.
  if (hb.fresh && now - hb.lastMs > 30000) hb.fresh = false;

  // IMU sampling runs unconditionally so we always know orientation and
  // can pick up shakes mid-prompt.
  tickIMU();

  // Melody (alert chime) — runs independently of the UI state machine
  // so it can keep playing while the GIF animates.
  tickMelody();

  switch (mode) {
    case UIMode::IdleDisconnected:
      // Stay until BLE connects.
      break;

    case UIMode::IdleSleeping:
      if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed()) {
        screenWake();
        drawIdleGlance();
        enterMode(UIMode::IdleGlancing);
      }
      break;

    case UIMode::IdleGlancing:
      // Auto-sleep after 6s, or any second press dismisses.
      if (now - modeEnteredMs > 6000) {
        screenSleep();
        enterMode(UIMode::IdleSleeping);
      }
      break;

    case UIMode::Pairing:
      // The screen stays up until onAuthenticationComplete fires (or
      // disconnect resets us). Refresh occasionally in case anything
      // overpainted it.
      break;

    case UIMode::PromptRevealing:
      // No longer used (the GIF's first held frame is the reveal); kept
      // in the enum to preserve API compatibility with future variations.
      break;

    case UIMode::PromptActive:
      if (consumeShake()) {
        // Vigorous shake = dismiss = Deny.
        activeChoice = Choice::Deny;
        confirmActiveChoice();
      } else if (M5.BtnB.wasPressed()) {
        activeChoice = (activeChoice == Choice::Approve) ? Choice::Deny : Choice::Approve;
        drawPromptChrome(/*fullClear=*/false);
      } else if (M5.BtnA.wasPressed()) {
        confirmActiveChoice();
      } else {
        tickGifPlayback();
      }
      // If desktop independently resolved the prompt, hb.promptActive
      // gets cleared in handleHeartbeat → onPromptCleared.
      break;

    case UIMode::DecisionFeedback:
      if (now - modeEnteredMs > 900) {
        screenSleep();
        enterMode(UIMode::IdleSleeping);
      }
      break;
  }

  delay(5);
}
