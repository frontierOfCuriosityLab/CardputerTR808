// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ryu_muto
// ============================================================
// TR808Engine.h - TR-808 style drum voices + 16-step sequencer core
// ============================================================
// Plain C++ (no M5/ESP32 dependency): the same code runs on the Cardputer and
// in test/host_test.cpp on a PC.
//
// Threading model (see CardputerTR808.ino):
//   * audio task : the ONLY caller of TR808Engine::render(). Owns every voice
//                  and all sequencer state (playing, step position).
//   * UI task    : edits patterns / parameters directly (plain loads and
//                  stores, benign races) and talks to the audio task via
//                  post() - a single-producer/single-consumer ring.
//
// Voices (all self-written, no per-sample powf/sinf/expf):
//   BD  bass drum   sine + pitch drop + click, soft-clipped
//   SD  snare drum  two sines (~180 Hz + ~330 Hz) + high-passed noise
//   HT  hi tom      sine + pitch drop
//   LT  low tom     sine + pitch drop
//   CL  claves      short ~2.4 kHz ping
//   RS  rim shot    low + high partial + noise tick
//   CP  hand clap   noise -> band-pass, 3 quick bursts + tail
//   CB  cowbell     two squares (540 / 800 Hz) -> band-pass
//   CY  cymbal      six detuned squares (808 metal bank) -> filters, long decay
//   OH  open hat    same metal bank, band-pass + high-pass
//   CH  closed hat  same metal bank, short; chokes the open hat
// Every step is off / on / accent (an accented hit is louder, like the 808's ACCENT).
// ============================================================
#pragma once

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <cmath>
#include <atomic>
#include <initializer_list>

// ------------------------------------------------------------
// Config
// ------------------------------------------------------------
#ifndef TR808_SAMPLE_RATE
#define TR808_SAMPLE_RATE 44100
#endif
static constexpr int   kSampleRate  = TR808_SAMPLE_RATE;
static constexpr int   kNumTracks   = 11;
static constexpr int   kNumParams   = 4;    // per track: Tune, Decay, a voice specific one, Level
static constexpr int   kNumSteps    = 16;
static constexpr int   kNumPatterns = 4;
static constexpr int   kScopeLen    = 240;
static constexpr float kOutScale    = 26000.0f;  // float(+-1) -> int16

enum Track : uint8_t { T_BD = 0, T_SD, T_HT, T_LT, T_CL, T_RS, T_CP, T_CB, T_CY, T_OH, T_CH };
enum Param : uint8_t { P_TUNE = 0, P_DECAY, P_TONE, P_LEVEL };
enum Step  : uint8_t { STEP_OFF = 0, STEP_ON = 1, STEP_ACCENT = 2 };

static const char* const trackNames[kNumTracks] = {"BD", "SD", "HT", "LT", "CL", "RS", "CP", "CB", "CY", "OH", "CH"};
static const char* const trackTitles[kNumTracks] = {
  "BASS DRUM", "SNARE DRUM", "HI TOM", "LOW TOM", "CLAVES", "RIM SHOT",
  "HAND CLAP", "COWBELL", "CYMBAL", "OPEN HI-HAT", "CLOSED HI-HAT"};
static const char* const paramNames[kNumTracks][kNumParams] = {
  {"Tune", "Decay", "Click",  "Level"},   // BD
  {"Tune", "Decay", "Snappy", "Level"},   // SD
  {"Tune", "Decay", "Click",  "Level"},   // HT
  {"Tune", "Decay", "Click",  "Level"},   // LT
  {"Tune", "Decay", "Click",  "Level"},   // CL
  {"Tune", "Decay", "Tone",   "Level"},   // RS
  {"Tune", "Decay", "Spread", "Level"},   // CP
  {"Tune", "Decay", "Tone",   "Level"},   // CB
  {"Tune", "Decay", "Tone",   "Level"},   // CY
  {"Tune", "Decay", "Tone",   "Level"},   // OH
  {"Tune", "Decay", "Tone",   "Level"},   // CH
};

// ------------------------------------------------------------
// Pattern data
// ------------------------------------------------------------
struct Pattern {
  uint8_t c[kNumTracks][kNumSteps];   // Step: 0 off, 1 on, 2 accent
};

struct DrumParams {
  volatile float p[kNumParams];   // all 0..1
};

