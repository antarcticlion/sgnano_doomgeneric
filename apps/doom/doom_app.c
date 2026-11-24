// Doom の外部実行ファイル（DOOM.SGX、2026-10-24。third_party/doomgeneric + Freedoom、GPL-2.0）。docs/notes/doom.md・docs/sgx_abi.md
//
// - ファームのファイラーが、WAD（DoomFiles/freedoom1.wad など）で Enter したときに "DOOM.SGX -iwad <WAD>" で走らせる。ファームのシンボルには結ばず、
//   入口に渡される SGX の API の表だけを使う（ファームに GPL がかからない）
// - 画面はモード 7（320x200x8 のビットマップ）でテキスト面は閉じる（SGX_MODE_NO_TEXT。SRAM を 2 枚の面とファイルの書き込みに空ける）
// - Doom の .data・.bss はこの像の中（PSRAM の後部、キャッシュの窓）。ゾーン（Z_Malloc）と malloc の置き場は API の psram_alloc、
//   ゾーンの残りは FB のページを借りる（fb_borrow。モード 7 では FB を使わない）
// - キー: USB キーボードは押している・離したを毎回見る（key_down）。UART のキー（SGX_KEY_UART）は押して少しあとに離したことにする。パッドも
// - 終わり: Doom のメニューの Quit（I_Quit -> exit -> API の exit）。止めるのは Shift+ESC（ESC は Doom のメニュー: SGX_F_ESC_KEY）
#include "sgx_api.h"
#include "dg_sys.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern const sgx_api_t *sgx;
#define DOOM_HEAP_BYTES  (768u * 1024u)       // malloc の置き場（WAD の目次 約 60KB・名前など）
#define DOOM_ZONE_MIN    (1u * 1024u * 1024u) // FB の 4 ページ（2.4MB）も足すので、ここは小さくてよい
#define DOOM_ZONE_MAX    (6u * 1024u * 1024u)
#define DOOM_PSRAM_SPARE (512u * 1024u)       // ゾーンを取ったあとに残す PSRAM（ファームのファイルの読み書き・一覧の置き場など。2026-11-13: OPL2 の BGM（音楽の状態・スタック・チップ）の分 +128KB）

extern uint32_t dg_frames;
extern int myargc;
extern char **myargv;
uint8_t *DG_ScreenBuffer = NULL;           // doomgeneric.c の代わり（使わない: I_VideoBuffer に直に描く）
enum {                                     // doomkeys.h の値（Doom の見出しは読まない）
    DK_RIGHTARROW = 0xae, DK_LEFTARROW = 0xac, DK_UPARROW = 0xad, DK_DOWNARROW = 0xaf, DK_STRAFE_L = 0xa0, DK_STRAFE_R = 0xa1,
    DK_USE = 0xa2, DK_FIRE = 0xa3, DK_ESCAPE = 27, DK_ENTER = 13, DK_TAB = 9, DK_F1 = 0x80 + 0x3b, DK_BACKSPACE = 0x7f,
    DK_EQUALS = 0x3d, DK_MINUS = 0x2d, DK_RSHIFT = 0x80 + 0x36, DK_RALT = 0x80 + 0x38,
};
void M_FindResponseFile(void);
void D_DoomMain(void);
void doomgeneric_Tick(void);

typedef struct { uint8_t *ptr; uint32_t size; } zseg_t;

static struct {
    uint8_t  *zone;
    uint32_t  zone_size;
    void     *fbp[8];
    unsigned  fbp_n;
    zseg_t    zseg[9];                  // zone + fbp[] を番地昇順に並べて隣接結合したもの
    unsigned  zseg_n;
    uint16_t  load_drawn;               // 読み込みの画面に描いた進み具合（千分率）。0xFFFF = 未だ描いていない
    char      wad[200], dir[200];
    char     *argv[16];
} D;

