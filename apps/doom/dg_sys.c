// Doom の libc の代わり（2026-10-24。外部実行ファイル DOOM.SGX の中）: PSRAM の置き場・ファイル（SGX の API の上）・終わり。dg_port.h の口。
// Doom のタスクの中からだけ呼ぶ。ファイルを書き換える API はファームの側で SRAM を空ける（後ろの面を返して取り直す）ので、そのあと描き先を直す
#include "dg_sys.h"
dg_file_t dg_stdout_obj, dg_stderr_obj;           // stdout・stderr の印（中身は使わない）
#include "sgx_api.h"
extern const sgx_api_t *sgx;
void dg_video_back_moved(void);    // dg_video.c: 後ろの面の番地が変わっていたら描き先を直す
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

// ---- 置き場（malloc の代わり）: 先頭から詰める、空いた塊は隣と合わせる。塊の頭は 8 バイト（大きさと使っているか）----
typedef struct blk { uint32_t size; uint32_t used; } blk_t;     // size は頭を含む塊の大きさ（8 の倍数）
static uint8_t *heap_base;
static uint32_t heap_size, heap_used, heap_peak;
void dg_heap_init(void *base, uint32_t size) {
    heap_base = base; heap_size = size & ~7u; heap_used = heap_peak = 0;
    blk_t *b = (blk_t *)heap_base;
    b->size = heap_size; b->used = 0;
}
uint32_t dg_heap_peak(void) { return heap_peak; }
void *dg_malloc(size_t n) {
    if (!heap_base) return NULL;
    const uint32_t need = ((uint32_t)n + sizeof(blk_t) + 7u) & ~7u;
    for (uint32_t o = 0; o < heap_size; ) {
        blk_t *b = (blk_t *)(heap_base + o);
        if (!b->used) {                                     // 後ろの空きと合わせてから見る
            for (;;) {
                const uint32_t nx = o + b->size;
                if (nx >= heap_size) break;
                const blk_t *c = (const blk_t *)(heap_base + nx);
                if (c->used) break;
                b->size += c->size;
            }
            if (b->size >= need) {
                if (b->size - need >= 32u) { blk_t *r = (blk_t *)(heap_base + o + need); r->size = b->size - need; r->used = 0; b->size = need; }
                b->used = 1;
                heap_used += b->size;
                if (heap_used > heap_peak) heap_peak = heap_used;
                return b + 1;
            }
        }
        o += b->size;
    }
    printf("[doom] malloc(%u) failed (pool %lu KB, used %lu KB)\n", (unsigned)n, (unsigned long)(heap_size / 1024u), (unsigned long)(heap_used / 1024u));
    return NULL;
}
void dg_free(void *p) {
    if (!p) return;
    blk_t *b = (blk_t *)p - 1;
    if (!b->used) return;
    b->used = 0; heap_used -= b->size;
}
void *dg_calloc(size_t n, size_t sz) { void *p = dg_malloc(n * sz); if (p) memset(p, 0, n * sz); return p; }
void *dg_realloc(void *p, size_t n) {
    if (!p) return dg_malloc(n);
    const blk_t *b = (const blk_t *)p - 1;
    const uint32_t have = b->size - sizeof(blk_t);
    if (n <= have) return p;
    void *q = dg_malloc(n);
    if (q) { memcpy(q, p, have); dg_free(p); }
    return q;
}
char *dg_strdup(const char *s) { const size_t n = strlen(s) + 1u; char *d = dg_malloc(n); if (d) memcpy(d, s, n); return d; }

// ---- 終わり（ファームへ戻る。設定は I_Quit が先に書いている）----
void dg_exit(int code) {
    printf("[doom] exit(%d)\n", code);
    sgx->exit(code);
    for (;;) {}
}
char *dg_getenv(const char *name) { (void)name; return NULL; }

