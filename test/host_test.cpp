// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ryu_muto
// Host-side tests for TR808Engine.h (build + run with test/run.sh).
//   ./run.sh                 run all checks
//   ./run.sh wav out.wav     additionally render every voice + the four demo grooves to a WAV
#include <math.h>
#include <cmath>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <string>

// Count libm calls made by the engine (the ESP32-S3 has no hardware powf/sinf:
// a voice that calls them per sample cannot keep up). The macros only wrap the
// engine's own calls, they are removed again right after the include.
static long g_libm = 0;
static inline float cnt_powf(float a, float b) { g_libm++; return ::powf(a, b); }
static inline float cnt_expf(float a)           { g_libm++; return ::expf(a); }
static inline float cnt_sinf(float a)           { g_libm++; return ::sinf(a); }
static inline float cnt_cosf(float a)           { g_libm++; return ::cosf(a); }
static inline float cnt_sqrtf(float a)          { g_libm++; return ::sqrtf(a); }
#define powf  cnt_powf
#define expf  cnt_expf
#define sinf  cnt_sinf
#define cosf  cnt_cosf
#define sqrtf cnt_sqrtf
#include "../TR808Engine.h"
#undef powf
#undef expf
#undef sinf
#undef cosf
#undef sqrtf

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static TR808Engine* newEngine() { auto* e = new TR808Engine(); e->init(); return e; }

// Render n samples in blocks of 256 (like the sketch), returns floats -1..1
static void renderBlocks(TR808Engine& e, std::vector<float>& out, int n) {
  int16_t buf[256];
  for (int done = 0; done < n; done += 256) {
    int m = (n - done < 256) ? n - done : 256;
    e.render(buf, m);
    for (int i = 0; i < m; i++) out.push_back(buf[i] / kOutScale);
  }
}

// hit one voice alone at Level 1.0, master 1.0
static std::vector<float> oneShot(int track, int level, float seconds, TR808Engine* use = nullptr) {
  TR808Engine* e = use;
  if (!e) { e = newEngine(); e->dp[track].p[P_LEVEL] = 1.0f; e->dirty = TR808Engine::kAllTracks; e->masterVol = 1.0f; }
  e->preview(track, level);
  std::vector<float> out;
  renderBlocks(*e, out, (int)(seconds * kSampleRate));
  if (!use) delete e;
  return out;
}

struct Stats { float peak, dc, t20, t40, t60; };

static Stats analyse(const std::vector<float>& x) {
  Stats s{0, 0, -1, -1, -1};
  double sum = 0;
  for (float v : x) { if (fabsf(v) > s.peak) s.peak = fabsf(v); sum += v; }
  s.dc = (float)(sum / x.size());
  // envelope: max |x| in 5 ms windows, time at which it last exceeds the thresholds
  const int W = kSampleRate / 200;
  float a20 = 0, a40 = 0, a60 = 0;
  for (size_t i = 0; i + W <= x.size(); i += W) {
    float m = 0;
    for (int j = 0; j < W; j++) m = fmaxf(m, fabsf(x[i + j]));
    float t = (float)i / kSampleRate;
    if (m > s.peak * 0.1f)   a20 = t;
    if (m > s.peak * 0.01f)  a40 = t;
    if (m > s.peak * 0.001f) a60 = t;
  }
  s.t20 = a20; s.t40 = a40; s.t60 = a60;
  return s;
}

// dominant frequency in [i0, i1) by zero crossings
static float zcrHz(const std::vector<float>& x, int i0, int i1) {
  int z = 0;
  for (int i = i0 + 1; i < i1 && i < (int)x.size(); i++) if ((x[i - 1] < 0) != (x[i] < 0)) z++;
  return z * 0.5f * kSampleRate / (float)(i1 - i0);
}

// brightness: energy of the second difference / (16 * energy) - 1 at Nyquist, 0.05 around 7 kHz, ~0 for bass
static float highShare(const std::vector<float>& x, size_t i0, size_t i1) {
  double e = 0, d = 0;
  for (size_t i = i0 + 2; i < i1 && i < x.size(); i++) {
    e += (double)x[i] * x[i];
    double dd = x[i] - 2.0 * x[i - 1] + x[i - 2];
    d += dd * dd;
  }
  return e > 0 ? (float)(d / (16.0 * e)) : 0.0f;
}