// ---- ゾーン（third_party/doomgeneric の i_system.c・z_zone.c の sgnano: から）----
// z_zone.c の Z_Init は DG_ZoneBase より後ろの番地だけ DG_ZoneExtra で連結する。
// ABI 5.18 以降は FB ページが PSRAM の最下位に配置されるため、D.zone と D.fbp[] を
// まとめて番地昇順ソート＋隣接結合し、先頭を DG_ZoneBase、残りを DG_ZoneExtra で返す。
uint8_t *DG_ZoneBase(int *size) {
    *size = D.zseg_n ? (int)D.zseg[0].size : 0;
    return D.zseg_n ? D.zseg[0].ptr : NULL;
}
uint8_t *DG_ZoneExtra(int i, int *size) {
    if (i < 0 || (unsigned)(i + 1) >= D.zseg_n) return NULL;
    *size = (int)D.zseg[i + 1].size;
    return D.zseg[i + 1].ptr;
}

// ---- 時刻・眠り ----
uint32_t DG_GetTicksMs(void) { return sgx->ms(); }
void DG_SleepMs(uint32_t ms) { sgx->sleep_us((ms ? ms : 1u) * 1000u); }
void DG_Init(void) {}
void DG_DrawFrame(void) {}
void DG_SetWindowTitle(const char *t) { (void)t; }

// ---- キー ----
// USB キーボード: sgnano のキー -> Doom のキー。毎回いまの押下を見て、変わったものを知らせる
static const struct { int sg; uint8_t dk; } keymap[] = {
    { SGX_KEY_UP, DK_UPARROW }, { SGX_KEY_DOWN, DK_DOWNARROW }, { SGX_KEY_LEFT, DK_LEFTARROW }, { SGX_KEY_RIGHT, DK_RIGHTARROW },
    { SGX_KEY_CTRL, DK_FIRE }, { ' ', DK_USE }, { SGX_KEY_ALT, DK_RALT }, { SGX_KEY_SHIFT, DK_RSHIFT },
    { ',', DK_STRAFE_L }, { '.', DK_STRAFE_R }, { SGX_KEY_ENTER, DK_ENTER }, { SGX_KEY_ESC, DK_ESCAPE }, { SGX_KEY_TAB, DK_TAB },
    { SGX_KEY_BS, DK_BACKSPACE }, { '-', DK_MINUS }, { '=', DK_EQUALS },
    { SGX_KEY_F(1), DK_F1 }, { SGX_KEY_F(2), DK_F1 + 1 }, { SGX_KEY_F(3), DK_F1 + 2 }, { SGX_KEY_F(4), DK_F1 + 3 }, { SGX_KEY_F(5), DK_F1 + 4 },
    { SGX_KEY_F(6), DK_F1 + 5 }, { SGX_KEY_F(7), DK_F1 + 6 }, { SGX_KEY_F(8), DK_F1 + 7 }, { SGX_KEY_F(9), DK_F1 + 8 }, { SGX_KEY_F(10), DK_F1 + 9 },
    { SGX_KEY_F(11), 0x80 + 0x57 }, { SGX_KEY_F(12), 0x80 + 0x58 },
};
#define NKEYS (sizeof keymap / sizeof keymap[0] + 36u)      // 表 + '0'〜'9'・'a'〜'z'
static uint8_t key_was[NKEYS];
static int key_sg(unsigned i) { return i < sizeof keymap / sizeof keymap[0] ? keymap[i].sg : (i -= sizeof keymap / sizeof keymap[0]) < 10u ? '0' + (int)i : 'a' + (int)(i - 10u); }
static uint8_t key_dk(unsigned i) { return i < sizeof keymap / sizeof keymap[0] ? keymap[i].dk : (uint8_t)key_sg(i); }
// UART のキー（SGX_KEY_UART の付いたもの）: 押して 4 回目の問い合わせで離す
#define UQ 16u
static uint8_t uq[UQ], uq_r, uq_w, u_hold_key, u_hold_left;
static uint8_t uart_doom_key(int k) {
    switch (k) {
    case SGX_KEY_UP: return DK_UPARROW; case SGX_KEY_DOWN: return DK_DOWNARROW; case SGX_KEY_LEFT: return DK_LEFTARROW; case SGX_KEY_RIGHT: return DK_RIGHTARROW;
    case SGX_KEY_ENTER: return DK_ENTER; case SGX_KEY_ESC: return DK_ESCAPE; case SGX_KEY_TAB: return DK_TAB; case SGX_KEY_BS: return DK_BACKSPACE;
    case ' ': return DK_USE; case 'f': return DK_FIRE;            // UART には Ctrl が無いので f を撃つに
    default: break;
    }
    if (k >= SGX_KEY_F(1) && k <= SGX_KEY_F(12)) return (uint8_t)(DK_F1 + (k - SGX_KEY_F(1)) + (k >= SGX_KEY_F(11) ? 0x57 - 0x45 : 0));
    if (k >= 0x20 && k < 0x7F) return (uint8_t)(k >= 'A' && k <= 'Z' ? k + 32 : k);
    return 0;
}
static void drain_keys(void) {                                  // 届いたキーを取る（USB キーボードの分は捨てる: 押下を直に見る）
    for (int k; (k = sgx->key(false)) >= 0; ) {
        if (!(k & SGX_KEY_UART)) continue;
        const uint8_t dk = uart_doom_key(k & 0xFFFF);
        if (dk && (uint8_t)(uq_w - uq_r) < UQ) { uq[uq_w % UQ] = dk; uq_w++; }
    }
}
// パッド: つながっている最初のパッドを、キーとして知らせる。並びは F310 の D モード（bit0 = X ...）。
// 十字キーか左スティックで前後・回る、A = 撃つ（メニューでは Enter）、B = 使う（メニューでは戻る = BS）、X = 走る、Y = 地図（Tab）、
// LB / RB = 横歩き、LT = 平行移動、RT = 撃つ、START = メニュー（ESC）、BACK = Enter
enum { PV_UP, PV_DOWN, PV_LEFT, PV_RIGHT, PV_A, PV_A2, PV_B, PV_B2, PV_X, PV_Y, PV_LB, PV_RB, PV_LT, PV_RT, PV_START, PV_BACK, PV_N };
static const uint8_t pv_key[PV_N] = { DK_UPARROW, DK_DOWNARROW, DK_LEFTARROW, DK_RIGHTARROW, DK_FIRE, DK_ENTER, DK_USE, DK_BACKSPACE,
                                      DK_RSHIFT, DK_TAB, DK_STRAFE_L, DK_STRAFE_R, DK_RALT, DK_FIRE, DK_ESCAPE, DK_ENTER };
