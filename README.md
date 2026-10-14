# SADES Engine — a reimplementation

[English](#sades-engine--a-reimplementation) · [日本語](#sadesエンジン--再実装)

---

## English

Back in 1990, a small Japanese studio called **Ankoku Baitai Softwares**
(暗黒媒体ソフトウェアーズ) released a visual-novel engine for the NEC PC-9801.
It was called **SADES**, written by a programmer credited only as **CP/You**,
and it shipped with three games between 1990 and 1993.

Nobody wrote anything down about how it worked. There is no SDK, no spec, no
documentation of any kind — just an executable and a folder of data files.

This is a from-scratch reimplementation of that engine. It reads the original
game files and runs them on modern hardware, in a window, without an emulator.

![Erion Quest](screenshots/erion_quest.png)
![SD Sadistic Gamers](screenshots/sd_sadistic.png)
![Yukonis no Mori de....](screenshots/yukonis.png)

*Erion Quest (1993) · SD Sadistic Gamers (1990) · Yukonis no Mori de.... (1990)*

### What it plays

| Game | Year | Engine | Status |
|------|------|--------|--------|
| ゆこにすの森で.... (Yukonis no Mori de....) | 1990 | v1.1 | Playable end-to-end |
| SD Sadistic Gamers | 1990 | v1.2/v1.5 | Playable end-to-end |
| Erion Quest | 1993 | v1.2/v1.5 | Playable end-to-end |

All three games run with **zero unknown opcodes** in the headless regression
tests. You can reach the end of Erion Quest, get to the SD credits sequence,
and clear the first chapter of Yukonis no Mori.

### What's in it

- A complete VM for the SADES bytecode — 42 opcodes, all working
- Both graphic formats the engine ever used:
  - `.GLZ` — LZ77-compressed bitplanes with an embedded palette
  - `.XSC` — per-plane byte-level RLE with per-codec layout rules
- Shift-JIS to UTF-8 conversion for all in-game text, rendered with FreeType
- The original layering: background, character slots, text overlay, tint pass
- The "blink marker" system — 8×6 pseudo-sprites with IDs 500–999 that control
  sprite visibility and screen tint from the script
- A headless mode for regression testing
- A portable C99 codebase. The only platform dependencies are SDL2 and FreeType.

### Building

You need a C99 compiler, SDL2 (2.0.20 or newer), FreeType (2.10 or newer),
and libiconv (or glibc's built-in iconv).

On Debian or Ubuntu:

```bash
sudo apt install build-essential libsdl2-dev libfreetype6-dev
make
```

On Arch:

```bash
sudo pacman -S base-devel sdl2 freetype2
make
```

On macOS:

```bash
brew install sdl2 freetype
make
```

On Android via Termux:

```bash
pkg install clang make sdl2 freetype libiconv
make
cp sades ~/bin/sades_new
```

That leaves you with a sades binary in the project root.

Running

Point the binary at a .SCE and its companion .M00:

```bash
./sades play path/to/EQ.SCE path/to/EQ.M00
```

The .GLZ, .XSC, .PAL and TITLE.SYS files are expected to sit in the
same folder as the .SCE.

There are a few other sub-commands that are useful when you're poking at
individual assets:

```
sades glz    <in.glz> <out.png> [--pal <pal.glz>]      decode a graphic to PNG
sades info   <in.glz>                                  print header and palette
sades title  <TITLE.SYS> <out.png>                     decode the engine logo
sades sce    <in.sce> <in.m00> [--trace] [--explore]   run the VM headless
sades show   <in.glz>                                  display one graphic
sades play   <in.sce> <in.m00> [--title <glz>] [--trace]   the real thing
```

sce is the one I use the most. It runs the VM without opening a window and
prints a summary at the end:

```
--- 100000 ops, 68434 CALLs, 5275 wait_keys, 0 unknown ---
```

Those numbers are how I check that a change didn't break anything. If
unknown goes above zero, something is wrong with the bytecode decoder.

Fonts

You'll need a Japanese font with CJK Unified Ideographs coverage. The engine
can draw the ▽ character itself if your font doesn't have it, so you don't
need a font with full symbol coverage — just the kanji and kana.

Anything in this list will do:

· IPAex Gothic — closest match to the original PC-98 look. My pick.
· VL Gothic — open source, widely available.
· Noto Sans CJK — comes with most Android devices.
· M+ 1m — lighter alternative if you want to bundle one.

To point the engine at a font, use the SADES_FONT environment variable:

```bash
SADES_FONT=/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf \
  ./sades play game.sce game.m00
```

If you don't set it, the engine tries a few common paths on Linux, macOS,
Windows and Android, and finally a fonts/ folder in the working directory.

Environment variables

Everything you can tweak without recompiling:

Variable Default What it does
SADES_DX, SADES_DY 0 Shift portrait positions globally (pixels)
SADES_DELAY_MULT 32 How many milliseconds one DELAY tick is worth
SADES_ANIM_MS 40 Frame delay between animation steps
SADES_ANIM 1 Set to 0 to disable animation delays entirely
SADES_FONT auto Path to the Japanese font
SADES_FORCE_TINT 0 Force a tint state (1 red, 2 white, 3 green)
SADES_TINT_TIMEOUT 400 Auto-clear a tint after N ms
SADES_CK_R, SADES_CK_G, SADES_CK_B -1 Optional color key for transparency
SADES_MODE<N>_R/_G/_B per-mode Override text color for mode N (0–7)

The SADES_MODE overrides are mostly for debugging. The text colors are the
eight standard PC-98 palette indices, and each one maps to a specific role in
the games (black for narration, blue for one combat state, red for another,
and so on).

What's missing

The engine is complete for the three games that exist, but a few things are
still on my list:

· Palette fade between scenes. Yukonis no Mori and Erion Quest both use
  a hardware palette fade for scene transitions. That's the next thing I want
  to wire up.
· Save/load. The original engine didn't have it and the games were
  designed to be played in one sitting, so I probably won't add it either.
· Audio. There is none. All three games are silent — the original engine
  had no sound subsystem.

Legal

Not affiliated with CP/You, Ankoku Baitai Softwares, or any current rights
holder of the games.

The engine code in this repository is my own work and is released under the
MIT license. The game data files — .SCE, .M00, .RC, .GLZ, .XSC,
.PAL, TITLE.SYS — are copyrighted by their original authors and are not
included here. You'll need your own legal copies of the games to use the
engine.

Screenshots in this README are used for documentation only.

Credits

· CP/You and Ankoku Baitai Softwares for the original engine and
  games. None of this would exist without them.
· SDL2 and FreeType
  for doing the boring parts.
· The PC-98 preservation and emulation scene, who keep the hardware alive so
  that people like me can keep reverse-engineering it.

License

MIT. See LICENSE.

---

日本語

English · 日本語

1990年、暗黒媒体ソフトウェアーズという小さなスタジオが、NEC PC-9801 向けの
ビジュアルノベルエンジンを発表しました。名前は SADES、作者は CP/You
というクレジットだけを残したプログラマーでした。1990年から1993年にかけて、
3本のゲームがこのエンジンで作られています。

エンジンの仕組みについて書かれた資料は、当時も今も存在しません。SDK も、
仕様書も、ドキュメントもありません。あるのは実行ファイルと、データ
ファイルの束だけです。

これは、そのエンジンを一から再実装したものです。オリジナルのゲーム
ファイルを読み込み、エミュレータを使わずに現代のハードウェア上で
動かします。

screenshots/erion_quest.png
screenshots/sd_sadistic.png
screenshots/yukonis.png

Erion Quest（1993）· SD Sadistic Gamers（1990）· ゆこにすの森で....（1990）

対応タイトル

タイトル 発売年 エンジン 状態
ゆこにすの森で.... 1990 v1.1 最初から最後までプレイ可能
SD Sadistic Gamers 1990 v1.2/v1.5 最初から最後までプレイ可能
Erion Quest 1993 v1.2/v1.5 最初から最後までプレイ可能

3本すべて、ヘッドレス回帰テストで 未知のオペコード0件 を達成しています。
Erion Quest はエンディングまで、SD はクレジット画面まで、ゆこにすの森で は
第1章のクリアまで到達できます。

実装されているもの

· SADES バイトコード用の完全な VM（42オペコードすべて動作）
· エンジンが使った2種類の画像フォーマット
  · .GLZ — LZ77 圧縮されたビットプレーン＋パレット埋め込み
  · .XSC — プレーンごとのバイト単位 RLE＋コーデック別レイアウト
· Shift-JIS から UTF-8 への変換と、FreeType によるテキスト描画
· オリジナルと同じレイヤ構造（背景・キャラクタ・テキスト・色調補正）
· 「ブリンクマーカー」システム — ID 500〜999 の 8×6 疑似スプライトで、
  スクリプトから表示制御と色調補正を行う仕組み
· ヘッドレスモード（回帰テスト用）
· 移植性の高い C99 コード。プラットフォーム依存は SDL2 と FreeType のみ

ビルド方法

必要なもの：C99 対応コンパイラ、SDL2（2.0.20以上）、FreeType（2.10以上）、
libiconv（または glibc 内蔵の iconv）。

Debian / Ubuntu：

```bash
sudo apt install build-essential libsdl2-dev libfreetype6-dev
make
```

Arch：

```bash
sudo pacman -S base-devel sdl2 freetype2
make
```

macOS：

```bash
brew install sdl2 freetype
make
```

Android（Termux）：

```bash
pkg install clang make sdl2 freetype libiconv
make
cp sades ~/bin/sades_re
```

プロジェクト直下に sades バイナリができます。

実行方法

.SCE と、それに対応する .M00 を指定します。

```bash
./sades play path/to/EQ.SCE path/to/EQ.M00
```

.GLZ、.XSC、.PAL、TITLE.SYS は、.SCE と同じフォルダに置いて
ください。

個別のアセットを覗くときは、いくつかサブコマンドが使えます。

```
sades glz    <in.glz> <out.png> [--pal <pal.glz>]      画像を PNG に変換
sades info   <in.glz>                                  ヘッダとパレットを表示
sades title  <TITLE.SYS> <out.png>                     エンジンのロゴをデコード
sades sce    <in.sce> <in.m00> [--trace] [--explore]   VM をヘッドレスで実行
sades show   <in.glz>                                  画像を1枚表示
sades play   <in.sce> <in.m00> [--title <glz>] [--trace]   通常の対話モード
```

一番よく使うのは sce です。ウィンドウを開かずに VM を走らせ、最後に
サマリを出します。

```
--- 100000 ops, 68434 CALLs, 5275 wait_keys, 0 unknown ---
```

この数字を見て、変更で何か壊れていないか確認します。unknown が 0 を
超えたら、バイトコードのデコーダに問題があります。

フォントについて

CJK統合漢字を収録した日本語フォントが必要です。▽ の文字は、フォントに
含まれていなくてもエンジン側で描画するので、記号の収録範囲は気にしなくて
大丈夫です。漢字と仮名が入っていれば十分です。

このあたりがおすすめです。

· IPAexゴシック — PC-98 の雰囲気に一番近い。個人的なおすすめ。
· VLゴシック — オープンソースで入手しやすい。
· Noto Sans CJK — Android 端末にだいたい入っている。
· M+ 1m — 同梱したい場合に軽くて済む。

フォントは SADES_FONT 環境変数で指定します。

```bash
SADES_FONT=/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf \
  ./sades play game.sce game.m00
```

指定しなかった場合、Linux、macOS、Windows、Android の一般的なパスを順に
探し、最後にカレントディレクトリの fonts/ を見ます。

環境変数

再コンパイルなしで調整できるもの：

変数 既定値 効果
SADES_DX, SADES_DY 0 立ち絵の位置を全体補正（ピクセル）
SADES_DELAY_MULT 32 DELAY 1ティックあたりのミリ秒
SADES_ANIM_MS 40 アニメーション間のフレーム遅延
SADES_ANIM 1 0 でアニメーション遅延を無効化
SADES_FONT 自動 日本語フォントのパス
SADES_FORCE_TINT 0 色調補正を強制（1=赤、2=白、3=緑）
SADES_TINT_TIMEOUT 400 色調補正の自動解除までのミリ秒
SADES_CK_R, SADES_CK_G, SADES_CK_B -1 透過用のカラーキー
SADES_MODE<N>_R/_G/_B モード別 テキスト色モード N（0〜7）の上書き

SADES_MODE 系は主にデバッグ用です。テキストの色は PC-98 の標準的な
8パレットで、それぞれのゲームで役割が決まっています（黒は地の文、青は
戦闘中の特定の状態、赤は別の状態、など）。

これから追加したいもの

3本のゲームが動く範囲では一通り完成していますが、いくつか残っています。

· シーン間のパレットフェード。 ゆこにすの森で と Erion Quest は、
  シーン切り替えでハードウェアパレットのフェードを使っています。次は
  これを実装したいと思っています。
· セーブ／ロード。 オリジナルには存在せず、各ゲームも1回のプレイで
  完結する作りなので、おそらく追加しません。
· 音声。 ありません。3本とも無音です。オリジナルエンジンに音声
  機能がなかったためです。

権利について

CP/You、暗黒媒体ソフトウェアーズ、および現在のゲームの権利者とは
一切関係ありません。

このリポジトリのエンジンコードは私自身が書いたもので、MIT ライセンスで
公開しています。ゲームデータファイル（.SCE、.M00、.RC、.GLZ、
.XSC、.PAL、TITLE.SYS）は原作者の著作物であり、本リポジトリには
含まれていません。各自が正規に入手したものをご使用ください。

README 内のスクリーンショットは、ドキュメント目的でのみ使用しています。

クレジット

· CP/You と 暗黒媒体ソフトウェアーズ — オリジナルエンジンと
  各ゲームの作者。これがなければ何も始まりませんでした。
· SDL2 と FreeType
  — 面倒な部分を全部やってくれています。
· PC-98 の保存・エミュレーションに関わるコミュニティの皆様 — ハードウェアを
  生かし続けてくれているおかげで、こういう解析ができます。

ライセンス

MIT。LICENSE を参照。