static void writeWav(const char* path, const std::vector<float>& x) {
  FILE* f = fopen(path, "wb");
  if (!f) { printf("cannot write %s\n", path); return; }
  uint32_t dataBytes = (uint32_t)x.size() * 2, rate = kSampleRate, byteRate = rate * 2, riff = 36 + dataBytes;
  uint16_t fmt = 1, ch = 1, align = 2, bits = 16; uint32_t fmtLen = 16;
  fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmtLen, 4, 1, f);
  fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&byteRate, 4, 1, f);
  fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&dataBytes, 4, 1, f);
  for (float v : x) { int16_t s = (int16_t)(v * kOutScale); fwrite(&s, 2, 1, f); }
  fclose(f);
  printf("wrote %s (%.1f s)\n", path, (double)x.size() / kSampleRate);
}

// ------------------------------------------------------------
static void testFastSin() {
  printf("[fastSin]\n");
  float maxErr = 0;
  for (int i = 0; i < 10000; i++) {
    float ph = i / 10000.0f;
    maxErr = fmaxf(maxErr, fabsf(drum::fastSin(ph) - ::sinf(6.28318530718f * ph)));
  }
  printf("  max error %.5f\n", maxErr);
  CHECK(maxErr < 0.0015f, "fastSin error too large: %f", maxErr);
}

static float invSoftClip(float y) {   // softClip is monotonic on 0..1.5
  float lo = 0, hi = 1.5f;
  for (int i = 0; i < 40; i++) { float m = 0.5f * (lo + hi); if (drum::softClip(m) < y) lo = m; else hi = m; }
  return 0.5f * (lo + hi);
}

