// Minimal host stand-in for M5Cardputer / Arduino / FreeRTOS, just enough to
// compile CardputerDrumTracker.ino on a PC (see ../ui_preview.cpp).
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <vector>
#include <string>
#include <atomic>
#include <math.h>

#define TFT_BLACK 0x0000
#define TFT_WHITE 0xFFFF
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))

extern uint32_t g_ms;
inline uint32_t millis() { return g_ms; }
inline void delay(uint32_t) {}

typedef void* TaskHandle_t;
typedef int BaseType_t;
inline void vTaskDelay(int) {}
#define pdMS_TO_TICKS(x) (x)
inline BaseType_t xTaskCreatePinnedToCore(void (*)(void*), const char*, int, void*, int, TaskHandle_t*, int) { return 1; }

// ---- display: records draw calls so the preview can turn them into SVG ----
struct DrawOp { char type; int x, y, w, h; uint16_t color; std::string text; };

struct DisplayStub { void setRotation(int) {} void fillScreen(uint16_t) {} };

struct M5Canvas {
  explicit M5Canvas(DisplayStub*) {}
  std::vector<DrawOp> ops, shown;   // ops = being drawn, shown = last pushSprite
  int cx = 0, cy = 0, ts = 1; uint16_t tc = 0xFFFF;
  void createSprite(int, int) {}
  void setTextFont(int) {}
  void setTextSize(int s) { ts = s; }
  void setTextColor(uint16_t c) { tc = c; }
  void setCursor(int x, int y) { cx = x; cy = y; }
  void fillSprite(uint16_t c) { ops.clear(); ops.push_back({'F', 0, 0, 240, 135, c, ""}); }
  void fillRect(int x, int y, int w, int h, uint16_t c) { ops.push_back({'f', x, y, w, h, c, ""}); }
  void drawRect(int x, int y, int w, int h, uint16_t c) { ops.push_back({'r', x, y, w, h, c, ""}); }
  void drawFastHLine(int x, int y, int w, uint16_t c) { ops.push_back({'l', x, y, w, 1, c, ""}); }
  void drawLine(int x0, int y0, int x1, int y1, uint16_t c) { ops.push_back({'L', x0, y0, x1, y1, c, ""}); }
  void print(const char* s) { ops.push_back({'T', cx, cy, (int)strlen(s) * 6 * ts, 8 * ts, tc, s}); cx += (int)strlen(s) * 6 * ts; }
  void printf(const char* f, ...) { char b[128]; va_list a; va_start(a, f); vsnprintf(b, sizeof(b), f, a); va_end(a); print(b); }
  void pushSprite(int, int) { shown = ops; }
};

// ---- keyboard / speaker / buttons ----
struct KeysStateStub {
  bool tab = false, fn = false, shift = false, ctrl = false, opt = false, alt = false, del = false, enter = false, space = false;
  std::vector<char> word;
};
struct KeyboardStub {
  KeysStateStub st;
  KeysStateStub& keysState() { return st; }
  bool isChange() { return false; }
  uint8_t isPressed() { return (uint8_t)st.word.size(); }
};
struct SpeakerStub {
  struct Cfg { int sample_rate = 0, task_priority = 0, dma_buf_count = 0, dma_buf_len = 0, task_pinned_core = 0; };
  Cfg config() { return Cfg(); }
  void config(const Cfg&) {}
  bool begin() { return true; }
  void setVolume(int) {}
  void setBufferReleaseCallback(void*, void (*)(void*, const void*, uint8_t)) {}
  bool playRaw(const int16_t*, size_t, uint32_t, bool, int, int) { return true; }
};
struct BtnStub { bool wasPressed() { return false; } };
struct M5CfgStub {};
struct M5Stub { M5CfgStub config() { return M5CfgStub(); } };
struct M5CardputerStub {
  DisplayStub Display; SpeakerStub Speaker; KeyboardStub Keyboard; BtnStub BtnA;
  void begin(const M5CfgStub&, bool) {}
  void update() {}
};
extern M5CardputerStub M5Cardputer;
extern M5Stub M5;