static uint8_t pv_was[PV_N], pv_sent[PV_N];
// 確かめの問い（「y を押すと終わります」など）が出ている間は、A = y・B = n（パッドだけで終われるように）。m_menu.c の変数
extern int messageToPrint;
extern int messageNeedsInput;                  // doomtype.h の boolean（int と同じ大きさ）
static uint8_t pad_key_now(unsigned v) {
    if (messageToPrint && messageNeedsInput) {
        if (v == PV_A) return 'y';
        if (v == PV_B) return 'n';
        if (v == PV_A2 || v == PV_B2) return 0;
    }
    return pv_key[v];
}
static sgx_pad_t pad_now;
static bool pad_ok;
static bool pad_state(unsigned v) {
    if (!pad_ok) return false;
    const sgx_pad_t *p = &pad_now;
    const unsigned h = p->hat;
    const int ax = (p->axes_mask & 1u) ? p->axis[0] : 0, ay = (p->axes_mask & 2u) ? p->axis[1] : 0;
    const uint32_t b = p->buttons;
    switch (v) {
    case PV_UP:    return (h < 8u && (h == 7u || h <= 1u)) || ay < -64;
    case PV_DOWN:  return (h >= 3u && h <= 5u) || ay > 64;
    case PV_LEFT:  return (h >= 5u && h <= 7u) || ax < -64;
    case PV_RIGHT: return (h >= 1u && h <= 3u) || ax > 64;
    case PV_A: case PV_A2: return (b >> 1) & 1u;
    case PV_B: case PV_B2: return (b >> 2) & 1u;
    case PV_X:     return b & 1u;
    case PV_Y:     return (b >> 3) & 1u;
    case PV_LB:    return (b >> 4) & 1u;
    case PV_RB:    return (b >> 5) & 1u;
    case PV_LT:    return (b >> 6) & 1u;
    case PV_RT:    return (b >> 7) & 1u;
    case PV_BACK:  return (b >> 8) & 1u;
    case PV_START: return (b >> 9) & 1u;
    default: return false;
    }
}
int DG_GetKey(int *pressed, unsigned char *key) {
    pad_ok = false;
    for (int i = 0; i < SGX_PAD_MAX && !pad_ok; i++) pad_ok = sgx->pad(i, &pad_now) && pad_now.connected;
    for (unsigned v = 0; v < PV_N; v++) {
        const uint8_t now = pad_state(v) ? 1u : 0u;
        if (now != pv_was[v]) {
            pv_was[v] = now;
            const uint8_t k = now ? pad_key_now(v) : pv_sent[v];
            if (now) pv_sent[v] = k;
            if (!k) continue;
            *pressed = now; *key = k; return 1;
        }
    }
    if (sgx->keyboard()) {
        for (unsigned i = 0; i < NKEYS; i++) {
            const uint8_t now = sgx->key_down(key_sg(i)) ? 1u : 0u;
            if (now != key_was[i]) { key_was[i] = now; *pressed = now; *key = key_dk(i); return 1; }
        }
    }
    drain_keys();
    if (u_hold_left && --u_hold_left == 0) { *pressed = 0; *key = u_hold_key; return 1; }
    if (!u_hold_left && uq_r != uq_w) {
        u_hold_key = uq[uq_r % UQ]; uq_r++;
        u_hold_left = 4;
        *pressed = 1; *key = u_hold_key; return 1;
    }
    return 0;
}