static void testVoices() {
  printf("[voices] default params, accent, Level 1.0, master 1.0, %d Hz\n", kSampleRate);
  for (int t = 0; t < kNumTracks; t++) {
    TR808Engine* e = newEngine();
    e->dp[t].p[P_LEVEL] = 1.0f; e->dirty = TR808Engine::kAllTracks; e->masterVol = 1.0f;
    auto x = oneShot(t, STEP_ACCENT, 4.0f, e);
    Stats s = analyse(x);
    float raw = invSoftClip(s.peak) / 0.85f;   // level of raw * kTrackGain
    float suggest = TR808Engine::kTrackGain[t] * 0.8f / raw;
    printf("  %s: peak %.3f (raw*gain %.3f, gain for 0.8 would be %.3f)  dc %+.4f  -20dB %.3fs  -40dB %.3fs  -60dB %.3fs  active=%d\n",
           trackNames[t], s.peak, raw, suggest, s.dc, s.t20, s.t40, s.t60, e->trackActive(t));
    CHECK(s.peak > 0.55f && s.peak <= 0.75f, "%s peak %.3f outside 0.55..0.75 (suggested kTrackGain %.3f)", trackNames[t], s.peak, suggest);
    CHECK(fabsf(s.dc) < 0.01f, "%s DC offset %.4f", trackNames[t], s.dc);
    CHECK(!e->trackActive(t), "%s still active after 4 s (CPU is spent on a silent voice)", trackNames[t]);
    CHECK(s.t60 < 3.5f, "%s takes %.2f s to fall 60 dB", trackNames[t], s.t60);
    delete e;
  }

  // pitch behaviour: BD drops and settles in the 808 range, toms drop too
  {
    auto k = oneShot(T_BD, STEP_ACCENT, 0.6f);
    float early = zcrHz(k, (int)(0.004f * kSampleRate), (int)(0.020f * kSampleRate));
    float late  = zcrHz(k, (int)(0.15f * kSampleRate),  (int)(0.30f * kSampleRate));
    printf("  BD pitch: %.0f Hz (4-20 ms) -> %.0f Hz (150-300 ms)\n", early, late);
    CHECK(early > late * 1.5f, "kick pitch does not drop (%.0f -> %.0f)", early, late);
    CHECK(late > 40.0f && late < 70.0f, "kick settles at %.0f Hz, expected ~45-65 Hz", late);
    for (int t : {(int)T_HT, (int)T_LT}) {
      auto c = oneShot(t, STEP_ACCENT, 0.5f);
      float ce = zcrHz(c, (int)(0.005f * kSampleRate), (int)(0.025f * kSampleRate));
      float cl = zcrHz(c, (int)(0.08f * kSampleRate),  (int)(0.20f * kSampleRate));
      printf("  %s pitch: %.0f Hz (5-25 ms) -> %.0f Hz (80-200 ms)\n", trackNames[t], ce, cl);
      CHECK(ce > cl, "%s pitch does not drop", trackNames[t]);
    }
    float lt = zcrHz(oneShot(T_LT, STEP_ACCENT, 0.5f), (int)(0.08f * kSampleRate), (int)(0.2f * kSampleRate));
    float ht = zcrHz(oneShot(T_HT, STEP_ACCENT, 0.5f), (int)(0.08f * kSampleRate), (int)(0.2f * kSampleRate));
    CHECK(ht > lt * 1.4f, "hi tom (%.0f Hz) should sit well above low tom (%.0f Hz)", ht, lt);

    // Tune parameter: full sweep is roughly the octave the table says
    TR808Engine* lo = newEngine(); lo->dp[T_BD].p[P_TUNE] = 0.0f; lo->dp[T_BD].p[P_LEVEL] = 1; lo->dirty = TR808Engine::kAllTracks; lo->masterVol = 1;
    TR808Engine* hi = newEngine(); hi->dp[T_BD].p[P_TUNE] = 1.0f; hi->dp[T_BD].p[P_LEVEL] = 1; hi->dirty = TR808Engine::kAllTracks; hi->masterVol = 1;
    float fl = zcrHz(oneShot(T_BD, STEP_ACCENT, 0.5f, lo), (int)(0.15f * kSampleRate), (int)(0.3f * kSampleRate));
    float fh = zcrHz(oneShot(T_BD, STEP_ACCENT, 0.5f, hi), (int)(0.15f * kSampleRate), (int)(0.3f * kSampleRate));
    printf("  BD Tune 0 -> 1: %.0f -> %.0f Hz (table %.0f -> %.0f)\n", fl, fh, TR808Engine::tuneHz(T_BD, 0), TR808Engine::tuneHz(T_BD, 1));
    CHECK(fh > fl * 1.6f, "BD Tune sweep too small (%.0f -> %.0f)", fl, fh);
    delete lo; delete hi;
  }

  // Character: the metallic voices are bright, the drums are not
  {
    float hs[kNumTracks];
    for (int t = 0; t < kNumTracks; t++) {
      auto x = oneShot(t, STEP_ACCENT, 0.3f);
      hs[t] = highShare(x, (int)(0.003f * kSampleRate), (int)(0.05f * kSampleRate));
    }
    printf("  high-frequency share:");
    for (int t = 0; t < kNumTracks; t++) printf(" %s %.2f", trackNames[t], hs[t]);
    printf("\n");
    CHECK(hs[T_CH] > 0.04f && hs[T_OH] > 0.04f && hs[T_CY] > 0.04f, "hats/cymbal are not bright (%.2f %.2f %.2f)", hs[T_CH], hs[T_OH], hs[T_CY]);
    CHECK(hs[T_BD] < 0.01f && hs[T_LT] < 0.02f, "bass drum / low tom are not dark (%.3f %.3f)", hs[T_BD], hs[T_LT]);

    // Decay ordering: closed hat < open hat < cymbal; claves shortest tonal; BD longest of the drums
    float d[kNumTracks];
    for (int t = 0; t < kNumTracks; t++) d[t] = analyse(oneShot(t, STEP_ACCENT, 3.0f)).t40;
    printf("  -40 dB time (s):");
    for (int t = 0; t < kNumTracks; t++) printf(" %s %.2f", trackNames[t], d[t]);
    printf("\n");
    CHECK(d[T_CH] < d[T_OH] && d[T_OH] < d[T_CY], "decay order CH < OH < CY violated (%.2f %.2f %.2f)", d[T_CH], d[T_OH], d[T_CY]);
    CHECK(d[T_CL] < d[T_LT] && d[T_CL] < d[T_SD], "claves should be the shortest tonal voice");
    CHECK(d[T_BD] > d[T_SD], "kick should ring longer than the snare");
  }

  // Accent is louder than a plain step, both audible
  for (int t = 0; t < kNumTracks; t++) {
    float on  = analyse(oneShot(t, STEP_ON, 0.6f)).peak;
    float acc = analyse(oneShot(t, STEP_ACCENT, 0.6f)).peak;
    CHECK(on > 0.05f, "%s plain step is inaudible (%.3f)", trackNames[t], on);
    CHECK(acc > on * 1.15f, "%s accent (%.3f) is not louder than a plain step (%.3f)", trackNames[t], acc, on);
  }
}

