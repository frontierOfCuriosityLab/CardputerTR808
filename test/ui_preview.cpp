// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ryu_muto
// Host-side UI test + preview: compiles CardputerTR808.ino itself against the
// stubs in test/stubs (no hardware needed), drives it with scripted key presses
// and checks the behaviour, then writes every screen as an SVG.
//   test/ui_preview.sh [outdir]      (default: $TMPDIR/tr808_ui)
// Text boxes are checked for overlaps and for leaving the 240x135 screen.
#include "stubs/M5Cardputer.h"
#include "stubs/Preferences.h"

uint32_t g_ms = 1000;
M5CardputerStub M5Cardputer;
M5Stub M5;

// The sketch under test (setup()/loop() and the FreeRTOS tasks are never run here)
#include "../CardputerTR808.ino"

#include <stdlib.h>
#include <sys/stat.h>

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

// ---------------------------------------------------------------- input helpers
static void frame(uint32_t dtMs = 8) { g_ms += dtMs; handleInput(); }

static void setKeys(const char* word, bool tab = false, bool enter = false, bool del = false, bool shift = false) {
  auto& st = M5Cardputer.Keyboard.st;
  st.word.assign(word, word + strlen(word));
  st.tab = tab; st.enter = enter; st.del = del; st.shift = shift;
}
static void release() { setKeys(""); frame(); }
static void tap(const char* word, bool tab = false, bool enter = false, bool del = false, bool shift = false) {
  setKeys(word, tab, enter, del, shift); frame(); release();
}
static void tapEnter() { tap("", false, true); }
static void tapDel()   { tap("", false, false, true); }
static void tapTab()   { tap("", true); }

// on the device the audio task drains the event ring every few ms
static void drain() { int16_t b[1]; engine.render(b, 1); }

static uint8_t& cell(int t, int s) { return engine.patterns[engine.curPattern].c[t][s]; }
static int countSteps(int pattern) {
  int n = 0;
  for (int t = 0; t < kNumTracks; t++) for (int s = 0; s < kNumSteps; s++) if (engine.patterns[pattern].c[t][s]) n++;
  return n;
}

static void resetUi() {
  engine.init();
  for (int i = 0; i < kNumPatterns; i++) engine.clearPattern(i);
  for (int t = 0; t < kNumTracks; t++) engine.mute[t] = false;
  engine.curPattern = 0; engine.playing = false; engine.playPattern = 0; engine.lastStep = 0;
  engine.bpm = 120; engine.masterVol = 0.7f;
  curTrack = 0; curStep = 0; edTab = 0; edRow = 0;
  curPage = PAGE_SEQ; toastUntil = 0; clearArmedUntil = 0; demoIdx = 0;
  M5Cardputer.Keyboard.st = KeysStateStub();
  drain();
  release();
}