// ---- 読み込みの進み具合（d_main.c・r_data.c の sgnano: から）----
// stage は起動の段、ループの中は i / n。千分率は実機（USB メモリの FAT32、23 秒）で測った段の時間の割合から:
// テクスチャの見本作り（R_GenerateLookup、すべてのパッチを読む）が 85%、スプライトの大きさの読み出しが 12%、ほかは 3%
static void draw_loading(unsigned pm);
static uint16_t load_pm;
void DG_Load(int stage, int i, int n) {
    unsigned pm;
    const unsigned f = n > 0 ? (unsigned)i * 1000u / (unsigned)n : 0u;
    switch (stage) {
    case 0:  pm = 0; break;
    case 1:  pm = 5; break;
    case 10: pm = 5u + f * 10u / 1000u; break;
    case 2:  pm = 15u + f * 5u / 1000u; break;
    case 11: pm = 20u + f * 840u / 1000u; break;
    case 12: case 3: pm = 860; break;
    case 4:  pm = 860u + f * 120u / 1000u; break;
    default: pm = stage >= 5 && stage <= 9 ? 980u + (unsigned)(stage - 5) * 4u : 0u; break;
    }
    if (pm > load_pm) { load_pm = (uint16_t)pm; draw_loading(pm); }
}
// 読み込みの画面: 表示の面（320x200、SRAM。いつ書いてもよい）にフォント ROM の字を描く。色は面の既定のパレット（PC-98 の 16 色）
enum { C_BLACK = 0, C_GREY = 7, C_DGREY = 8, C_CYAN = 13, C_YELLOW = 14, C_WHITE = 15 };
static void bm_fill(uint8_t *v, unsigned x, unsigned y, unsigned w, unsigned h, uint8_t c) {
    for (unsigned j = y; j < y + h && j < SGX_BM_H; j++) memset(v + j * SGX_BM_W + x, c, x + w <= SGX_BM_W ? w : SGX_BM_W - x);
}
static unsigned utf8_cp(const char **p) {
    const uint8_t *s = (const uint8_t *)*p;
    unsigned c = s[0], n = c < 0x80u ? 1u : c < 0xE0u ? 2u : c < 0xF0u ? 3u : 4u;
    if (n > 1u) { c &= 0x3Fu >> (n - 1u); for (unsigned k = 1; k < n && s[k]; k++) c = c << 6 | (s[k] & 0x3Fu); }
    *p += n;
    return c;
}
static unsigned glyph_w(unsigned cp) { uint16_t rows[16]; bool half = true; sgx->font_glyph(cp, rows, &half); return half ? 8u : 16u; }
static unsigned bm_width(const char *s, unsigned scale) { unsigned w = 0; while (*s) w += glyph_w(utf8_cp(&s)) * scale; return w; }
static void bm_puts(uint8_t *v, unsigned x, unsigned y, const char *s, uint8_t c, unsigned scale) {
    while (*s && x < SGX_BM_W) {
        const unsigned cp = utf8_cp(&s);
        uint16_t rows[16]; bool half = true;
        sgx->font_glyph(cp, rows, &half);
        const unsigned w = half ? 8u : 16u;
        for (unsigned r = 0; r < 16u; r++)
            for (unsigned b = 0; b < w; b++)
                if (rows[r] & (0x8000u >> b)) bm_fill(v, x + b * scale, y + r * scale, scale, scale, c);
        x += w * scale;
    }
}
#define BAR_X 40u
#define BAR_Y 128u
#define BAR_W 240u
#define BAR_H 12u
static void draw_loading(unsigned pm) {
    uint8_t *v = sgx->bm_vram();
    if (!v || dg_frames) return;                                // 最初の絵が出たら描かない
    if (D.load_drawn == 0xFFFFu) {                              // 初めの 1 回: 題・名前・棒の枠
        memset(v, C_BLACK, SGX_BM_W * SGX_BM_H);
        const char *t = "読み込んでいます";
        bm_puts(v, (SGX_BM_W - bm_width(t, 2)) / 2u, 56, t, C_WHITE, 2);
        const char *name = strrchr(D.wad, '/');
        name = name ? name + 1 : D.wad;
        const unsigned nw = bm_width(name, 1);
        bm_puts(v, nw < SGX_BM_W ? (SGX_BM_W - nw) / 2u : 0u, 100, name, C_YELLOW, 1);
        bm_fill(v, BAR_X - 2u, BAR_Y - 2u, BAR_W + 4u, BAR_H + 4u, C_GREY);
        bm_fill(v, BAR_X, BAR_Y, BAR_W, BAR_H, C_DGREY);
        D.load_drawn = 0;
    }
    if (pm / 10u == D.load_drawn / 10u && pm) return;           // 1% ごとに描き直す
    bm_fill(v, BAR_X, BAR_Y, BAR_W * pm / 1000u, BAR_H, C_CYAN);
    char p[8]; snprintf(p, sizeof p, "%u%%", pm / 10u);
    bm_fill(v, 0, BAR_Y + BAR_H + 6u, SGX_BM_W, 16, C_BLACK);
    bm_puts(v, (SGX_BM_W - bm_width(p, 1)) / 2u, BAR_Y + BAR_H + 6u, p, C_WHITE, 1);
    if (sgx->size >= offsetof(sgx_api_t, bm_flush) + sizeof sgx->bm_flush && sgx->bm_flush)
        sgx->bm_flush(0, 0, SGX_BM_W, SGX_BM_H);
    D.load_drawn = (uint16_t)pm;
}

