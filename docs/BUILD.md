# ビルドの詳しい話

このリポジトリは、sgnano の外部実行ファイル `DOOM.SGX` を **sgnano 本体のビルドツリーの外**で組むためのものです。
ファームとは結ばないので、Pico SDK は要りません。要るのは ARM の GNU ツールチェーン（newlib-nano 付き）だけです。

## 道具

```sh
# Ubuntu 22.04 / 24.04
sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi cmake ninja-build python3
```

- `arm-none-eabi-gcc`: 10.3.1（Ubuntu 22.04）で確認。GCC 10 以降なら動く見込みです。
- CMake 3.20 以上。`CMakePresets.json` は version 3（CMake 3.21 以降で読める形）です。
- Ninja。Make でも組めますが、preset は Ninja を指定しています。
- Python 3: リンクのあとに `sdk/sgx/sgx_link.py` を走らせて `.sgx` を作ります。

## 組む

```sh
cmake --preset sgx
cmake --build --preset sgx
```

`CMakePresets.json` の `sgx` は、`cmake/toolchain-arm-none-eabi.cmake` を使って `build/` に組む preset です。
`cmake --build --preset sgx` は `doom` だけを組みます（いまはほかに target はありません）。

手で:

```sh
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm-none-eabi.cmake
cmake --build build
```

できるもの:

| ファイル | 中身 |
|---|---|
| `build/sgx/doom.sgx` | ファームに置く像（見出し + コード + 定数 + 再配置の表） |
| `build/sgx/doom.elf` | 読み解き用の ELF。ファームには置かない |
| `build/sgx/doom.map` | リンクの地図 |

### コンパイルの決まり

`cmake/toolchain-arm-none-eabi.cmake` が sgnano のファームと同じ並びを渡します。

```text
-mcpu=cortex-m33 -mthumb -march=armv8-m.main+fp+dsp -mfloat-abi=softfp -mcmse
```

SGX の呼び出しは AAPCS（softfp）です。`-mcmse` は sgnano のビルドに合わせているだけです。
`sgx.cmake` が `-O2 -ffunction-sections -fdata-sections -fno-common` と
`-nostartfiles --specs=nano.specs -Wl,--gc-sections -Wl,-q` を足します。

### ソースの分け方

- `third_party/doomgeneric/*.c` のうち、`doomgeneric.c`（始め方）と `i_video.c`（画面）は組みません。
  代わりに `apps/doom/doom_app.c` と `apps/doom/dg_video.c` を使います。
- 取り込んだソースには `-include apps/doom/dg_port.h` を付けて、`fopen`・`malloc`・`exit` などを
  `apps/doom/dg_sys.c` の口へ向けます。ソースそのものは変えません。
- 取り込んだソースは `-w` で警告を止めます。`apps/doom` の側は `-Wall -Wextra` で見ます。
- `DOOM_WERROR=ON` を付けると、`apps/doom` の警告を誤りにします。

## 検査

```sh
python3 scripts/check_sgx.py --name Doom --flags 1 --abi-major 5 build/sgx/doom.sgx
```

見るもの:

- 見出しの印 `SGX1`
- ABI の major / minor
- `link_base` が 0
- `image_size + reloc_count * 4` がファイルの大きさと一致
- 入口が像の中にあり、Thumb の印（奇数）が付いている
- 再配置の表が像の中にあり、4 バイトに整っていて、番地の順

`sgx_link.py` はリンクのときに同じことを見ていますが、CI では成果物をもう一度見ます。

## CI（GitHub Actions）

`.github/workflows/build.yml`:

1. `actions/checkout@v4`
2. `gcc-arm-none-eabi`・`libnewlib-arm-none-eabi`・`cmake`・`ninja-build`・`python3` を入れる
3. `./scripts/build.sh`
4. `scripts/check_sgx.py --name Doom --flags 1 --abi-major 5 build/sgx/doom.sgx`
5. `build/sgx/doom.sgx` を `DOOM.SGX` に名前を変えて artifact `doom-sgx` に上げる

`push`・`pull_request`・手動（`workflow_dispatch`）で走ります。同じ ref の古い実行は打ち切ります。

## sgnano との同期（手で写す）

`apps/doom/`・`sdk/sgx/`・`third_party/doomgeneric/` は sgnano の開発ツリー（2026-11-17 の
`67544c0`）から写したものです。更新するときは、sgnano の側でコミットしてから:

```sh
cd ../sgnano
git archive HEAD sdk/sgx apps/doom third_party/doomgeneric | tar -x -C ../sgnano_doomgeneric
```

を実行し、`git diff` でこのリポジトリの手直し（`README.md`・`LICENSES/`・
`third_party/doomgeneric/SGNANO_PATCHES.md`・`apps/doom/dg_sys.c` の `tmp[512]`）が消えていないか
確かめてください。SDK の ABI が上がったら、`README.md` の「音」と CI の `--abi-major` も見直します。

## こまったとき

- **`CMAKE_C_COMPILER` が見つからない**: `arm-none-eabi-gcc` が PATH にあるか、
  `cmake --preset sgx` を使っているかを見てください。
- **`nano.specs` が無い**: `libnewlib-arm-none-eabi` が入っていません。
- **`sgx_link.py` が `not a 32-bit little-endian ELF` と言う**: ホストの GCC で組んでいます。
  toolchain を通していないときの間違いです。
- **`sgx_link.py` が `MOVW / MOVT` で止まる**: 位置に依らない像にするため、32 ビットの絶対番地の語だけを
  再配置できます。`-mpure-code` などは使わないでください。
