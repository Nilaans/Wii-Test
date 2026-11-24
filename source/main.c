/*
 * MathCanvas Wii - a tiny scratchpad for solving math problems with the Wiimote.
 * Native Wii homebrew (devkitPPC + libogc). Point at the TV, hold A to draw.
 *
 * Canvas is a 640x480 8-bit paletted bitmap; the toolbar covers the top 40 px.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <fat.h>
#include <sys/stat.h>

#define W 640
#define H 480
#define TB_H 40
#define GRID 32
#define SAVE_DIR "/apps/mathcanvas"
#define SAVE_FILE "/apps/mathcanvas/canvas.bin"

enum { C_WHITE, C_BLACK, C_RED, C_BLUE, C_GREEN, C_ORANGE, C_PURPLE,
       C_TBBG, C_BTN, C_SEL, C_OUTLINE, C_GRID, C_CURSOR, NCOL };

static const u8 rgbtab[NCOL][3] = {
    {255, 255, 255}, {20, 20, 20},   {220, 40, 40},  {40, 90, 220},
    {30, 160, 70},   {240, 140, 20}, {140, 60, 190}, {225, 230, 238},
    {250, 250, 252}, {150, 190, 255},{110, 120, 135},{215, 225, 238},
    {255, 0, 200}};

enum { T_PEN, T_ERASE, T_LINE, T_RECT, T_CIRCLE, NTOOLS };
enum { B_GRID = 5, B_UNDO, B_CLEAR, B_SAVE, B_LOAD, NBTN };

typedef void (*PlotFn)(int, int, int);

static GXRModeObj *rmode;
static u32 *xfb[2];
static int fb = 0, yoff = 0;
static u8 *canvas, *undobuf;
static u32 pairc[NCOL];
static u8 Yv[NCOL], Cbv[NCOL], Crv[NCOL];

static int tool = T_PEN, penColor = C_BLACK, thick = 3, gridOn = 0;
static int toastColor = -1, toastFrames = 0;

/* ---------- colour setup ---------- */
static int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static void init_palette(void) {
    for (int i = 0; i < NCOL; i++) {
        int r = rgbtab[i][0], g = rgbtab[i][1], b = rgbtab[i][2];
        Yv[i] = clamp255(16 + ((66 * r + 129 * g + 25 * b) >> 8));
        Cbv[i] = clamp255(128 + ((-38 * r - 74 * g + 112 * b) >> 8));
        Crv[i] = clamp255(128 + ((112 * r - 94 * g - 18 * b) >> 8));
        pairc[i] = ((u32)Yv[i] << 24) | ((u32)Cbv[i] << 16) | ((u32)Yv[i] << 8) | Crv[i];
    }
}

/* ---------- plotting targets ---------- */
static void plot_canvas(int x, int y, int c) {
    if (x < 0 || x >= W || y < TB_H || y >= H) return;
    canvas[y * W + x] = (u8)c;
}

/* XFB writes colour a pixel pair (YUY2), so overlays are 2px wide horizontally */
static void plot_xfb(int x, int y, int c) {
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    xfb[fb][(y + yoff) * (W / 2) + (x >> 1)] = pairc[c];
}

/* ---------- rasterisers ---------- */
static void stamp(PlotFn p, int cx, int cy, int c, int t) {
    int r = t / 2;
    if (r == 0) { p(cx, cy, c); return; }
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r + r) p(cx + dx, cy + dy, c);
}

static void line(PlotFn p, int x0, int y0, int x1, int y1, int c, int t) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        stamp(p, x0, y0, c, t);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void rectd(PlotFn p, int x0, int y0, int x1, int y1, int c, int t) {
    line(p, x0, y0, x1, y0, c, t);
    line(p, x1, y0, x1, y1, c, t);
    line(p, x1, y1, x0, y1, c, t);
    line(p, x0, y1, x0, y0, c, t);
}

static void circled(PlotFn p, int cx, int cy, int r, int c, int t) {
    if (r <= 0) { stamp(p, cx, cy, c, t); return; }
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        stamp(p, cx + x, cy + y, c, t); stamp(p, cx - x, cy + y, c, t);
        stamp(p, cx + x, cy - y, c, t); stamp(p, cx - x, cy - y, c, t);
        stamp(p, cx + y, cy + x, c, t); stamp(p, cx - y, cy + x, c, t);
        stamp(p, cx + y, cy - x, c, t); stamp(p, cx - y, cy - x, c, t);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

static void fillrect(int x, int y, int w, int h, int c) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i += 2) plot_xfb(i, j, c);
}