// ---------------------------------------------------------------- behaviour tests
static void testSequencerPage() {
  printf("[ui] sequencer page: cursor, step editing\n");
  resetUi();

  // ENTER: off -> on -> accent -> off, the cursor stays on the step
  tapEnter(); CHECK(cell(T_BD, 0) == STEP_ON, "first ENTER = on (%d)", cell(T_BD, 0));
  tapEnter(); CHECK(cell(T_BD, 0) == STEP_ACCENT, "second ENTER = accent (%d)", cell(T_BD, 0));
  tapEnter(); CHECK(cell(T_BD, 0) == STEP_OFF, "third ENTER = off (%d)", cell(T_BD, 0));
  CHECK(curStep == 0 && curTrack == 0, "ENTER must not move the cursor");

  // audition: placing a step plays the voice
  drain();
  curTrack = T_CP; tapEnter();
  drain();
  CHECK(engine.trackActive(T_CP), "placing a step should sound the voice");
  curTrack = T_BD;

  // DEL clears
  curStep = 5; curTrack = T_SD; tapEnter(); tapEnter();
  CHECK(cell(T_SD, 5) == STEP_ACCENT, "setup");
  tapDel(); CHECK(cell(T_SD, 5) == STEP_OFF, "DEL should clear the step");

  // steps are independent per track / per pattern
  engine.clearPattern(0);
  curTrack = T_LT; curStep = 15; tapEnter();
  CHECK(cell(T_LT, 15) == STEP_ON && countSteps(0) == 1, "one step set (%d)", countSteps(0));
  tap("2"); CHECK(engine.curPattern == 1 && countSteps(1) == 0, "pattern B is empty");
  tapEnter(); CHECK(countSteps(1) == 1 && countSteps(0) == 1, "editing B leaves A alone");
  tap("1"); CHECK(engine.curPattern == 0, "1 = pattern A");

  // navigation, both key sets, clamped: W/S = voice (rows), A/D = step (columns)
  curTrack = 0; curStep = 0;
  tap("s"); tap("."); CHECK(curTrack == 2, "S and . move down (%d)", curTrack);
  tap("w"); tap(";"); tap(";"); CHECK(curTrack == 0, "W and ; move up, clamped (%d)", curTrack);
  for (int i = 0; i < 20; i++) tap("s");
  CHECK(curTrack == kNumTracks - 1, "S clamps at the last voice (%d)", curTrack);
  tap("d"); tap("/"); CHECK(curStep == 2, "D and / move right (%d)", curStep);
  tap("a"); tap(","); tap(","); CHECK(curStep == 0, "A and , move left, clamped (%d)", curStep);
  for (int i = 0; i < 30; i++) tap("d");
  CHECK(curStep == kNumSteps - 1, "D clamps at step 16 (%d)", curStep);
  curStep = 6; tap("q"); CHECK(curStep == 2, "Q = back one beat (%d)", curStep);
  tap("q"); CHECK(curStep == 0, "Q clamps at 0");
  curStep = 9; tap("e"); CHECK(curStep == 13, "E = forward one beat (%d)", curStep);
  tap("e"); CHECK(curStep == 15, "E clamps at 15 (%d)", curStep);

  // hear the voice
  curTrack = T_HT; drain(); tap("z"); drain();
  CHECK(engine.trackActive(T_HT), "Z should audition the selected voice");
  CHECK(countSteps(0) == 1, "Z must not edit the pattern");

  // pattern keys: only 1-4
  tap("3"); CHECK(engine.curPattern == 2, "3 = pattern C");
  tap("4"); CHECK(engine.curPattern == 3, "4 = pattern D");
  tap("5"); tap("0"); CHECK(engine.curPattern == 3, "5 / 0 must not change the pattern");
  tap("1");

  // mute
  curTrack = T_CY; tap("m"); CHECK(engine.mute[T_CY] && !engine.mute[T_BD], "M mutes the selected voice");
  CHECK(!strcmp(toastMsg, "MUTE CY"), "toast '%s'", toastMsg);
  tap("m"); CHECK(!engine.mute[T_CY], "M again unmutes");
  CHECK(!strcmp(toastMsg, "UNMUTE CY"), "toast '%s'", toastMsg);

  // bpm and volume
  engine.bpm = 120;
  tap("]"); CHECK(engine.bpm == 121, "]: +1 (%d)", engine.bpm);
  tap("["); tap("["); CHECK(engine.bpm == 119, "[: -1 (%d)", engine.bpm);
  tap("{"); CHECK(engine.bpm == 109, "Shift+[: -10 (%d)", engine.bpm);
  tap("}"); CHECK(engine.bpm == 119, "Shift+]: +10 (%d)", engine.bpm);
  engine.bpm = 41; tap("{"); CHECK(engine.bpm == 40, "bpm clamps at 40");
  engine.bpm = 299; tap("}"); CHECK(engine.bpm == 300, "bpm clamps at 300");
  engine.masterVol = 0.5f; tap("="); CHECK(fabsf(engine.masterVol - 0.55f) < 1e-4f, "= volume up (%f)", engine.masterVol);
  tap("-"); tap("-"); CHECK(fabsf(engine.masterVol - 0.45f) < 1e-4f, "- volume down (%f)", engine.masterVol);
  for (int i = 0; i < 40; i++) tap("=");
  CHECK(engine.masterVol == 1.0f, "volume clamps at 1");
  for (int i = 0; i < 40; i++) tap("-");
  CHECK(engine.masterVol == 0.0f, "volume clamps at 0");

  // play/stop goes through the event ring
  int16_t buf[8];
  drain();
  tap(" "); engine.render(buf, 8); CHECK(engine.playing, "SPACE should start playback");
  tap(" "); engine.render(buf, 8); CHECK(!engine.playing, "SPACE again should stop");

  // chord: holding 'd' while another key goes down must not repeat the step move
  resetUi();
  setKeys("d"); frame();
  setKeys("de"); frame();
  setKeys("d"); frame();
  release();
  CHECK(curStep == 1 + 4, "chord D then E: one step + one beat (%d)", curStep);
}

