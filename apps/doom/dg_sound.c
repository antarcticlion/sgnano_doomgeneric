// Doom の音（2026-10-24、外部実行ファイル DOOM.SGX）: doomgeneric の音の部品（DG_sound_module）と音楽の部品（DG_music_module）。
// - 音楽: Freedoom は MIDI（MThd）、市販版は MUS（doomgeneric の mus2mid で MIDI に直す）。
//   【2026-11-13】OPL2 で鳴らす: WAD の GENMIDI で YM3812 の VGM に直し（dg_opl.c）、ファームの音楽の口（music_play_mem、ABI 5.5）で。
//   直せない・鳴らせない（VGM の合成の区画が取れない、古いファーム）ときは今までどおり SGX の API のシーケンサー（seq_*）
// - 効果音: DMX の 8 ビットの PCM（11025Hz など）。SGX の API の PCM（pcm_*、ファームが 8 チャンネルを混ぜて最後のポートの流しで鳴らす）
#include <stddef.h>
#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "memio.h"
#include "mus2mid.h"
#include "deh_str.h"
#include "m_misc.h"                   // M_snprintf・M_StringCopy（宣言が無く、GCC 14 で誤りになった。2026-11-01）
#include "dg_sound.h"
#include "dg_opl.h"
#include "sgx_api.h"
extern const sgx_api_t *sgx;

#define CHANNELS    8
#define MIX_RATE    11025u

// i_sound.c が設定の値として結ぶ（SDL の音の部品が持っていたもの。ここでは使わない）
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

// ---- 効果音 ----
static bool sfx_ok;
static boolean use_prefix;
static boolean S_Init(boolean use_sfx_prefix) {
    use_prefix = use_sfx_prefix;
    sfx_ok = sgx->pcm_open(MIX_RATE, CHANNELS);
    if (!sfx_ok) printf("[doom] no PCM for the sound effects\n");
    return true;
}
static void S_Shutdown(void) { for (unsigned i = 0; i < CHANNELS; i++) sgx->pcm_stop(i); sfx_ok = false; }
static int S_GetSfxLumpNum(sfxinfo_t *sfx) {
    char namebuf[9];
    if (use_prefix) M_snprintf(namebuf, sizeof namebuf, "ds%s", DEH_String(sfx->name));
    else M_StringCopy(namebuf, DEH_String(sfx->name), sizeof namebuf);
    return W_GetNumForName(namebuf);
}
static void S_Update(void) {}
// vol 0〜127、sep 0〜254（128 が真ん中）-> 左右 0〜255
static void vols(int vol, int sep, unsigned *l, unsigned *r) {
    if (vol < 0) vol = 0;
    if (vol > 127) vol = 127;
    if (sep < 0) sep = 0;
    if (sep > 254) sep = 254;
    *l = (unsigned)((254 - sep) * vol / 127);
    *r = (unsigned)(sep * vol / 127);
}
static void S_UpdateSoundParams(int channel, int vol, int sep) {
    if (channel < 0 || channel >= CHANNELS) return;
    unsigned l, r; vols(vol, sep, &l, &r);
    sgx->pcm_set_volume((unsigned)channel, l, r);
}
static int S_StartSound(sfxinfo_t *sfx, int channel, int vol, int sep) {
    if (!sfx_ok || channel < 0 || channel >= CHANNELS) return -1;
    if (sfx->lumpnum < 0) sfx->lumpnum = S_GetSfxLumpNum(sfx);
    const uint8_t *d = W_CacheLumpNum(sfx->lumpnum, PU_STATIC);
    const int lumplen = W_LumpLength(sfx->lumpnum);
    // DMX: 形式 3（2 バイト）・レート（2）・長さ（4）、そのあとに前後 16 バイトずつの詰め物つきの中身
    if (lumplen < 8 || d[0] != 3 || d[1] != 0) return -1;
    const uint32_t rate = (uint32_t)(d[2] | d[3] << 8);
    uint32_t len = (uint32_t)(d[4] | d[5] << 8 | d[6] << 16 | (uint32_t)d[7] << 24);
    if (len > (uint32_t)lumplen - 8u || len <= 48u || !rate) return -1;
    unsigned l, r; vols(vol, sep, &l, &r);
    // ファームの PCM は 8 ビットを 16 ビットに広げて音量（/255）を掛ける。ファームに組んでいたときの大きさ（2026-10-24 にユーザーが OK）と同じ
    return sgx->pcm_play((unsigned)channel, d + 8 + 16, len - 32u, SGX_PCM_U8, rate, l, r) ? channel : -1;
}
static void S_StopSound(int channel) { if (channel >= 0 && channel < CHANNELS) sgx->pcm_stop((unsigned)channel); }
static boolean S_SoundIsPlaying(int channel) { return channel >= 0 && channel < CHANNELS && sgx->pcm_playing((unsigned)channel); }
static void S_CacheSounds(sfxinfo_t *sounds, int num) { (void)sounds; (void)num; }
static snddevice_t sfx_devices[] = { SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32 };
sound_module_t DG_sound_module = {
    sfx_devices, sizeof sfx_devices / sizeof sfx_devices[0],
    S_Init, S_Shutdown, S_GetSfxLumpNum, S_Update, S_UpdateSoundParams, S_StartSound, S_StopSound, S_SoundIsPlaying, S_CacheSounds,
};