/* ---------- canvas -> XFB ---------- */
static void blit(void) {
    u32 *dst = xfb[fb] + yoff * (W / 2);
    for (int y = 0; y < H; y++) {
        const u8 *row = canvas + y * W;
        int gy = gridOn && (y % GRID == 0);
        for (int x = 0; x < W; x += 2) {
            int a = row[x], b = row[x + 1];
            if (gridOn) {
                if (a == C_WHITE && (gy || (x % GRID) == 0)) a = C_GRID;
                if (b == C_WHITE && (gy || ((x + 1) % GRID) == 0)) b = C_GRID;
            }
            if (a == b) dst[x >> 1] = pairc[a];
            else {
                u32 cb = (Cbv[a] + Cbv[b]) >> 1, cr = (Crv[a] + Crv[b]) >> 1;
                dst[x >> 1] = ((u32)Yv[a] << 24) | (cb << 16) | ((u32)Yv[b] << 8) | cr;
            }
        }
        dst += W / 2;
    }
}

/* ---------- toolbar ---------- */
static int btnX(int i) { return 4 + i * 40; }
static int colX(int i) { return 410 + i * 26; }
#define THK_MINUS_X 570
#define THK_PLUS_X 602

static int eraser_size(void) { return thick * 3 + 6; }

static void draw_icon(int i, int bx) {
    int cy = 20;
    switch (i) {
    case T_PEN:
        line(plot_xfb, bx + 9, 28, bx + 26, 10, C_BLACK, 3);
        break;
    case T_ERASE:
        rectd(plot_xfb, bx + 8, 14, bx + 28, 26, C_OUTLINE, 2);
        fillrect(bx + 8, 14, 8, 12, C_RED);
        break;
    case T_LINE:
        line(plot_xfb, bx + 8, 28, bx + 28, 12, C_BLACK, 2);
        break;
    case T_RECT:
        rectd(plot_xfb, bx + 8, 12, bx + 28, 28, C_BLACK, 2);
        break;
    case T_CIRCLE:
        circled(plot_xfb, bx + 18, cy, 10, C_BLACK, 2);
        break;
    case B_GRID:
        line(plot_xfb, bx + 13, 10, bx + 13, 30, C_BLACK, 1);
        line(plot_xfb, bx + 23, 10, bx + 23, 30, C_BLACK, 1);
        line(plot_xfb, bx + 8, 15, bx + 28, 15, C_BLACK, 1);
        line(plot_xfb, bx + 8, 25, bx + 28, 25, C_BLACK, 1);
        break;
    case B_UNDO:
        line(plot_xfb, bx + 10, cy, bx + 28, cy, C_BLACK, 2);
        line(plot_xfb, bx + 10, cy, bx + 17, cy - 7, C_BLACK, 2);
        line(plot_xfb, bx + 10, cy, bx + 17, cy + 7, C_BLACK, 2);
        break;
    case B_CLEAR:
        line(plot_xfb, bx + 10, 11, bx + 26, 29, C_RED, 3);
        line(plot_xfb, bx + 26, 11, bx + 10, 29, C_RED, 3);
        break;
    case B_SAVE:
        line(plot_xfb, bx + 18, 8, bx + 18, 22, C_BLACK, 2);
        line(plot_xfb, bx + 18, 24, bx + 11, 17, C_BLACK, 2);
        line(plot_xfb, bx + 18, 24, bx + 25, 17, C_BLACK, 2);
        line(plot_xfb, bx + 9, 30, bx + 27, 30, C_BLACK, 2);
        break;
    case B_LOAD:
        line(plot_xfb, bx + 18, 24, bx + 18, 10, C_BLACK, 2);
        line(plot_xfb, bx + 18, 8, bx + 11, 15, C_BLACK, 2);
        line(plot_xfb, bx + 18, 8, bx + 25, 15, C_BLACK, 2);
        line(plot_xfb, bx + 9, 30, bx + 27, 30, C_BLACK, 2);
        break;
    }
}