// ------------------------------------------------------------
// DSP helpers
// ------------------------------------------------------------
namespace drum {

constexpr float kPi    = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717959f;

inline float fclamp(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

// v in 0..1 -> lo..hi on an exponential (musical) scale
inline float expMap(float v, float lo, float hi) { return lo * powf(hi / lo, fclamp(v, 0.0f, 1.0f)); }

// per-sample multiplier that falls by 1/e every `tau` seconds
inline float tauToMul(float tau) { return expf(-1.0f / (tau * (float)kSampleRate)); }

// sin(2*pi*phase), phase in [0,1): parabolic approximation with a correction
// term (max error ~1e-3, i.e. -60 dB) - no libm call per sample.
inline float fastSin(float phase) {
  float x = (phase - 0.5f) * kTwoPi;                     // -pi..pi, x = 2*pi*phase - pi
  float y = 1.27323954f * x - 0.405284735f * x * fabsf(x);
  y = 0.225f * (y * fabsf(y) - y) + y;
  return -y;                                             // sin(x + pi) = -sin(x)
}

// Smooth saturation: unity slope near 0, exactly +-1 from |x| >= 1.5
inline float softClip(float x) {
  x = fclamp(x, -1.5f, 1.5f);
  return x - x * x * x * (1.0f / 6.75f);
}

// xorshift32 white noise, -1..1
struct Noise {
  uint32_t s;
  void seed(uint32_t v) { s = v ? v : 0x2545F491u; }
  inline float next() {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return (float)(int32_t)s * (1.0f / 2147483648.0f);
  }
};

// RBJ biquad, transposed direct form II. Coefficients are computed only when a
// parameter changes (they need sinf/cosf), never per sample.
struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

  void reset() { z1 = z2 = 0; }

  inline float process(float x) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }

  void set(float nb0, float nb1, float nb2, float a0, float na1, float na2) {
    float inv = 1.0f / a0;
    b0 = nb0 * inv; b1 = nb1 * inv; b2 = nb2 * inv; a1 = na1 * inv; a2 = na2 * inv;
  }

  static float clampFc(float fc) { return fclamp(fc, 20.0f, 0.45f * (float)kSampleRate); }

  void bandpass(float fc, float q) {   // constant 0 dB peak gain
    float w = kTwoPi * clampFc(fc) / (float)kSampleRate;
    float al = sinf(w) / (2.0f * q), c = cosf(w);
    set(al, 0, -al, 1.0f + al, -2.0f * c, 1.0f - al);
  }

  void highpass(float fc, float q) {
    float w = kTwoPi * clampFc(fc) / (float)kSampleRate;
    float al = sinf(w) / (2.0f * q), c = cosf(w);
    set((1 + c) * 0.5f, -(1 + c), (1 + c) * 0.5f, 1.0f + al, -2.0f * c, 1.0f - al);
  }
};

// Amplitude envelope: exponential decay, but a retrigger ramps up from the
// current level over ~0.5 ms instead of jumping (no click when a long tail is
// still ringing).
struct Env {
  float v = 0, mul = 0.999f;
  bool  atk = false;
  static constexpr float kStep = 1.0f / (0.0005f * (float)kSampleRate);
  void  trig() { atk = true; }
  inline float next() {
    if (atk) { v += kStep; if (v >= 1.0f) { v = 1.0f; atk = false; } }
    else     { v *= mul; }
    return v;
  }
  void kill() { v = 0; atk = false; }
};

}  // namespace drum

// ------------------------------------------------------------
// Tonal voices: BD SD HT LT CL RS share one structure, described by a table.
//   body  = sine (+ optional 2nd partial) with a fast pitch drop
//   noise = high-passed noise with its own envelope   (the snare's "snappy")
//   click = short low-passed noise tick               (the attack)
// The third parameter ("X") scales the noise and/or the click.
// ------------------------------------------------------------
struct TonalCfg {
  float fLo, fHi;                       // Tune range, Hz
  float drop, dropTau;                  // start pitch = f * (1 + drop), falls with time constant dropTau (s)
  float ampLo, ampHi;                   // Decay range: time constant of the body, s
  float p2Ratio, p2Amp, p2Tau;          // second partial: frequency ratio, level, decay relative to the body
  float noiseBase, noiseX, noiseHp;     // noise level = base + X * noiseX, high-pass Hz
  float nTauLo, nTauHi;                 // noise decay range (follows Decay)
  float clickBase, clickX;              // click level = base + X * clickX
  float outGain;                        // scales the sum so that a full-level hit peaks near 1 before the soft clip
  float drive;                          // into the soft clip
};

//                                fLo   fHi   drop  dTau   ampLo ampHi p2R   p2A  p2T   nB    nX   nHp   nTLo  nTHi  cB    cX   out   drive
static const TonalCfg kCfgBD = {  40,   80,   1.3f, 0.012f, 0.06f, 0.50f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1500, 0.05f, 0.20f, 0.10f, 0.9f, 0.8f, 1.1f};
static const TonalCfg kCfgSD = { 140,  260,   0.35f, 0.008f, 0.02f, 0.09f, 1.83f, 0.55f, 0.6f, 0.15f, 1.2f, 1800, 0.04f, 0.20f, 0.0f, 0.0f, 0.45f, 1.0f};
static const TonalCfg kCfgHT = { 160,  320,   0.6f, 0.020f, 0.05f, 0.30f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1500, 0.05f, 0.20f, 0.05f, 0.5f, 0.9f, 1.0f};
static const TonalCfg kCfgLT = {  80,  160,   0.6f, 0.020f, 0.06f, 0.40f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1500, 0.05f, 0.20f, 0.05f, 0.5f, 0.9f, 1.0f};
static const TonalCfg kCfgCL = {1800, 3200,   0.05f, 0.004f, 0.006f, 0.05f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1500, 0.05f, 0.20f, 0.10f, 0.9f, 0.9f, 1.0f};
static const TonalCfg kCfgRS = { 350,  700,   0.15f, 0.004f, 0.005f, 0.04f, 3.6f, 0.9f, 1.0f, 0.0f, 0.5f, 3000, 0.004f, 0.015f, 0.3f, 0.0f, 0.5f, 1.2f};