// ---- ファイル ----
// パスは "A:/dir/name" か、ドライブ文字の無いもの（Doom の設定・セーブ）。無いものは基のフォルダ（WAD のフォルダ）の下
static char base_dir[200];
void dg_set_base_dir(const char *abs_dir) { snprintf(base_dir, sizeof base_dir, "%s", abs_dir); }
static char config_dir[96] = "./";
void dg_set_config_dir(const char *abs_dir) { snprintf(config_dir, sizeof config_dir, "%s", abs_dir); }
const char *dg_config_dir(void) { return config_dir; }
static void to_abs(const char *path, char *out, unsigned n) {
    char tmp[512];                                          // 基のフォルダ 200 + パス。Doom の長いパスでも途中で切らない（2026-11-17）
    if (path[0] && path[1] == ':') snprintf(tmp, sizeof tmp, "%s", path);
    else snprintf(tmp, sizeof tmp, "%s/%s", base_dir, path);
    // 整える: "/./" -> "/"、"//" -> "/"、終わりの "/" と "/." を取る（Doom は ".//.savegame/" のようにつなぐ）。"X:/" は残す
    unsigned o = 0;
    for (const char *p = tmp; *p && o + 1u < n; p++) {
        if (*p == '/' && o && out[o - 1] == '/') continue;
        if (*p == '.' && o && out[o - 1] == '/' && (p[1] == '/' || !p[1])) { if (p[1]) p++; continue; }
        out[o++] = *p;
    }
    out[o] = 0;
    while (o > 3u && out[o - 1] == '/') out[--o] = 0;
}
#define SMALL_FILE (64u * 1024u)        // これ以下は丸ごと読む（exFAT・NTFS でも読める）。大きいものは好きな位置から（いまは FAT だけ）
struct dg_file {
    struct dg_file *next;                // 開いているものの鎖（終わるときに閉じる: Doom は WAD を閉じないので、SRAM の受け場が残った）
    bool      write, eof;
    char      path[200];
    uint32_t  pos, size, cap;
    uint8_t  *data;                     // 丸ごと読んだ中身・書く中身（置き場）
    sgx_file_t *ra;                     // 大きいファイル
};
static dg_file_t *open_files;
static void link_open(dg_file_t *f) { f->next = open_files; open_files = f; }
static void unlink_open(dg_file_t *f) { for (dg_file_t **p = &open_files; *p; p = &(*p)->next) if (*p == f) { *p = f->next; return; } }
// 終わるとき（dg_main.c）: 開いたままの大きいファイルの受け場（SRAM 16KB）を返す。書きかけは書かない
void dg_close_all(void) {
    for (dg_file_t *f = open_files; f; f = f->next) if (f->ra) { sgx->ra_close(f->ra); f->ra = NULL; }
    open_files = NULL;
}
dg_file_t *dg_fopen(const char *path, const char *mode) {
    char abs[200];
    to_abs(path, abs, sizeof abs);
    dg_file_t *f = dg_calloc(1, sizeof *f);
    if (!f) return NULL;
    snprintf(f->path, sizeof f->path, "%s", abs);
    if (mode[0] == 'w' || mode[0] == 'a') { f->write = true; link_open(f); return f; }
    sgx_stat_t st;
    if (sgx->stat(abs, &st) || st.is_dir) { dg_free(f); return NULL; }
    const uint32_t size = st.size;
    f->size = size;
    if (size <= SMALL_FILE) {
        f->data = dg_malloc(size ? size : 1u);
        uint32_t got = 0;
        int e = SGX_ENOMEM;
        if (f->data) { e = sgx->read_file(abs, f->data, size, &got); dg_video_back_moved(); }
        if (e || got != size) {
            if (f->data) printf("[doom] could not read %s: %s\n", abs, e ? sgx->strerror(e) : "short read");
            dg_free(f->data); dg_free(f); return NULL;
        }
        link_open(f);
        return f;
    }
    int e = 0;
    f->ra = sgx->ra_open(abs, &e);
    if (!f->ra) { printf("[doom] %s: %s\n", abs, sgx->strerror(e)); dg_free(f); return NULL; }
    printf("[doom] opened %s (%lu bytes)\n", abs, (unsigned long)size);
    link_open(f);
    return f;
}
// 読めなかったとき（2026-10-24）: USB のホストが作り直されるとドライブが外れて、少したつと戻る。機器の番号は変わりうるので、
// パスから開き直して読む（15 秒まで。Doom のタスクは眠って待つ）
static bool ra_reopen_read(dg_file_t *f, void *p, uint32_t want) {
    printf("[doom] could not read %s -- waiting for the drive\n", f->path);
    for (unsigned i = 0; i < 75u; i++) {
        sgx->sleep_us(200000);
        if (f->ra) { sgx->ra_close(f->ra); f->ra = NULL; }
        int e = 0;
        f->ra = sgx->ra_open(f->path, &e);
        if (!f->ra) continue;
        if (sgx->ra_size(f->ra) != f->size) break;          // 別のファイルになった
        if (!sgx->ra_read(f->ra, f->pos, p, want)) { printf("[doom] reopened %s\n", f->path); return true; }
    }
    printf("[doom] gave up reading %s\n", f->path);
    return false;
}
size_t dg_fread(void *p, size_t sz, size_t n, dg_file_t *f) {
    if (!f || f->write || !sz) return 0;
    uint32_t want = (uint32_t)(sz * n);
    if (f->pos >= f->size) { f->eof = true; return 0; }
    if (want > f->size - f->pos) { want = f->size - f->pos; f->eof = true; }
    if (f->data) memcpy(p, f->data + f->pos, want);
    else if ((!f->ra || sgx->ra_read(f->ra, f->pos, p, want)) && !ra_reopen_read(f, p, want)) return 0;
    f->pos += want;
    return want / sz;
}
size_t dg_fwrite(const void *p, size_t sz, size_t n, dg_file_t *f) {
    if (f == DG_STDOUT || f == DG_STDERR) {                 // UART のログへ
        const char *c = p;
        for (size_t i = 0; i < sz * n; i++) putchar(c[i]);
        return n;
    }
    if (!f || !f->write) return 0;
    const uint32_t len = (uint32_t)(sz * n);
    if (f->pos + len > f->cap) {
        uint32_t cap = f->cap ? f->cap : 4096u;
        while (cap < f->pos + len) cap *= 2u;
        uint8_t *d = dg_realloc(f->data, cap);
        if (!d) return 0;
        f->data = d; f->cap = cap;
    }
    memcpy(f->data + f->pos, p, len);
    f->pos += len;
    if (f->pos > f->size) f->size = f->pos;
    return n;
}
int dg_fseek(dg_file_t *f, long off, int whence) {
    if (!f || f == DG_STDOUT || f == DG_STDERR) return -1;
    const long base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (long)f->pos : (long)f->size;
    if (base + off < 0) return -1;
    f->pos = (uint32_t)(base + off); f->eof = false;
    return 0;
}
long dg_ftell(const dg_file_t *f) { return f && f != DG_STDOUT && f != DG_STDERR ? (long)f->pos : -1; }
int dg_feof(const dg_file_t *f) { return f && f != DG_STDOUT && f != DG_STDERR && f->eof; }
int dg_fflush(dg_file_t *f) { (void)f; return 0; }
int dg_fclose(dg_file_t *f) {
    if (!f || f == DG_STDOUT || f == DG_STDERR) return 0;
    unlink_open(f);
    int r = 0;
    if (f->write) {
        const int e = sgx->write_file(f->path, f->data ? f->data : (const uint8_t *)"", f->size);   // SRAM はファームが空ける
        dg_video_back_moved();
        if (e) { printf("[doom] could not write %s: %s\n", f->path, sgx->strerror(e)); r = -1; }
        else printf("[doom] wrote %s (%lu bytes)\n", f->path, (unsigned long)f->size);
    }
    if (f->ra) sgx->ra_close(f->ra);
    dg_free(f->data);
    dg_free(f);
    return r;
}
int dg_vfprintf(dg_file_t *f, const char *fmt, va_list ap) {
    char b[256];
    const int n = vsnprintf(b, sizeof b, fmt, ap);
    if (n <= 0) return n;
    if (f == DG_STDOUT || f == DG_STDERR) {
        printf("%s", b);
        if (f == DG_STDERR) sgx->term_write(b, (unsigned)strlen(b));   // 誤り（I_Error）は端末にも（終わったあとファームが見せる）
        return n;
    }
    return (int)dg_fwrite(b, 1, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1), f);
}
int dg_fprintf(dg_file_t *f, const char *fmt, ...) { va_list ap; va_start(ap, fmt); const int n = dg_vfprintf(f, fmt, ap); va_end(ap); return n; }
int dg_fputs(const char *s, dg_file_t *f) { return (int)dg_fwrite(s, 1, strlen(s), f); }
int dg_fputc(int c, dg_file_t *f) { const char ch = (char)c; return dg_fwrite(&ch, 1, 1, f) ? c : -1; }
char *dg_fgets(char *s, int n, dg_file_t *f) {
    int i = 0;
    while (i + 1 < n) {
        char c;
        if (dg_fread(&c, 1, 1, f) != 1) break;
        s[i++] = c;
        if (c == '\n') break;
    }
    if (!i) return NULL;
    s[i] = 0;
    return s;
}
// フォルダの作成・消す・名前の変更（セーブは仮の名前で書いて名前を変える）。SRAM はファームが空けるので、そのあと描き先を直す
int dg_remove(const char *path) {
    char abs[200]; to_abs(path, abs, sizeof abs);
    const int e = sgx->remove(abs); dg_video_back_moved();
    return e ? -1 : 0;
}
int dg_rename(const char *a, const char *b) {
    char x[200], y[200]; to_abs(a, x, sizeof x); to_abs(b, y, sizeof y);
    const int e = sgx->rename(x, y); dg_video_back_moved();
    if (e) printf("[doom] could not rename %s to %s: %s\n", x, y, sgx->strerror(e));
    return e ? -1 : 0;
}
int dg_mkdir(const char *path) {
    if (!path[0] || !strcmp(path, ".") || !strcmp(path, "./")) return 0;   // いまのフォルダ（Doom の設定・セーブの置き場）はもうある
    char abs[200]; to_abs(path, abs, sizeof abs);
    const int e = sgx->mkdir(abs); dg_video_back_moved();
    if (e && e != SGX_EEXIST) { printf("[doom] could not make %s: %s\n", abs, sgx->strerror(e)); return -1; }
    return 0;
}

