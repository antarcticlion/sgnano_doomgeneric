# ライセンス

このリポジトリは、次の部品からできています。

| 範囲 | ライセンス | 文 |
|---|---|---|
| `apps/doom/`・`third_party/doomgeneric/`・このリポジトリの文書 | **GPL-2.0**（GNU General Public License v2.0） | ルートの `LICENSE`、`third_party/doomgeneric/LICENSE` |
| `sdk/sgx/`（sgnano の SGX SDK） | **MIT** | `sdk/sgx/LICENSE` |

`DOOM.SGX` は doomgeneric と `apps/doom/` を結んだ一つのプログラムなので、配布するときは GPL-2.0 に従ってください。
ソース（このリポジトリ）と、ビルドに使った SDK を一緒に配れる形にしてあります。

Freedoom の WAD はこのリポジトリに入れていません。WAD の配布条件は Freedoom の配布元を見てください。
