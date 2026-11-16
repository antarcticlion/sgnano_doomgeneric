// Doom の BGM を OPL2（YM3812）の VGM に直す（2026-11-13）。dg_opl.h の説明を先に。
// 声の割り当て・音量の曲線・2 声の音色・打楽器の決まりは DMX（Chocolate Doom の i_oplmusic.c が真似たもの）に倣う。
//   - 声は 9 つ。空きが無ければいちばん古い声を止めて使う。2 声の音色（GENMIDI の flags の 4）は 2 つ目の声が空いていれば使う
//   - 音程: 1/32 半音の単位。2 声目は fine_tuning / 2 - 64 だけずらす。ピッチベンドは ±2 半音
//   - 音量: DMX の音量の表（volume_map）でベロシティとチャンネルの音量を掛け、キャリアの減衰にする（加算の音色はモジュレータも）
//   - 打楽器（MIDI のチャンネル 10）: GENMIDI の 128 + (鍵盤 - 35)、鍵盤 35〜81
#include "dg_opl.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define OPL_CLOCK   3579545u
#define VGM_RATE    44100u
#define NVOICE      9
#define GENMIDI_HDR 8
#define GENMIDI_N   175
#define INSTR_BYTES 36

typedef struct { uint8_t tremolo, attack, sustain, waveform, scale, level; } op_t;
typedef struct { op_t mod; uint8_t feedback; op_t car; int16_t base_note; } ivoice_t;
typedef struct { uint16_t flags; uint8_t fine_tune, fixed_note; ivoice_t v[2]; } instr_t;

// DMX の音量の表（MIDI の 0〜127 -> 0〜127）
static const uint8_t volume_map[128] = {
    0, 1, 3, 5, 6, 8, 10, 11, 13, 14, 16, 17, 19, 20, 22, 23, 25, 26, 27, 29, 30, 32, 33, 34, 36, 37, 39, 41, 43, 45, 47, 49,
    50, 52, 54, 55, 57, 59, 60, 61, 63, 64, 66, 67, 68, 69, 71, 72, 73, 74, 75, 76, 77, 79, 80, 81, 82, 83, 84, 84, 85, 86,
    87, 88, 89, 90, 91, 92, 92, 93, 94, 95, 96, 96, 97, 98, 99, 99, 100, 101, 101, 102, 103, 103, 104, 105, 105, 106, 107, 107, 108, 109,
    109, 110, 110, 111, 112, 112, 113, 113, 114, 114, 115, 115, 116, 117, 117, 118, 118, 119, 119, 120, 120, 121, 121, 122, 122, 123, 123, 123, 124, 124, 125, 125,
};
static const uint8_t op_off[NVOICE] = { 0, 1, 2, 8, 9, 10, 16, 17, 18 };   // 声 -> モジュレータの演算子（キャリアは +3）

typedef struct {
    bool on;                    // 鍵盤が押されている（離したら false、声は余韻のまま）
    bool used;                  // 鳴っているか余韻（割り当て済み）
    uint8_t ch, key, note_vol;  // MIDI のチャンネル・鍵盤・ベロシティ
    const instr_t *in; unsigned which;   // 音色と、その何声目か
    uint32_t age;               // 割り当てた順
    uint8_t b0;                 // 0xB0 に書いた値（キーオフで使う）
} voice_t;
typedef struct { uint8_t prog, vol, expr; int bend32; } chan_t;   // bend32: 1/32 半音

typedef struct {
    uint8_t *b; uint32_t n, cap; bool oom;
    bool count;                 // 1 周目: 書かずに大きさだけ数える（2 周目にちょうどの大きさを取る。Doom は PSRAM をほとんどゾーンに使う）
    uint64_t wait;              // まだ書いていない待ち（標本）
    uint64_t total;             // 書いた待ちの合計
    instr_t instr[GENMIDI_N];
    voice_t v[NVOICE];
    chan_t c[16];
    uint32_t age;
    uint8_t shadow[256]; bool known[256];   // 同じ値の書き込みは省く
} st_t;

