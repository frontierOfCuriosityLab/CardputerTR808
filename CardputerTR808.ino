// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ryu_muto
// The structure of the UI / keyboard / audio-task code descends from Cardputer-Adv-Tracker
// (MIT, see LICENSE-CardputerTracker; https://github.com/qwertyuu/Cardputer-Adv-Tracker)
// via CardputerDaisyTracker and CardputerDrumTracker.
// ============================================================
// CardputerTR808 - TR-808 style rhythm machine
// For M5Stack Cardputer / Cardputer-Adv (ESP32-S3)
// ============================================================
// 11 voices (BD SD HT LT CL RS CP CB CY OH CH) on a 16-step grid that the playhead
// walks through from left to right, 4 patterns (A-D), a sound-edit page per voice
// and per-voice mute. Every step is off / on / accent.
//
// All DSP and the sequencer live in TR808Engine.h (host-testable, see
// test/host_test.cpp). This file is only UI, keyboard and audio plumbing.
//
// Core layout:
//   Core 1: audioTask - engine.render() -> Speaker.playRaw()
//   Core 0: uiTask    - keyboard, display; M5Unified's speaker task
// ============================================================

#include "M5Cardputer.h"
#include <Preferences.h>
#include "TR808Engine.h"

#define AUDIO_BUF_LEN 256
#define SCREEN_W      240
#define SCREEN_H      135

enum Page : uint8_t { PAGE_SEQ = 0, PAGE_EDIT, PAGE_HELP, PAGE_COUNT };

static constexpr int kMasterTab = kNumTracks;   // Edit page: tab 11 = MS (volume / bpm / swing)

// ============================================================
// GLOBALS
// ============================================================
TR808Engine engine;

// Editing cursor (UI only)
uint8_t curTrack = 0;                 // 0-10 = BD SD HT LT CL RS CP CB CY OH CH
uint8_t curStep = 0;                  // 0-15
uint8_t edTab = 0;                    // Edit page: 0-10 = voice, 11 = master
uint8_t edRow = 0;
uint8_t demoIdx = 0;
Page    curPage = PAGE_SEQ;
volatile bool needRedraw = true;

static M5Canvas canvas(&M5Cardputer.Display);

const uint16_t trackColors[kNumTracks] = {
  0xF800, 0xFD20, 0xFFE0, 0xBFE0, 0x07E0, 0x07F3, 0x07FF, 0x3B7F, 0xA33F, 0xF81F, 0xFBB6 };
const uint16_t kDimColor    = 0x4208;
const uint16_t kMasterColor = 0xC618;

// The 808's step buttons come in four colours, one per group of four steps
const uint16_t kGroupOn[4]  = { 0xB000, 0xB380, 0xB580, 0xB596 };   // plain step
const uint16_t kGroupAcc[4] = { 0xF800, 0xFD20, 0xFFE0, 0xFFFF };   // accent

Pattern& pat() { return engine.patterns[engine.curPattern]; }

// Short message in the header (SAVED, LOADED, ...)
static char     toastMsg[16] = "";
static uint32_t toastUntil = 0;
static uint32_t clearArmedUntil = 0;

static void toast(const char* msg, uint32_t ms = 1400) {
  strncpy(toastMsg, msg, sizeof(toastMsg) - 1);
  toastMsg[sizeof(toastMsg) - 1] = 0;
  toastUntil = millis() + ms;
  needRedraw = true;
}

// ============================================================
// AUDIO PLUMBING
// ============================================================
// Three buffers in rotation, gated by the speaker's buffer-release callback:
// with only two, the DMA could still be reading a buffer while we refilled it.
static constexpr size_t kBufCount = 3;
int16_t audioBuf[kBufCount][AUDIO_BUF_LEN];
std::atomic<bool> bufBusy[kBufCount] = { { false }, { false }, { false } };

TaskHandle_t audioTaskHandle = NULL;
TaskHandle_t uiTaskHandle = NULL;

void audioTask(void* param);
void uiTask(void* param);

void onBufferReleased(void* args, const void* data, uint8_t channel) {
  for (size_t i = 0; i < kBufCount; ++i) {
    if (data == audioBuf[i]) { bufBusy[i] = false; return; }
  }
}