struct Tonal {
  const TonalCfg* cfg = &kCfgBD;
  float baseHz, dropMul, noiseAmt, clickAmt, noiseMul, clickMul;
  float ph1, ph2, pitchEnv, noiseEnv, clickEnv, clickLp, gain;
  drum::Env e1, e2;
  drum::Biquad hp;
  drum::Noise noise;
  bool active;

  void init(const TonalCfg* c, uint32_t seed, const float* defaults) {
    cfg = c;
    noise.seed(seed);
    ph1 = ph2 = 0; pitchEnv = noiseEnv = clickEnv = clickLp = 0; gain = 0;
    e1.kill(); e2.kill(); hp.reset(); active = false;
    baseHz = cfg->fLo;
    clickMul = drum::tauToMul(0.002f);
    dropMul  = drum::tauToMul(cfg->dropTau);
    set(defaults);
  }

  void set(const volatile float* p) {
    baseHz = drum::expMap(p[P_TUNE], cfg->fLo, cfg->fHi);
    float tau = cfg->ampLo * powf(cfg->ampHi / cfg->ampLo, drum::fclamp(p[P_DECAY], 0.0f, 1.0f));
    e1.mul = drum::tauToMul(tau);
    e2.mul = drum::tauToMul(tau * cfg->p2Tau);
    float nt = cfg->nTauLo * powf(cfg->nTauHi / cfg->nTauLo, drum::fclamp(p[P_DECAY], 0.0f, 1.0f));
    noiseMul = drum::tauToMul(nt);
    hp.highpass(cfg->noiseHp, 0.707f);
    noiseAmt = cfg->noiseBase + cfg->noiseX * p[P_TONE];
    clickAmt = cfg->clickBase + cfg->clickX * p[P_TONE];
  }
  void set(const float* p) { set((const volatile float*)p); }

  void trigger(float g) {
    if (!active) ph1 = ph2 = 0;
    pitchEnv = noiseEnv = clickEnv = 1.0f;
    e1.trig(); e2.trig();
    gain = g; active = true;
  }

  inline float process() {
    if (!active) return 0.0f;
    float f = baseHz * (1.0f + cfg->drop * pitchEnv);
    pitchEnv *= dropMul;
    ph1 += f * (1.0f / (float)kSampleRate);
    if (ph1 >= 1.0f) ph1 -= 1.0f;

    float a1 = e1.next();
    float body = drum::fastSin(ph1) * a1;
    float a2 = e2.next();
    if (cfg->p2Amp > 0.0f) {
      ph2 += f * cfg->p2Ratio * (1.0f / (float)kSampleRate);
      if (ph2 >= 1.0f) ph2 -= 1.0f;
      body += cfg->p2Amp * drum::fastSin(ph2) * a2;
    }

    float n = 0.0f;
    if (noiseAmt > 0.0f) n = hp.process(noise.next()) * noiseEnv * noiseAmt;
    noiseEnv *= noiseMul;

    clickLp += 0.35f * (noise.next() - clickLp);
    float click = clickLp * clickEnv * clickAmt * 2.0f;
    clickEnv *= clickMul;

    if (!e1.atk && a1 < 0.0015f && a2 < 0.0015f && noiseEnv < 0.002f) active = false;
    return drum::softClip((body + n + click) * gain * cfg->outGain * cfg->drive);
  }
};

// ------------------------------------------------------------
// CP - hand clap: noise -> band-pass, 3 quick bursts, then the tail
//   Tune = band-pass centre, Decay = tail, X = Spread (gap between the bursts)
// ------------------------------------------------------------
struct Clap {
  float baseHz, q, makeup, fastMul, tailMul, env, gain;
  uint32_t gap, t;
  uint8_t  burst;
  drum::Biquad bp;
  drum::Noise noise;
  bool active;

  static constexpr uint8_t kBursts = 4;   // 3 fast + the tail

  void init(const float* defaults) {
    noise.seed(0x7f4a7c15u);
    env = 0; gain = 0; t = 0; burst = kBursts; bp.reset(); active = false;
    fastMul = drum::tauToMul(0.0035f);
    set(defaults);
  }

  void set(const volatile float* p) {
    baseHz  = drum::expMap(p[P_TUNE], 700.0f, 2400.0f);
    q       = 2.0f;
    tailMul = drum::tauToMul(0.02f + 0.16f * p[P_DECAY]);
    gap     = (uint32_t)((0.004f + 0.014f * p[P_TONE]) * (float)kSampleRate);
    filter();
  }
  void set(const float* p) { set((const volatile float*)p); }

  // White noise through a band-pass: the output amplitude follows sqrt(bandwidth / fs)
  // with bandwidth = fc/q; the make-up gain is its inverse, so the level stays put when
  // Tune or the sample rate change.
  void filter() {
    bp.bandpass(baseHz, q);
    makeup = 0.352f * sqrtf((float)kSampleRate * q / baseHz);
  }