static void put(st_t *s, const uint8_t *p, uint32_t k) {
    if (s->oom) return;
    if (s->count) { s->n += k; return; }
    if (s->n + k > s->cap) { s->oom = true; return; }      // 1 周目で数えた大きさを越えることは無い
    memcpy(s->b + s->n, p, k); s->n += k;
}
static void flush_wait(st_t *s) {
    while (s->wait) {
        const uint32_t w = s->wait > 65535u ? 65535u : (uint32_t)s->wait;
        const uint8_t c[3] = { 0x61, (uint8_t)w, (uint8_t)(w >> 8) };
        put(s, c, 3);
        s->wait -= w; s->total += w;
    }
}
static void wr(st_t *s, uint8_t reg, uint8_t val) {
    if (s->known[reg] && s->shadow[reg] == val) return;
    flush_wait(s);
    const uint8_t c[3] = { 0x5A, reg, val };
    put(s, c, 3);
    s->shadow[reg] = val; s->known[reg] = true;
}

static void load_genmidi(st_t *s, const uint8_t *g) {
    for (unsigned i = 0; i < GENMIDI_N; i++) {
        const uint8_t *p = g + GENMIDI_HDR + i * INSTR_BYTES;
        instr_t *in = &s->instr[i];
        in->flags = (uint16_t)(p[0] | p[1] << 8); in->fine_tune = p[2]; in->fixed_note = p[3];
        for (unsigned k = 0; k < 2; k++) {
            const uint8_t *q = p + 4 + k * 16;
            ivoice_t *v = &in->v[k];
            v->mod = (op_t){ q[0], q[1], q[2], q[3], q[4], q[5] };
            v->feedback = q[6];
            v->car = (op_t){ q[7], q[8], q[9], q[10], q[11], q[12] };
            v->base_note = (int16_t)(q[14] | q[15] << 8);
        }
    }
}

// 声 vi の音量を書く（キャリア、加算の音色はモジュレータも）
static void set_volume(st_t *s, unsigned vi) {
    voice_t *v = &s->v[vi];
    const ivoice_t *iv = &v->in->v[v->which];
    const chan_t *c = &s->c[v->ch];
    const unsigned chv = (unsigned)c->vol * c->expr / 127u;
    const unsigned midi_volume = 2u * (volume_map[chv & 127u] + 1u);
    const unsigned full = (volume_map[v->note_vol & 127u] * midi_volume) >> 9;   // 0..63
    unsigned car = 0x3Fu - (full > 0x3Fu ? 0x3Fu : full);
    const uint8_t op = op_off[vi];
    wr(s, (uint8_t)(0x43 + op), (uint8_t)(car | (iv->car.scale & 0xC0u)));
    if ((iv->feedback & 1u) && iv->mod.level != 0x3Fu) {                        // 加算（モジュレータも聞こえる）
        unsigned mod = iv->mod.level & 0x3Fu;
        if (mod < car) mod = car;
        wr(s, (uint8_t)(0x40 + op), (uint8_t)(mod | (iv->mod.scale & 0xC0u)));
    }
}

static void set_instrument(st_t *s, unsigned vi) {
    voice_t *v = &s->v[vi];
    const ivoice_t *iv = &v->in->v[v->which];
    const uint8_t op = op_off[vi];
    wr(s, (uint8_t)(0x40 + op), (uint8_t)((iv->mod.level & 0x3Fu) | (iv->mod.scale & 0xC0u)));
    wr(s, (uint8_t)(0x20 + op), iv->mod.tremolo); wr(s, (uint8_t)(0x60 + op), iv->mod.attack);
    wr(s, (uint8_t)(0x80 + op), iv->mod.sustain); wr(s, (uint8_t)(0xE0 + op), iv->mod.waveform);
    wr(s, (uint8_t)(0x23 + op), iv->car.tremolo); wr(s, (uint8_t)(0x63 + op), iv->car.attack);
    wr(s, (uint8_t)(0x83 + op), iv->car.sustain); wr(s, (uint8_t)(0xE3 + op), iv->car.waveform);
    wr(s, (uint8_t)(0xC0 + vi), (uint8_t)(iv->feedback & 0x0Fu));
}