// ---- 文字列（newlib の scanf・strtod を引き込まないため）----
static int digit(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99; }
unsigned long dg_strtoul(const char *s, char **end, int base) {
    const char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    bool neg = false;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && digit(p[2]) < 16) { p += 2; base = 16; }
    else if (base == 0) base = p[0] == '0' ? 8 : 10;
    const char *d0 = p;
    unsigned long v = 0;
    while (digit(*p) < base) v = v * (unsigned long)base + (unsigned long)digit(*p++);
    if (end) *end = (char *)(p == d0 ? s : p);
    return neg ? (unsigned long)-(long)v : v;
}
long dg_strtol(const char *s, char **end, int base) { return (long)dg_strtoul(s, end, base); }
int dg_atoi(const char *s) { return (int)dg_strtol(s, NULL, 10); }
double dg_strtod(const char *s, char **end) {
    const char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    bool neg = false;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    double v = 0, f = 1;
    bool any = false;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p++ - '0'); any = true; }
    if (*p == '.') { p++; while (*p >= '0' && *p <= '9') { f /= 10; v += (*p++ - '0') * f; any = true; } }
    if (end) *end = (char *)(any ? p : s);
    return neg ? -v : v;
}
char *dg_strstr(const char *h, const char *n) {
    const size_t l = strlen(n);
    for (; *h; h++) if (!strncmp(h, n, l)) return (char *)h;
    return l ? NULL : (char *)h;
}
static bool sp(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
int dg_sscanf(const char *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = 0;
    while (*fmt) {
        if (sp(*fmt)) { while (sp(*s)) s++; fmt++; continue; }
        if (*fmt != '%') { if (*s != *fmt) break; s++; fmt++; continue; }
        fmt++;
        const char c = *fmt++;
        int base = c == 'x' ? 16 : c == 'o' ? 8 : c == 'i' ? 0 : c == 'd' ? 10 : -1;
        if (base < 0) break;
        while (sp(*s)) s++;
        char *end;
        const long v = dg_strtol(s, &end, base);
        if (end == s) break;
        *va_arg(ap, int *) = (int)v;
        s = end; n++;
    }
    va_end(ap);
    return n ? n : (*s ? 0 : -1);
}

// 設定の読み込み（m_config.c の "%79s %99[^\n]\n"）が使う分だけ: %Ns（空白まで）、%N[^\n]（行の終わりまで）、空白（いくつでも飛ばす）、字
static int getc_f(dg_file_t *f) { uint8_t c; return dg_fread(&c, 1, 1, f) == 1 ? c : -1; }
static void ungetc_f(dg_file_t *f) { if (f->pos) { f->pos--; f->eof = false; } }
int dg_fscanf(dg_file_t *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = 0, c;
    while (*fmt) {
        if (sp(*fmt)) { while ((c = getc_f(f)) >= 0 && sp((char)c)) {} if (c >= 0) ungetc_f(f); fmt++; continue; }
        if (*fmt != '%') { c = getc_f(f); if (c != (uint8_t)*fmt) { if (c >= 0) ungetc_f(f); break; } fmt++; continue; }
        fmt++;
        unsigned width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10u + (unsigned)(*fmt++ - '0');
        if (!width) width = 0xFFFFu;
        const bool line = fmt[0] == '[' && fmt[1] == '^' && fmt[2] == '\\' && fmt[3] == 'n' && fmt[4] == ']';
        const bool line2 = fmt[0] == '[' && fmt[1] == '^' && fmt[2] == '\n' && fmt[3] == ']';
        if (*fmt == 's') { fmt++; while ((c = getc_f(f)) >= 0 && sp((char)c)) {} }
        else if (line2) { fmt += 4; c = getc_f(f); }
        else if (line) { fmt += 5; c = getc_f(f); }
        else break;
        char *out = va_arg(ap, char *);
        unsigned k = 0;
        while (c >= 0 && k < width && (line || line2 ? c != '\n' : !sp((char)c))) { out[k++] = (char)c; c = getc_f(f); }
        if (c >= 0) ungetc_f(f);
        out[k] = 0;
        if (!k) break;
        n++;
    }
    va_end(ap);
    return n ? n : (dg_feof(f) ? -1 : 0);
}
