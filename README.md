# SADES Engine — a reimplementation

[English](#english) · [日本語](#japanese)

---

<a id="english"></a>
## English

SADES is a reimplementation of the SADES visual-novel engine (NEC PC-9801) by
CP/You / Ankoku Baitai Softwares. This project reads the original game data
files and runs them on modern systems without an emulator.

Quick points

- What it plays
- Features
- Building
- Running
- Fonts
- Environment variables
- Missing features
- License & Legal

### What it plays

| Game | Year | Engine | Status |
|------|------|--------|--------|
| ゆこにすの森で.... (Yukonis no Mori de....) | 1990 | v1.1 | Playable end-to-end |
| SD Sadistic Gamers | 1990 | v1.5 | Playable end-to-end |
| Erion Quest | 1993 | v1.5 | Playable end-to-end |

All three games pass the headless regression tests with zero unknown opcodes.

### Features

- Complete VM for SADES bytecode (42 opcodes implemented)
- Graphics: .GLZ (LZ77 + palette) and .XSC (plane RLE)
- Shift-JIS → UTF-8 conversion, text rendering with FreeType
- Original layering: background, character slots, text overlay, tint pass
- "Blink marker" system (IDs 500–999) for pseudo-sprites and tint control
- Headless mode for regression testing
- Portable C99 codebase; dependencies: SDL2, FreeType

### Building

Requirements: C99 compiler, SDL2 (2.0.20+), FreeType (2.10+), libiconv (or glibc
iconv).

Debian / Ubuntu

```bash
sudo apt install build-essential libsdl2-dev libfreetype6-dev
make
```

Arch

```bash
sudo pacman -S base-devel sdl2 freetype2
make
```

macOS

```bash
brew install sdl2 freetype
make
```

Android (Termux)

```bash
pkg install clang make sdl2 freetype libiconv
make
cp sades ~/bin/sades
```

The `make` step produces a `sades` binary in the project root.

### Running

Basic play command:

```bash
./sades play path/to/GAME.SCE path/to/GAME.M00
```

Place .GLZ, .XSC, .PAL and TITLE.SYS next to the .SCE file.

Useful subcommands:

```
sades glz    <in.glz> <out.png> [--pal <pal.glz>]      decode a graphic to PNG
sades info   <in.glz>                                  print header and palette
sades title  <TITLE.SYS> <out.png>                     decode the engine logo
sades sce    <in.sce> <in.m00> [--trace] [--explore]   run the VM headless
sades show   <in.glz>                                  display one graphic
sades play   <in.sce> <in.m00> [--title <glz>] [--trace]   play with window
```

The headless runner (`sades sce`) prints a summary like:

```
--- 100000 ops, 68434 CALLs, 5275 wait_keys, 0 unknown ---
```

This summary is used to verify changes; unknown > 0 indicates a decoder problem.

### Fonts

You need a Japanese font (CJK Unified Ideographs). The engine renders ▽ itself
if the font lacks that symbol.

Recommended fonts:

- IPAex Gothic (closest to PC-98 look)
- VL Gothic
- Noto Sans CJK
- M+ 1m

Set font via SADES_FONT environment variable:

```bash
SADES_FONT=/path/to/ipag.ttf ./sades play game.sce game.m00
```

If unset, the engine searches common system font paths and a local `fonts/`
folder.

### Environment variables

- SADES_DX, SADES_DY = 0 — Adjust portrait positions (pixels)
- SADES_DELAY_MULT = 32 — milliseconds per DELAY tick
- SADES_ANIM_MS = 40 — frame delay between animation steps
- SADES_ANIM = 1 — set 0 to disable animation delays
- SADES_FONT = auto — path to Japanese font
- SADES_FORCE_TINT = 0 — force tint (1=red,2=white,3=green)
- SADES_TINT_TIMEOUT = 400 — auto-clear tint after N ms
- SADES_CK_R/G/B = -1 — optional color key for transparency
- SADES_MODE<N>_R/_G/_B — per-mode text color overrides (N = 0..7)

### Missing / planned

- Save/load (original games were designed for one sitting; unlikely to add)
- Audio (none — original engine had no sound)

### Legal and license

This project is not affiliated with CP/You or Ankoku Baitai Softwares. Engine
code here is the author's work and is released under the MIT license. Game
assets (.SCE, .M00, .GLZ, .XSC, .PAL, TITLE.SYS) are copyrighted by their
original authors and are not included. Use your own legal copies.

Screenshots are included for documentation only.

---

<a id="japanese"></a>
## 日本語

SADES は、NEC PC-9801 向けのビジュアルノベルエンジン（CP/You / 暗黒媒体ソフトウェアーズ）の再実装です。
このプロジェクトは、オリジナルのゲームデータを読み込み、エミュレータなしで現代の環境で動作させます。

クイックポイント

- 対応タイトル
- 特徴
- ビルド
- 実行方法
- フォント
- 環境変数
- 未実装 / 計画
- ライセンスと法的注意

### 対応タイトル

| ゲ���ム | 年 | エンジン | ステータス |
|--------|----|---------|------------|
| ゆこにすの森で.... (Yukonis no Mori de....) | 1990 | v1.1 | 最初から最後までプレイ可能 |
| SD Sadistic Gamers | 1990 | v1.5 | 最初から最後までプレイ可能 |
| Erion Quest | 1993 | v1.5 | 最初から最後までプレイ可能 |

上記の全タイトルはヘッドレス回帰テストで未知のオペコードが 0 であることを確認しています。

### 特徴

- SADES バイトコード用の完全な VM（42 オペコード実装）
- 画像: .GLZ（LZ77 + パレット）および .XSC（プレーン別 RLE）
- Shift-JIS → UTF-8 変換、FreeType によるテキスト描画
- オリジナルのレイヤ構造：背景、立ち絵スロット、テキストオーバーレイ、色調パス
- 「ブリンクマーカー」システム（ID 500〜999）による擬似スプライトおよび色調制御
- 回帰テスト用のヘッドレスモード
- 移植性の高い C99 実装（依存: SDL2、FreeType）

### ビルド

必要要件: C99 コンパイラ、SDL2 (2.0.20+)、FreeType (2.10+)、libiconv（または glibc の iconv）。

Debian / Ubuntu

```bash
sudo apt install build-essential libsdl2-dev libfreetype6-dev
make
```

Arch

```bash
sudo pacman -S base-devel sdl2 freetype2
make
```

macOS

```bash
brew install sdl2 freetype
make
```

Android (Termux)

```bash
pkg install clang make sdl2 freetype libiconv
make
cp sades ~/bin/sades
```

`make` ステップにより、プロジェクトルートに `sades` バイナリが生成されます。

### 実行方法

基本的な実行コマンド:

```bash
./sades play path/to/GAME.SCE path/to/GAME.M00
```

.SCE ファイルの隣に .GLZ、.XSC、.PAL、TITLE.SYS を配置してください。

便利なサブコマンド:

```
sades glz    <in.glz> <out.png> [--pal <pal.glz>]      decode a graphic to PNG
sades info   <in.glz>                                  print header and palette
sades title  <TITLE.SYS> <out.png>                     decode the engine logo
sades sce    <in.sce> <in.m00> [--trace] [--explore]   run the VM headless
sades show   <in.glz>                                  display one graphic
sades play   <in.sce> <in.m00> [--title <glz>] [--trace]   play with window
```

ヘッドレスランナー（`sades sce`）は次のようなサマリを出力します:

```
--- 100000 ops, 68434 CALLs, 5275 wait_keys, 0 unknown ---
```

このサマリは変更の検証に使われます。unknown > 0 はデコーダに問題があることを示します。

### フォント

日本語フォント（CJK 統合漢字）が必要です。フォントに▽が含まれていない場合、エンジン側で ▽ を描画します。

推奨フォント:

- IPAex Gothic（PC-98 の見た目に近い）
- VL Gothic
- Noto Sans CJK
- M+ 1m

環境変数 SADES_FONT でフォントを指定できます:

```bash
SADES_FONT=/path/to/ipag.ttf ./sades play game.sce game.m00
```

未設定の場合、エンジンは一般的なシステムフォントパスとローカルの `fonts/` フォルダを検索します。

### 環境変数

- SADES_DX, SADES_DY = 0 — 立ち絵位置の調整（ピクセル）
- SADES_DELAY_MULT = 32 — DELAY の 1 tick 当たりのミリ秒
- SADES_ANIM_MS = 40 — アニメーションステップ間のフレーム遅延
- SADES_ANIM = 1 — アニメーション遅延を無効にするには 0 に設定
- SADES_FONT = auto — 日本語フォントのパス
- SADES_FORCE_TINT = 0 — 色調を強制する（1=赤,2=白,3=緑）
- SADES_TINT_TIMEOUT = 400 — N ミリ秒後に自動で色調をクリア
- SADES_CK_R/G/B = -1 — 透過用の任意のカラ―キー
- SADES_MODE<N>_R/_G/_B — モードごとのテキスト色上書き（N = 0..7）

### 未実装 / 計画

- セーブ/ロード（オリジナルは一度にプレイする想定のため、追加は可能性低）
- オーディオ（なし — オリジナルエンジンに音声はありません）

### ライセンスと法的注意

このプロジェクトは CP/You や Ankoku Baitai Softwares とは関係ありません。エンジンのコードは著者の作業であり MIT ライセンスで公開されています。ゲーム資産（.SCE、.M00、.GLZ、.XSC、.PAL、TITLE.SYS）は元の著作者の著作物であり含まれていません。あなた自身の合法的なコピーを使用してください。

スクリーンショットはドキュメント用にのみ含まれています。

---

License: MIT. See LICENSE.