// 音程を書く（key_on: キーオンのビット）
static void set_freq(st_t *s, unsigned vi, bool key_on) {
    voice_t *v = &s->v[vi];
    const ivoice_t *iv = &v->in->v[v->which];
    int note = (v->in->flags & 1u) ? v->in->fixed_note : v->key;
    note += iv->base_note;
    while (note < 0) note += 12;
    while (note > 127) note -= 12;
    int f32 = note * 32 + s->c[v->ch].bend32;
    if (v->which == 1) f32 += v->in->fine_tune / 2 - 64;    // 2 声目のずらし
    const double hz = 440.0 * pow(2.0, ((double)f32 / 32.0 - 69.0) / 12.0);
    unsigned block = 0;
    double fn = hz * (double)(1u << 20) / ((double)OPL_CLOCK / 72.0);
    while (fn >= 1023.5 && block < 7u) { fn /= 2.0; block++; }
    unsigned fnum = (unsigned)(fn + 0.5);
    if (fnum > 1023u) fnum = 1023u;
    wr(s, (uint8_t)(0xA0 + vi), (uint8_t)fnum);
    v->b0 = (uint8_t)((block << 2) | (fnum >> 8));
    wr(s, (uint8_t)(0xB0 + vi), (uint8_t)(v->b0 | (key_on ? 0x20u : 0u)));
}

static void voice_off(st_t *s, unsigned vi) {
    voice_t *v = &s->v[vi];
    if (!v->used) return;
    wr(s, (uint8_t)(0xB0 + vi), v->b0);                    // キーオフ（余韻）
    v->on = false;
}
static void voice_free(st_t *s, unsigned vi) { voice_off(s, vi); s->v[vi].used = false; }

static int pick_voice(st_t *s) {
    int best = -1;
    for (unsigned i = 0; i < NVOICE; i++) if (!s->v[i].used) return (int)i;   // 空き
    for (unsigned i = 0; i < NVOICE; i++)                                      // 離した声のいちばん古いもの
        if (!s->v[i].on && (best < 0 || s->v[i].age < s->v[best].age)) best = (int)i;
    if (best >= 0) return best;
    for (unsigned i = 0; i < NVOICE; i++) if (best < 0 || s->v[i].age < s->v[best].age) best = (int)i;   // いちばん古いもの
    return best;
}

static void note_on(st_t *s, unsigned ch, unsigned key, unsigned vel) {
    const instr_t *in;
    if (ch == 9) {
        if (key < 35u || key > 81u) return;
        in = &s->instr[128u + key - 35u];
    } else in = &s->instr[s->c[ch].prog & 127u];
    const unsigned nv = (in->flags & 4u) ? 2u : 1u;
    for (unsigned w = 0; w < nv; w++) {
        int vi;
        if (w == 1) {                                       // 2 声目は空きがあるときだけ
            vi = -1;
            for (unsigned i = 0; i < NVOICE; i++) if (!s->v[i].used) { vi = (int)i; break; }
            if (vi < 0) break;
        } else vi = pick_voice(s);
        if (vi < 0) break;
        voice_free(s, (unsigned)vi);
        voice_t *v = &s->v[vi];
        v->used = true; v->on = true; v->ch = (uint8_t)ch; v->key = (uint8_t)key; v->note_vol = (uint8_t)vel;
        v->in = in; v->which = w; v->age = ++s->age;
        set_instrument(s, (unsigned)vi);
        set_volume(s, (unsigned)vi);
        set_freq(s, (unsigned)vi, true);
    }
}
static void note_off(st_t *s, unsigned ch, unsigned key) {
    for (unsigned i = 0; i < NVOICE; i++)
        if (s->v[i].used && s->v[i].on && s->v[i].ch == ch && s->v[i].key == key) voice_off(s, i);
}
static void all_off(st_t *s, int ch) {
    for (unsigned i = 0; i < NVOICE; i++) if (s->v[i].used && (ch < 0 || s->v[i].ch == ch)) voice_off(s, i);
}

