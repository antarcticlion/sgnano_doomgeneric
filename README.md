# sgnano_doomgeneric

sgnano（RP2350B のパーソナルコンピュータ）で動く **Doom の外部実行ファイル（DOOM.SGX）** のリポジトリです。
[doomgeneric](https://github.com/ozkl/doomgeneric)（GPL-2.0）と sgnano の SGX SDK だけを持ち、
sgnano 本体のソースやファームウェアとは結ばず、SGX の API の表だけでつながります。

- 単体でビルドできます（`arm-none-eabi-gcc` + CMake + Ninja + Python 3）
- GitHub Actions で push / Pull Request ごとにビルドし、`DOOM.SGX` を artifact に出します
- WAD（Freedoom・市販版 Doom）は含みません。`wads/README.md` を見てください

## ビルド

必要なもの:

| 道具 | 確認した版 | 備考 |
|---|---|---|
| `arm-none-eabi-gcc` | 10.3.1 2021.07（Ubuntu 22.04） | newlib-nano が要ります |
| CMake | 3.22.1 | 3.20 以上 |
| Ninja | 1.10.1 | |
| Python 3 | 3.10 | `sdk/sgx/sgx_link.py` が使います |

Ubuntu / Debian では:

```sh
sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi cmake ninja-build python3
```

ビルド:

```sh
./scripts/build.sh
```

手で回すとき:

```sh
cmake --preset sgx
cmake --build --preset sgx
python3 scripts/check_sgx.py --name Doom --flags 1 --abi-major 5 build/sgx/doom.sgx
```

preset を使わないとき:

```sh
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm-none-eabi.cmake
cmake --build build
```

できるもの:

```text
build/sgx/doom.sgx   DOOM.SGX（ファームに置く像）
build/sgx/doom.elf   読み解き用（置かない）
build/sgx/doom.map   リンクの地図
```

`apps/doom` の警告を誤りにしたいときは `-DDOOM_WERROR=ON` を付けます。取り込んだ doomgeneric の
ソースは警告を出さない設定（`-w`）です。

### CI

`.github/workflows/build.yml` が Ubuntu の `gcc-arm-none-eabi` を入れて、`./scripts/build.sh` と
`scripts/check_sgx.py` を通し、`DOOM.SGX` を `doom-sgx` という名前の artifact にします。
タグやリリースはまだ作りません（必要になったら足します）。

## sgnano で使う

1. Freedoom かお持ちの Doom の WAD を、USB メモリなどの `DoomFiles/freedoom1.wad`（Doom / Ultimate Doom の形）
   か `Doom2Files/freedoom2.wad`（Doom II の形）に置きます。
2. `build/sgx/doom.sgx` を `DOOM.SGX` に名前を変えて、同じドライブの好きな所（`DoomFiles` の隣など）に置きます。
3. ファイラーで WAD を Enter するか、`DOOM.SGX` を Enter すると始まります。

操作（既定）:

| キー | 動き |
|---|---|
| 矢印 | 歩く・回る |
| Ctrl / マウス（パッドの A） | 撃つ |
| Space（パッドの B） | 使う・ドア |
| Shift | 走る |
| Alt | 平行移動 |
| `,` `.` | 横歩き |
| Tab | 地図 |
| ESC | メニュー |
| Shift+ESC | sgnano のファイラーへ戻る |
| F1〜F12 | Doom の機能 |

設定（`default.cfg`・`doomgenericdoom.cfg`）は `S:/SystemSettings/DoomSettings/`（Doom II の形は
`Doom2Settings/`）に残ります。セーブは WAD のフォルダの `.savegame/` です。

音は、ファームが SGX ABI 5.5 以降なら OPL2（YM3812）の BGM、それより前ならシーケンサーで鳴ります。
効果音は PCM です。画面はモード 7（320x200x8）です。

## 中身

```text
apps/doom/                  sgnano のつなぎ（画面・キー・ファイル・音・読み込みの表示）
third_party/doomgeneric/    doomgeneric（GPL-2.0、手直しは SGNANO_PATCHES.md）
sdk/sgx/                    sgnano の SGX SDK（MIT。ABI 5.11 で同期）
cmake/                      arm-none-eabi の toolchain
docs/                       ビルドの詳しい話・SGX ABI の入口
scripts/                    build.sh・check_sgx.py
wads/                       WAD の説明（WAD は置かない）
```

作りの約束:

- doomgeneric は `e257a09527062bfb986b6e5e0780839b6cbd5b71`（2026-10-11）を取り込んでいます。
  手直しの一覧は [`third_party/doomgeneric/SGNANO_PATCHES.md`](third_party/doomgeneric/SGNANO_PATCHES.md)。
- `doomgeneric.c`（始め方）と `i_video.c`（画面）は組みません。代わりに `apps/doom/` の
  `doom_app.c`・`dg_video.c` を使います。
- 取り込んだソースの libc の口は、`apps/doom/dg_port.h` を `-include` して `dg_sys.c` へ向けます。
  ソースそのものは変えていません。
- ファームのシンボルには結びません。使うのは SGX の入口に渡される API の表だけです。

## ライセンス

- `apps/doom/`・`third_party/doomgeneric/`・このリポジトリの文書: **GPL-2.0**（ルートの `LICENSE`）
- `sdk/sgx/`: **MIT**（`sdk/sgx/LICENSE`）
- 全体の説明: [`LICENSES/README.md`](LICENSES/README.md)

Doom は id Software の著作物です。doomgeneric（ozkl さん）、Chocolate Doom、Freedoom の作った人たちに感謝します。
