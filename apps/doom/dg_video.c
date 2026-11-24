// Doom の映像（2026-10-24、外部実行ファイル DOOM.SGX）: doomgeneric の i_video.c の代わり。画面モード 7（320x200x8）。
// Doom は後ろの面（SGX の API の bm_back）に描き、描き終えたら bm_present（ファームがフレームの境目に表示の面へ写す。物のちらつきを無くす）。
// 後ろの面が取れなければ表示の面（bm_vram）に直に描く。パレットは 256 色をビットマップのパレットへ
#include "doomtype.h"
#include "i_video.h"
#include "tables.h"
#include "v_video.h"
#include "sgx_api.h"

extern const sgx_api_t *sgx;
byte *I_VideoBuffer = NULL;
boolean screensaver_mode = false;
boolean screenvisible = true;
float mouse_acceleration = 2.0f;
int mouse_threshold = 10;
int usegamma = 0;
int usemouse = 0;
uint32_t dg_frames;                     // 描き終えたフレーム（速さを測る）
extern boolean setsizeneeded;           // r_main.c

void I_InitGraphics(void) {
    uint8_t *b = sgx->bm_back();
    I_VideoBuffer = b ? b : sgx->bm_vram();   // 320 x 200（Doom の SCREENWIDTH x SCREENHEIGHT と同じ）
    if (I_VideoBuffer) memset(I_VideoBuffer, 0, SGX_BM_W * SGX_BM_H);
}
// ファイルを書き換える API のあと（dg_sys.c）: ファームは後ろの面を一時返して取り直すので、番地が変わることがある。
// 行の先頭の表（ylookup）は R_ExecuteSetViewSize が作り直し、V_ の描き先は V_RestoreBuffer
void dg_video_back_moved(void) {
    if (!I_VideoBuffer) return;
    uint8_t *b = sgx->bm_back();
    if (!b) b = sgx->bm_vram();
    if (b == I_VideoBuffer) return;
    I_VideoBuffer = b;
    V_RestoreBuffer();
    setsizeneeded = true;
}
void I_ShutdownGraphics(void) {}
void I_StartFrame(void) {}
void I_GetEvent(void);
void I_StartTic(void) { I_GetEvent(); }
void I_UpdateNoBlit(void) {}
void I_FinishUpdate(void) {
    static uint32_t last_ms, last_frames;
    dg_frames++;
    if (I_VideoBuffer != sgx->bm_vram()) sgx->bm_present();   // 写すまで次を描かない（描きかけが写らないように）
    const uint32_t now = sgx->ms();
    if (!last_ms) { last_ms = now; last_frames = dg_frames; return; }
    if (now - last_ms >= 5000u) {
        const uint32_t df = dg_frames - last_frames;
        const uint32_t dt = now - last_ms;
        const uint32_t fps_x10 = dt ? (df * 10000u) / dt : 0u;
        printf("[doom] fps=%lu.%lu (%lu fr)\n",
               (unsigned long)(fps_x10 / 10u), (unsigned long)(fps_x10 % 10u), (unsigned long)df);
        last_ms = now;
        last_frames = dg_frames;
    }
}
void I_ReadScreen(byte *scr) { memcpy(scr, I_VideoBuffer, SGX_BM_W * SGX_BM_H); }
void I_SetPalette(byte *palette) {
    for (int i = 0; i < 256; i++) {
        sgx->bm_palette(i, sgx->rgb(gammatable[usegamma][palette[0]], gammatable[usegamma][palette[1]], gammatable[usegamma][palette[2]]));
        palette += 3;
    }
}
int I_GetPaletteIndex(int r, int g, int b) {
    int best = 0, best_d = 0x7FFFFFFF;
    for (int i = 0; i < 256; i++) {
        const uint16_t c = sgx->bm_palette_get(i);
        const int cr = (c & 31) << 3, cg = ((c >> 5) & 31) << 3, cb = ((c >> 10) & 31) << 3;
        const int d = (r - cr) * (r - cr) + (g - cg) * (g - cg) + (b - cb) * (b - cb);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}
void I_BeginRead(void) {}
void I_EndRead(void) {}
void I_SetWindowTitle(char *title) { (void)title; }
void I_GraphicsCheckCommandLine(void) {}
void I_SetGrabMouseCallback(grabmouse_callback_t func) { (void)func; }
void I_EnableLoadingDisk(void) {}
void I_BindVideoVariables(void) {}
void I_DisplayFPSDots(boolean dots_on) { (void)dots_on; }
void I_CheckIsScreensaver(void) {}