  void trigger(float g) {
    t = 0; burst = 0; env = 0;
    gain = g; active = true;
  }

  inline float process() {
    if (!active) return 0.0f;
    if (burst < kBursts && t >= (uint32_t)burst * gap) { env = 1.0f; burst++; }
    float o = bp.process(noise.next()) * env * makeup;
    env *= (burst >= kBursts) ? tailMul : fastMul;
    t++;
    if (burst >= kBursts && env < 0.0015f) active = false;
    return o * gain;
  }
};

// ------------------------------------------------------------
// The 808's metallic source: six square oscillators, free-running, shared by
// CY / OH / CH (each filters and shapes it differently).
// ------------------------------------------------------------
struct MetalBank {
  static constexpr int kN = 6;
  float ph[kN], inc[kN];

  void init() {
    static const float hz[kN] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};
    for (int i = 0; i < kN; i++) { ph[i] = (float)i * 0.137f; inc[i] = hz[i] / (float)kSampleRate; }
  }

  inline float next() {
    float s = 0.0f;
    for (int i = 0; i < kN; i++) {
      ph[i] += inc[i];
      if (ph[i] >= 1.0f) ph[i] -= 1.0f;
      s += (ph[i] < 0.5f) ? 1.0f : -1.0f;
    }
    return s * (1.0f / (float)kN);
  }
};

// ------------------------------------------------------------
// OH / CH - hi-hats: metal bank -> band-pass (Tune) -> high-pass (Tone) -> envelope.
// A closed hat chokes the open one, as on the real machine.
// ------------------------------------------------------------
struct HatCfg {
  float bpLo, bpHi;      // Tune range (band-pass centre), Hz
  float hpLo, hpHi;      // Tone range (high-pass cutoff), Hz
  float tauLo, tauHi;    // Decay range, s
};
static const HatCfg kCfgOH = {5000.0f, 9500.0f, 3000.0f, 7000.0f, 0.04f, 0.35f};
static const HatCfg kCfgCH = {5000.0f, 9500.0f, 3000.0f, 7000.0f, 0.008f, 0.05f};

struct Hat {
  const HatCfg* cfg = &kCfgOH;
  drum::Biquad bp, hp;
  drum::Env env;
  float gain, normalMul, chokeMul;
  bool choking, active;

  void init(const HatCfg* c, const float* defaults) {
    cfg = c;
    bp.reset(); hp.reset(); env.kill();
    gain = 0; choking = false; active = false;
    chokeMul = drum::tauToMul(0.004f);
    set(defaults);
  }

  void set(const volatile float* p) {
    bp.bandpass(drum::expMap(p[P_TUNE], cfg->bpLo, cfg->bpHi), 1.0f);
    hp.highpass(drum::expMap(p[P_TONE], cfg->hpLo, cfg->hpHi), 0.707f);
    normalMul = drum::tauToMul(cfg->tauLo * powf(cfg->tauHi / cfg->tauLo, drum::fclamp(p[P_DECAY], 0.0f, 1.0f)));
    env.mul = choking ? chokeMul : normalMul;
  }
  void set(const float* p) { set((const volatile float*)p); }

  void trigger(float g) {
    choking = false; env.mul = normalMul;
    env.trig();
    gain = g; active = true;
  }
  void choke() { if (active) { choking = true; env.mul = chokeMul; } }

  inline float process(float metal) {
    if (!active) return 0.0f;
    float x = hp.process(bp.process(metal)) * env.next();
    if (!env.atk && env.v < 0.0015f) active = false;
    return x * gain;
  }
};

// ------------------------------------------------------------
// CY - cymbal: metal bank -> (high-pass + band-pass) crossfaded by Tone; long decay
//   with a brighter, louder first ~50 ms.
// ------------------------------------------------------------
struct Cymbal {
  drum::Biquad hp, bp;
  drum::Env env;
  float mixHi, mixLo, fast, fastMul, gain;
  bool active;

  void init(const float* defaults) {
    hp.reset(); bp.reset(); env.kill();
    fast = 0; gain = 0; active = false;
    fastMul = drum::tauToMul(0.04f);
    set(defaults);
  }

  void set(const volatile float* p) {
    bp.bandpass(drum::expMap(p[P_TUNE], 2500.0f, 5500.0f), 1.0f);
    hp.highpass(6000.0f, 0.707f);
    env.mul = drum::tauToMul(0.12f * powf(0.45f / 0.12f, drum::fclamp(p[P_DECAY], 0.0f, 1.0f)));
    mixHi = 0.3f + 0.7f * p[P_TONE];
    mixLo = 1.0f - 0.7f * p[P_TONE];
  }
  void set(const float* p) { set((const volatile float*)p); }

  void trigger(float g) {
    env.trig(); fast = 1.0f;
    gain = g; active = true;
  }

  inline float process(float metal) {
    if (!active) return 0.0f;
    float x = hp.process(metal) * mixHi + bp.process(metal) * mixLo;
    float e = env.next();
    float o = x * e * (0.55f + 0.45f * fast);
    fast *= fastMul;
    if (!env.atk && e < 0.0015f) active = false;
    return o * gain;
  }
};

