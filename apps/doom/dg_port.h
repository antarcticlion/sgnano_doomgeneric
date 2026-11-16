// Doom（third_party/doomgeneric）を sgnano の外部実行ファイル（DOOM.SGX、apps/doom）で組むための前置き（2026-10-24）。取り込んだソースの全部に -include で読ませる。
// ソースは変えずに、libc の口を sgnano の下回りへ向ける（mruby/c の rblibc と同じやり方）:
//   - fopen / fread / fwrite / fseek / ftell / fclose / fprintf / fputs / fgets / feof / remove / rename -> dg_*（src/doom/dg_sys.c。
//     ファイルは rbfs の上。WAD のような大きいファイルは好きな位置から読み、小さいものは丸ごと PSRAM へ）
//   - malloc / calloc / realloc / free / strdup -> PSRAM の Doom 専用の置き場（dg_sys.c）。newlib の malloc は使わない（SRAM に置き場が無い）
//   - exit -> ファイラーへ戻る（longjmp）、getenv -> いつも無し、stdout / stderr への書き出しは printf（UART）へ
#ifndef SGNANO_DOOM_DG_PORT_H
#define SGNANO_DOOM_DG_PORT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "dg_sys.h"

#define FILE dg_file_t
#undef stdout
#undef stderr
#define stdout DG_STDOUT
#define stderr DG_STDERR
#define fopen dg_fopen
#define fread dg_fread
#define fwrite dg_fwrite
#define fseek dg_fseek
#define ftell dg_ftell
#define fclose dg_fclose
#define feof dg_feof
#define fflush dg_fflush
#define fprintf dg_fprintf
#define vfprintf dg_vfprintf
#define fputs dg_fputs
#undef fputc
#define fputc dg_fputc
#undef putc
#define putc dg_fputc
#define fgets dg_fgets
#define remove dg_remove
#define rename dg_rename
#define malloc dg_malloc
#define calloc dg_calloc
#define realloc dg_realloc
#define free dg_free
#define strdup dg_strdup
#define exit dg_exit
#define getenv dg_getenv
#define mkdir(p, m) dg_mkdir(p)
#define sscanf dg_sscanf
#define fscanf dg_fscanf
#define strstr dg_strstr
#define atof(s) dg_strtod((s), NULL)
#define atoi dg_atoi
#define strtol dg_strtol
#define strtoul dg_strtoul

#endif