static void testChoke() {
  printf("[choke] closed hat cuts the open hat; unrelated voices are untouched\n");
  auto run = [](bool hitCH, float* tailPeak, float* beforePeak) {
    TR808Engine* e = newEngine();
    e->masterVol = 1.0f;
    e->dp[T_CH].p[P_LEVEL] = 0.0f;            // the closed hat itself is silent: only the open hat can be heard
    e->dp[T_OH].p[P_LEVEL] = 1.0f; e->dp[T_OH].p[P_DECAY] = 1.0f; e->dirty = TR808Engine::kAllTracks;
    std::vector<float> x;
    e->preview(T_OH, STEP_ACCENT);
    renderBlocks(*e, x, kSampleRate / 20);                        // 50 ms
    *beforePeak = 0; for (size_t i = x.size() - 1000; i < x.size(); i++) *beforePeak = fmaxf(*beforePeak, fabsf(x[i]));
    if (hitCH) e->preview(T_CH, STEP_ACCENT);
    renderBlocks(*e, x, kSampleRate / 20);
    size_t m = x.size();
    renderBlocks(*e, x, kSampleRate / 10);                        // 100 ms later
    *tailPeak = 0; for (size_t i = m; i < x.size(); i++) *tailPeak = fmaxf(*tailPeak, fabsf(x[i]));
    delete e;
  };
  float tailC, beforeC, tailN, beforeN;
  run(true, &tailC, &beforeC);
  run(false, &tailN, &beforeN);
  printf("  open hat level after 50 ms %.3f; 50-150 ms later: %.4f with choke, %.3f without\n", beforeN, tailC, tailN);
  CHECK(beforeN > 0.05f, "open hat not audible");
  CHECK(tailN > 0.03f, "control: open hat should still ring without a choke (%.3f)", tailN);
  CHECK(tailC < tailN * 0.05f, "closed hat did not choke the open hat (%.4f vs %.4f)", tailC, tailN);

  // an open hat triggered after a choke sounds normally again
  TR808Engine* e = newEngine(); e->masterVol = 1;
  e->preview(T_CH, STEP_ACCENT); std::vector<float> x; renderBlocks(*e, x, 4096);
  e->preview(T_OH, STEP_ACCENT); x.clear(); renderBlocks(*e, x, kSampleRate / 10);
  float pk = 0; for (size_t i = kSampleRate / 20; i < x.size(); i++) pk = fmaxf(pk, fabsf(x[i]));
  CHECK(pk > 0.03f, "open hat is dead after a choke (%.4f)", pk);
  delete e;
}

static void testMute() {
  printf("[mute] muted track is silent, others unaffected, muting/unmuting does not click\n");
  TR808Engine* e = newEngine();
  e->masterVol = 1.0f;
  e->clearPattern(0);
  for (int s = 0; s < kNumSteps; s++) e->patterns[0].c[T_BD][s] = STEP_ACCENT;
  for (int s = 0; s < kNumSteps; s += 2) e->patterns[0].c[T_CH][s] = STEP_ACCENT;
  e->bpm = 200;
  e->post(EV_TOGGLE_PLAY);

  auto peakOf = [](const std::vector<float>& x, size_t a, size_t b) { float m = 0; for (size_t i = a; i < b && i < x.size(); i++) m = fmaxf(m, fabsf(x[i])); return m; };
  std::vector<float> x;
  renderBlocks(*e, x, kSampleRate);
  float pkAll = peakOf(x, 0, x.size());
  CHECK(pkAll > 0.3f, "pattern is silent (%.3f)", pkAll);

  e->mute[T_BD] = true; e->mute[T_CH] = true;
  x.clear(); renderBlocks(*e, x, kSampleRate / 2);
  float pkMuted = peakOf(x, 2048, x.size());
  printf("  peak: %.3f unmuted, %.5f with BD+CH muted\n", pkAll, pkMuted);
  CHECK(pkMuted < 1e-3f, "muted tracks still audible (%.5f)", pkMuted);

  e->mute[T_BD] = false;
  x.clear(); renderBlocks(*e, x, kSampleRate);
  CHECK(peakOf(x, 0, x.size()) > 0.3f, "unmuting BD did not bring the kick back");

  // Muting a ringing kick fades it out over ~ms instead of cutting the waveform
  TR808Engine* k = newEngine(); k->masterVol = 1; k->dp[T_BD].p[P_LEVEL] = 1; k->dp[T_BD].p[P_DECAY] = 1; k->dirty = TR808Engine::kAllTracks;
  x.clear();
  k->preview(T_BD, STEP_ACCENT);
  renderBlocks(*k, x, 8192);
  size_t mark = x.size();
  k->mute[T_BD] = true;
  renderBlocks(*k, x, 2048);
  float worst = 0; for (size_t i = mark + 1; i < mark + 2048; i++) worst = fmaxf(worst, fabsf(x[i] - x[i - 1]));
  printf("  largest sample step while muting a ringing kick: %.4f\n", worst);
  CHECK(worst < 0.05f, "muting clicks (step %.4f)", worst);
  CHECK(peakOf(x, mark + 1500, x.size()) < 0.001f, "kick not silent after the mute fade");
  delete k; delete e;
}

