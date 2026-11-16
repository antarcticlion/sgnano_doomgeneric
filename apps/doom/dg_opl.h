// Doom の BGM を OPL2（YM3812）で（2026-11-13、ユーザー指示）。MIDI（Freedoom の MThd、市販版は mus2mid で直したもの）を、WAD の
// GENMIDI（DMX の OPL2 の音色表）で YM3812 のレジスタ書き込みの並びに直し、メモリの VGM にする。ファームの音楽の口（music_play_mem、
// SGX ABI 5.5）が VGM の合成（ymfm）で鳴らす。GPL の部分（MIDI -> OPL の直し）は Doom の側に閉じる
#ifndef DG_OPL_H
#define DG_OPL_H

#include <stdbool.h>
#include <stdint.h>

// genmidi: GENMIDI の塊（"#OPL_II#" + 175 x 36 バイト）。loop: 曲の頭へのループを付ける。返り値は malloc した VGM（NULL = 直せない）
uint8_t *dg_opl_vgm(const uint8_t *mid, uint32_t len, const uint8_t *genmidi, uint32_t genmidi_len, bool loop, uint32_t *out_len);

#endif