// ------------------------------------------------------------
// CB - cowbell: two squares (540 / 800 Hz, scaled by Tune) -> band-pass (Tone)
//   with a punchy first few ms.
// ------------------------------------------------------------
struct Cowbell {
  float inc1, inc2, ph1, ph2, fast, fastMul, gain;
  drum::Biquad bp;
  drum::Env env;
  bool active;

  void init(const float* defaults) {
    ph1 = ph2 = 0; fast = 0; gain = 0; bp.reset(); env.kill(); active = false;
    fastMul = drum::tauToMul(0.008f);
    set(defaults);
  }

  void set(const volatile float* p) {
    float mult = drum::expMap(p[P_TUNE], 0.7f, 1.4f);
    inc1 = 540.0f * mult / (float)kSampleRate;
    inc2 = 800.0f * mult / (float)kSampleRate;
    bp.bandpass(drum::expMap(p[P_TONE], 1200.0f, 3600.0f), 1.2f);
    env.mul = drum::tauToMul(0.04f * powf(0.4f / 0.04f, drum::fclamp(p[P_DECAY], 0.0f, 1.0f)));
  }
  void set(const float* p) { set((const volatile float*)p); }

  void trigger(float g) {
    if (!active) ph1 = ph2 = 0;
    env.trig(); fast = 1.0f;
    gain = g; active = true;
  }

  inline float process() {
    if (!active) return 0.0f;
    ph1 += inc1; if (ph1 >= 1.0f) ph1 -= 1.0f;
    ph2 += inc2; if (ph2 >= 1.0f) ph2 -= 1.0f;
    float s = ((ph1 < 0.5f) ? 0.5f : -0.5f) + ((ph2 < 0.5f) ? 0.5f : -0.5f);
    float e = env.next();
    float o = bp.process(s) * e * (0.55f + 0.45f * fast);
    fast *= fastMul;
    if (!env.atk && e < 0.0015f) active = false;
    return o * gain;
  }
};

// ------------------------------------------------------------
// Events from the UI task to the audio task
// ------------------------------------------------------------
enum EventType : uint8_t { EV_TOGGLE_PLAY = 0, EV_PREVIEW };

struct Event {
  uint8_t type;
  uint8_t a;   // track
  uint8_t b;   // Step level (ON / ACCENT)
};

// ------------------------------------------------------------
// Engine
// ------------------------------------------------------------
class TR808Engine {
 public:
  // --- shared with the UI ---
  Pattern    patterns[kNumPatterns];
  DrumParams dp[kNumTracks];
  volatile uint8_t  curPattern = 0;      // pattern being edited; also the next one to play
  volatile uint8_t  playPattern = 0;     // pattern currently playing (audio task writes)
  volatile uint16_t bpm = 120;
  volatile float    swing = 0.0f;        // 0..1 -> 0..33 % delay of the off-beat 16ths
  volatile float    masterVol = 0.7f;
  volatile bool     mute[kNumTracks] = {};
  volatile bool     playing = false;     // audio task writes only
  volatile uint8_t  lastStep = 0;        // step most recently triggered
  volatile float    scope[kScopeLen];
  volatile uint16_t scopeIdx = 0;
  volatile uint32_t healCount = 0;       // voices reset because of NaN / runaway values
  std::atomic<uint16_t> dirty{0xFFFF};   // bit per track: re-apply parameters

  static constexpr float kMaxSwing   = 0.33f;
  static constexpr float kNormalGain = 0.65f;   // a plain step
  static constexpr float kAccentGain = 1.0f;    // an accented step
  static constexpr uint16_t kAllTracks = (1u << kNumTracks) - 1;

  // Per-voice output gain, calibrated on the host (test/host_test.cpp): with the
  // default parameters, an accented hit and Level 1.0 every voice peaks at about 0.8.
  static constexpr float kTrackGain[kNumTracks] = {
    0.924f, 1.017f, 0.970f, 0.986f, 0.945f, 0.852f, 0.751f, 1.152f, 2.306f, 3.418f, 4.683f};

  static const float* defaultParams(int t) {
    //                                      Tune   Decay  X      Level
    static const float d[kNumTracks][kNumParams] = {
      {0.45f, 0.45f, 0.35f, 0.90f},   // BD
      {0.40f, 0.40f, 0.55f, 0.80f},   // SD
      {0.35f, 0.40f, 0.30f, 0.80f},   // HT
      {0.35f, 0.40f, 0.30f, 0.80f},   // LT
      {0.50f, 0.40f, 0.30f, 0.80f},   // CL
      {0.50f, 0.50f, 0.50f, 0.80f},   // RS
      {0.40f, 0.45f, 0.50f, 0.80f},   // CP
      {0.50f, 0.40f, 0.50f, 0.80f},   // CB
      {0.50f, 0.50f, 0.60f, 0.70f},   // CY
      {0.50f, 0.50f, 0.50f, 0.75f},   // OH
      {0.50f, 0.40f, 0.50f, 0.75f},   // CH
    };
    return d[t];
  }