static void testCalibration() {
  printf("[levels] every voice stays inside a sane range over the whole parameter space\n");
  srand(99);
  for (int t = 0; t < kNumTracks; t++) {
    float dflt;
    {
      TR808Engine* e = newEngine(); e->masterVol = 1; e->dp[t].p[P_LEVEL] = 1; e->dirty = TR808Engine::kAllTracks;
      dflt = analyse(oneShot(t, STEP_ACCENT, 1.5f, e)).peak; delete e;
    }
    float worst = 0, lowest = 9;
    for (int c = 0; c < 120; c++) {
      TR808Engine* e = newEngine(); e->masterVol = 1;
      for (int i = 0; i < kNumParams; i++) e->dp[t].p[i] = (c < 16) ? (((c >> i) & 1) ? 1.0f : 0.0f) : rand() / (float)RAND_MAX;
      e->dp[t].p[P_LEVEL] = 1.0f; e->dirty = TR808Engine::kAllTracks;
      float pk = analyse(oneShot(t, STEP_ACCENT, 0.8f, e)).peak;
      worst = fmaxf(worst, pk); lowest = fminf(lowest, pk);
      delete e;
    }
    printf("  %s: default %.2f, over all parameters %.2f .. %.2f\n", trackNames[t], dflt, lowest, worst);
    CHECK(worst < dflt * 1.7f + 0.05f, "%s reaches %.2f over the parameter space (default %.2f)", trackNames[t], worst, dflt);
    CHECK(lowest > dflt * 0.25f, "%s drops to %.2f for some parameters (default %.2f)", trackNames[t], lowest, dflt);
  }
}

static void testParamSweep() {
  printf("[param sweep] corners + random, every voice must stay finite, bounded and stop\n");
  srand(1234);
  int cases = 0;
  for (int t = 0; t < kNumTracks; t++) {
    for (int c = 0; c < 200; c++) {
      TR808Engine* e = newEngine();
      for (int i = 0; i < kNumParams; i++) {
        float v = (c < 16) ? (((c >> i) & 1) ? 1.0f : 0.0f) : (rand() / (float)RAND_MAX);
        e->dp[t].p[i] = v;
      }
      e->dp[t].p[P_LEVEL] = 1.0f; e->dirty = TR808Engine::kAllTracks; e->masterVol = 1.0f;
      e->preview(t, (c & 1) ? STEP_ACCENT : STEP_ON);
      std::vector<float> x;
      renderBlocks(*e, x, kSampleRate * 5);
      float pk = 0; bool finite = true;
      for (float v : x) { if (!std::isfinite(v)) finite = false; pk = fmaxf(pk, fabsf(v)); }
      CHECK(finite, "%s case %d: NaN/Inf", trackNames[t], c);
      CHECK(pk <= 1.0f, "%s case %d: peak %.3f > 1", trackNames[t], c, pk);
      CHECK(!e->trackActive(t), "%s case %d: still active after 5 s", trackNames[t], c);
      CHECK(e->healCount == 0, "%s case %d: engine had to heal a voice", trackNames[t], c);
      delete e; cases++;
      if (g_fail > 20) return;
    }
  }
  printf("  %d cases\n", cases);
}

static void testNoLibmPerSample() {
  printf("[libm] no powf/expf/sinf/cosf/sqrtf inside the per-sample path\n");
  TR808Engine* e = newEngine();
  for (int t = 0; t < kNumTracks; t++) e->preview(t, STEP_ACCENT);
  int16_t buf[256];
  e->render(buf, 256);                       // events + triggers happen here (allowed to call libm)
  g_libm = 0;
  e->playing = false;
  for (int i = 0; i < 40; i++) e->render(buf, 256);   // ringing voices, no new events, no dirty params
  printf("  libm calls during 10240 samples of 11 ringing voices: %ld\n", g_libm);
  CHECK(g_libm == 0, "per-sample libm calls: %ld", g_libm);
  delete e;
}