// ---- 入口 ----
static void build_zone_segs(uint32_t fb_bytes) {
    zseg_t raw[9];
    unsigned n = 0;
    if (D.zone && D.zone_size) raw[n++] = (zseg_t){ D.zone, D.zone_size };
    for (unsigned i = 0; i < D.fbp_n && n < 9u; i++) {
        if (D.fbp[i] && fb_bytes) raw[n++] = (zseg_t){ (uint8_t *)D.fbp[i], fb_bytes };
    }
    for (unsigned i = 1; i < n; i++) {
        for (unsigned j = i; j && (uintptr_t)raw[j - 1].ptr > (uintptr_t)raw[j].ptr; j--) {
            zseg_t t = raw[j]; raw[j] = raw[j - 1]; raw[j - 1] = t;
        }
    }
    D.zseg_n = 0;
    for (unsigned i = 0; i < n; i++) {
        if (D.zseg_n && D.zseg[D.zseg_n - 1u].ptr + D.zseg[D.zseg_n - 1u].size == raw[i].ptr) {
            D.zseg[D.zseg_n - 1u].size += raw[i].size;
        } else {
            D.zseg[D.zseg_n++] = raw[i];
        }
    }
}
// 設定（default.cfg・doomgenericdoom.cfg）はシステムのドライブ S: に（2026-10-25 ユーザー指示）。Doom II の形の IWAD は Doom2Settings、ほかは DoomSettings。
// フォルダが無ければ作り、前の置き場（WAD のフォルダ）にだけ設定があれば写す（元は残す）。セーブは WAD のフォルダのまま
static void set_config_dir(void) {
    static const char *const doom2[] = { "freedoom2.wad", "doom2.wad", "plutonia.wad", "tnt.wad", NULL };
    const char *base = strrchr(D.wad, '/'); base = base ? base + 1 : D.wad;
    bool d2 = false;
    for (const char *const *n = doom2; *n; n++) { const char *a = base, *b = *n; while (*a && *b && (*a | 0x20) == *b) { a++; b++; } if (!*a && !*b) d2 = true; }
    char dir[96];
    snprintf(dir, sizeof dir, "S:/SystemSettings/%s", d2 ? "Doom2Settings" : "DoomSettings");
    sgx_stat_t st;
    if (sgx->stat(dir, &st) != SGX_OK) {
        const int e = sgx->mkdir(dir);
        if (e != SGX_OK) { printf("[doom] could not make %s (%s), the settings stay in %s\n", dir, sgx->strerror(e), D.dir); return; }
    }
    static const char *const files[] = { "default.cfg", "doomgenericdoom.cfg", NULL };
    for (const char *const *f = files; *f; f++) {
        char np[128], op[256];
        snprintf(np, sizeof np, "%s/%s", dir, *f);
        snprintf(op, sizeof op, "%s/%s", D.dir, *f);
        if (sgx->stat(np, &st) == SGX_OK || sgx->stat(op, &st) != SGX_OK || st.size > 64u * 1024u) continue;
        void *buf = sgx->psram_alloc(st.size ? st.size : 1u, 4);
        uint32_t got = 0;
        if (buf && sgx->read_file(op, buf, st.size, &got) == SGX_OK && sgx->write_file(np, buf, got) == SGX_OK)
            printf("[doom] copied the settings %s -> %s\n", op, np);
        if (buf) sgx->psram_free(buf);
    }
    char cd[100];
    snprintf(cd, sizeof cd, "%s/", dir);
    dg_set_config_dir(cd);
}