void audioTask(void* param) {
  size_t idx = 0;
  while (true) {
    while (bufBusy[idx]) vTaskDelay(1);

    int16_t* buf = audioBuf[idx];
    engine.render(buf, AUDIO_BUF_LEN);

    bufBusy[idx] = true;  // before playRaw(): the release can fire right after queuing
    while (!M5Cardputer.Speaker.playRaw(buf, AUDIO_BUF_LEN, kSampleRate, false, 1, 0)) {
      vTaskDelay(1);
    }
    if (++idx >= kBufCount) idx = 0;
  }
}

// ============================================================
// SAVE / LOAD (NVS)
// ============================================================
static bool saveToFlash() {
  static uint8_t blob[TR808Engine::kBlobSize];
  size_t n = engine.serialize(blob, sizeof(blob));
  if (!n) return false;
  Preferences pr;
  if (!pr.begin("tr808", false)) return false;
  size_t w = pr.putBytes("song", blob, n);
  pr.end();
  return w == n;
}

static bool loadFromFlash() {
  static uint8_t blob[TR808Engine::kBlobSize];
  Preferences pr;
  if (!pr.begin("tr808", true)) return false;   // read-only: fails if the namespace does not exist yet
  size_t len = pr.getBytesLength("song");
  bool ok = false;
  if (len == TR808Engine::kBlobSize) {
    pr.getBytes("song", blob, len);
    ok = engine.deserialize(blob, len);
  }
  pr.end();
  return ok;
}

// ============================================================
// DISPLAY - HEADER (shared by every page)
// ============================================================
//  BPM120 >>  [A][B][C][D]  status / toast        VOL 70
//  The yellow box is the pattern being edited, the green outline the one playing
//  (they differ while a switch waits for the bar line).
static void drawHeader(const char* status) {
  canvas.setTextSize(1);
  canvas.setTextColor(TFT_WHITE);
  canvas.setCursor(1, 2);
  canvas.printf("BPM%3d", engine.bpm);

  canvas.setCursor(40, 2);
  if (engine.playing) { canvas.setTextColor(0x07E0); canvas.print(">>"); }
  else                { canvas.setTextColor(0xF800); canvas.print("||"); }

  for (int p = 0; p < kNumPatterns; p++) {
    int x = 56 + p * 12;
    bool editing = (p == engine.curPattern);
    bool playingNow = engine.playing && p == engine.playPattern;
    if (editing) canvas.fillRect(x, 1, 11, 10, 0xFFE0);
    canvas.drawRect(x, 1, 11, 10, playingNow ? 0x07E0 : (editing ? 0xFFE0 : 0x2104));
    canvas.setTextColor(editing ? TFT_BLACK : 0x7BEF);
    canvas.setCursor(x + 3, 2);
    canvas.printf("%c", 'A' + p);
  }

  canvas.setCursor(108, 2);
  if (millis() < toastUntil) { canvas.setTextColor(0xFFE0); canvas.print(toastMsg); }
  else if (status)           { canvas.setTextColor(0x7BEF); canvas.print(status); }

  canvas.setCursor(196, 2);
  canvas.setTextColor(0x07E0);
  canvas.printf("VOL%3d", (int)(engine.masterVol * 100 + 0.5f));
  canvas.drawFastHLine(0, 12, SCREEN_W, 0x2104);
}

// ============================================================
// DISPLAY - SEQUENCER PAGE
// ============================================================
//  11 rows (voices) x 16 columns (steps): the playhead moves left -> right.
static constexpr int kGridX = 24, kCellW = 11, kColW = 13, kGridY = 15, kRowH = 10;

static int cellX(int s) { return kGridX + s * kColW + (s / 4) * 2; }