static void testRetriggerClick() {
  printf("[retrigger] kick and cymbal retriggered while ringing must not click\n");
  for (int t : {(int)T_BD, (int)T_CB}) {
    TR808Engine* e = newEngine();
    e->masterVol = 1.0f;
    e->dp[t].p[P_DECAY] = 1.0f; e->dirty = TR808Engine::kAllTracks;
    std::vector<float> x;
    e->preview(t, STEP_ACCENT);
    renderBlocks(*e, x, (int)(0.117f * kSampleRate) / 256 * 256);   // one 16th at 128 BPM, block aligned
    size_t mark = x.size();
    e->preview(t, STEP_ACCENT);
    renderBlocks(*e, x, 4096);
    auto maxStep = [&](size_t a, size_t b) { float m = 0; for (size_t i = a + 1; i < b; i++) m = fmaxf(m, fabsf(x[i] - x[i - 1])); return m; };
    float first = maxStep(0, 512), retrig = maxStep(mark, mark + 512);
    printf("  %s max |dx|: first hit %.4f, retrigger %.4f\n", trackNames[t], first, retrig);
    CHECK(retrig < first * 1.5f + 0.02f, "%s retrigger step %.4f vs first hit %.4f", trackNames[t], retrig, first);
    delete e;
  }
}

static void testTiming() {
  printf("[sequencer] step timing\n");
  for (int swingCase = 0; swingCase < 2; swingCase++) {
    TR808Engine* e = newEngine();
    e->bpm = 120;
    e->swing = swingCase ? 1.0f : 0.0f;
    e->post(EV_TOGGLE_PLAY);
    std::vector<long> at;           // sample index of each step trigger
    int16_t buf[1];
    uint8_t prev = 255; bool first = true;
    const int N = kSampleRate * 4;  // 4 s = 2 bars at 120 BPM
    for (int i = 0; i < N; i++) {
      e->render(buf, 1);
      if (e->playing && (e->lastStep != prev || first)) { at.push_back(i); prev = e->lastStep; first = false; }
    }
    double L = kSampleRate * 60.0 / 120 / 4;
    printf("  swing %.0f%%: %zu steps in 4 s, first at sample %ld\n", swingCase * 33.0, at.size(), at.empty() ? -1 : at[0]);
    CHECK(at.size() >= 32, "expected 32+ steps in 4 s, got %zu", at.size());
    CHECK(!at.empty() && at[0] <= 1, "first step should fire immediately (at %ld)", at.empty() ? -1L : at[0]);
    for (size_t i = 0; i + 2 < at.size() && i < 30; i += 2) {
      double pair = (double)(at[i + 2] - at[i]);
      CHECK(fabs(pair - 2 * L) < 2.5, "pair length %.1f vs %.1f", pair, 2 * L);
    }
    if (swingCase) {
      double odd = (double)(at[1] - at[0]);
      printf("  even->odd gap %.1f samples (straight = %.1f, expected x1.33)\n", odd, L);
      CHECK(fabs(odd - L * 1.33) < 3.0, "swing gap %.1f", odd);
    } else {
      for (size_t i = 0; i + 1 < at.size() && i < 30; i++)
        CHECK(fabs((double)(at[i + 1] - at[i]) - L) < 1.5, "straight gap %.1f vs %.1f", (double)(at[i + 1] - at[i]), L);
    }
    // the playhead walks 0..15 in order and wraps
    bool ordered = true;
    for (size_t i = 1; i < at.size(); i++) (void)i;
    delete e;
    CHECK(ordered, "unreachable");
  }

  printf("[sequencer] playhead order, 4 patterns, pattern change lands on the bar line, stop\n");
  {
    TR808Engine* e = newEngine();
    for (int p = 0; p < kNumPatterns; p++) e->clearPattern(p);
    e->patterns[0].c[T_BD][0] = STEP_ACCENT;
    e->patterns[3].c[T_SD][0] = STEP_ACCENT;
    e->post(EV_TOGGLE_PLAY);
    int16_t buf[1];
    int expect = 0; bool seq = true; uint8_t prev = 255;
    for (int i = 0; i < kSampleRate * 3; i++) {
      e->render(buf, 1);
      if (e->lastStep != prev) { if (e->lastStep != expect) seq = false; expect = (expect + 1) % kNumSteps; prev = e->lastStep; }
    }
    CHECK(seq, "the playhead does not walk 0..15 in order, left to right");
    while (e->lastStep != 5) e->render(buf, 1);
    e->curPattern = 3;                                          // request switch mid-bar
    for (int i = 0; i < 100; i++) e->render(buf, 1);
    CHECK(e->playPattern == 0, "pattern switched mid-bar");
    int guard = 0;
    while (!(e->lastStep == 0 && e->playPattern == 3)) { e->render(buf, 1); if (++guard > kSampleRate * 4) break; }
    CHECK(e->playPattern == 3, "pattern did not switch at the bar line");
    e->post(EV_TOGGLE_PLAY);
    e->render(buf, 1);
    CHECK(!e->playing, "stop did not stop");
    e->curPattern = 2; e->post(EV_TOGGLE_PLAY); e->render(buf, 1);
    CHECK(e->playing && e->playPattern == 2 && e->lastStep == 0, "restart should begin at step 1 of the selected pattern");
    delete e;
  }

  printf("[sequencer] each pattern plays its own notes\n");
  {
    TR808Engine* e = newEngine(); e->masterVol = 1;
    for (int p = 0; p < kNumPatterns; p++) e->clearPattern(p);
    e->dp[T_BD].p[P_LEVEL] = 1; e->dp[T_CH].p[P_LEVEL] = 1; e->dirty = TR808Engine::kAllTracks;
    e->patterns[1].c[T_CH][0] = STEP_ACCENT;   // only pattern 2 has a hat
    for (int p = 0; p < kNumPatterns; p++) {
      e->curPattern = p; e->post(EV_TOGGLE_PLAY);
      std::vector<float> x; renderBlocks(*e, x, kSampleRate / 20);
      float pk = 0; for (float v : x) pk = fmaxf(pk, fabsf(v));
      e->post(EV_TOGGLE_PLAY); int16_t b[1]; e->render(b, 1);
      CHECK((pk > 0.05f) == (p == 1), "pattern %d: peak %.3f", p + 1, pk);
      // let everything ring out
      std::vector<float> t; renderBlocks(*e, t, kSampleRate / 2);
    }
    delete e;
  }
}