  void init() {
    memset((void*)patterns, 0, sizeof(patterns));
    resetParams();
    initVoices();
    applyDirty();
    for (int t = 0; t < kNumTracks; t++) muteGain[t] = mute[t] ? 0.0f : 1.0f;
    for (int i = 0; i < kScopeLen; i++) scope[i] = 0;
  }

  void resetParams() {
    for (int t = 0; t < kNumTracks; t++) {
      const float* d = defaultParams(t);
      for (int i = 0; i < kNumParams; i++) dp[t].p[i] = d[i];
    }
    dirty = kAllTracks;
  }

  // ---- UI -> audio ----
  bool post(uint8_t type, uint8_t a = 0, uint8_t b = 0) {
    uint8_t h  = head.load(std::memory_order_relaxed);
    uint8_t nx = (h + 1) & (kRing - 1);
    if (nx == tail.load(std::memory_order_acquire)) return false;  // full: drop
    ring[h] = {type, a, b};
    head.store(nx, std::memory_order_release);
    return true;
  }
  void preview(int track, int level = STEP_ACCENT) { post(EV_PREVIEW, (uint8_t)track, (uint8_t)level); }

  bool trackActive(int t) const {
    switch (t) {
      case T_BD: return bd.active;  case T_SD: return sd.active;  case T_HT: return ht.active;
      case T_LT: return lt.active;  case T_CL: return cl.active;  case T_RS: return rs.active;
      case T_CP: return cp.active;  case T_CB: return cb.active;  case T_CY: return cy.active;
      case T_OH: return oh.active;  default:   return ch.active;
    }
  }

  // The Tune parameter in Hz (for the display)
  static float tuneHz(int track, float p) {
    switch (track) {
      case T_BD: return drum::expMap(p, kCfgBD.fLo, kCfgBD.fHi);
      case T_SD: return drum::expMap(p, kCfgSD.fLo, kCfgSD.fHi);
      case T_HT: return drum::expMap(p, kCfgHT.fLo, kCfgHT.fHi);
      case T_LT: return drum::expMap(p, kCfgLT.fLo, kCfgLT.fHi);
      case T_CL: return drum::expMap(p, kCfgCL.fLo, kCfgCL.fHi);
      case T_RS: return drum::expMap(p, kCfgRS.fLo, kCfgRS.fHi);
      case T_CP: return drum::expMap(p, 700.0f, 2400.0f);
      case T_CB: return 540.0f * drum::expMap(p, 0.7f, 1.4f);
      case T_CY: return drum::expMap(p, 2500.0f, 5500.0f);
      case T_OH: return drum::expMap(p, kCfgOH.bpLo, kCfgOH.bpHi);
      default:   return drum::expMap(p, kCfgCH.bpLo, kCfgCH.bpHi);
    }
  }

  // ---- audio task entry point ----
  void render(int16_t* out, int n) {
    processEvents();
    applyDirty();

    const float stepSamples = (float)kSampleRate * 60.0f / (float)bpm * 0.25f;   // 16th notes
    const float sw = fminf(fmaxf(swing, 0.0f), 1.0f) * kMaxSwing;
    const float vol = masterVol;
    float lvl[kNumTracks];
    for (int t = 0; t < kNumTracks; t++) lvl[t] = dp[t].p[P_LEVEL] * kTrackGain[t];
    float target[kNumTracks];
    for (int t = 0; t < kNumTracks; t++) target[t] = mute[t] ? 0.0f : 1.0f;

    for (int i = 0; i < n; i++) {
      if (playing) {
        stepTimer -= 1.0f;
        if (stepTimer <= 0.0f) {
          if (nextStep == 0) playPattern = curPattern;      // pattern changes land on the bar line
          triggerStep(nextStep);
          lastStep = nextStep;
          // after an even step the next (odd) one is late, after an odd one it is early
          stepTimer += stepSamples * ((nextStep & 1) ? 1.0f - sw : 1.0f + sw);
          nextStep = (nextStep + 1) % kNumSteps;
        }
      }

      const float m = (cy.active || oh.active || ch.active) ? metal.next() : 0.0f;
      float o[kNumTracks];
      o[T_BD] = bd.process();  o[T_SD] = sd.process();  o[T_HT] = ht.process();
      o[T_LT] = lt.process();  o[T_CL] = cl.process();  o[T_RS] = rs.process();
      o[T_CP] = cp.process();  o[T_CB] = cb.process();
      o[T_CY] = cy.process(m); o[T_OH] = oh.process(m); o[T_CH] = ch.process(m);

      float mix = 0.0f;
      for (int t = 0; t < kNumTracks; t++) {
        muteGain[t] += (target[t] - muteGain[t]) * 0.01f;     // ~2 ms: muting never clicks
        mix += o[t] * lvl[t] * muteGain[t];
      }

      if (!(fabsf(mix) < 16.0f)) {   // NaN or runaway: reset every voice
        initVoices(); dirty = kAllTracks; applyDirty();
        healCount = healCount + 1;
        mix = 0.0f;
      }

      mix = drum::softClip(mix * vol * kMasterGain);
      out[i] = (int16_t)(mix * kOutScale);

      if ((scopeDiv++ & 15) == 0) {
        scope[scopeIdx] = mix;
        scopeIdx = (scopeIdx + 1 >= kScopeLen) ? 0 : scopeIdx + 1;
      }
    }
  }