void drawSeqPage() {
  canvas.fillSprite(TFT_BLACK);

  const bool showHead = engine.playing && engine.playPattern == engine.curPattern;
  const int playStep = engine.lastStep;

  // status in the header: voice, step number and its state
  char status[16];
  {
    uint8_t v = pat().c[curTrack][curStep];
    snprintf(status, sizeof(status), "%s %2d %s", trackNames[curTrack], curStep + 1, v == STEP_ACCENT ? "ACC" : (v ? "ON" : "--"));
  }
  drawHeader(status);

  // === Rows ===
  for (int t = 0; t < kNumTracks; t++) {
    const int y = kGridY + t * kRowH;
    const bool muted = engine.mute[t];
    const bool isCurRow = (t == curTrack);

    if (isCurRow) canvas.fillRect(0, y - 1, 22, 10, 0x2945);
    canvas.setTextColor(muted ? kDimColor : (isCurRow ? TFT_WHITE : 0xBDF7));
    canvas.setCursor(2, y);
    canvas.print(trackNames[t]);
    if (muted) canvas.drawFastHLine(1, y + 3, 20, 0xF800);   // struck through = muted

    for (int s = 0; s < kNumSteps; s++) {
      const int x = cellX(s);
      const uint8_t v = pat().c[t][s];
      const bool head = showHead && s == playStep;
      const int g = s / 4;

      uint16_t col;
      if (v == STEP_OFF) col = head ? 0x5AEB : ((g & 1) ? 0x2104 : 0x2945);
      else if (muted)    col = head ? 0x8410 : (v == STEP_ACCENT ? 0x632C : kDimColor);
      else if (head)     col = TFT_WHITE;
      else               col = (v == STEP_ACCENT) ? kGroupAcc[g] : kGroupOn[g];
      canvas.fillRect(x, y, kCellW, 8, col);
      if (v == STEP_ACCENT) canvas.fillRect(x, y, kCellW, 2, muted ? 0x8410 : TFT_WHITE);   // white cap = accent

      if (isCurRow && s == curStep) canvas.drawRect(x - 1, y - 1, kCellW + 2, 10, TFT_WHITE);
    }
  }

  // === Footer: playhead marker + step numbers (beat starts, and the cursor's step) ===
  if (showHead) canvas.fillRect(cellX(playStep), 125, kCellW, 2, TFT_WHITE);
  for (int b = 0; b < 4; b++) {
    if (curStep / 4 == b && curStep % 4 == 0) continue;   // drawn below, highlighted
    canvas.setTextColor(kGroupAcc[b]);
    canvas.setCursor(cellX(b * 4), 127);
    canvas.printf("%d", b * 4 + 1);
  }
  canvas.setTextColor(TFT_WHITE);
  canvas.setCursor(cellX(curStep), 127);
  canvas.printf("%d", curStep + 1);

  canvas.pushSprite(0, 0);
}

// ============================================================
// DISPLAY - EDIT PAGE
// ============================================================
static void drawBar(int y, int selected, const char* label, float v01, uint16_t col, const char* value) {
  const int labelX = 6, barX = 46, barW = 70, valX = 122, barH = 7;
  if (selected) {
    canvas.fillRect(0, y - 2, 160, 11, 0x1082);
    canvas.setTextColor(TFT_WHITE);
    canvas.setCursor(0, y);
    canvas.print(">");
  }
  canvas.setTextColor(selected ? TFT_WHITE : 0xBDF7);
  canvas.setCursor(labelX, y);
  canvas.print(label);
  canvas.drawRect(barX, y, barW, barH, selected ? 0x7BEF : 0x4208);
  canvas.fillRect(barX + 1, y + 1, (int)((barW - 2) * constrain(v01, 0.0f, 1.0f)), barH - 2, col);
  canvas.setTextColor(selected ? TFT_WHITE : 0x9CD3);
  canvas.setCursor(valX, y);
  canvas.print(value);
}

static int edRows() { return (edTab < kNumTracks) ? kNumParams : 3; }

