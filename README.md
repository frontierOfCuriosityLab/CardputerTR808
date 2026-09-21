# CardputerTR808

**English** | [日本語](README.ja.md)

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

A **TR-808 style rhythm machine** for the M5Stack Cardputer / Cardputer-Adv (ESP32-S3).
Every sound is synthesized from scratch (no samples), and the engine is kept separate from the UI so it can be tested on a PC.

![Sequencer page](docs/screen-sequencer.png)
![Edit page](docs/screen-edit.png)

> The images are renderings of the screen produced by the host-side test (`test/ui_preview.sh`), not photos of the device.

- **16-step sequencer** – the playhead runs **left → right** (11 voices as rows, 16 steps as columns)
- **4 patterns** (A–D) – switchable while playing (the switch lands on the bar line), savable to flash
- **11 voices**: BD SD HT LT CL RS CP CB CY OH CH
- **An edit page for every voice** (Tune / Decay / one voice-specific knob / Level) plus a master tab (Volume / BPM / Swing)
- **Per-voice mute** – applies to all patterns, is saved, and fades over ~2 ms so it never clicks
- Each step is **off / on / accent** (an accented step is louder, like the 808's ACCENT)

## Requirements

- M5Stack **Cardputer** or **Cardputer-Adv** (ESP32-S3)
- Arduino IDE (or arduino-cli) with the M5Stack board package (board: `M5Stack > M5Cardputer`) and the **M5Cardputer** library

## Build and upload

The sketch folder must be named `CardputerTR808`, the same as the `.ino` file (an Arduino IDE rule).

Arduino IDE: open `CardputerTR808.ino`, select the board `M5Stack > M5Cardputer`, upload.
CLI:

```sh
arduino-cli compile --fqbn m5stack:esp32:m5stack_cardputer CardputerTR808
arduino-cli upload  --fqbn m5stack:esp32:m5stack_cardputer -p <PORT> CardputerTR808
```

## Voices

| Key | Sound | Built from | Voice-specific knob |
|---|---|---|---|
| **BD** | Bass drum | sine + pitch drop + click, soft-clipped | Click |
| **SD** | Snare drum | two sines (~180 Hz + ~330 Hz) + high-passed noise | Snappy (noise level) |
| **HT** | Hi tom | sine + pitch drop | Click |
| **LT** | Low tom | same, lower range | Click |
| **CL** | Claves | short ~2.4 kHz ping | Click |
| **RS** | Rim shot | low + high partial + noise tick | Tone (noise level) |
| **CP** | Hand clap | noise → band-pass, 3 quick bursts + tail | Spread (gap between bursts) |
| **CB** | Cowbell | two squares (540 Hz / 800 Hz) → band-pass | Tone (filter) |
| **CY** | Cymbal | the 808's metallic bank (six squares) → filters, long decay | Tone (high / mid balance) |
| **OH** | Open hi-hat | metallic bank → band-pass → high-pass | Tone (high-pass) |
| **CH** | Closed hi-hat | same, short decay; **chokes the open hat** | Tone (high-pass) |

Every voice has `Tune`, `Decay` and `Level`. CY / OH / CH share six free-running square oscillators
(205.3 / 304.4 / 369.6 / 522.7 / 540 / 800 Hz, the values of the original circuit).

## Controls

### Sequencer page (start screen)

| Key | Action |
|---|---|
| `SPACE` | Play / stop (playback starts at step 1) |
| `W` `S` (`;` `.`) | Voice (row) up / down |
| `A` `D` (`,` `/`) | Step (column) left / right (hold to repeat) |
| `Q` `E` | Back / forward one beat (4 steps) |
| `ENTER` | Cycle the step at the cursor: **off → on → accent → off** (the cursor stays) |
| `DEL` | Clear the step |
| `Z` | Audition the selected voice |
| `M` | Mute / unmute the selected voice (its label gets a red strike-through) |
| `1` `2` `3` `4` | Select pattern A / B / C / D (**while playing, the change lands on the bar line**; green frame = playing, yellow = being edited) |
| `[` `]` | BPM −/+ 1 (Shift: ±10) |
| `-` `=` | Master volume −/+ |
| `P` | Load a demo groove into the current pattern (four-on-the-floor → boom bap → electro → latin) |
| `\` twice | Clear the pattern (the first press only asks for confirmation) |
| `K` / `L` | Save / load (on-board flash) |
| `TAB` / `BtnA` | Change page (sequencer → edit → help) |

Step colors follow the original machine: red / orange / yellow / white for each group of four steps.
A darker color is "on"; a bright color with a white cap is an accent.

### Edit page

The tabs at the top are `BD SD HT LT CL RS CP CB CY OH CH MS` (muted voices are struck through).

| Key | Action |
|---|---|
| `Q` `E` | Previous / next voice (the last tab, MS, is the master) |
| `W` `S` | Select a parameter |
| `A` `D` | Change the value (5 % per step, hold to repeat; **Shift + A/D = 1 %**) |
| `Z` / `ENTER` | Audition (changing a value while stopped also plays the voice) |
| `M` | Mute (on the MS tab: the voice selected on the sequencer page) |

A scope on the right shows the last ~90 ms of the master output. `1`–`4` (pattern), `SPACE`, `[` `]` also work here, so you can shape sounds while the pattern plays.

On first start, patterns A–D contain four demo grooves. `K` saves; the song is restored at the next start
(saved: 4 patterns, all sound settings, BPM, swing, volume, mutes. The sound may drop out briefly while flash is written).

## Files

| File | Content |
|---|---|
| `CardputerTR808.ino` | UI / keyboard / speaker / saving (the only M5-dependent file) |
| `TR808Engine.h` | 11 voices + sequencer + save format (no M5 dependency → testable on a PC) |
| `test/run.sh` | Engine tests (`test/host_test.cpp`). `test/run.sh wav out.wav` writes a WAV with every voice and the 4 grooves for listening |
| `test/ui_preview.sh` | Builds the `.ino` itself against stubs (`test/stubs`) on the PC, tests the key handling and writes every screen as SVG |
| `LICENSE` / `LICENSE-CardputerTracker` / `THIRD_PARTY_NOTICES.md` | Licenses (see below) |

Threading: core 1 = `audioTask` (`engine.render()` → `Speaker.playRaw()`), core 0 = `uiTask` (keys, drawing).
The UI talks to the audio task through a ring buffer (`post()`).

## Tests (run on a PC, only a C++ compiler is needed)

```sh
test/run.sh          # voices, sequencer, save format
test/ui_preview.sh   # key handling, screen layout (also writes SVGs)
```

`run.sh` checks: all 11 voices have matching peak levels / every voice stays finite, bounded and returns to silence over the whole
parameter space (2200 cases) / BD and toms fall in pitch and the Tune ranges match the tables / hats are bright and the kick is dark, decay order CH < OH < CY /
**CH chokes OH** / **a muted voice is silent, and muting a ringing voice does not click** / accents are louder than plain steps /
**zero `powf`/`sinf`/`expf` calls per sample** / retriggering does not click /
step spacing and swing are sample-accurate, the playhead walks 1→16 in order, pattern changes land on the bar line, each of the 4 patterns plays its own notes /
save data is validated (corrupt data is rejected).
About 40 ns per sample on a PC in the worst case (all 11 voices on every step). Even 30–50× slower on the ESP32-S3 fits
the per-sample budget (22.7 µs at 44.1 kHz).

`ui_preview.sh` verifies the key handling (three step states, cursor movement, pattern switching, mute, edit-page tabs / parameters / key repeat, save / load),
and that text on every screen neither overlaps nor leaves the screen, and that the 16 × 11 grid fits.

## Status

The PC tests and the build for the Cardputer (`m5stack:esp32:m5stack_cardputer`, no warnings in this project's files, 44 % flash / 10 % RAM) pass, and the author has confirmed that it runs on a real Cardputer. The sound character (kick weight, cowbell and hat brightness, ...) is an original DSP aiming at "808-like",
not a circuit-accurate model, so adjust it on the edit page to taste.

- The sample rate is 44.1 kHz. If audio stutters on the device, set `TR808_SAMPLE_RATE` to `22050` in `TR808Engine.h`
  (the host tests pass at 22.05 kHz, but cymbal and hats become 2–4 dB quieter and lose the highs around 9 kHz).
- Master volume is `-` `=` (or MS → Volume on the edit page). Lower it if the speaker distorts.
- If all voices hit loudly at once, the master soft clipper engages.

## License

This project is released under the **MIT License** ([LICENSE](LICENSE)).
Part of the code descends from [*Cardputer-Adv-Tracker*](https://github.com/qwertyuu/Cardputer-Adv-Tracker) (also MIT); its copyright notice is kept in [LICENSE-CardputerTracker](LICENSE-CardputerTracker).
The libraries needed to build it (M5Cardputer / M5Unified / M5GFX are MIT; the Arduino-ESP32 core is a mix of LGPL-2.1, Apache-2.0 and others) are not part of this repository.
See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for a plain-language explanation.

"TR-808" and "Roland" are trademarks of Roland Corporation. This is an independent, unofficial hobby project, not affiliated with Roland, and it contains no Roland code, samples or artwork.