// ---- SMF ----
typedef struct { const uint8_t *p, *end; uint64_t tick; uint8_t run; bool done; } trk_t;
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t vlq(trk_t *t) {
    uint32_t v = 0;
    for (unsigned k = 0; k < 4 && t->p < t->end; k++) { const uint8_t b = *t->p++; v = (v << 7) | (b & 0x7Fu); if (!(b & 0x80u)) break; }
    return v;
}
static void trk_next_delta(trk_t *t) { if (t->p >= t->end) { t->done = true; return; } t->tick += vlq(t); }

// 1 周（s->count なら数えるだけ）。t は呼び手が ntrk 個ぶん取ってある
static void run(st_t *s, trk_t *t, const uint8_t *mid, uint32_t len, const uint8_t *genmidi) {
    const uint32_t hlen = be32(mid + 4);
    const unsigned ntrk = (unsigned)(mid[10] << 8 | mid[11]);
    const unsigned div = (unsigned)(mid[12] << 8 | mid[13]);
    memset(t, 0, ntrk * sizeof *t);
    unsigned nt = 0;
    for (uint32_t p = 8u + hlen; p + 8u <= len && nt < ntrk; ) {
        const uint32_t n = be32(mid + p + 4);
        if (!memcmp(mid + p, "MTrk", 4) && p + 8u + n <= len) {
            t[nt].p = mid + p + 8u; t[nt].end = t[nt].p + n; trk_next_delta(&t[nt]); nt++;
        }
        p += 8u + n;
    }
    load_genmidi(s, genmidi);
    for (unsigned c = 0; c < 16; c++) s->c[c] = (chan_t){ 0, 100, 127, 0 };

    // VGM の頭（0x100 バイト）は最後に書き込む。ここでは場所だけ取る
    uint8_t hdr[0x100]; memset(hdr, 0, sizeof hdr);
    put(s, hdr, sizeof hdr);
    wr(s, 0x01, 0x20);                                      // 波形の選択を使う
    wr(s, 0x08, 0x40);
    wr(s, 0xBD, 0x00);                                      // リズムのモードは使わない
    for (unsigned i = 0; i < NVOICE; i++) { wr(s, (uint8_t)(0xB0 + i), 0); }

    uint32_t tempo = 500000;                                // 4 分音符の µs
    uint64_t now_tick = 0;
    double frac = 0.0;                                      // 標本の端数
    for (;;) {
        int k = -1;
        for (unsigned i = 0; i < nt; i++) if (!t[i].done && (k < 0 || t[i].tick < t[k].tick)) k = (int)i;
        if (k < 0) break;
        trk_t *tr = &t[k];
        if (tr->tick > now_tick) {                          // 時間を進める
            const double smp = (double)(tr->tick - now_tick) * tempo / div * VGM_RATE / 1e6 + frac;
            const uint64_t w = (uint64_t)smp;
            frac = smp - (double)w;
            s->wait += w;
            now_tick = tr->tick;
        }
        if (tr->p >= tr->end) { tr->done = true; continue; }
        uint8_t st = *tr->p;
        if (st & 0x80u) { tr->p++; if (st < 0xF0u) tr->run = st; } else st = tr->run;
        const unsigned ch = st & 15u;
        switch (st & 0xF0u) {
        case 0x80: if (tr->p + 2 <= tr->end) note_off(s, ch, tr->p[0]); tr->p += 2; break;
        case 0x90:
            if (tr->p + 2 <= tr->end) { if (tr->p[1]) note_on(s, ch, tr->p[0] & 127u, tr->p[1] & 127u); else note_off(s, ch, tr->p[0]); }
            tr->p += 2; break;
        case 0xA0: tr->p += 2; break;
        case 0xB0:
            if (tr->p + 2 <= tr->end) {
                const unsigned cc = tr->p[0], val = tr->p[1] & 127u;
                if (cc == 7 || cc == 11) {                  // 音量・エクスプレッション
                    if (cc == 7) s->c[ch].vol = (uint8_t)val; else s->c[ch].expr = (uint8_t)val;
                    for (unsigned i = 0; i < NVOICE; i++) if (s->v[i].used && s->v[i].ch == ch) set_volume(s, i);
                } else if (cc == 120 || cc == 123) all_off(s, (int)ch);
                else if (cc == 121) { s->c[ch].bend32 = 0; s->c[ch].expr = 127; }
            }
            tr->p += 2; break;
        case 0xC0: if (tr->p < tr->end) s->c[ch].prog = *tr->p & 127u; tr->p += 1; break;
        case 0xD0: tr->p += 1; break;
        case 0xE0:
            if (tr->p + 2 <= tr->end) {
                const int bend = (int)(tr->p[0] | tr->p[1] << 7) - 8192;
                s->c[ch].bend32 = bend * 64 / 8192;          // ±2 半音
                for (unsigned i = 0; i < NVOICE; i++) if (s->v[i].used && s->v[i].ch == ch) set_freq(s, i, s->v[i].on);
            }
            tr->p += 2; break;
        default:                                             // 0xF0..: SysEx・メタ
            if (st == 0xFFu) {
                if (tr->p >= tr->end) { tr->done = true; break; }
                const uint8_t type = *tr->p++;
                const uint32_t n = vlq(tr);
                if (type == 0x51 && n == 3 && tr->p + 3 <= tr->end) tempo = (uint32_t)(tr->p[0] << 16 | tr->p[1] << 8 | tr->p[2]);
                if (type == 0x2F) { tr->done = true; break; }
                tr->p += n;
            } else if (st == 0xF0u || st == 0xF7u) tr->p += vlq(tr);
            break;
        }
        if (!tr->done) trk_next_delta(tr);
    }
    all_off(s, -1);
    flush_wait(s);
    const uint8_t end = 0x66; put(s, &end, 1);
}