void drawEditPage() {
  canvas.fillSprite(TFT_BLACK);
  drawHeader("[EDIT]");

  // === Tabs: BD SD HT LT CL RS CP CB CY OH CH MS ===
  for (int i = 0; i <= kMasterTab; i++) {
    const int x = i * 20;
    const uint16_t col = (i < kNumTracks) ? trackColors[i] : kMasterColor;
    const bool muted = (i < kNumTracks) && engine.mute[i];
    if (i == edTab) {
      canvas.fillRect(x, 14, 19, 10, muted ? kDimColor : col);
      canvas.setTextColor(TFT_BLACK);
    } else {
      canvas.drawRect(x, 14, 19, 10, 0x2104);
      canvas.setTextColor(muted ? kDimColor : col);
    }
    canvas.setCursor(x + 4, 15);
    canvas.print(i < kNumTracks ? trackNames[i] : "MS");
    if (muted) canvas.drawFastHLine(x + 2, 19, 15, 0xF800);
  }

  const uint16_t tabCol = (edTab < kNumTracks) ? trackColors[edTab] : kMasterColor;
  canvas.setTextColor(tabCol);
  canvas.setCursor(4, 29);
  canvas.print(edTab < kNumTracks ? trackTitles[edTab] : "MASTER");
  if (edTab < kNumTracks && engine.mute[edTab]) {
    canvas.setTextColor(0xF800);
    canvas.setCursor(100, 29);
    canvas.print("MUTED");
  }

  // === Parameter rows ===
  int y = 44;
  char val[16];
  if (edTab < kNumTracks) {
    for (int i = 0; i < kNumParams; i++) {
      float v = engine.dp[edTab].p[i];
      if (i == P_TUNE) snprintf(val, sizeof(val), "%dHz", (int)(TR808Engine::tuneHz(edTab, v) + 0.5f));
      else             snprintf(val, sizeof(val), "%d%%", (int)(v * 100 + 0.5f));
      drawBar(y, edRow == i, paramNames[edTab][i], v, i == P_LEVEL ? 0x07E0 : tabCol, val);
      y += 12;
    }
  } else {
    snprintf(val, sizeof(val), "%d%%", (int)(engine.masterVol * 100 + 0.5f));
    drawBar(y, edRow == 0, "Volume", engine.masterVol, 0x07E0, val);
    y += 12;
    snprintf(val, sizeof(val), "%d", engine.bpm);
    drawBar(y, edRow == 1, "BPM", (engine.bpm - 40) / 260.0f, kMasterColor, val);
    y += 12;
    snprintf(val, sizeof(val), "%d%%", 50 + (int)(engine.swing * TR808Engine::kMaxSwing * 50.0f + 0.5f));
    drawBar(y, edRow == 2, "Swing", engine.swing, 0xFBE0, val);
  }

  canvas.setTextColor(0x7BEF);
  canvas.setCursor(4, 98);
  canvas.print("Q/E voice  W/S param  A/D value");
  canvas.setCursor(4, 108);
  canvas.print("Shift+A/D fine   Z/ENTER hear");
  canvas.setCursor(4, 118);
  canvas.print("M mute  1-4 pattern  SPACE play");

  // === Right side: scope (last ~90 ms of the master bus) ===
  const int scopeX = 164, scopeY = 28, scopeW = 72, scopeH = 44;
  canvas.drawRect(scopeX, scopeY, scopeW, scopeH, 0x2104);
  int midY = scopeY + scopeH / 2;
  canvas.drawFastHLine(scopeX, midY, scopeW, 0x1082);
  int start = engine.scopeIdx;
  int prevY = midY;
  for (int i = 0; i < scopeW; i++) {
    float sv = engine.scope[(start + i * kScopeLen / scopeW) % kScopeLen];
    int yy = constrain(midY - (int)(sv * (scopeH / 2 - 2)), scopeY + 1, scopeY + scopeH - 2);
    if (i > 0) canvas.drawLine(scopeX + i - 1, prevY, scopeX + i, yy, tabCol);
    prevY = yy;
  }

  canvas.pushSprite(0, 0);
}

// ============================================================
// DISPLAY - HELP PAGE
// ============================================================
void drawHelpPage() {
  canvas.fillSprite(TFT_BLACK);

  int y = 2;
  canvas.setTextSize(1);
  canvas.setTextColor(0xF800);
  canvas.setCursor(2, y); canvas.print("CARDPUTER TR-808");
  y += 12;
  canvas.drawFastHLine(0, y - 2, SCREEN_W, 0x2104);

  auto line = [&](uint16_t c, const char* t) {
    canvas.setTextColor(c);
    canvas.setCursor(2, y); canvas.print(t);
    y += 9;
  };
  line(0x07E0, "SPACE play/stop   TAB/BtnA page");
  line(TFT_WHITE, "W/S voice  A/D step  Q/E +-4 steps");
  line(TFT_WHITE, "  (; . , /  work as arrow keys)");
  line(0xFFE0, "ENTER off>on>accent   DEL clear step");
  line(0xFFE0, "Z hear the selected voice");
  line(0xFBE0, "M mute voice   1-4 pattern A-D");
  line(0xFBE0, "  (a switch waits for the bar line)");
  line(TFT_WHITE, "[ ] bpm (Shift: 10)   - = volume");
  line(0x07FF, "P demo groove  \\ x2 clear pattern");
  line(0x07FF, "K save  L load  (NVS flash)");
  line(0xF81F, "EDIT: Q/E voice W/S param A/D value");
  line(0x4A49, "BD SD HT LT CL RS CP CB CY OH CH");

  canvas.pushSprite(0, 0);
}