static void draw_toolbar(void) {
    fillrect(0, 0, W, TB_H, C_TBBG);
    for (int i = 0; i < NBTN; i++) {
        int bx = btnX(i);
        int active = (i < NTOOLS && i == tool) || (i == B_GRID && gridOn);
        fillrect(bx, 4, 36, 32, active ? C_SEL : C_BTN);
        rectd(plot_xfb, bx, 4, bx + 35, 35, C_OUTLINE, 1);
        draw_icon(i, bx);
    }
    for (int i = 0; i < 6; i++) {
        int x = colX(i), c = C_BLACK + i;
        fillrect(x, 8, 22, 24, c);
        rectd(plot_xfb, x, 8, x + 21, 31, c == penColor ? C_CURSOR : C_OUTLINE, c == penColor ? 3 : 1);
    }
    fillrect(THK_MINUS_X, 6, 28, 28, C_BTN);
    rectd(plot_xfb, THK_MINUS_X, 6, THK_MINUS_X + 27, 33, C_OUTLINE, 1);
    line(plot_xfb, THK_MINUS_X + 7, 20, THK_MINUS_X + 20, 20, C_BLACK, 2);
    fillrect(THK_PLUS_X, 6, 28, 28, C_BTN);
    rectd(plot_xfb, THK_PLUS_X, 6, THK_PLUS_X + 27, 33, C_OUTLINE, 1);
    line(plot_xfb, THK_PLUS_X + 7, 20, THK_PLUS_X + 20, 20, C_BLACK, 2);
    line(plot_xfb, THK_PLUS_X + 14, 12, THK_PLUS_X + 14, 28, C_BLACK, 2);
}

/* ---------- undo / file ---------- */
static void push_undo(void) { memcpy(undobuf, canvas, W * H); }
static void do_undo(void) { u8 *t = canvas; canvas = undobuf; undobuf = t; }

static void toast(int ok) { toastColor = ok ? C_GREEN : C_RED; toastFrames = 60; }

static void save_canvas(void) {
    mkdir("/apps", 0777);
    mkdir(SAVE_DIR, 0777);
    FILE *f = fopen(SAVE_FILE, "wb");
    if (!f) { toast(0); return; }
    size_t n = fwrite(canvas, 1, W * H, f);
    fclose(f);
    toast(n == (size_t)(W * H));
}

static void load_canvas(void) {
    FILE *f = fopen(SAVE_FILE, "rb");
    if (!f) { toast(0); return; }
    push_undo();
    size_t n = fread(canvas, 1, W * H, f);
    fclose(f);
    if (n != (size_t)(W * H)) { do_undo(); toast(0); return; }
    toast(1);
}

/* ---------- toolbar clicks ---------- */
static void toolbar_click(int x, int y) {
    for (int i = 0; i < NBTN; i++) {
        int bx = btnX(i);
        if (x >= bx && x < bx + 36 && y >= 4 && y < 36) {
            if (i < NTOOLS) tool = i;
            else if (i == B_GRID) gridOn = !gridOn;
            else if (i == B_UNDO) do_undo();
            else if (i == B_CLEAR) { push_undo(); memset(canvas, 0, W * H); }
            else if (i == B_SAVE) save_canvas();
            else if (i == B_LOAD) load_canvas();
            return;
        }
    }
    for (int i = 0; i < 6; i++)
        if (x >= colX(i) && x < colX(i) + 22 && y >= 8 && y < 32) { penColor = C_BLACK + i; if (tool == T_ERASE) tool = T_PEN; return; }
    if (y >= 6 && y < 34) {
        if (x >= THK_MINUS_X && x < THK_MINUS_X + 28 && thick > 1) thick--;
        if (x >= THK_PLUS_X && x < THK_PLUS_X + 28 && thick < 16) thick++;
    }
}

static int snap(int v) { return gridOn ? ((v + GRID / 2) / GRID) * GRID : v; }