  // ---- patterns ----
  void clearPattern(int p) { memset((void*)&patterns[p], 0, sizeof(Pattern)); }

  // Four demo grooves: 0 = four-on-the-floor, 1 = boom bap, 2 = electro, 3 = latin
  void loadDemo(int pat, int which) {
    Pattern& p = patterns[pat];
    memset((void*)&p, 0, sizeof(Pattern));
    auto on  = [&](int t, std::initializer_list<int> s) { for (int i : s) p.c[t][i] = STEP_ON; };
    auto acc = [&](int t, std::initializer_list<int> s) { for (int i : s) p.c[t][i] = STEP_ACCENT; };
    switch (which & 3) {
      case 0:  // four on the floor
        acc(T_BD, {0, 8});  on(T_BD, {4, 12});
        on(T_CP, {4, 12});
        on(T_CH, {0, 1, 3, 4, 5, 7, 8, 9, 11, 12, 13, 15});  acc(T_CH, {0, 8});
        on(T_OH, {2, 6, 10, 14});
        break;
      case 1:  // boom bap
        acc(T_BD, {0, 10});  on(T_BD, {3, 7, 11});
        acc(T_SD, {4, 12});  on(T_SD, {15});
        on(T_CH, {0, 2, 4, 6, 8, 10, 12, 14});  acc(T_CH, {0, 8});
        on(T_RS, {7});  on(T_OH, {15});
        break;
      case 2:  // electro
        acc(T_BD, {0, 10});  on(T_BD, {3, 6, 12});
        acc(T_SD, {4, 12});
        on(T_CP, {4, 12});
        on(T_CB, {2, 7, 14});
        on(T_CL, {5, 13});
        on(T_CH, {0, 2, 4, 6, 8, 10, 12, 14});
        acc(T_CY, {0});
        on(T_HT, {13});  on(T_LT, {14, 15});
        break;
      default:  // latin: 3-2 son clave, tom tumbao
        acc(T_CL, {0, 3, 6, 10, 12});
        acc(T_BD, {0, 8});  on(T_BD, {6});
        on(T_LT, {2, 4, 10, 14});  on(T_HT, {6, 7, 12, 15});
        on(T_CB, {0, 4, 8, 12});
        on(T_CH, {0, 2, 4, 6, 8, 10, 12, 14});
        on(T_RS, {5, 13});  on(T_CP, {12});
        acc(T_CY, {0});
        break;
    }
  }

  // ---- persistence: flat byte blob (host-testable; the sketch stores it in NVS) ----
  static constexpr uint32_t kMagic = 0x54523831u;   // "TR81"
  static constexpr size_t   kBlobSize = 4 + 2 + 4 + 4 + 2 + kNumTracks * kNumParams * 4
                                        + kNumPatterns * kNumTracks * kNumSteps + 2;

  size_t serialize(uint8_t* buf, size_t cap) const {
    if (cap < kBlobSize) return 0;
    uint8_t* w = buf;
    auto put = [&](const void* src, size_t n) { memcpy(w, src, n); w += n; };
    uint32_t magic = kMagic; put(&magic, 4);
    uint16_t b = bpm;        put(&b, 2);
    float sw = swing;        put(&sw, 4);
    float mv = masterVol;    put(&mv, 4);
    uint16_t mm = 0;
    for (int t = 0; t < kNumTracks; t++) if (mute[t]) mm |= (uint16_t)(1u << t);
    put(&mm, 2);
    for (int t = 0; t < kNumTracks; t++)
      for (int i = 0; i < kNumParams; i++) { float v = dp[t].p[i]; put(&v, 4); }
    for (int p = 0; p < kNumPatterns; p++)
      for (int t = 0; t < kNumTracks; t++)
        for (int s = 0; s < kNumSteps; s++) *w++ = patterns[p].c[t][s];
    uint16_t sum = checksum(buf, (size_t)(w - buf));
    put(&sum, 2);
    return (size_t)(w - buf);
  }

  // Validates everything before touching the engine; returns false (and changes
  // nothing) on a wrong size, magic, checksum or non-finite value.
  bool deserialize(const uint8_t* buf, size_t len) {
    if (len != kBlobSize) return false;
    uint32_t magic; memcpy(&magic, buf, 4);
    if (magic != kMagic) return false;
    uint16_t stored; memcpy(&stored, buf + len - 2, 2);
    if (stored != checksum(buf, len - 2)) return false;

    const uint8_t* r = buf + 4;
    auto get = [&](void* dst, size_t n) { memcpy(dst, r, n); r += n; };
    uint16_t b; get(&b, 2);
    float sw, mv; get(&sw, 4); get(&mv, 4);
    uint16_t mm; get(&mm, 2);
    float par[kNumTracks][kNumParams];
    for (int t = 0; t < kNumTracks; t++)
      for (int i = 0; i < kNumParams; i++) { get(&par[t][i], 4); if (!std::isfinite(par[t][i])) return false; }
    if (!std::isfinite(sw) || !std::isfinite(mv)) return false;

    Pattern tmp[kNumPatterns];
    for (int p = 0; p < kNumPatterns; p++)
      for (int t = 0; t < kNumTracks; t++)
        for (int s = 0; s < kNumSteps; s++) {
          uint8_t v = *r++;
          tmp[p].c[t][s] = v > STEP_ACCENT ? (uint8_t)STEP_ACCENT : v;
        }

    bpm = b < 40 ? 40 : (b > 300 ? 300 : b);
    swing = drum::fclamp(sw, 0.0f, 1.0f);
    masterVol = drum::fclamp(mv, 0.0f, 1.0f);
    for (int t = 0; t < kNumTracks; t++) {
      mute[t] = (mm >> t) & 1;
      for (int i = 0; i < kNumParams; i++) dp[t].p[i] = drum::fclamp(par[t][i], 0.0f, 1.0f);
    }
    memcpy((void*)patterns, tmp, sizeof(patterns));
    dirty = kAllTracks;
    return true;
  }