void drawScreen() {
  switch (curPage) {
    case PAGE_SEQ:  drawSeqPage();  break;
    case PAGE_EDIT: drawEditPage(); break;
    case PAGE_HELP: drawHelpPage(); break;
    default: break;
  }
}

// ============================================================
// EDITING
// ============================================================
static void audition(int track, int level = STEP_ACCENT) { engine.preview(track, level); }

// off -> on -> accent -> off
static void cycleStep(int t, int s) {
  uint8_t& c = pat().c[t][s];
  c = (uint8_t)((c + 1) % 3);
  if (c) audition(t, c);
  needRedraw = true;
}

static float step01(float v, float d) { return constrain(v + d, 0.0f, 1.0f); }

static void selectTab(int tab) {
  edTab = (uint8_t)tab;
  if (tab < kNumTracks) curTrack = (uint8_t)tab;
  if (edRow >= edRows()) edRow = (uint8_t)(edRows() - 1);
  needRedraw = true;
}

// Edit page: A/D on the selected row. `quiet` = key auto-repeat (throttled audition).
static void adjustParam(int dir, bool fine, bool quiet) {
  static uint32_t lastAudition = 0;
  const float d = (fine ? 0.01f : 0.05f) * dir;
  if (edTab < kNumTracks) {
    volatile float& v = engine.dp[edTab].p[edRow];
    v = step01(v, d);
    engine.dirty.fetch_or((uint16_t)(1u << edTab));
    if (!engine.playing && (!quiet || millis() - lastAudition > 300)) {
      lastAudition = millis();
      audition(edTab);
    }
  } else {
    switch (edRow) {
      case 0: engine.masterVol = step01(engine.masterVol, d); break;
      case 1: engine.bpm = (uint16_t)constrain((int)engine.bpm + dir, 40, 300); break;
      case 2: engine.swing = step01(engine.swing, d); break;
    }
  }
  needRedraw = true;
}

static void changeBpm(int d) {
  engine.bpm = (uint16_t)constrain((int)engine.bpm + d, 40, 300);
  needRedraw = true;
}

static void changeVolume(int d) {
  engine.masterVol = step01(engine.masterVol, 0.05f * d);
  needRedraw = true;
}

static void nextPage() {
  curPage = (Page)((curPage + 1) % PAGE_COUNT);
  if (curPage == PAGE_EDIT) selectTab(curTrack);
  needRedraw = true;
}

static void toggleMute(int t) {
  engine.mute[t] = !engine.mute[t];
  char msg[16];
  snprintf(msg, sizeof(msg), "%s %s", engine.mute[t] ? "MUTE" : "UNMUTE", trackNames[t]);
  toast(msg);
}

// ============================================================
// INPUT HANDLING
// ============================================================
static bool isRepeatable(char k) {
  if (k >= 'A' && k <= 'Z') k = (char)(k + 32);
  switch (k) {
    case 'w': case 's': case 'a': case 'd': case 'q': case 'e':
    case ';': case '.': case ',': case '/': case '[': case ']': case '-': case '=':
    case '{': case '}': case '_': case '+':
      return true;
  }
  return false;
}