// ---- 音楽（SGX の API のシーケンサー。データはファームが写して持つ）----
typedef struct { uint8_t *mid; uint32_t len; } song_t;
static bool music_playing;
static bool music_opl;                  // いま OPL2（ファームの音楽の口）で鳴らしている（false = シーケンサー）
static song_t *cur_song;
static const uint8_t *genmidi;          // WAD の GENMIDI（無ければ OPL2 を使わない）
static uint32_t genmidi_len;
static unsigned music_vol = 127;
static uint8_t *cur_vgm;                // 鳴らしている OPL2 の VGM（SGX_MUSIC_NOCOPY でファームがそのまま読む。止めてから返す）

static bool opl_ok(void) {              // ファームが music_play_mem を持つ（ABI 5.5 以降）
    return genmidi && sgx->size >= offsetof(sgx_api_t, music_play_mem) + sizeof sgx->music_play_mem && sgx->music_play_mem;
}
static void music_halt(void) {
    if (music_opl) sgx->music_stop(); else sgx->seq_stop();
    music_playing = false; music_opl = false;
    free(cur_vgm); cur_vgm = NULL;          // ファームが止まってから
}
static boolean M_Init(void) {
    const int lump = W_CheckNumForName("GENMIDI");
    if (lump >= 0) { genmidi = W_CacheLumpNum(lump, PU_STATIC); genmidi_len = (uint32_t)W_LumpLength((unsigned)lump); }
    printf("[doom] music: %s\n", opl_ok() ? "OPL2 (GENMIDI -> YM3812 VGM)" : "sequencer");
    return true;
}
static void M_Shutdown(void) { music_halt(); }
// Doom の音量は 0〜127（メニューの 0〜15 の 8 倍、既定 8 = 64）。既定で最大（127）になるよう 2 倍（2026-10-24、小さいとユーザー報告）
static void M_SetMusicVolume(int v) {
    music_vol = v * 2 > 127 ? 127u : (unsigned)(v < 0 ? 0 : v * 2);
    sgx->snd_master(music_vol);
    if (music_opl) sgx->music_volume(music_vol);
}
static void M_Pause(void) { if (music_opl) sgx->music_pause(true); else sgx->seq_pause(true); }
static void M_Resume(void) { if (music_opl) sgx->music_pause(false); else sgx->seq_pause(false); }
static void *M_RegisterSong(void *data, int len) {
    song_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    const uint8_t *d = data;
    if (len >= 4 && d[0] == 'M' && d[1] == 'T' && d[2] == 'h' && d[3] == 'd') {   // もう MIDI（Freedoom）
        s->mid = malloc((size_t)len);
        if (!s->mid) { free(s); return NULL; }
        memcpy(s->mid, d, (size_t)len); s->len = (uint32_t)len;
        return s;
    }
    MEMFILE *in = mem_fopen_read(data, (size_t)len), *out = mem_fopen_write();   // MUS（市販版）-> MIDI
    void *buf = NULL; size_t blen = 0;
    const boolean bad = mus2mid(in, out);
    if (!bad) mem_get_buf(out, &buf, &blen);
    s->mid = !bad && blen ? malloc(blen) : NULL;
    if (s->mid) { memcpy(s->mid, buf, blen); s->len = (uint32_t)blen; }
    mem_fclose(in); mem_fclose(out);
    if (!s->mid) { free(s); return NULL; }
    return s;
}
static void M_UnRegisterSong(void *h) {
    song_t *s = h;
    if (!s) return;
    if (cur_song == s) { music_halt(); cur_song = NULL; }
    free(s->mid); free(s);                  // シーケンサーも音楽の口も中身を自分の置き場に写して持つ
}
static void M_PlaySong(void *h, boolean looping) {
    if (!h) return;
    song_t *s = h;
    const char *why = NULL;
    music_halt();
    cur_song = s;
    if (opl_ok()) {                         // OPL2: 鳴らすたびに VGM へ直す（ファームが写して持つので、すぐ返す。PSRAM はゾーンでほとんど埋まっている）
        uint32_t vlen = 0;
        uint8_t *vgm = dg_opl_vgm(s->mid, s->len, genmidi, genmidi_len, looping, &vlen);
        // 写さずに鳴らす（PSRAM はゾーンでほとんど埋まっていて、同じ VGM を 2 つ持てなかった。2026-11-13）
        const bool ok = vgm && sgx->music_play_mem(vgm, vlen, SGX_MUSIC_NOCOPY | (looping ? SGX_MUSIC_LOOP : 0u)) == 0;
        if (ok) cur_vgm = vgm; else free(vgm);
        if (ok) {
            sgx->music_volume(music_vol);
            music_playing = music_opl = true;
            return;
        }
        printf("[doom] music: OPL2 failed (%s); using the sequencer\n", vgm ? "music_play_mem" : "conversion");
    }
    music_playing = sgx->seq_play(s->mid, s->len, looping, &why);
    if (!music_playing) printf("[doom] music: %s\n", why ? why : "?");
}
static void M_StopSong(void) { music_halt(); }
static boolean M_IsPlaying(void) { return music_playing; }
static void M_Poll(void) {}
static snddevice_t music_devices[] = { SNDDEVICE_SB, SNDDEVICE_GENMIDI, SNDDEVICE_GUS, SNDDEVICE_AWE32, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_WAVEBLASTER };
music_module_t DG_music_module = {
    music_devices, sizeof music_devices / sizeof music_devices[0],
    M_Init, M_Shutdown, M_SetMusicVolume, M_Pause, M_Resume, M_RegisterSong, M_UnRegisterSong, M_PlaySong, M_StopSong, M_IsPlaying, M_Poll,
};