 private:
  static constexpr float    kMasterGain = 0.85f;
  static constexpr uint8_t  kRing = 16;  // power of two
  Event ring[kRing];
  std::atomic<uint8_t> head{0}, tail{0};

  // sequencer (audio-task private)
  float    stepTimer = 0.0f;
  uint8_t  nextStep = 0;
  uint32_t scopeDiv = 0;
  float    muteGain[kNumTracks] = {};

  Tonal    bd, sd, ht, lt, cl, rs;
  Clap     cp;
  Cowbell  cb;
  Cymbal   cy;
  Hat      oh, ch;
  MetalBank metal;

  void initVoices() {
    bd.init(&kCfgBD, 0x1badb002u, defaultParams(T_BD));
    sd.init(&kCfgSD, 0x2545f491u, defaultParams(T_SD));
    ht.init(&kCfgHT, 0x9e3779b9u, defaultParams(T_HT));
    lt.init(&kCfgLT, 0x85ebca6bu, defaultParams(T_LT));
    cl.init(&kCfgCL, 0xc2b2ae35u, defaultParams(T_CL));
    rs.init(&kCfgRS, 0x27d4eb2fu, defaultParams(T_RS));
    cp.init(defaultParams(T_CP));
    cb.init(defaultParams(T_CB));
    cy.init(defaultParams(T_CY));
    oh.init(&kCfgOH, defaultParams(T_OH));
    ch.init(&kCfgCH, defaultParams(T_CH));
    metal.init();
  }

  static uint16_t checksum(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0;   // Adler-style
    for (size_t i = 0; i < n; i++) { a = (a + p[i]) % 251; b = (b + a) % 251; }
    return (uint16_t)((b << 8) | a);
  }

  void processEvents() {
    uint8_t t = tail.load(std::memory_order_relaxed);
    while (t != head.load(std::memory_order_acquire)) {
      Event e = ring[t];
      t = (t + 1) & (kRing - 1);
      tail.store(t, std::memory_order_release);
      switch (e.type) {
        case EV_TOGGLE_PLAY:
          playing = !playing;
          if (playing) { nextStep = 0; stepTimer = 0.0f; playPattern = curPattern; }  // step 0 on the very next sample
          break;
        case EV_PREVIEW:
          if (e.a < kNumTracks) trig(e.a, e.b);
          break;
      }
    }
  }

  void applyDirty() {
    uint16_t m = dirty.exchange(0);
    for (int t = 0; t < kNumTracks; t++) {
      if (!(m & (1u << t))) continue;
      const volatile float* p = dp[t].p;
      switch (t) {
        case T_BD: bd.set(p); break;  case T_SD: sd.set(p); break;  case T_HT: ht.set(p); break;
        case T_LT: lt.set(p); break;  case T_CL: cl.set(p); break;  case T_RS: rs.set(p); break;
        case T_CP: cp.set(p); break;  case T_CB: cb.set(p); break;  case T_CY: cy.set(p); break;
        case T_OH: oh.set(p); break;  case T_CH: ch.set(p); break;
      }
    }
  }

  void trig(int t, int level) {
    float g = (level >= STEP_ACCENT) ? kAccentGain : kNormalGain;
    switch (t) {
      case T_BD: bd.trigger(g); break;  case T_SD: sd.trigger(g); break;  case T_HT: ht.trigger(g); break;
      case T_LT: lt.trigger(g); break;  case T_CL: cl.trigger(g); break;  case T_RS: rs.trigger(g); break;
      case T_CP: cp.trigger(g); break;  case T_CB: cb.trigger(g); break;  case T_CY: cy.trigger(g); break;
      case T_OH: oh.trigger(g); break;
      case T_CH: oh.choke(); ch.trigger(g); break;
    }
  }

  void triggerStep(uint8_t step) {
    const Pattern& p = patterns[playPattern];
    for (int t = 0; t < kNumTracks; t++) {
      uint8_t c = p.c[t][step];
      if (c && !mute[t]) trig(t, c);
    }
  }
};

// C++14 (older Arduino-ESP32 cores): constexpr static data members need an
// out-of-class definition when ODR-used.
constexpr float TR808Engine::kTrackGain[kNumTracks];