/* ---------- main ---------- */
int main(void) {
    VIDEO_Init();
    WPAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
    WPAD_SetVRes(WPAD_CHAN_0, W, H);

    rmode = VIDEO_GetPreferredMode(NULL);
    xfb[0] = (u32 *)MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    xfb[1] = (u32 *)MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    VIDEO_Configure(rmode);
    VIDEO_ClearFrameBuffer(rmode, xfb[0], COLOR_BLACK);
    VIDEO_ClearFrameBuffer(rmode, xfb[1], COLOR_BLACK);
    VIDEO_SetNextFramebuffer(xfb[0]);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
    yoff = rmode->xfbHeight > H ? (rmode->xfbHeight - H) / 2 : 0;

    init_palette();
    canvas = (u8 *)calloc(W * H, 1);
    undobuf = (u8 *)calloc(W * H, 1);
    if (!canvas || !undobuf) return 1;
    fatInitDefault();

    int px = W / 2, py = H / 2;
    int drawing = 0, sx0 = 0, sy0 = 0, lx = 0, ly = 0;

    for (;;) {
        WPAD_ScanPads();
        u32 down = WPAD_ButtonsDown(0), held = WPAD_ButtonsHeld(0), up = WPAD_ButtonsUp(0);
        if (down & WPAD_BUTTON_HOME) exit(0);

        ir_t ir;
        WPAD_IR(0, &ir);
        if (ir.valid) {
            px += ((int)ir.x - px) / 2;
            py += ((int)ir.y - py) / 2;
        }
        if (px < 0) px = 0;
        if (px >= W) px = W - 1;
        if (py < 0) py = 0;
        if (py >= H) py = H - 1;
        int sx = snap(px), sy = snap(py);
        if (sx >= W) sx = W - 1;
        if (sy >= H) sy = H - 1;

        /* buttons */
        if (down & WPAD_BUTTON_UP) { if (thick < 16) thick++; }
        if (down & WPAD_BUTTON_DOWN) { if (thick > 1) thick--; }
        if (down & WPAD_BUTTON_RIGHT) { penColor = penColor >= C_PURPLE ? C_BLACK : penColor + 1; if (tool == T_ERASE) tool = T_PEN; }
        if (down & WPAD_BUTTON_LEFT) { penColor = penColor <= C_BLACK ? C_PURPLE : penColor - 1; if (tool == T_ERASE) tool = T_PEN; }
        if (down & WPAD_BUTTON_PLUS) tool = (tool + 1) % NTOOLS;
        if (down & WPAD_BUTTON_MINUS) tool = (tool + NTOOLS - 1) % NTOOLS;
        if (down & WPAD_BUTTON_1) gridOn = !gridOn;
        if (down & WPAD_BUTTON_2) save_canvas();
        if (down & WPAD_BUTTON_B) { drawing = 0; do_undo(); }

        int col = tool == T_ERASE ? C_WHITE : penColor;
        int size = tool == T_ERASE ? eraser_size() : thick;

        if (down & WPAD_BUTTON_A) {
            if (py < TB_H) toolbar_click(px, py);
            else {
                push_undo();
                drawing = 1;
                sx0 = lx = sx; sy0 = ly = sy;
                if (tool == T_PEN || tool == T_ERASE) stamp(plot_canvas, sx, sy, col, size);
            }
        }
        if (drawing && (held & WPAD_BUTTON_A) && (tool == T_PEN || tool == T_ERASE)) {
            line(plot_canvas, lx, ly, sx, sy, col, size);
            lx = sx; ly = sy;
        }
        if (drawing && (up & WPAD_BUTTON_A)) {
            if (tool == T_LINE) line(plot_canvas, sx0, sy0, sx, sy, col, size);
            else if (tool == T_RECT) rectd(plot_canvas, sx0, sy0, sx, sy, col, size);
            else if (tool == T_CIRCLE) {
                int dx = sx - sx0, dy = sy - sy0;
                circled(plot_canvas, sx0, sy0, (int)sqrt((double)(dx * dx + dy * dy)), col, size);
            }
            drawing = 0;
        }

        /* render */
        blit();
        if (drawing) {
            if (tool == T_LINE) line(plot_xfb, sx0, sy0, sx, sy, col, size);
            else if (tool == T_RECT) rectd(plot_xfb, sx0, sy0, sx, sy, col, size);
            else if (tool == T_CIRCLE) {
                int dx = sx - sx0, dy = sy - sy0;
                circled(plot_xfb, sx0, sy0, (int)sqrt((double)(dx * dx + dy * dy)), col, size);
            }
        }
        draw_toolbar();
        if (toastFrames > 0) { fillrect(W - 20, H - 20, 14, 14, toastColor); toastFrames--; }

        /* cursor: crosshair plus brush-size ring */
        line(plot_xfb, px - 8, py, px + 8, py, C_CURSOR, 1);
        line(plot_xfb, px, py - 8, px, py + 8, C_CURSOR, 1);
        if (py >= TB_H) circled(plot_xfb, sx, sy, size / 2 + 1, C_CURSOR, 1);

        VIDEO_SetNextFramebuffer(xfb[fb]);
        VIDEO_Flush();
        VIDEO_WaitVSync();
        fb ^= 1;
    }
    return 0;
}