uint8_t *dg_opl_vgm(const uint8_t *mid, uint32_t len, const uint8_t *genmidi, uint32_t genmidi_len, bool loop, uint32_t *out_len) {
    if (!mid || len < 14 || memcmp(mid, "MThd", 4) || !genmidi || genmidi_len < GENMIDI_HDR + GENMIDI_N * INSTR_BYTES ||
        memcmp(genmidi, "#OPL_II#", 8)) return NULL;
    const uint32_t hlen = be32(mid + 4);
    const unsigned ntrk = (unsigned)(mid[10] << 8 | mid[11]);
    const unsigned div = (unsigned)(mid[12] << 8 | mid[13]);
    if (!ntrk || !div || (div & 0x8000u) || 8u + hlen > len) return NULL;   // SMPTE の時間は使わない（Doom の曲には無い）
    trk_t *t = calloc(ntrk, sizeof *t);
    st_t *s = calloc(1, sizeof *s);
    if (!t || !s) { free(t); free(s); return NULL; }
    s->count = true;                                        // 1 周目: 大きさを数える
    run(s, t, mid, len, genmidi);
    const uint32_t need = s->n;
    memset(s, 0, sizeof *s);
    s->b = need > 0x100u ? malloc(need) : NULL;
    s->cap = need;
    if (s->b) run(s, t, mid, len, genmidi);                 // 2 周目: 書く
    free(t);
    if (!s->b || s->oom || s->n != need) { free(s->b); free(s); return NULL; }
    // 頭を書く
    uint8_t *h = s->b;
    memcpy(h, "Vgm ", 4);
    const uint32_t eof = s->n - 4u, ver = 0x151u, total = (uint32_t)s->total, data = 0x100u - 0x34u, clock = OPL_CLOCK;
    memcpy(h + 0x04, &eof, 4); memcpy(h + 0x08, &ver, 4); memcpy(h + 0x18, &total, 4);
    if (loop && total) {                                    // 曲の頭（データの始め）へ戻る
        const uint32_t loff = 0x100u - 0x1Cu;
        memcpy(h + 0x1C, &loff, 4); memcpy(h + 0x20, &total, 4);
    }
    memcpy(h + 0x34, &data, 4); memcpy(h + 0x50, &clock, 4);
    uint8_t *out = s->b;
    *out_len = s->n;
    free(s);
    return out;
}