static void testEditPage() {
  printf("[ui] edit page: tabs, parameters, repeat, mute\n");
  resetUi();
  curTrack = T_SD;
  tapTab();  CHECK(curPage == PAGE_EDIT && edTab == T_SD, "TAB -> edit page on the selected voice (page %d, tab %d)", curPage, edTab);
  tapTab();  CHECK(curPage == PAGE_HELP, "TAB -> help page");
  tapTab();  CHECK(curPage == PAGE_SEQ, "TAB -> sequencer page");
  tapTab();

  // voices with Q/E: 11 voices + master, wraps
  edTab = 0; edRow = 0;
  tap("e"); CHECK(edTab == 1 && curTrack == 1, "E: next voice (%d), cursor follows (%d)", edTab, curTrack);
  tap("q"); tap("q"); CHECK(edTab == kMasterTab, "Q from BD wraps to the master tab (%d)", edTab);
  tap("e"); CHECK(edTab == 0, "E from master wraps to BD");
  int seen = 0; for (int i = 0; i <= kMasterTab; i++) { seen |= 1 << edTab; tap("e"); }
  CHECK(seen == (1 << (kMasterTab + 1)) - 1, "Q/E must visit all 12 tabs (mask %x)", seen);
  tap("q"); CHECK(curTrack == kNumTracks - 1 || curTrack < kNumTracks, "cursor stays a voice");
  // Q/E do not auto-repeat (a held key would spin through all the tabs)
  edTab = 0; setKeys("e"); frame(); for (int i = 0; i < 120; i++) frame(8); release();
  CHECK(edTab == 1, "holding E must not spin the tabs (tab %d)", edTab);

  // parameter rows and values
  selectTab(T_BD); edRow = 0;
  tap("s"); tap("s"); tap("s"); tap("s");
  CHECK(edRow == kNumParams - 1, "voice tabs have %d rows (row %d)", kNumParams, edRow);
  tap("w"); tap(";"); tap("w"); tap("w"); CHECK(edRow == 0, "W/; up, clamped (%d)", edRow);
  tap("s"); tap("s");
  float before = engine.dp[T_BD].p[P_TONE];
  setKeys("d"); frame();
  float once = engine.dp[T_BD].p[P_TONE];
  CHECK(fabsf(once - before - 0.05f) < 1e-4f, "one press = +0.05 (%f -> %f)", before, once);
  for (int i = 0; i < 20; i++) frame(8);          // 160 ms: below the repeat delay
  CHECK(engine.dp[T_BD].p[P_TONE] == once, "no repeat before 380 ms");
  for (int i = 0; i < 100; i++) frame(8);
  float held = engine.dp[T_BD].p[P_TONE];
  printf("  held D for ~1 s: %f -> %f\n", once, held);
  CHECK(held > once + 0.15f, "holding D should keep increasing the value");
  release();
  float after = engine.dp[T_BD].p[P_TONE];
  for (int i = 0; i < 50; i++) frame(8);
  CHECK(engine.dp[T_BD].p[P_TONE] == after, "value keeps changing after release");
  CHECK(engine.dirty.load() & 1, "BD parameter change must mark the voice dirty");

  // Shift+A/D = fine steps
  engine.dp[T_BD].p[P_TONE] = 0.5f;
  tap("D", false, false, false, true); CHECK(fabsf(engine.dp[T_BD].p[P_TONE] - 0.51f) < 1e-4f, "Shift+D = +0.01 (%f)", engine.dp[T_BD].p[P_TONE]);
  tap("A", false, false, false, true); tap("A", false, false, false, true);
  CHECK(fabsf(engine.dp[T_BD].p[P_TONE] - 0.49f) < 1e-4f, "Shift+A = -0.01 (%f)", engine.dp[T_BD].p[P_TONE]);

  // clamps
  for (int i = 0; i < 40; i++) tap("d");
  CHECK(engine.dp[T_BD].p[P_TONE] == 1.0f, "value clamps at 1");
  for (int i = 0; i < 40; i++) tap("a");
  CHECK(engine.dp[T_BD].p[P_TONE] == 0.0f, "value clamps at 0");

  // every voice reacts to its parameters in the right slot (dirty bit per voice)
  for (int t = 0; t < kNumTracks; t++) {
    selectTab(t); edRow = P_DECAY; drain();
    engine.dirty = 0;
    tap("d");
    CHECK(engine.dirty.load() == (1u << t), "%s: dirty mask %x", trackNames[t], engine.dirty.load());
  }

  // master tab: 3 rows: Volume, BPM, Swing
  selectTab(kMasterTab);
  for (int i = 0; i < 10; i++) tap("s");
  CHECK(edRow == 2, "master has 3 rows (row %d)", edRow);
  float s0 = engine.swing; tap("d"); CHECK(engine.swing > s0, "swing row: up");
  edRow = 1; engine.bpm = 120; tap("d"); CHECK(engine.bpm == 121, "BPM row: +1"); tap("a"); tap("a"); CHECK(engine.bpm == 119, "BPM row: -1");
  edRow = 0; float v0 = engine.masterVol; tap("a"); CHECK(engine.masterVol < v0, "volume row: down");
  edRow = 2; selectTab(T_CB); CHECK(edRow == 2, "row kept when it exists on the next tab");
  edRow = 3; selectTab(kMasterTab); CHECK(edRow == 2, "row clamped when switching to master (%d)", edRow);

  // mute on the edit page hits the shown voice; on the master tab it hits the cursor's voice
  selectTab(T_OH); tap("m"); CHECK(engine.mute[T_OH], "M on the edit page mutes the shown voice");
  tap("m"); CHECK(!engine.mute[T_OH], "M again unmutes");
  curTrack = T_CL; selectTab(kMasterTab); curTrack = T_CL; tap("m"); CHECK(engine.mute[T_CL], "M on master mutes the cursor voice");
  tap("m");

  // audition and pattern selection work here, ENTER never edits the pattern
  selectTab(T_LT); drain(); tap("z"); drain(); CHECK(engine.trackActive(T_LT), "Z auditions the shown voice");
  for (int t = 0; t < kNumTracks; t++) engine.preview(t, STEP_ACCENT);
  drain();
  tapEnter(); tapEnter();
  CHECK(countSteps(0) == 0, "ENTER on the edit page must not edit the pattern");
  tap("3"); CHECK(engine.curPattern == 2, "1-4 select a pattern on the edit page too");
  tapDel(); CHECK(countSteps(2) == 0, "DEL on the edit page does nothing");
}

