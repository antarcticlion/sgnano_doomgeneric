# SGX の ABI（このリポジトリでの入口）

sgnano の外部実行ファイル（`.sgx`）は、ファームのシンボルには結ばず、入口に渡される
**`sgx_api_t` の関数の表**だけを使います。このリポジトリで ABI の**正**は次の見出しです。

- [`sdk/sgx/include/sgx_api.h`](../sdk/sgx/include/sgx_api.h): 表の並び・版（`SGX_ABI_MAJOR` / `SGX_ABI_MINOR`）・定数
- [`sdk/sgx/sgx.ld`](../sdk/sgx/sgx.ld): 番地 0 に結ぶリンカの台本
- [`sdk/sgx/sgx_crt0.c`](../sdk/sgx/sgx_crt0.c): 始まり（`sgx_start` -> `sgx_main`）
- [`sdk/sgx/sgx_link.py`](../sdk/sgx/sgx_link.py): ELF から `.sgx`（像 + 再配置の表）を作る

sgnano の開発ツリーには、ABI 5.2（2026-11-11）時点の詳しい文書 `docs/sgx_abi.md` があります。
このリポジトリの SDK は ABI 5.11（2026-11-17）で、**5.2 より後ろに足した口**（OPL2 の
`music_play_mem` など）も含みます。食い違ったら `sgx_api.h` を見てください。

## DOOM.SGX が使う決まり

- 入口: `int sgx_main(const sgx_api_t *api, int argc, char **argv)`
- 見出し: `SGX_APP_NAME`（"Doom"）・`SGX_APP_FLAGS`（1 = `SGX_F_ESC_KEY`）・`SGX_APP_STACK`（16384）
- 画面: `set_mode(SGX_MODE_TEXT_BM8 | SGX_MODE_NO_TEXT)`（モード 7、320x200x8。テキスト面は閉じる）
- 終わり: `exit()` は `api->exit` へ。`SGX_F_ESC_KEY` なので ESC は Doom に渡り、Shift+ESC で止まる
- メモリ: `psram_alloc` で Doom のゾーンと malloc の置き場を取り、使わない FB のページを
  `fb_borrow` で借りてゾーンに足す
- ファイル: `stat`・`read_file`・`ra_open` / `ra_read`（大きい WAD）・`write_file`・`mkdir`・`remove`・`rename`
- 音: `pcm_*`（効果音）、`seq_*`（MIDI のシーケンサー）、`music_play_mem`（OPL2 の VGM。ABI 5.5 以降）
- キー: `key` / `key_down`・`pad`

## ビルドの決まり

`sdk/sgx/sgx.cmake` の `sgx_add_program` が、`sgx_crt0.c`・`sgx_libc.c`・`sgx_dmath.S` を足して、
`-nostartfiles --specs=nano.specs -Wl,--gc-sections -Wl,-q` で結びます。リンクのあとに
`sgx_link.py` が走り、`build/sgx/doom.sgx` を作ります。位置に依らない像にするため、**32 ビットの
絶対番地の語**（`R_ARM_ABS32` / `R_ARM_TARGET1`）だけを再配置できます。`-mpure-code` などは使えません。

ABI を上げたファームでは、`SGX_ABI_MINOR` がこの SDK の値以下なら読み込まれます。逆に、この SDK で
組んだプログラムは、ファームの `SGX_ABI_MINOR` が足りないと「新しいファームが要る」と断られます。
