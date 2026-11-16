# sgnano での手直し（2026-10-24、単体リポジトリ向けに 2026-11-17 更新）

取り込み元・版は [`../README.md`](../README.md)。取り込み直すときは次を当て直す。

- `i_system.c`: `I_ZoneBase()` が `SGNANO` のとき `DG_ZoneBase()`（`apps/doom/doom_app.c`）からゾーンを取る
- `m_config.c`: `GetDefaultConfigDir()` が `SGNANO` のとき `dg_config_dir()`（S: の `SystemSettings/DoomSettings/` か `Doom2Settings/`、2026-10-25。以前は "./"）を返す。`M_GetSaveGameDir()` は `SGNANO` のとき設定の置き場でなく "./"（WAD のフォルダ）の下の `.savegame/`
- `i_sound.c`: `SGNANO` のとき SDL_mixer を読まない（`FEATURE_SOUND` を立てて、音と音楽の部品は `apps/doom/dg_sound.c`）
- `z_zone.c`: `Z_Init()` が `SGNANO` のとき、`DG_ZoneExtra()` の離れた場所（FB のページ）をゾーンに足す（間は「いつも使っている塊」でつなぐ）
- `m_config.c`: `SaveDefaultCollection()`・`LoadDefaultCollection()` を `SGNANO` のときも有効に（doomgeneric は設定を読み書きしない）
- `d_main.c`・`r_data.c`: `SGNANO` のとき、起動の段の境目とテクスチャ・パッチ名・スプライトのループで `DG_Load(段, i, n)`（`apps/doom/doom_app.c`、読み込みの画面の進み具合）を呼ぶ
- `m_config.c`: `SGNANO` のとき、設定のキーの値 128〜255（doomgeneric の決まったキー。撃つ 0xa3 など）を読み込みで 0 にしない（2 回目の起動から撃つ・使うが効かなかった）
- ほかのファイルは変えていない。libc の口は `apps/doom/dg_port.h`（`-include`）で向け直す
- 組まないもの: `doomgeneric.c`（`apps/doom/doom_app.c` が代わり）、`i_video.c`（`apps/doom/dg_video.c` が代わり）