static void testPersistenceAndClear() {
  printf("[ui] demo, clear confirmation, save/load of 4 patterns + mute\n");
  resetUi();
  tap("p");
  CHECK(countSteps(0) > 12, "P should load a demo groove (%d steps)", countSteps(0));
  CHECK(millis() < toastUntil && !strcmp(toastMsg, "DEMO"), "toast after P");
  int first = countSteps(0);
  tap("p"); CHECK(countSteps(0) != first || memcmp(&engine.patterns[0], &engine.patterns[1], sizeof(Pattern)) != 0, "P again loads the next groove");

  tap("\\");
  CHECK(countSteps(0) > 12, "a single \\ must not clear the pattern");
  tap("\\");
  CHECK(countSteps(0) == 0, "\\ twice clears the pattern (%d left)", countSteps(0));
  tap("\\"); g_ms += 2000; tap("\\");
  CHECK(!strcmp(toastMsg, "CLEAR? \\ x2"), "the confirmation should time out");

  Preferences::store().clear();
  tap("l"); CHECK(!strcmp(toastMsg, "NO SAVE DATA"), "L without a save: '%s'", toastMsg);
  for (int i = 0; i < kNumPatterns; i++) engine.loadDemo(i, i);
  engine.bpm = 133; engine.dp[T_SD].p[P_TONE] = 0.77f; engine.mute[T_CB] = true;
  Pattern keep[kNumPatterns]; memcpy(keep, (void*)engine.patterns, sizeof(keep));
  tap("k"); CHECK(!strcmp(toastMsg, "SAVED"), "K: '%s'", toastMsg);
  for (int i = 0; i < kNumPatterns; i++) engine.clearPattern(i);
  engine.bpm = 90; engine.dp[T_SD].p[P_TONE] = 0.1f; engine.mute[T_CB] = false;
  tap("l"); CHECK(!strcmp(toastMsg, "LOADED"), "L: '%s'", toastMsg);
  CHECK(engine.bpm == 133 && fabsf(engine.dp[T_SD].p[P_TONE] - 0.77f) < 1e-6f && engine.mute[T_CB], "globals / mute restored");
  CHECK(memcmp(keep, (void*)engine.patterns, sizeof(keep)) == 0, "all four patterns restored");
}

