// Doom の libc の代わり（2026-10-24、dg_sys.c）。dg_port.h がこれを読んでから libc の名前を向け直す
#ifndef SGNANO_DOOM_DG_SYS_H
#define SGNANO_DOOM_DG_SYS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <setjmp.h>

typedef struct dg_file dg_file_t;
dg_file_t *dg_fopen(const char *path, const char *mode);
size_t dg_fread(void *p, size_t sz, size_t n, dg_file_t *f);
size_t dg_fwrite(const void *p, size_t sz, size_t n, dg_file_t *f);
int    dg_fseek(dg_file_t *f, long off, int whence);
long   dg_ftell(const dg_file_t *f);
int    dg_fclose(dg_file_t *f);
int    dg_feof(const dg_file_t *f);
int    dg_fflush(dg_file_t *f);
int    dg_fprintf(dg_file_t *f, const char *fmt, ...);
int    dg_vfprintf(dg_file_t *f, const char *fmt, va_list ap);
int    dg_fputs(const char *s, dg_file_t *f);
int    dg_fputc(int c, dg_file_t *f);
char  *dg_fgets(char *s, int n, dg_file_t *f);
int    dg_remove(const char *path);
int    dg_rename(const char *a, const char *b);
void  *dg_malloc(size_t n);
void  *dg_calloc(size_t n, size_t sz);
void  *dg_realloc(void *p, size_t n);
void   dg_free(void *p);
char  *dg_strdup(const char *s);
void   dg_exit(int code) __attribute__((noreturn));
char  *dg_getenv(const char *name);
int    dg_mkdir(const char *path);
int    dg_sscanf(const char *s, const char *fmt, ...);   // %d %i %x %o（int *）と字・空白だけ（Doom が使う分）
int    dg_fscanf(dg_file_t *f, const char *fmt, ...);   // %Ns・%N[^\n] と空白・字だけ（設定の読み込みが使う分）
char  *dg_strstr(const char *h, const char *n);
double dg_strtod(const char *s, char **end);             // newlib の strtod・scanf は SRAM に約 25KB 入るので自前（設定の数を読むだけ）
long   dg_strtol(const char *s, char **end, int base);
unsigned long dg_strtoul(const char *s, char **end, int base);
int    dg_atoi(const char *s);

extern dg_file_t dg_stdout_obj, dg_stderr_obj;   // 印だけの物（2026-10-25: 1・2 を番地にしていたのを、本物の番地に）
#define DG_STDOUT (&dg_stdout_obj)
#define DG_STDERR (&dg_stderr_obj)

// 走らせる側（doom_app.c）から
void     dg_heap_init(void *base, uint32_t size);      // malloc の置き場（PSRAM）
uint32_t dg_heap_peak(void);
void     dg_set_base_dir(const char *abs_dir);         // ドライブ文字の無いパスの基（"A:/DoomFiles"）
void     dg_set_config_dir(const char *abs_dir);       // 設定（default.cfg など）の置き場（"S:/SystemSettings/DoomSettings/"、2026-10-25）
const char *dg_config_dir(void);
void     dg_close_all(void);                           // 終わるとき: 開いたままのファイルを閉じる

#endif