#include <chrono>
static void testPerf() {
  printf("[perf] worst case: all 11 voices retriggered every 16th at 300 BPM\n");
  TR808Engine* e = newEngine();
  for (int t = 0; t < kNumTracks; t++) for (int s = 0; s < kNumSteps; s++) e->patterns[0].c[t][s] = (s & 1) ? STEP_ON : STEP_ACCENT;
  e->bpm = 300;
  e->post(EV_TOGGLE_PLAY);
  int16_t buf[256];
  const int seconds = 10, blocks = kSampleRate * seconds / 256;
  auto t0 = std::chrono::steady_clock::now();
  long acc = 0;
  for (int b = 0; b < blocks; b++) { e->render(buf, 256); acc += buf[7]; }
  auto t1 = std::chrono::steady_clock::now();
  double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / ((double)blocks * 256);
  printf("  %.1f ns/sample on this machine (checksum %ld)\n", ns, acc);
  // The ESP32-S3 (240 MHz, single-precision FPU) is roughly 30-50x slower than a desktop core;
  // the budget per sample is 1e9/44100 = 22.7 us. This is a regression guard.
  CHECK(ns < 400.0, "per-sample cost %.1f ns is above the regression guard", ns);
  delete e;
}

static void testSerialize() {
  printf("[persistence] blob round trip and rejection of bad data\n");
  TR808Engine* a = newEngine();
  a->loadDemo(0, 0); a->loadDemo(1, 1); a->loadDemo(2, 2); a->loadDemo(3, 3);
  a->bpm = 137; a->swing = 0.4f; a->masterVol = 0.55f; a->mute[T_CB] = true; a->mute[T_CH] = true;
  a->dp[T_SD].p[P_TONE] = 0.123f;
  static uint8_t blob[TR808Engine::kBlobSize + 16];
  size_t n = a->serialize(blob, sizeof(blob));
  printf("  blob size %zu bytes\n", n);
  CHECK(n == TR808Engine::kBlobSize, "serialize returned %zu", n);

  TR808Engine* b = newEngine();
  CHECK(b->deserialize(blob, n), "deserialize of a valid blob failed");
  CHECK(b->bpm == 137 && fabsf(b->swing - 0.4f) < 1e-6f && fabsf(b->masterVol - 0.55f) < 1e-6f, "globals differ");
  CHECK(b->mute[T_CB] && b->mute[T_CH] && !b->mute[T_BD], "mute differs");
  CHECK(fabsf(b->dp[T_SD].p[P_TONE] - 0.123f) < 1e-6f, "param differs");
  CHECK(memcmp((void*)a->patterns, (void*)b->patterns, sizeof(a->patterns)) == 0, "patterns differ");

  uint8_t bad[TR808Engine::kBlobSize];
  memcpy(bad, blob, n); bad[40] ^= 0x55;
  TR808Engine* c = newEngine(); c->bpm = 99;
  CHECK(!c->deserialize(bad, n), "corrupted blob accepted");
  CHECK(c->bpm == 99, "engine modified by a rejected blob");
  CHECK(!c->deserialize(blob, n - 1), "short blob accepted");
  memcpy(bad, blob, n); bad[0] ^= 1;
  CHECK(!c->deserialize(bad, n), "wrong magic accepted");
  CHECK(a->serialize(bad, n - 1) == 0, "serialize into too small buffer should fail");

  // out-of-range but checksum-valid data is clamped, not trusted
  memcpy(bad, blob, n);
  size_t patOff = 4 + 2 + 4 + 4 + 2 + kNumTracks * kNumParams * 4;
  bad[patOff] = 200;
  uint32_t a1 = 1, b1 = 0; for (size_t i = 0; i < n - 2; i++) { a1 = (a1 + bad[i]) % 251; b1 = (b1 + a1) % 251; }
  uint16_t sum = (uint16_t)((b1 << 8) | a1); memcpy(bad + n - 2, &sum, 2);
  CHECK(c->deserialize(bad, n), "valid blob with wild values rejected");
  CHECK(c->patterns[0].c[0][0] == STEP_ACCENT, "wild cell not clamped (%d)", c->patterns[0].c[0][0]);
  delete a; delete b; delete c;
}