// One key. `repeat` = generated by holding the key down.
void handleKey(char raw, bool repeat) {
  const bool upper = (raw >= 'A' && raw <= 'Z');
  const char key = upper ? (char)(raw + 32) : raw;

  // ---- keys that mean the same on every page ----
  switch (key) {
    case ' ': if (!repeat) { engine.post(EV_TOGGLE_PLAY); needRedraw = true; } return;
    case '1': case '2': case '3': case '4':
      if (!repeat) { engine.curPattern = (uint8_t)(key - '1'); needRedraw = true; }
      return;
    case '[': changeBpm(-1);  return;
    case ']': changeBpm(+1);  return;
    case '{': changeBpm(-10); return;
    case '}': changeBpm(+10); return;
    case '-': case '_': changeVolume(-1); return;
    case '=': case '+': changeVolume(+1); return;
    case 'm':
      if (!repeat) toggleMute((curPage == PAGE_EDIT && edTab < kNumTracks) ? edTab : curTrack);
      return;
    case 'p':
      if (!repeat) { engine.loadDemo(engine.curPattern, demoIdx++); toast("DEMO"); }
      return;
    case 'k':
      if (!repeat) toast(saveToFlash() ? "SAVED" : "SAVE FAILED");
      return;
    case 'l':
      if (!repeat) toast(loadFromFlash() ? "LOADED" : "NO SAVE DATA");
      return;
    case '\\':
      if (repeat) return;
      if (millis() < clearArmedUntil) {
        engine.clearPattern(engine.curPattern);
        clearArmedUntil = 0;
        toast("CLEARED");
      } else {
        clearArmedUntil = millis() + 1500;
        toast("CLEAR? \\ x2", 1500);
      }
      return;
  }

  // ---- Edit page ----
  if (curPage == PAGE_EDIT) {
    switch (key) {
      case 'q': if (!repeat) selectTab((edTab + kMasterTab) % (kMasterTab + 1)); return;
      case 'e': if (!repeat) selectTab((edTab + 1) % (kMasterTab + 1)); return;
      case 'w': case ';': if (edRow > 0) edRow--; needRedraw = true; return;
      case 's': case '.': if (edRow < edRows() - 1) edRow++; needRedraw = true; return;
      case 'a': case ',': adjustParam(-1, upper, repeat); return;
      case 'd': case '/': adjustParam(+1, upper, repeat); return;
      case 'z': if (!repeat && edTab < kNumTracks) audition(edTab); return;
    }
    return;
  }

  // ---- Sequencer page ----
  switch (key) {
    case 'w': case ';': if (curTrack > 0) curTrack--; needRedraw = true; break;
    case 's': case '.': if (curTrack < kNumTracks - 1) curTrack++; needRedraw = true; break;
    case 'a': case ',': if (curStep > 0) curStep--; needRedraw = true; break;
    case 'd': case '/': if (curStep < kNumSteps - 1) curStep++; needRedraw = true; break;
    case 'q': curStep = (uint8_t)(curStep >= 4 ? curStep - 4 : 0); needRedraw = true; break;
    case 'e': curStep = (uint8_t)(curStep + 4 < kNumSteps ? curStep + 4 : kNumSteps - 1); needRedraw = true; break;
    case 'z': if (!repeat) audition(curTrack); break;
  }
}

void handleSpecial(bool tab, bool enter, bool del) {
  if (tab) nextPage();
  if (curPage == PAGE_HELP) return;
  if (enter) {
    if (curPage == PAGE_SEQ) cycleStep(curTrack, curStep);
    else if (edTab < kNumTracks) audition(edTab);
    needRedraw = true;
  }
  if (del && curPage == PAGE_SEQ) {
    pat().c[curTrack][curStep] = STEP_OFF;
    needRedraw = true;
  }
}

void handleInput() {
  static char     prevWord[8];
  static uint8_t  prevN = 0;
  static bool     prevTab = false, prevEnter = false, prevDel = false;
  static char     heldKey = 0;
  static uint32_t heldSince = 0, lastRepeat = 0;

  M5Cardputer.update();

  if (M5Cardputer.BtnA.wasPressed()) nextPage();

  auto& st = M5Cardputer.Keyboard.keysState();
  const uint32_t now = millis();

  // Keys that are down now but were not on the previous poll are "new"
  char word[8];
  uint8_t n = 0;
  for (char c : st.word) { if (n < sizeof(word)) word[n++] = c; }

  bool freshTab = st.tab && !prevTab, freshEnter = st.enter && !prevEnter, freshDel = st.del && !prevDel;
  prevTab = st.tab; prevEnter = st.enter; prevDel = st.del;

  for (uint8_t i = 0; i < n; i++) {
    bool wasDown = false;
    for (uint8_t j = 0; j < prevN; j++) if (prevWord[j] == word[i]) wasDown = true;
    if (wasDown) continue;
    handleKey(word[i], false);
    if (isRepeatable(word[i])) { heldKey = word[i]; heldSince = now; lastRepeat = now; }
  }
  memcpy(prevWord, word, n);
  prevN = n;

  if (freshTab || freshEnter || freshDel) handleSpecial(freshTab, freshEnter, freshDel);

  // Auto-repeat for the key that was pressed last, while it stays down
  if (heldKey) {
    bool stillDown = false;
    for (uint8_t i = 0; i < n; i++) if (word[i] == heldKey) stillDown = true;
    if (!stillDown) {
      heldKey = 0;
    } else if (now - heldSince > 380 && now - lastRepeat > 70) {
      lastRepeat = now;
      handleKey(heldKey, true);
    }
  }
}