int sgx_main(const sgx_api_t *api, int argc, char **argv) {
    // 引数: "-iwad <WAD の絶対パス>"（ファイラーが WAD の Enter で付ける）。無ければ（DOOM.SGX を Enter したとき）
    // プログラムのフォルダ、その下の DoomFiles / Doom2Files、親の DoomFiles / Doom2Files から知っている名前の IWAD を探す
    // （2026-10-24: 無かったので Doom2Files で freedoom1.wad を探して黙って終わり、「起動しない」に見えた。
    //   2026-10-29: A:/DOOM.SGX から A:/DoomFiles/freedoom1.wad を見つけられるようにした）
    const char *wad = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-iwad") && i + 1 < argc) wad = argv[++i];
    }
    if (wad) snprintf(D.wad, sizeof D.wad, "%s", wad);
    else {
        static const char *const names[] = { "freedoom1.wad", "freedoom2.wad", "doom.wad", "doomu.wad", "doom1.wad", "doom2.wad", "plutonia.wad", "tnt.wad", NULL };
        static const char *const subs[] = { "DoomFiles", "Doom2Files", NULL };
        char base[200];
        snprintf(base, sizeof base, "%s", api->program_dir());
        size_t bl = strlen(base);
        while (bl > 1u && base[bl - 1u] == '/') base[--bl] = 0;
        D.wad[0] = 0;
        // 1. the folder of DOOM.SGX (the old behavior)
        for (const char *const *n = names; *n && !D.wad[0]; n++) {
            char p[200]; sgx_stat_t st;
            snprintf(p, sizeof p, "%s/%s", base, *n);
            if (!api->stat(p, &st) && !st.is_dir) snprintf(D.wad, sizeof D.wad, "%s", p);
        }
        // 2. DoomFiles / Doom2Files under the program's folder, then under its parent
        //    (A:/DOOM.SGX with A:/DoomFiles/freedoom1.wad starts; 2026-10-29)
        if (!D.wad[0]) {
            char parent[200];
            snprintf(parent, sizeof parent, "%s", base);
            char *sl = strrchr(parent, '/');
            if (sl) *sl = 0; else parent[0] = 0;
            const char *const roots[2] = { base, parent };
            for (unsigned r = 0; r < 2u && !D.wad[0]; r++) {
                if (!roots[r][0]) continue;
                for (const char *const *s = subs; *s && !D.wad[0]; s++) {
                    for (const char *const *n = names; *n && !D.wad[0]; n++) {
                        char p[200]; sgx_stat_t st;
                        snprintf(p, sizeof p, "%s/%s/%s", roots[r], *s, *n);
                        if (!api->stat(p, &st) && !st.is_dir) snprintf(D.wad, sizeof D.wad, "%s", p);
                    }
                }
            }
        }
        if (!D.wad[0]) {
            const char *m = "Doom: no IWAD (freedoom1.wad, freedoom2.wad, doom.wad, doom2.wad ...) in the folder of DOOM.SGX\n"
                            "or its DoomFiles / Doom2Files folders.\n"
                            "Put DOOM.SGX next to the WAD, or press Enter on the WAD in the filer.\n";
            api->term_write(m, (unsigned)strlen(m));
            return 2;
        }
    }
    snprintf(D.dir, sizeof D.dir, "%s", D.wad);
    char *sl = strrchr(D.dir, '/');
    if (sl) *sl = 0;
    // 画面モード 7。テキスト面は閉じ、SGX_MODE_BM_CACHED で音楽オーバーレイ（OPL2）を守る
    const char *e = api->set_mode(SGX_MODE_TEXT_BM8 | SGX_MODE_NO_TEXT | SGX_MODE_BM_CACHED);
    if (e) { printf("[doom] screen: %s\n", e); return 1; }
    const uint8_t *back = api->bm_back();
    if (!back) printf("[doom] no SRAM for the back buffer, drawing on the shown bitmap\n");
    D.load_drawn = 0xFFFFu; load_pm = 0;
    draw_loading(0);                                            // 最初の絵が出るまで（R_Init、媒体しだいで数十秒）
    // メモリ（PSRAM）
    void *heap = api->psram_alloc(DOOM_HEAP_BYTES, 32);
    uint32_t z = api->psram_largest();
    z = z > DOOM_PSRAM_SPARE ? z - DOOM_PSRAM_SPARE : 0;
    if (z > DOOM_ZONE_MAX) z = DOOM_ZONE_MAX;
    z &= ~4095u;
    D.zone = z >= DOOM_ZONE_MIN ? api->psram_alloc(z, 32) : NULL;
    D.zone_size = z;
    if (!heap || !D.zone) { printf("[doom] not enough PSRAM (zone %lu KB)\n", (unsigned long)(z / 1024u)); return 1; }
    uint32_t pb = 0;
    D.fbp_n = api->fb_borrow(D.fbp, 8, &pb);                    // FB のページもゾーンに（画面モード 7 では FB を使わない）
    build_zone_segs(pb);
    uint32_t total_z = 0;
    for (unsigned i = 0; i < D.zseg_n; i++) total_z += D.zseg[i].size;
    dg_heap_init(heap, DOOM_HEAP_BYTES);
    dg_set_base_dir(D.dir);
    set_config_dir();
    static char a0[] = "doom", a1[] = "-iwad";
    int narg = 0;
    D.argv[narg++] = a0; D.argv[narg++] = a1; D.argv[narg++] = D.wad;
    for (int i = 1; i < argc && narg < 15; i++) {
        if (!strcmp(argv[i], "-iwad")) { i++; continue; }
        D.argv[narg++] = argv[i];
    }
    D.argv[narg] = NULL;
    printf("[doom] start %s (zone %lu KB in %u segs: alloc %lu KB + FB %u x %lu KB)\n",
           D.wad, (unsigned long)(total_z / 1024u), D.zseg_n,
           (unsigned long)(D.zone_size / 1024u), D.fbp_n, (unsigned long)(pb / 1024u));
    myargc = narg; myargv = D.argv;
    M_FindResponseFile();
    D_DoomMain();                                               // 初めの 1 回分まで進む
    for (;;) doomgeneric_Tick();
}