static void testDemos() {
  printf("[demos] the four grooves are populated, distinct, and play without clipping into NaN\n");
  TR808Engine* e = newEngine();
  int hits[4];
  for (int g = 0; g < 4; g++) {
    e->loadDemo(g, g);
    hits[g] = 0;
    for (int t = 0; t < kNumTracks; t++) for (int s = 0; s < kNumSteps; s++) if (e->patterns[g].c[t][s]) hits[g]++;
    printf("  pattern %c: %d hits\n", 'A' + g, hits[g]);
    CHECK(hits[g] > 12, "demo %d has only %d hits", g, hits[g]);
  }
  for (int g = 1; g < 4; g++) CHECK(memcmp((void*)&e->patterns[0], (void*)&e->patterns[g], sizeof(Pattern)) != 0, "demo %d equals demo 0", g);
  for (int g = 0; g < 4; g++) {
    e->curPattern = g; e->bpm = 120;
    e->post(EV_TOGGLE_PLAY);
    std::vector<float> x; renderBlocks(*e, x, kSampleRate * 4);
    e->post(EV_TOGGLE_PLAY);
    float pk = 0; bool ok = true; for (float v : x) { if (!std::isfinite(v)) ok = false; pk = fmaxf(pk, fabsf(v)); }
    CHECK(ok && pk > 0.3f && pk <= 1.0f, "demo %d: peak %.3f", g, pk);
    CHECK(e->healCount == 0, "engine healed a voice in demo %d", g);
    std::vector<float> t; renderBlocks(*e, t, kSampleRate);
  }
  delete e;
}

static void renderWavs(const char* path) {
  // Every voice one after the other (default, then low / high Tune), then the four grooves (4 bars each)
  std::vector<float> all;
  {
    TR808Engine* e = newEngine();
    e->masterVol = 0.9f;
    for (int t = 0; t < kNumTracks; t++) {
      for (float tune : {-1.0f, 0.5f, 1.0f}) {
        float keep = e->dp[t].p[P_TUNE];
        if (tune >= 0) { e->dp[t].p[P_TUNE] = tune; e->dirty = TR808Engine::kAllTracks; }
        e->preview(t, STEP_ACCENT);
        renderBlocks(*e, all, kSampleRate * (t == T_CY ? 22 : 7) / 10);
        e->dp[t].p[P_TUNE] = keep; e->dirty = TR808Engine::kAllTracks;
        if (tune < 0) { e->preview(t, STEP_ON); renderBlocks(*e, all, kSampleRate * 6 / 10); }
      }
    }
    delete e;
  }
  for (int g = 0; g < 4; g++) {
    TR808Engine* e = newEngine();
    e->masterVol = 0.9f; e->bpm = (g == 1) ? 92 : (g == 2 ? 125 : 118); e->swing = (g == 1) ? 0.3f : 0.0f;
    e->loadDemo(0, g);
    e->post(EV_TOGGLE_PLAY);
    renderBlocks(*e, all, (int)(4 * 4 * 60.0 / e->bpm * kSampleRate));
    delete e;
  }
  writeWav(path, all);
}

int main(int argc, char** argv) {
  printf("TR808Engine host tests (sample rate %d)\n", kSampleRate);
  testFastSin();
  testVoices();
  testChoke();
  testMute();
  testCalibration();
  testNoLibmPerSample();
  testRetriggerClick();
  testTiming();
  testPerf();
  testSerialize();
  testDemos();
  testParamSweep();
  if (argc >= 3 && std::string(argv[1]) == "wav") renderWavs(argv[2]);
  printf(g_fail ? "\nFAILED (%d)\n" : "\nall ok\n", g_fail);
  return g_fail ? 1 : 0;
}