// ============================================================
// UI TASK (Core 0)
// ============================================================
void uiTask(void* param) {
  bool lastPlaying = false;
  uint8_t lastStep = 0xFF, lastPlayPat = 0xFF;
  uint32_t lastScope = 0;
  bool toastShown = false;

  while (true) {
    handleInput();

    // Follow the audio task's sequencer
    bool playing = engine.playing;
    uint8_t step = engine.lastStep, pp = engine.playPattern;
    if (playing != lastPlaying || (playing && (step != lastStep || pp != lastPlayPat))) {
      lastPlaying = playing;
      lastStep = step;
      lastPlayPat = pp;
      needRedraw = true;
    }

    // Toast expiry
    bool toastNow = millis() < toastUntil;
    if (toastShown && !toastNow) needRedraw = true;
    toastShown = toastNow;

    // Edit page: live scope (~16 fps)
    if (curPage == PAGE_EDIT && millis() - lastScope > 60) {
      lastScope = millis();
      needRedraw = true;
    }

    if (needRedraw) {
      needRedraw = false;
      drawScreen();
    }

    delay(8);
  }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);

  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.fillScreen(TFT_BLACK);

  canvas.createSprite(SCREEN_W, SCREEN_H);
  canvas.setTextFont(1);
  canvas.setTextSize(1);

  auto spk_cfg = M5Cardputer.Speaker.config();
  spk_cfg.sample_rate = kSampleRate;
  spk_cfg.task_priority = 3;
  spk_cfg.dma_buf_count = 4;
  spk_cfg.dma_buf_len = AUDIO_BUF_LEN;
  spk_cfg.task_pinned_core = 0;   // keep M5Unified's I2S writer off the audio task's core
  M5Cardputer.Speaker.config(spk_cfg);
  M5Cardputer.Speaker.begin();
  M5Cardputer.Speaker.setVolume(200);
  M5Cardputer.Speaker.setBufferReleaseCallback(nullptr, onBufferReleased);

  engine.init();
  bool restored = loadFromFlash();
  if (!restored) {
    // First run: four demo grooves in A, B, C, D so there is something to play
    for (int i = 0; i < kNumPatterns; i++) engine.loadDemo(i, i);
    demoIdx = kNumPatterns;
  }

  // Splash screen
  canvas.fillSprite(TFT_BLACK);
  canvas.setTextSize(2);
  canvas.setTextColor(0xF800);
  canvas.setCursor(15, 8);
  canvas.print("CARDPUTER");
  canvas.setTextColor(0xFD20);
  canvas.setCursor(15, 28);
  canvas.print("TR-808");
  // the four step-button colours of the original
  for (int g = 0; g < 4; g++) canvas.fillRect(112 + g * 28, 30, 24, 10, kGroupAcc[g]);

  canvas.setTextSize(1);
  for (int t = 0; t < kNumTracks; t++) {
    canvas.setTextColor(trackColors[t]);
    canvas.setCursor(10 + t * 20, 54);
    canvas.print(trackNames[t]);
  }

  canvas.setTextColor(TFT_WHITE);
  canvas.setCursor(10, 72);
  canvas.print("SPACE=Play  TAB=Page");
  canvas.setCursor(10, 84);
  canvas.print("ENTER=Step  M=Mute");
  canvas.setCursor(10, 96);
  canvas.print("1-4=Pattern A-D");
  canvas.setTextColor(0x7BEF);
  canvas.setCursor(10, 108);
  canvas.print(restored ? "Saved song restored" : "Demo grooves in A B C D");

  canvas.setTextColor(0xFFE0);
  canvas.setCursor(10, 122);
  canvas.print("Press any key to start...");
  canvas.pushSprite(0, 0);

  while (true) {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) break;
    if (M5Cardputer.BtnA.wasPressed()) break;
    delay(50);
  }

  // Arduino's loop task is pinned to Core 1 and can't be moved, so real work
  // lives in two explicitly pinned tasks instead (see file header).
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, NULL, 1, &audioTaskHandle, 1);
  xTaskCreatePinnedToCore(uiTask,    "ui",    8192, NULL, 1, &uiTaskHandle,    0);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
