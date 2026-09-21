# Third-party notices / 第三者ソフトウェアの表記

English first, 日本語は下にあります。

---

## English

### What this repository contains, and under which license

| Part | License | Notes |
|---|---|---|
| `CardputerTR808.ino`, `TR808Engine.h`, `test/`, docs | **MIT** (`LICENSE`) | Written for this project. |
| Structure of the UI / keyboard / audio-task code in `CardputerTR808.ino` | **MIT** (`LICENSE-CardputerTracker`) | Lineage: [*Cardputer-Adv-Tracker*](https://github.com/qwertyuu/Cardputer-Adv-Tracker) → *CardputerDaisyTracker* → *CardputerDrumTracker* → this project. The upstream copyright notice must be kept. |
| All 11 drum voices (DSP) | **MIT** (`LICENSE`) | Self-written, **no samples, no DaisySP, no third-party DSP code**. |

Upstream: [https://github.com/qwertyuu/Cardputer-Adv-Tracker](https://github.com/qwertyuu/Cardputer-Adv-Tracker) (MIT, "Copyright (c) 2026 Cardputer-Adv-Tracker Contributors"). `LICENSE-CardputerTracker` is a verbatim copy of its `LICENSE`.

Nothing from the other sketches in the author's sketchbook (for example DaisySP-based ones) is included.

### Libraries you need to build it (not included in this repository)

| Component | License | Used for |
|---|---|---|
| [M5Cardputer](https://github.com/m5stack/M5Cardputer) | MIT (M5Stack) | keyboard, board glue |
| [M5Unified](https://github.com/m5stack/M5Unified), [M5GFX](https://github.com/m5stack/M5GFX), M5Utility | MIT (M5Stack) | speaker output, display, canvas |
| Arduino-ESP32 core, M5Stack fork (`Preferences`, FreeRTOS, `Arduino.h`, ...) | Mixed: **LGPL-2.1** for parts of the core, **Apache-2.0** for others (e.g. `Preferences`), MIT for others | build environment, flash storage |

### What this means in practice

* **You publish only source code (the normal case for a GitHub repository).**
  The libraries above are *not* copied into the repository; users install them themselves. Their licenses do not change how you may license your own source. You only have to **keep the MIT notice of Cardputer-Adv-Tracker** (`LICENSE-CardputerTracker`), because part of the code descends from it.
* **You also publish a compiled firmware (`.bin`) in Releases.**
  Then the binary contains code from the libraries, and their conditions apply to the binary:
  MIT / Apache-2.0 → include their copyright and license notices (for example a copy of this file plus the upstream license texts);
  LGPL-2.1 → say that the firmware uses LGPL parts, and make sure users can rebuild the firmware with a modified version of those parts. Publishing the full source of this project (as this repository does) together with the build instructions is the usual way to satisfy this.
  Check the exact license files of the versions you built with before releasing a binary.
* **Cardputer-Adv-Tracker's MIT license** allows any use (including relicensing your derived work) as long as its copyright notice and permission text stay in "all copies or substantial portions". That is why `LICENSE-CardputerTracker` sits next to `LICENSE`.

### Trademarks

"TR-808" and "Roland" are trademarks of Roland Corporation. This project is an independent, unofficial hobby implementation, is not affiliated with or endorsed by Roland, and contains no Roland code, samples or artwork. The sounds are synthesized from scratch; the oscillator frequencies of the metallic voices (205.3, 304.4, 369.6, 522.7, 540, 800 Hz) are publicly documented facts about the original circuit.

This file is an explanation, not legal advice.

---

## 日本語

### このリポジトリの中身とライセンス

| 部分 | ライセンス | 補足 |
|---|---|---|
| `CardputerTR808.ino`、`TR808Engine.h`、`test/`、docs | **MIT** (`LICENSE`) | このプロジェクトのために書いたもの |
| `CardputerTR808.ino` の UI・キー入力・オーディオタスクの骨格 | **MIT** (`LICENSE-CardputerTracker`) | 系譜: [*Cardputer-Adv-Tracker*](https://github.com/qwertyuu/Cardputer-Adv-Tracker) → *CardputerDaisyTracker* → *CardputerDrumTracker* → 本プロジェクト。上流の著作権表示を残す必要があります |
| 11 音源すべて (DSP) | **MIT** (`LICENSE`) | 自作。**サンプル音源・DaisySP・他人の DSP コードは不使用** |

上流: [https://github.com/qwertyuu/Cardputer-Adv-Tracker](https://github.com/qwertyuu/Cardputer-Adv-Tracker) (MIT、"Copyright (c) 2026 Cardputer-Adv-Tracker Contributors")。`LICENSE-CardputerTracker` は、その `LICENSE` をそのまま複製したものです。

作者の他のスケッチ (DaisySP を使うものなど) のコードは含まれていません。

### ビルドに必要なライブラリ (このリポジトリには含まれません)

| 部品 | ライセンス | 用途 |
|---|---|---|
| [M5Cardputer](https://github.com/m5stack/M5Cardputer) | MIT (M5Stack) | キーボード、ボード依存部分 |
| [M5Unified](https://github.com/m5stack/M5Unified)、[M5GFX](https://github.com/m5stack/M5GFX)、M5Utility | MIT (M5Stack) | スピーカー出力、ディスプレイ、キャンバス |
| Arduino-ESP32 コア (M5Stack 版。`Preferences`、FreeRTOS、`Arduino.h` など) | ファイルごとに異なる: コアの一部は **LGPL-2.1**、`Preferences` などは **Apache-2.0**、MIT のものもある | ビルド環境、フラッシュ保存 |

### 実際に何をすればよいか

* **ソースコードだけを公開する場合 (GitHub リポジトリの普通の使い方)**
  上のライブラリはリポジトリに**コピーされない**ので、それらのライセンスがあなたのソースのライセンス選択を縛ることはありません。
  やることは **Cardputer-Adv-Tracker の MIT 表記 (`LICENSE-CardputerTracker`) を残すこと**だけです (コードの一部がその子孫のため)。
* **ビルド済みファームウェア (`.bin`) も Releases に置く場合**
  バイナリにはライブラリのコードが入るので、その条件がバイナリに及びます。
  MIT / Apache-2.0 → 著作権表示とライセンス文を同梱する (このファイルと各ライセンス文のコピーなど)。
  LGPL-2.1 → LGPL の部分を使っていると明記し、利用者がその部分を改変版に差し替えてビルドし直せるようにする。
  このプロジェクトの全ソースとビルド手順を公開していれば、通常はこれで満たせます。
  バイナリを出す前に、実際にビルドに使ったバージョンのライセンスファイルを確認してください。
* **Cardputer-Adv-Tracker の MIT ライセンス**は、著作権表示と許諾文が「すべてのコピーまたは実質的な部分」に残っていれば、派生物を別のライセンスにすることも含めて自由に使えます。`LICENSE` の隣に `LICENSE-CardputerTracker` を置いているのはそのためです。

### 商標について

「TR-808」「Roland」はローランド株式会社の商標です。本プロジェクトは個人による非公式の趣味の実装で、ローランドとは無関係であり、公認・推奨を受けたものでもありません。Roland のコード・サンプル音・画像は含まず、音はすべてゼロから合成しています。メタル系音源の発振周波数 (205.3 / 304.4 / 369.6 / 522.7 / 540 / 800 Hz) は、実機回路について広く公開されている事実です。

このファイルは説明であり、法的助言ではありません。