// ---------------------------------------------------------------- rendering
static std::string rgb(uint16_t c) {
  char b[16];
  snprintf(b, sizeof(b), "#%02x%02x%02x", ((c >> 11) & 31) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31);
  return b;
}
static std::string esc(const std::string& s) {
  std::string o;
  for (char ch : s) { if (ch == '&') o += "&amp;"; else if (ch == '<') o += "&lt;"; else if (ch == '>') o += "&gt;"; else o += ch; }
  return o;
}

static void writeSvg(const std::string& path, const std::vector<DrawOp>& ops) {
  FILE* f = fopen(path.c_str(), "w");
  if (!f) { printf("cannot write %s\n", path.c_str()); return; }
  const int S = 4;
  fprintf(f, "<svg xmlns='http://www.w3.org/2000/svg' width='%d' height='%d' viewBox='0 0 240 135'>\n", 240 * S, 135 * S);
  fprintf(f, "<rect width='240' height='135' fill='#000'/>\n");
  for (const DrawOp& o : ops) {
    std::string c = rgb(o.color);
    switch (o.type) {
      case 'F': case 'f': fprintf(f, "<rect x='%d' y='%d' width='%d' height='%d' fill='%s'/>\n", o.x, o.y, o.w, o.h, c.c_str()); break;
      case 'r': fprintf(f, "<rect x='%.1f' y='%.1f' width='%d' height='%d' fill='none' stroke='%s' stroke-width='1'/>\n", o.x + 0.5, o.y + 0.5, o.w - 1, o.h - 1, c.c_str()); break;
      case 'l': fprintf(f, "<rect x='%d' y='%d' width='%d' height='1' fill='%s'/>\n", o.x, o.y, o.w, c.c_str()); break;
      case 'L': fprintf(f, "<line x1='%d.5' y1='%d.5' x2='%d.5' y2='%d.5' stroke='%s' stroke-width='1'/>\n", o.x, o.y, o.w, o.h, c.c_str()); break;
      case 'T': fprintf(f, "<text x='%d' y='%d' font-family='Menlo,Courier,monospace' font-size='8.4' textLength='%d' lengthAdjust='spacing' fill='%s' xml:space='preserve'>%s</text>\n",
                        o.x, o.y + 7, o.w, c.c_str(), esc(o.text).c_str()); break;
    }
  }
  fprintf(f, "</svg>\n");
  fclose(f);
}

// Text boxes (glyph cell 6x8, ink 5x7) must not overlap each other or leave the screen
static void checkLayout(const char* name, const std::vector<DrawOp>& ops) {
  std::vector<const DrawOp*> t;
  for (const DrawOp& o : ops) if (o.type == 'T') t.push_back(&o);
  int problems = 0;
  for (size_t i = 0; i < t.size(); i++) {
    const DrawOp& a = *t[i];
    if (a.x < 0 || a.y < 0 || a.x + a.w - 1 > 240 || a.y + 7 > 135) {
      printf("  layout %s: \"%s\" leaves the screen (x %d..%d, y %d..%d)\n", name, a.text.c_str(), a.x, a.x + a.w, a.y, a.y + 7);
      problems++;
    }
    for (size_t j = i + 1; j < t.size(); j++) {
      const DrawOp& b = *t[j];
      bool ox = a.x < b.x + b.w - 1 && b.x < a.x + a.w - 1;
      bool oy = a.y < b.y + 7 && b.y < a.y + 7;
      if (ox && oy) {
        printf("  layout %s: \"%s\" (x %d..%d y %d) overlaps \"%s\" (x %d..%d y %d)\n", name, a.text.c_str(), a.x, a.x + a.w, a.y, b.text.c_str(), b.x, b.x + b.w, b.y);
        problems++;
      }
    }
  }
  CHECK(problems == 0, "%s: %d layout problems", name, problems);
}

// Grid geometry: 11 rows x 16 columns inside the screen, columns strictly left to right, no cell overlaps
static void checkGrid(const char* name, const std::vector<DrawOp>& ops) {
  int cells = 0, bad = 0;
  for (const DrawOp& o : ops) {
    if (o.type != 'f' || o.w != kCellW || o.h != 8) continue;
    cells++;
    if (o.x < 0 || o.x + o.w > 240 || o.y < 0 || o.y + o.h > 135) bad++;
  }
  CHECK(cells >= kNumTracks * kNumSteps, "%s: only %d step cells drawn", name, cells);
  CHECK(bad == 0, "%s: %d cells outside the screen", name, bad);
  for (int s = 1; s < kNumSteps; s++) CHECK(cellX(s) > cellX(s - 1) + kCellW, "cell %d overlaps cell %d", s, s - 1);
  CHECK(cellX(kNumSteps - 1) + kCellW <= 240, "the last step leaves the screen");
  CHECK(kGridY + (kNumTracks - 1) * kRowH + 8 <= 125, "the grid runs into the footer");
}

static void shot(const std::string& dir, const char* name) {
  drawScreen();
  writeSvg(dir + "/" + name + ".svg", canvas.shown);
  checkLayout(name, canvas.shown);
  if (curPage == PAGE_SEQ) checkGrid(name, canvas.shown);
}

static void renderScreens(const std::string& dir) {
  printf("[ui] rendering screens to %s\n", dir.c_str());
  resetUi();
  for (int i = 0; i < kNumPatterns; i++) engine.loadDemo(i, i);
  engine.bpm = 122;

  // Sequencer: stopped, cursor on an accent
  curTrack = T_SD; curStep = 4;
  shot(dir, "seq_stopped");
  // playing pattern A, playhead at step 6, CB and CH muted
  engine.mute[T_CH] = true; engine.mute[T_CB] = true;
  engine.playing = true; engine.playPattern = 0; engine.lastStep = 6; curTrack = T_CH; curStep = 8;
  shot(dir, "seq_playing_muted");
  engine.mute[T_CH] = false; engine.mute[T_CB] = false;
  // pattern B queued while A plays (green outline on A, yellow on B), playhead in the last group
  engine.curPattern = 1; engine.lastStep = 13; curTrack = T_LT; curStep = 15;
  shot(dir, "seq_pattern_switch_pending");
  // pattern D (latin), playhead at the first step, last voice
  engine.curPattern = 3; engine.playPattern = 3; engine.lastStep = 0; curTrack = T_CH; curStep = 0;
  shot(dir, "seq_pattern_d_playhead_start");
  engine.playing = false;
  toast("CLEAR? \\ x2");
  shot(dir, "seq_toast");
  toastUntil = 0;
  engine.curPattern = 2; engine.playing = false;
  curTrack = T_CY; curStep = 12;
  shot(dir, "seq_pattern_c");

  // Edit page
  curPage = PAGE_EDIT;
  for (int i = 0; i < kScopeLen; i++) engine.scope[i] = 0.6f * sinf(i * 0.35f) * expf(-i / 120.0f);
  selectTab(T_BD); edRow = 0; shot(dir, "edit_bd");
  selectTab(T_SD); edRow = 2; shot(dir, "edit_sd");
  selectTab(T_CB); engine.mute[T_CB] = true; edRow = 3; shot(dir, "edit_cb_muted");
  engine.mute[T_CB] = false;
  selectTab(T_CH); engine.dp[T_CH].p[P_TUNE] = 1.0f; edRow = 0; shot(dir, "edit_ch_tune_max");
  selectTab(kMasterTab); edRow = 1; engine.swing = 0.5f; engine.bpm = 300; shot(dir, "edit_master");
  curPage = PAGE_HELP; shot(dir, "help");
}

int main(int argc, char** argv) {
  std::string dir = argc > 1 ? argv[1] : "/tmp/tr808_ui";
  mkdir(dir.c_str(), 0755);
  testSequencerPage();
  testEditPage();
  testPersistenceAndClear();
  renderScreens(dir);
  printf(g_fail ? "\nFAILED (%d)\n" : "\nall ok\n", g_fail);
  return g_fail ? 1 : 0;
}
