/* On-screen MOD overlay for the SC Live 4 port.
 *
 * rbp owns the whole rekordbox UI, so this draws after the rotated frame
 * and steals taps before they reach rbp. A MOD tab at the top center of
 * the upright UI opens a panel. Each row names the setting on the left,
 * then the value: MODE (SINGLE, CONTINUE, REPEAT, ALL REPEAT), JOG
 * (− percent +), WAVE (BLUE, RGB, 3BAND), BEAT (OFF, BARS, DRIFT: the beat
 * meter right of the tab, see beat_paint), QUANT (ON, OFF), TRACK
 * (TAG, TAGS, FIND), SCREEN (− backlight percent +), LEDS (− panel LED
 * percent +, applied by knobshim), EJECT, STATS (read-only: CPU load percent
 * and display frames per second, counted at each FBIOPAN), LINK (ON, OFF:
 * LINK CUE, which lets Track Preview play into the headphones), TCUE (ON, OFF:
 * Touch Cue on the deck overview waveforms while a deck plays), SKIP (SEARCH, 16 BEATS:
 * what the SEARCH < > buttons do, applied by knobshim), INFO (OFF, ON: OFF leaves the two deck info boxes
 * to rbp), ROWS (SRC, KEY, CUE, LOOP: which rows of those boxes are shown, applied by knobshim), COUNT (BARS,
 * BEATS: the unit of the CUE row), and POWER.
 * POWER always stays the last row.
 * The MODE button cycles those four play modes. EJECT asks usb-watch to
 * release the stick; the button then reads PULL until the stick is removed.
 * Play mode is UiSetUtilAutoPlayMode, the same call the RX3 utility menu
 * makes. The jog number is a percent of the unscaled wheel. 100 is the
 * original calibration; the panel starts at 40.
 * The − and + buttons write jog_gain_milli; knobshim applies it on the
 * next wheel sample, so it changes while the player is running.
 *
 * DFB_ROTATE=left maps upright visual (vx, vy) in 1280x800 onto the physical
 * 800x1280 buffer as px = vy, py = 1279 - vx. fbshim's touch transform stores
 * that as lx = py, ly = px, so visual coords are vx = 1279 - lx, vy = ly.
 */
#define _GNU_SOURCE
#include "overlay_playmode.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef SYS_mmap2
# ifdef __NR_mmap2
#  define SYS_mmap2 __NR_mmap2
# else
/* ARM EABI mmap2. The Mac editor has no Linux syscall table; the device
 * build defines SYS_mmap2 from its own headers and never uses this. */
#  define SYS_mmap2 192
# endif
#endif

#ifdef __APPLE__
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#pragma clang diagnostic ignored "-Wint-to-pointer-cast"
#endif

/* rbp-audio, not PIE. UiSetUtilAutoPlayMode / UiGetUtilAutoPlayMode. */
#define UI_SET_AUTOPLAY ((void (*)(int))0x000fe9bc)
#define UI_GET_AUTOPLAY ((int (*)(void))0x000fe998)
/* CmnFunc_CmnInfo_Set/Get/CheckDevSetWaveFormColor. Color 1 BLUE, 3 RGB, 4 3BAND. */
#define UI_SET_WAVE ((void (*)(int, int, int, int))0x00185c78)
#define UI_GET_WAVE ((int (*)(int, int))0x00185b14)
#define UI_CHK_WAVE ((int (*)(int, int))0x00185ba4)
/* UiSetQuantizeOnOff(deck, on) / UiGetPlayQuantizeOn(deck). Deck is 0 or 1.
 * This is the QUANT button, not the quantize-beat-value setting. */
#define UI_SET_QUANTIZE ((void (*)(int, int))0x000fe184)
#define UI_GET_QUANTIZE ((int (*)(int))0x000fd36c)
/* MixerEngine::setPreviewChHeadphoneCue(this, bool) / getPreviewChHeadphoneCue.
 * The singleton pointer lives at 0x011493c0. This is the RX3 LINK CUE button
 * (key 0x4408), which feeds Track Preview into the headphone bus. */
#define ME_SINGLETON        0x011493c0UL
#define ME_SET_PREVIEW_CUE  ((void (*)(void *, int))0x0005762c)
#define ME_GET_PREVIEW_CUE  ((int (*)(void *))0x0005764c)
#define LINK_RECHECK_MS     1000
/* KeyManager::sendKey. Same path knobshim uses for the hardware buttons. */
#define UI_OBJ_MGR_GLOBAL 0x2685f2cUL
#define KEY_MANAGER_OFF   100
#define SENDKEY_WORD      2
#define OP_PRESS          0
#define OP_RELEASE        2
#define CH_GLOBAL         1
#define K_TAGLIST         0x0203
#define K_SEARCH          0x0205
#define K_TAGTRACK        0x420e
#define KEY_TAP_MS        80
#define CMN_BASE ((volatile unsigned char *)0x03253564)
/* SetPlayInfo passes this per-deck pair (device, kind) into WaveDispColor. */
#define DECK_WAVE ((volatile unsigned char *)0x0216b3c0)
#define DECK_WAVE_STRIDE 220

#define SETTINGS_PATH "/root/settings/XdjSettings.dat"
#define SETTINGS_OFF  0x57c
#define LOG_PATH      "/tmp/overlay.log"
#define USB_NAME1     "/tmp/usb-name-1"
#define USB_NAME2     "/tmp/usb-name-2"
#define USB_PULL1     "/tmp/usb-pull-1"
#define USB_PULL2     "/tmp/usb-pull-2"
#define POWER_REQ     "/tmp/rb-poweroff"
#define BL_DIR        "/sys/class/backlight/mipi-backlight/"

/* Upright 1280x800 layout, top center. */
#define TAB_W 80
#define TAB_H 32
#define TAB_X ((1280 - TAB_W) / 2)
#define TAB_Y 8

/* Name on the left, value on the right. Seventeen rows under the MOD tab:
 * PAN_H = 2 * PAD + rows * ROW_H + (rows - 1) * ROW_GAP. The panel shows PAN_VIS_H of that and scrolls
 * (drag inside it) when PAN_H is taller. */
#define PAN_W 348
#define PAN_H 760
#define PAN_VIS_H (PAN_H < 800 - PAN_Y - 4 ? PAN_H : 800 - PAN_Y - 4)
#define SCROLL_MAX (PAN_H - PAN_VIS_H)
#define PAN_X ((1280 - PAN_W) / 2)
#define PAN_Y 48

#define PAD 10
#define LAB_W 80   /* fits SCREEN, the longest label */
#define ROW_H 36
#define ROW_GAP 8
#define LAB_X (PAN_X + PAD)
#define VAL_X (LAB_X + LAB_W)
#define VAL_W (PAN_W - PAD - (VAL_X - PAN_X))
#define ROW_Y(i) (PAN_Y + PAD + (i) * (ROW_H + ROW_GAP) - ov_scroll)

#define MODE_Y ROW_Y(0)
#define JOG_Y  ROW_Y(1)
#define STEP_W 44
#define STEP_DN_X VAL_X
#define STEP_UP_X (VAL_X + VAL_W - STEP_W)

#define WAVE_Y ROW_Y(2)
#define WAVE_GAP 6
#define WAVE_X VAL_X
#define WAVE_W VAL_W
#define WAVE_BTN ((WAVE_W - 2 * WAVE_GAP) / 3)
#define BEAT_Y ROW_Y(3)
#define QUANT_Y ROW_Y(4)
#define QUANT_GAP 8
#define QUANT_HALF ((VAL_W - QUANT_GAP) / 2)
#define QUANT_ON_X VAL_X
#define QUANT_OFF_X (VAL_X + QUANT_HALF + QUANT_GAP)
#define TRACK_Y ROW_Y(5)
#define SCR_Y  ROW_Y(6)
#define LED_Y  ROW_Y(7)
#define USB_Y  ROW_Y(8)
#define USB_GAP 8
#define USB_HALF ((VAL_W - USB_GAP) / 2)
#define USB_L_X VAL_X
#define USB_R_X (VAL_X + USB_HALF + USB_GAP)
#define STATS_Y ROW_Y(9)
#define LINK_Y ROW_Y(10)
#define TCUE_Y ROW_Y(11)
#define SKIP_Y ROW_Y(12)
#define INFO_Y ROW_Y(13)
#define ROWS_Y ROW_Y(14)
#define INFO_GAP 6
#define INFO_BTN ((VAL_W - 3 * INFO_GAP) / 4)
#define CNT_Y  ROW_Y(15)
/* POWER is always the last row; add new rows above it. */
#define PWR_Y  ROW_Y(16)

#define COL_TAB    0xff1c2128u
#define COL_PANEL  0xff121418u
#define COL_BTN    0xff2a3038u
#define COL_ON     0xff1b7a3au
#define COL_TEXT   0xfff2f2f2u
#define COL_TITLE  0xffb7bdc6u
#define COL_EDGE   0xff3a424cu

#define SCALE 2

static int ov_scroll;              /* panel scroll, 0..SCROLL_MAX px */
static int clip_y0, clip_y1 = 800;  /* drawing is clipped to these rows while the panel paints */

static struct rb_overlay_shm *ov_shm;
static volatile int ov_mode;          /* 0 SINGLE, 1 CONTINUE, 2 REPEAT, 3 ALL REPEAT */
static volatile int ov_mode_known;
static volatile int ov_apply_seq;
static volatile int ov_apply_mode;
static int          ov_seen_seq;
static volatile int ov_wave_seq;
static volatile int ov_wave_color;
static int          ov_wave_seen;
static volatile int ov_quant_seq;
static volatile int ov_quant_on;
static int          ov_quant_seen;
static int          ov_quant_known;
static int          ov_quant[2];
static volatile int ov_link_seq;      /* bumped by a tap: apply now */
static int          ov_link_seen;
static int          ov_link = 1;      /* 1 ON, 0 OFF, for drawing */
static unsigned long long ov_link_ms;
static int          ov_tcue = 1;      /* 1 ON, 0 OFF, for drawing */
static int          ov_skip;          /* 1 = 16 BEATS, 0 = SEARCH, for drawing */
static unsigned     ov_info = INFO_DEF;   /* shm info_cfg with the default filled in, for drawing */
static volatile int ov_track_seq;
static volatile int ov_track_act;     /* 1 TAG, 2 TAGS, 3 FIND */
static int          ov_track_seen;
static volatile int ov_key_up;
static unsigned long long ov_key_up_ms;
static int          ov_wave_cur;
static int          ov_grab;          /* finger went down on the overlay */

static void *fb_map;
static unsigned fb_map_len;
static unsigned fb_pitch;
static int fb_map_fd = -1;

/* 5x7 glyphs, bit 4 is the leftmost pixel. */
static const unsigned char GLYPH_A[7] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11};
static const unsigned char GLYPH_B[7] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E};
static const unsigned char GLYPH_C[7] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E};
static const unsigned char GLYPH_D[7] = {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E};
static const unsigned char GLYPH_E[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F};
static const unsigned char GLYPH_F[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10};
static const unsigned char GLYPH_G[7] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F};
static const unsigned char GLYPH_H[7] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11};
static const unsigned char GLYPH_I[7] = {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E};
static const unsigned char GLYPH_J[7] = {0x01,0x01,0x01,0x01,0x11,0x11,0x0E};
static const unsigned char GLYPH_K[7] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11};
static const unsigned char GLYPH_L[7] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F};
static const unsigned char GLYPH_M[7] = {0x11,0x1B,0x15,0x11,0x11,0x11,0x11};
static const unsigned char GLYPH_N[7] = {0x11,0x19,0x15,0x13,0x11,0x11,0x11};
static const unsigned char GLYPH_O[7] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E};
static const unsigned char GLYPH_P[7] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10};
static const unsigned char GLYPH_Q[7] = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D};
static const unsigned char GLYPH_R[7] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11};
static const unsigned char GLYPH_S[7] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E};
static const unsigned char GLYPH_T[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04};
static const unsigned char GLYPH_U[7] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E};
static const unsigned char GLYPH_V[7] = {0x11,0x11,0x11,0x11,0x11,0x0A,0x04};
static const unsigned char GLYPH_W[7] = {0x11,0x11,0x11,0x15,0x15,0x1B,0x11};
static const unsigned char GLYPH_X[7] = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11};
static const unsigned char GLYPH_Y[7] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04};
static const unsigned char GLYPH_Z[7] = {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F};
static const unsigned char GLYPH_DOT[7] = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C};
static const unsigned char GLYPH_PCT[7] = {0x18,0x19,0x02,0x04,0x08,0x13,0x03};
static const unsigned char GLYPH_PLUS[7] = {0x00,0x04,0x04,0x1F,0x04,0x04,0x00};
static const unsigned char GLYPH_MINUS[7] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00};
static const unsigned char GLYPH_EQ[7] = {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00};

/* bit 4 is the leftmost pixel */
static const unsigned char GLYPH_DIG[10][7] = {
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F},
    {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    {0x1F,0x10,0x10,0x1E,0x01,0x11,0x0E},
    {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E},
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E},
};

static const unsigned char *glyph(char c)
{
    if (c >= '0' && c <= '9')
        return GLYPH_DIG[c - '0'];
    switch (c) {
    case 'A': return GLYPH_A;
    case 'B': return GLYPH_B;
    case 'C': return GLYPH_C;
    case 'D': return GLYPH_D;
    case 'E': return GLYPH_E;
    case 'F': return GLYPH_F;
    case 'G': return GLYPH_G;
    case 'H': return GLYPH_H;
    case 'I': return GLYPH_I;
    case 'J': return GLYPH_J;
    case 'K': return GLYPH_K;
    case 'L': return GLYPH_L;
    case 'M': return GLYPH_M;
    case 'N': return GLYPH_N;
    case 'O': return GLYPH_O;
    case 'P': return GLYPH_P;
    case 'Q': return GLYPH_Q;
    case 'R': return GLYPH_R;
    case 'S': return GLYPH_S;
    case 'T': return GLYPH_T;
    case 'U': return GLYPH_U;
    case 'V': return GLYPH_V;
    case 'W': return GLYPH_W;
    case 'X': return GLYPH_X;
    case 'Y': return GLYPH_Y;
    case 'Z': return GLYPH_Z;
    case '.': return GLYPH_DOT;
    case '%': return GLYPH_PCT;
    case '+': return GLYPH_PLUS;
    case '-': return GLYPH_MINUS;
    case '=': return GLYPH_EQ;
    default:  return NULL;
    }
}

static void olog(const char *msg)
{
    int fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return;
    (void)write(fd, msg, strlen(msg));
    close(fd);
}

static int ov_is_open(void)
{
    return ov_shm && ov_shm->open;
}

static void ov_set_open(int open)
{
    if (ov_shm)
        ov_shm->open = open ? 1 : 0;
}

__attribute__((constructor)) static void overlay_shm_init(void)
{
    int fd = open(RB_OVERLAY_SHM, O_RDWR | O_CREAT, 0666);
    if (fd < 0)
        return;
    if (ftruncate(fd, sizeof(*ov_shm)) != 0) {
        close(fd);
        return;
    }
    ov_shm = (struct rb_overlay_shm *)syscall(SYS_mmap2, NULL, sizeof(*ov_shm),
                                              PROT_READ | PROT_WRITE, MAP_SHARED,
                                              fd, 0);
    close(fd);
    if (ov_shm == (void *)-1) {
        ov_shm = NULL;
        return;
    }
    ov_shm->tab_x = TAB_X;
    ov_shm->tab_y = TAB_Y;
    ov_shm->tab_w = TAB_W;
    ov_shm->tab_h = TAB_H;
    {
        int g = ov_shm->jog_gain_milli;
        if (g < JOG_GAIN_MIN || g > JOG_GAIN_MAX || (g % JOG_GAIN_STEP) != 0)
            g = JOG_GAIN_DEF;
        ov_shm->jog_gain_milli = g;
    }
    ov_shm->pan_x = PAN_X;
    ov_shm->pan_y = PAN_Y;
    ov_shm->pan_w = PAN_W;
    ov_shm->pan_h = PAN_VIS_H;
    ov_shm->open = 0;
}

static int in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

static void put_visual(unsigned char *base, int vx, int vy, unsigned color)
{
    int px = vy;
    int py = 1279 - vx;
    unsigned char *p;
    if (px < clip_y0 || py < 0 || px >= clip_y1 || py >= 1280)
        return;
    p = base + (unsigned)py * fb_pitch + (unsigned)px * 4;
    *(unsigned *)p = color;
}

/* One visual column is one contiguous physical row, so fill column by
 * column: the beat meter repaints every frame. */
static void fill_visual(unsigned char *base, int x, int y, int w, int h, unsigned color)
{
    int iy, ix;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < clip_y0) {
        h -= clip_y0 - y;
        y = clip_y0;
    }
    if (x + w > 1280)
        w = 1280 - x;
    if (y + h > clip_y1)
        h = clip_y1 - y;
    for (ix = 0; ix < w; ix++) {
        unsigned *p = (unsigned *)(base + (unsigned)(1279 - x - ix) * fb_pitch) + y;
        for (iy = 0; iy < h; iy++)
            p[iy] = color;
    }
}

static int text_width(const char *s)
{
    int n = 0;
    for (; *s; s++)
        n++;
    if (n <= 0)
        return 0;
    return n * (5 * SCALE + SCALE) - SCALE;
}

static void draw_text(unsigned char *base, int x, int y, const char *s, unsigned color)
{
    for (; *s; s++) {
        const unsigned char *g = glyph(*s);
        int row, col;
        if (g) {
            for (row = 0; row < 7; row++) {
                for (col = 0; col < 5; col++) {
                    if (g[row] & (0x10 >> col)) {
                        int sx, sy;
                        for (sy = 0; sy < SCALE; sy++)
                            for (sx = 0; sx < SCALE; sx++)
                                put_visual(base,
                                           x + col * SCALE + sx,
                                           y + row * SCALE + sy,
                                           color);
                    }
                }
            }
        }
        x += 5 * SCALE + SCALE;
    }
}

static void draw_text_centered(unsigned char *base, int rx, int ry, int rw, int rh,
                               const char *s, unsigned color)
{
    int tw = text_width(s);
    int th = 7 * SCALE;
    int x = rx + (rw - tw) / 2;
    int y = ry + (rh - th) / 2;
    if (x < rx)
        x = rx;
    if (y < ry)
        y = ry;
    draw_text(base, x, y, s, color);
}

static void draw_label(unsigned char *base, int y, const char *s)
{
    int th = 7 * SCALE;
    draw_text(base, LAB_X, y + (ROW_H - th) / 2, s, COL_TITLE);
}

static int jog_milli(void)
{
    int g;
    if (!ov_shm)
        return JOG_GAIN_DEF;
    g = ov_shm->jog_gain_milli;
    if (g < JOG_GAIN_MIN || g > JOG_GAIN_MAX)
        return JOG_GAIN_DEF;
    return g;
}

static void jog_label(char *dst, int milli)
{
    int pct = milli / 10;
    int i = 0;
    if (pct >= 100)
        dst[i++] = (char)('0' + pct / 100);
    if (pct >= 10)
        dst[i++] = (char)('0' + (pct / 10) % 10);
    dst[i++] = (char)('0' + pct % 10);
    dst[i] = '\0';
}

static void log_gain(int milli)
{
    char msg[32];
    char label[16];
    int n = 0;
    const char *p = "overlay: JOG ";
    jog_label(label, milli);
    while (*p)
        msg[n++] = *p++;
    for (p = label; *p; p++)
        msg[n++] = *p;
    msg[n++] = '\n';
    msg[n] = '\0';
    olog(msg);
}

static unsigned long long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL +
           (unsigned long long)ts.tv_nsec / 1000000ULL;
}

/* Frames shown over the last second, x10. Published in shm so it can be
 * read over SSH: od -An -t d4 /tmp/rb-overlay, last number. */
void overlay_frame(void)
{
    static unsigned long long win_ms;
    static int frames;
    unsigned long long now = mono_ms();
    if (!win_ms)
        win_ms = now;
    frames++;
    if (now - win_ms >= 1000) {
        if (ov_shm)
            ov_shm->fps_x10 = (int)((frames * 10000ULL + (now - win_ms) / 2) / (now - win_ms));
        win_ms = now;
        frames = 0;
    }
}

static int fps_x10(void)
{
    return ov_shm ? ov_shm->fps_x10 : 0;
}

/* "59.9" */
static void fps_label(char *dst, int x10)
{
    char tmp[12];
    int n = 0, i = 0;
    int whole = x10 / 10;
    if (x10 < 0)
        x10 = whole = 0;
    do {
        tmp[n++] = (char)('0' + whole % 10);
        whole /= 10;
    } while (whole && n < 6);
    while (n)
        dst[i++] = tmp[--n];
    dst[i++] = '.';
    dst[i++] = (char)('0' + x10 % 10);
    dst[i] = '\0';
}

static void read_file(const char *path, char *dst, int cap);

/* Append a non-negative integer, return the new end. */
static char *put_uint(char *d, int v)
{
    char tmp[12];
    int n = 0;
    if (v < 0)
        v = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 11);
    while (n)
        *d++ = tmp[--n];
    *d = '\0';
    return d;
}

static void pct_label(char *dst, int pct)
{
    char *d = put_uint(dst, pct);
    *d++ = '%';
    *d = '\0';
}

static int read_int_file(const char *path, int fallback)
{
    char buf[24];
    int i, v = 0, any = 0;
    read_file(path, buf, sizeof(buf));
    for (i = 0; buf[i] >= '0' && buf[i] <= '9'; i++) {
        v = v * 10 + (buf[i] - '0');
        any = 1;
    }
    return any ? v : fallback;
}

static void write_int_file(const char *path, int v)
{
    char buf[16];
    char *e = put_uint(buf, v);
    int fd = open(path, O_WRONLY);
    *e++ = '\n';
    if (fd < 0)
        return;
    (void)write(fd, buf, (size_t)(e - buf));
    close(fd);
}

/* SCREEN: the backlight in percent of max_brightness. The first look reads
 * what Engine OS left it at, rounded to a 10% step, without changing it. */
static int screen_pct(void)
{
    int p;
    if (!ov_shm)
        return 0;
    p = ov_shm->screen_pct;
    if (p < SCREEN_PCT_MIN || p > PCT_MAX) {
        int max = read_int_file(BL_DIR "max_brightness", 255);
        int cur = read_int_file(BL_DIR "brightness", max);
        if (max <= 0)
            max = 255;
        p = ((cur * 100 / max + PCT_STEP / 2) / PCT_STEP) * PCT_STEP;
        if (p < SCREEN_PCT_MIN)
            p = SCREEN_PCT_MIN;
        if (p > PCT_MAX)
            p = PCT_MAX;
        ov_shm->screen_pct = p;
    }
    return p;
}

static void nudge_screen(int dir)
{
    int p, max, v;
    char msg[40];
    if (!ov_shm)
        return;
    p = screen_pct() + dir * PCT_STEP;
    if (p < SCREEN_PCT_MIN)
        p = SCREEN_PCT_MIN;
    if (p > PCT_MAX)
        p = PCT_MAX;
    ov_shm->screen_pct = p;
    max = read_int_file(BL_DIR "max_brightness", 255);
    v = (p * max + 50) / 100;
    if (v < 1)
        v = 1;
    write_int_file(BL_DIR "brightness", v);
    strcpy(msg, "overlay: SCREEN ");
    pct_label(msg + strlen(msg), p);
    strcat(msg, "\n");
    olog(msg);
}

/* LEDS: knobshim reads led_pct on its 20 Hz LED tick. 0 = unset = 100. */
static int led_pct(void)
{
    int p = ov_shm ? ov_shm->led_pct : 0;
    if (p < LED_PCT_MIN || p > PCT_MAX)
        p = PCT_MAX;
    return p;
}

static void nudge_led(int dir)
{
    int p;
    char msg[40];
    if (!ov_shm)
        return;
    p = led_pct() + dir * PCT_STEP;
    if (p < LED_PCT_MIN)
        p = LED_PCT_MIN;
    if (p > PCT_MAX)
        p = PCT_MAX;
    ov_shm->led_pct = p;
    __sync_synchronize();
    strcpy(msg, "overlay: LEDS ");
    pct_label(msg + strlen(msg), p);
    strcat(msg, "\n");
    olog(msg);
}

/* CPU: busy share of all cores since the previous sample, from /proc/stat.
 * Sampled once a second, and only while the panel is open. */
static int cpu_pct_v = -1;

static void cpu_sample(void)
{
    static unsigned long long last_ms, last_busy, last_total;
    unsigned long long now = mono_ms(), f[8], busy, total;
    char buf[160];
    const char *p;
    int i;
    if (last_ms && now - last_ms < 1000)
        return;
    last_ms = now;
    read_file("/proc/stat", buf, sizeof(buf));
    p = buf;
    if (strncmp(p, "cpu ", 4) != 0)
        return;
    p += 4;
    for (i = 0; i < 8; i++) {
        while (*p == ' ')
            p++;
        f[i] = 0;
        while (*p >= '0' && *p <= '9')
            f[i] = f[i] * 10 + (unsigned long long)(*p++ - '0');
    }
    /* user nice system idle iowait irq softirq steal */
    total = f[0] + f[1] + f[2] + f[3] + f[4] + f[5] + f[6] + f[7];
    busy = total - f[3] - f[4];
    if (last_total && total > last_total)
        cpu_pct_v = (int)((busy - last_busy) * 100 / (total - last_total));
    last_busy = busy;
    last_total = total;
}

static int pull1, pull2;
/* 0 = idle. 1 or 2 = that slot is waiting for the YES tap. */
static int eject_arm;
static int power_arm;

static void read_file(const char *path, char *dst, int cap)
{
    int fd, n, i;
    dst[0] = '\0';
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    n = (int)read(fd, dst, cap - 1);
    close(fd);
    if (n < 0)
        n = 0;
    dst[n] = '\0';
    for (i = 0; i < n; i++) {
        if (dst[i] == '\n' || dst[i] == '\r') {
            dst[i] = '\0';
            break;
        }
    }
}

static int file_exists(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

static unsigned hash_bytes(unsigned h, const void *data, unsigned len)
{
    const unsigned char *p = data;
    while (len--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

static unsigned hash_string(unsigned h, const char *s)
{
    return hash_bytes(h, s, (unsigned)strlen(s) + 1);
}

static void eject_request(int slot)
{
    const char *path = (slot == 1) ? "/tmp/usb-eject-1" : "/tmp/usb-eject-2";
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        (void)write(fd, "1\n", 2);
        close(fd);
    }
    olog(slot == 1 ? "overlay: eject 1\n" : "overlay: eject 2\n");
}

static void power_request(void)
{
    int fd = open(POWER_REQ, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
        close(fd);
    olog("overlay: poweroff\n");
}

/* Four letters of the volume label, then "..." when the name is longer.
 * SPACE-SHUTTLE is shown as SPAC...  Empty slot falls back to USB 1 / USB 2. */
static void draw_volume(unsigned char *base, int rx, int ry, int rw, int rh,
                        const char *name, const char *fallback, int pulling)
{
    char shown[8];
    const char *src;
    int n = 0, i, dots, tw, extra, x, y;
    if (pulling) {
        draw_text_centered(base, rx, ry, rw, rh, "PULL", COL_TEXT);
        return;
    }
    /* An empty slot keeps the full "USB 1" / "USB 2" label. The four-letter
     * cut is only for a real volume name. */
    if (!name || !name[0]) {
        draw_text_centered(base, rx, ry, rw, rh, fallback, COL_TEXT);
        return;
    }
    src = name;
    dots = 0;
    {
        int len = 0;
        while (name[len])
            len++;
        dots = len > 4;
    }
    for (i = 0; src[i] && n < 4; i++) {
        char c = src[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 32);
        if (c == ' ' || glyph(c))
            shown[n++] = (c == ' ') ? ' ' : c;
    }
    shown[n] = '\0';
    if (n == 0) {
        draw_text_centered(base, rx, ry, rw, rh, fallback, COL_TEXT);
        return;
    }
    tw = text_width(shown);
    extra = dots ? 16 : 0;
    x = rx + (rw - tw - extra) / 2;
    y = ry + (rh - 7 * SCALE) / 2;
    if (x < rx)
        x = rx;
    draw_text(base, x, y, shown, COL_TEXT);
    if (dots) {
        int dx = x + tw + 4;
        int dy = y + 7 * SCALE - 3;
        for (i = 0; i < 3; i++)
            fill_visual(base, dx + i * 5, dy, 2, 2, COL_TEXT);
    }
}

static void nudge_jog(int dir)
{
    int g;
    if (!ov_shm)
        return;
    g = jog_milli() + dir * JOG_GAIN_STEP;
    if (g < JOG_GAIN_MIN)
        g = JOG_GAIN_MIN;
    if (g > JOG_GAIN_MAX)
        g = JOG_GAIN_MAX;
    ov_shm->jog_gain_milli = g;
    __sync_synchronize();
    log_gain(g);
}

static void draw_minus_mark(unsigned char *base, int x, int y, int w, int h)
{
    int bw = w / 2;
    int bh = 4;
    fill_visual(base, x + (w - bw) / 2, y + (h - bh) / 2, bw, bh, COL_TEXT);
}

static void draw_plus_mark(unsigned char *base, int x, int y, int w, int h)
{
    int bw = 4;
    int bh = h / 2;
    draw_minus_mark(base, x, y, w, h);
    fill_visual(base, x + (w - bw) / 2, y + (h - bh) / 2, bw, bh, COL_TEXT);
}

static void persist_mode(int mode)
{
    uint32_t v = (uint32_t)(144 + mode);
    int fd = open(SETTINGS_PATH, O_RDWR);
    if (fd < 0)
        return;
    if (lseek(fd, SETTINGS_OFF, SEEK_SET) == SETTINGS_OFF)
        (void)write(fd, &v, sizeof(v));
    close(fd);
}

static void wave_store(int dev, int kind, int color)
{
    unsigned off;
    if (dev < 0 || dev > 44 || kind < 2 || kind > 4)
        return;
    off = 1176u * (unsigned)dev + 188u * (unsigned)kind + 12894u;
    CMN_BASE[off] = (unsigned char)color;
}

static void wave_apply_one(int dev, int kind, int color)
{
    if (dev < 0 || dev > 44 || kind < 2 || kind > 4)
        return;
    if (UI_CHK_WAVE(dev, kind))
        UI_SET_WAVE(dev, kind, color, 0);
    wave_store(dev, kind, color);
}

static void apply_wave(void)
{
    int seq = ov_wave_seq;
    int color, deck, dev, kind;
    if (seq == ov_wave_seen)
        return;
    __sync_synchronize();
    color = ov_wave_color;
    ov_wave_seen = seq;
    if (color != 1 && color != 3 && color != 4)
        return;
    wave_apply_one(0, 2, color);
    for (deck = 0; deck < 2; deck++) {
        dev = DECK_WAVE[deck * DECK_WAVE_STRIDE];
        kind = DECK_WAVE[deck * DECK_WAVE_STRIDE + 1];
        wave_apply_one(dev, kind, color);
        wave_apply_one(0, kind, color);
    }
    ov_wave_cur = color;
    if (color == 1)
        olog("overlay: wave BLUE\n");
    else if (color == 3)
        olog("overlay: wave RGB\n");
    else
        olog("overlay: wave 3BAND\n");
}

static int read_wave(void)
{
    int c = UI_GET_WAVE(0, 2);
    if (c == 1 || c == 3 || c == 4)
        return c;
    return 1;
}

static void refresh_quant(void)
{
    int deck;
    if (ov_quant_known)
        return;
    for (deck = 0; deck < 2; deck++)
        ov_quant[deck] = UI_GET_QUANTIZE(deck) ? 1 : 0;
    ov_quant_known = 1;
}

static void apply_quant(void)
{
    int seq = ov_quant_seq;
    int on, deck;
    if (seq == ov_quant_seen)
        return;
    __sync_synchronize();
    on = ov_quant_on ? 1 : 0;
    ov_quant_seen = seq;
    for (deck = 0; deck < 2; deck++)
        UI_SET_QUANTIZE(deck, on);
    ov_quant[0] = on;
    ov_quant[1] = on;
    ov_quant_known = 1;
    olog(on ? "overlay: quantize ON\n" : "overlay: quantize OFF\n");
}

static int link_want(void)
{
    return !(ov_shm && ov_shm->link_cue == LINK_OFF);
}

/* Keep rbp's preview headphone cue at the wanted state. rbp resets it to off
 * at startup and the SC Live 4 has no LINK CUE button to set it, so check
 * about once a second, and at once after a tap. Runs on rbp's GUI thread. */
static void apply_link(void)
{
    unsigned long long now = mono_ms();
    int seq = ov_link_seq;
    int want, cur;
    void *me;

    if (seq == ov_link_seen && ov_link_ms && now - ov_link_ms < LINK_RECHECK_MS)
        return;
    ov_link_ms = now;
    __sync_synchronize();
    ov_link_seen = seq;
    want = link_want();
    ov_link = want;
    me = *(void **)ME_SINGLETON;
    if (!me)
        return;
    cur = ME_GET_PREVIEW_CUE(me) ? 1 : 0;
    if (cur == want)
        return;
    ME_SET_PREVIEW_CUE(me, want);
    olog(want ? "overlay: link cue ON\n" : "overlay: link cue OFF\n");
}

static void request_link(int on)
{
    on = on ? 1 : 0;
    ov_link = on;
    if (ov_shm)
        ov_shm->link_cue = on ? LINK_ON : LINK_OFF;
    __sync_synchronize();
    ov_link_seq++;
}

static void request_skip(int beats)
{
    ov_skip = beats ? 1 : 0;
    if (ov_shm)
        ov_shm->skip_mode = beats ? SKIP_BEATS : SKIP_SEARCH;
    __sync_synchronize();
}

/* The INFO and COUNT rows: flip one INFO_* bit.  knobshim reads the word on every deck info update. */
static void request_info(unsigned bit)
{
    if (ov_shm)
        ov_shm->info_cfg = (ov_info ^ bit) | INFO_SET;
    __sync_synchronize();
}

static void request_tcue(int on)
{
    on = on ? 1 : 0;
    ov_tcue = on;
    if (ov_shm)
        ov_shm->tcue_mode = on ? TCUE_ON : TCUE_OFF;
    __sync_synchronize();
}

static void request_quant(int on)
{
    on = on ? 1 : 0;
    ov_quant[0] = on;
    ov_quant[1] = on;
    ov_quant_known = 1;
    ov_quant_on = on;
    __sync_synchronize();
    ov_quant_seq++;
}

static void request_wave(int color)
{
    ov_wave_color = color;
    ov_wave_cur = color;
    __sync_synchronize();
    ov_wave_seq++;
}

static void apply_pending(void)
{
    int seq = ov_apply_seq;
    int mode;
    if (seq == ov_seen_seq)
        return;
    __sync_synchronize();
    mode = ov_apply_mode;
    ov_seen_seq = seq;
    if (mode < 0 || mode > 3)
        return;
    UI_SET_AUTOPLAY(mode);
    ov_mode = mode;
    ov_mode_known = 1;
    persist_mode(mode);
    olog(mode == 1 ? "overlay: CONTINUE\n" :
         mode == 2 ? "overlay: REPEAT\n" :
         mode == 3 ? "overlay: ALL REPEAT\n" : "overlay: SINGLE\n");
}

static const char *mode_name(int mode)
{
    if (mode == 1)
        return "CONTINUE";
    if (mode == 2)
        return "REPEAT";
    if (mode == 3)
        return "ALL REPEAT";
    return "SINGLE";
}

static void refresh_mode(void)
{
    int mode;
    if (ov_mode_known)
        return;
    mode = UI_GET_AUTOPLAY();
    if (mode < 0 || mode > 3)
        mode = 0;
    ov_mode = mode;
    ov_mode_known = 1;
}

static void send_key(int key, int op)
{
    void *mgr = *(void **)UI_OBJ_MGR_GLOBAL;
    void *km;
    void **vt;
    void (*fn)(void *, int, int, int, long, float, long);
    if (!mgr)
        return;
    km = *(void **)((char *)mgr + KEY_MANAGER_OFF);
    if (!km)
        return;
    vt = *(void ***)km;
    fn = (void (*)(void *, int, int, int, long, float, long))vt[SENDKEY_WORD];
    if (!fn)
        return;
    fn(km, key, op, CH_GLOBAL, 0, 0.0f, 0);
}

/* Press now, release on a later paint. The browse task has to see the
 * down edge; a press and release in the same call never opens the screen. */
static void tap_key(int key)
{
    if (ov_key_up) {
        send_key(ov_key_up, OP_RELEASE);
        ov_key_up = 0;
    }
    send_key(key, OP_PRESS);
    ov_key_up = key;
    ov_key_up_ms = mono_ms() + KEY_TAP_MS;
}

static void release_key(void)
{
    int key;
    if (!ov_key_up || mono_ms() < ov_key_up_ms)
        return;
    key = ov_key_up;
    ov_key_up = 0;
    send_key(key, OP_RELEASE);
}

static void request_track(int act)
{
    ov_track_act = act;
    __sync_synchronize();
    ov_track_seq++;
}

static void apply_track(void)
{
    int seq = ov_track_seq;
    int act;
    if (seq == ov_track_seen)
        return;
    __sync_synchronize();
    act = ov_track_act;
    ov_track_seen = seq;
    if (act == 1)
        tap_key(K_TAGTRACK);
    else if (act == 2)
        tap_key(K_TAGLIST);
    else if (act == 3)
        tap_key(K_SEARCH);
    olog(act == 1 ? "overlay: TAG\n" :
         act == 2 ? "overlay: TAGS\n" : "overlay: FIND\n");
}

/* Beat meter. rbp's own beat cue is the small red bar tick over each
 * waveform. This draws both decks' bar position in the blank top bar right
 * of the MOD tab, stacked on one x scale so an offset reads directly.
 *
 * Grid: PlayEngine::getBeatPosInfo(ch) is a common::BeatPosition. Its beats
 * are a vector of 8-byte myBeat at +52 / +56 (beat in bar 1..4, BPM x100,
 * time in ms), the grid offset in ms at +40. The engine looks up a beat at
 * (playing time - offset), so this does too. Only reads: the engine's own
 * Quantize::calc*Beat* helpers write into the BeatPosition. */
#define DJENGINEIF_GLOBAL 0x02686178UL
#define PLAYENGINE_GLOBAL 0x011497d0UL
#define DJ_IS_LOADED   ((int (*)(void *, int))0x0004518c)
#define DJ_IS_LOADING  ((int (*)(void *, int))0x0004523c)
#define DJ_PLAY_TIME   ((int (*)(void *, int))0x00045aec)
#define DJ_GRID_OFFSET ((int (*)(void *, int))0x0004a7b0)
#define DJ_TEMPO_X100  ((int (*)(void *, int))0x00045dbc)
#define DJ_SYNC_MASTER ((int (*)(void *))0x0004b450)
#define PE_BEAT_POS    ((void *(*)(void *, int))0x0005f5bc)
#define BEAT_LOG       "/tmp/rb-beat"

/* x 690..1072 is black in rbp's top bar: MOD ends at 680, the recording
 * dot starts near 1080. Two rows, one per deck. */
#define MET_X 690
#define MET_W 382
#define MET_Y TAB_Y
#define MET_H TAB_H
#define MET_ROW_H 14
#define MET_ROW0 (MET_Y + 1)
#define MET_ROW1 (MET_Y + MET_H - 1 - MET_ROW_H)
#define MET_LAB_W 14
#define MET_BAR_X (MET_X + MET_LAB_W)
#define MET_BAR_W (MET_W - MET_LAB_W)
#define MET_GAP 4
#define MET_CELL ((MET_BAR_W - 3 * MET_GAP) / 4)

#define COL_BLACK   0xff000000u
#define COL_CELL    0xff262b33u
#define COL_CELL1   0xff4a1616u   /* downbeat cell, idle */
#define COL_LIT     0xff8c96a3u
#define COL_FILL    0xfff2f2f2u
#define COL_LIT1    0xff7a1a1au
#define COL_FILL1   0xffff3030u
#define COL_MASTER  0xffff8a1cu
#define COL_DIM     0xff5a626cu
#define COL_LOCK    0xff2fd05au
#define COL_NEAR    0xffffb020u
#define COL_FAR     0xffff3030u

/* Locked within this many ms and this tempo difference (BPM x100). */
#define LOCK_MS   8
#define NEAR_MS   30
#define LOCK_BPM  5

struct my_beat {
    unsigned short beat;   /* 1..4 */
    unsigned short bpm;    /* x100, before the tempo fader */
    unsigned int   ms;
};

struct deck_beat {
    int ok;
    int pos;      /* bar position, milli-beats, 0..3999 */
    int bpm;      /* x100, with the tempo fader */
    int t, n, idx;
};

static int beat_mode(void)
{
    int m = ov_shm ? ov_shm->beat_mode : 0;
    if (m != BEAT_OFF && m != BEAT_BARS && m != BEAT_DRIFT)
        m = BEAT_BARS;
    return m;
}

static int wrap4(int b)
{
    b %= 4;
    return b < 0 ? b + 4 : b;
}

static void deck_beat_read(void *dj, void *pe, int ch, struct deck_beat *d)
{
    const char *bp;
    const struct my_beat *b, *e;
    int n, lo, hi, i, t, base, w, beat, tempo, raw;

    memset(d, 0, sizeof(*d));
    if (!(DJ_IS_LOADED(dj, ch) & 0xff) || (DJ_IS_LOADING(dj, ch) & 0xff))
        return;
    bp = PE_BEAT_POS(pe, ch);
    if (!bp || *(const unsigned *)(bp + 36) == 0)
        return;
    /* Read the vector ends once; a load swaps the BeatPosition. */
    b = *(const struct my_beat *const *)(bp + 52);
    e = *(const struct my_beat *const *)(bp + 56);
    if (!b || e <= b)
        return;
    n = (int)(e - b);
    if (n < 2 || n > 200000)
        return;
    t = DJ_PLAY_TIME(dj, ch) - *(const int *)(bp + 40);

    /* Last beat at or before t. Before the first beat or after the last,
     * extend the grid with the nearest beat width. */
    if (t < (int)b[0].ms) {
        w = (int)(b[1].ms - b[0].ms);
        if (w <= 0)
            return;
        i = ((int)b[0].ms - t + w - 1) / w;
        base = (int)b[0].ms - i * w;
        beat = wrap4((int)b[0].beat - 1 - i);
        raw = b[0].bpm;
        i = 0;
    } else if (t >= (int)b[n - 1].ms) {
        w = (int)(b[n - 1].ms - b[n - 2].ms);
        if (w <= 0)
            return;
        i = (t - (int)b[n - 1].ms) / w;
        base = (int)b[n - 1].ms + i * w;
        beat = wrap4((int)b[n - 1].beat - 1 + i);
        raw = b[n - 1].bpm;
        i = n - 1;
    } else {
        lo = 0;
        hi = n - 1;   /* b[lo].ms <= t < b[hi].ms */
        while (hi - lo > 1) {
            int mid = (lo + hi) / 2;
            if ((int)b[mid].ms <= t)
                lo = mid;
            else
                hi = mid;
        }
        i = lo;
        base = (int)b[i].ms;
        w = (int)(b[i + 1].ms - b[i].ms);
        if (w <= 0)
            return;
        beat = (b[i].beat >= 1 && b[i].beat <= 4) ? b[i].beat - 1 : wrap4(i);
        raw = b[i].bpm;
    }
    d->pos = beat * 1000 + (int)((long long)(t - base) * 1000 / w);
    if (d->pos > 3999)
        d->pos = 3999;
    if (d->pos < 0)
        d->pos = 0;
    tempo = DJ_TEMPO_X100(dj, ch);
    if (tempo <= -10000 || tempo >= 10000)
        tempo = 0;
    d->bpm = (int)(((long long)raw * (10000 + tempo) + 5000) / 10000);
    d->t = t;
    d->n = n;
    d->idx = i;
    d->ok = 1;
}

/* "+12" / "-3" / "0" */
static char *put_int(char *d, int v)
{
    if (v > 0)
        *d++ = '+';
    else if (v < 0) {
        *d++ = '-';
        v = -v;
    }
    return put_uint(d, v);
}

/* BPM x100 as "+0.40", two decimals. */
static char *put_bpm_delta(char *d, int x100)
{
    int a = x100 < 0 ? -x100 : x100;
    *d++ = x100 < 0 ? '-' : '+';
    d = put_uint(d, a / 100);
    *d++ = '.';
    *d++ = (char)('0' + (a / 10) % 10);
    *d++ = (char)('0' + a % 10);
    *d = '\0';
    return d;
}

static void draw_deck_bar(unsigned char *base, int y, int deck,
                          const struct deck_beat *d, int master)
{
    char lab[2] = { (char)('1' + deck), '\0' };
    int c, cur = d->ok ? d->pos / 1000 : -1;
    fill_visual(base, MET_X, y, MET_LAB_W, MET_ROW_H, COL_BLACK);
    draw_text(base, MET_X, y, lab, !d->ok ? COL_DIM : master ? COL_MASTER : COL_TEXT);
    for (c = 0; c < 4; c++) {
        int x = MET_BAR_X + c * (MET_CELL + MET_GAP);
        if (c != cur) {
            fill_visual(base, x, y, MET_CELL, MET_ROW_H, c == 0 ? COL_CELL1 : COL_CELL);
        } else {
            /* The lit beat fills left to right through the beat, so the two
             * rows' fill edges line up when the decks are in phase. */
            int f = (d->pos % 1000) * MET_CELL / 1000;
            fill_visual(base, x, y, f, MET_ROW_H, c == 0 ? COL_FILL1 : COL_FILL);
            fill_visual(base, x + f, y, MET_CELL - f, MET_ROW_H, c == 0 ? COL_LIT1 : COL_LIT);
        }
    }
}

/* Top: a center-zero gauge, half a beat each side, of where the other deck's
 * beat sits against the reference deck's. Right of center = ahead.
 * Bottom: that offset in ms, the tempo difference, and the bar offset when
 * the beats line up but the downbeats do not. */
static void draw_drift(unsigned char *base, const struct deck_beat *dk, int ref)
{
    int oth = 1 - ref;
    const struct deck_beat *r = &dk[ref], *o = &dk[oth];
    int cx = MET_BAR_X + MET_BAR_W / 2;
    int half = MET_BAR_W / 2;
    int y0 = MET_ROW0, y1 = MET_ROW1;
    char lab[2] = { (char)('1' + oth), '\0' };
    char txt[40], *p;
    int diff, bars, frac, ms, dbpm, am, col, x;

    fill_visual(base, MET_X, y0, MET_LAB_W, MET_ROW_H, COL_BLACK);
    fill_visual(base, MET_X, y1, MET_W, MET_ROW_H, COL_BLACK);
    fill_visual(base, MET_BAR_X, y0, MET_BAR_W, MET_ROW_H, COL_CELL);
    fill_visual(base, MET_BAR_X + half / 2, y0, 1, MET_ROW_H, COL_DIM);
    fill_visual(base, cx + half / 2, y0, 1, MET_ROW_H, COL_DIM);
    if (!r->ok || !o->ok || r->bpm <= 0) {
        fill_visual(base, cx - 1, y0, 2, MET_ROW_H, COL_DIM);
        draw_text(base, MET_X, y0, lab, COL_DIM);
        draw_text(base, MET_BAR_X, y1, "NO GRID", COL_DIM);
        return;
    }
    diff = o->pos - r->pos;            /* milli-beats, wrap to -2000..1999 */
    diff = ((diff % 4000) + 4000 + 2000) % 4000 - 2000;
    bars = (diff + (diff >= 0 ? 500 : -500)) / 1000;
    frac = diff - bars * 1000;         /* -500..500 */
    ms = (int)((long long)frac * 6000 / r->bpm);
    dbpm = o->bpm - r->bpm;
    am = ms < 0 ? -ms : ms;
    col = am <= LOCK_MS ? COL_LOCK : am <= NEAR_MS ? COL_NEAR : COL_FAR;

    x = frac * half / 500;
    if (x > 0)
        fill_visual(base, cx, y0 + 3, x, MET_ROW_H - 6, col);
    else if (x < 0)
        fill_visual(base, cx + x, y0 + 3, -x, MET_ROW_H - 6, col);
    fill_visual(base, cx + x - 2, y0, 4, MET_ROW_H, col);
    fill_visual(base, cx, y0, 1, MET_ROW_H, COL_TEXT);
    draw_text(base, MET_X, y0, lab, COL_TEXT);

    p = put_int(txt, ms);
    strcpy(p, "MS");
    draw_text(base, MET_BAR_X, y1, txt, col);
    p = txt;
    strcpy(p, "BPM ");
    p += 4;
    if (dbpm > -LOCK_BPM && dbpm < LOCK_BPM)
        strcpy(p, "=");
    else
        put_bpm_delta(p, dbpm);
    draw_text(base, MET_BAR_X + 96, y1,
              txt, (dbpm > -LOCK_BPM && dbpm < LOCK_BPM) ? COL_LOCK : COL_TEXT);
    if (bars) {
        p = txt;
        strcpy(p, "BEAT ");
        put_int(p + 5, bars);
        draw_text(base, MET_X + MET_W - text_width(txt), y1, txt, COL_NEAR);
    }
}

static int beat_paint_us;   /* slowest beat_paint since the last log */

static void beat_log(const struct deck_beat *dk, int master)
{
    static unsigned long long last;
    unsigned long long now = mono_ms();
    char buf[200];
    int fd, n, i;
    if (now - last < 1000)
        return;
    last = now;
    n = snprintf(buf, sizeof(buf), "master %d paint_us %d\n", master, beat_paint_us);
    beat_paint_us = 0;
    for (i = 0; i < 2; i++)
        n += snprintf(buf + n, sizeof(buf) - n,
                      "deck %d ok %d t %d beat %d/%d pos %d bpm %d\n",
                      i + 1, dk[i].ok, dk[i].t, dk[i].idx, dk[i].n,
                      dk[i].pos, dk[i].bpm);
    fd = open(BEAT_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    (void)write(fd, buf, (size_t)n);
    close(fd);
}

/* Every frame while on: the bars move, and the page about to be shown is
 * three frames old. rbp's pixels here are black, so the driver's unchanged
 * tile skip leaves ours alone; turning the meter off is undone by the full
 * redraw that closing the panel forces. */
static void beat_paint(unsigned char *base, int page)
{
    static int painted[3];
    struct timespec t0, t1;
    int us;
    struct deck_beat dk[2];
    void *dj = *(void **)DJENGINEIF_GLOBAL;
    void *pe = *(void **)PLAYENGINE_GLOBAL;
    int mode = beat_mode(), master, ref;

    if (mode == BEAT_OFF || !dj || !pe) {
        painted[page] = 0;
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    deck_beat_read(dj, pe, 0, &dk[0]);
    deck_beat_read(dj, pe, 1, &dk[1]);
    master = DJ_SYNC_MASTER(dj);
    if (master != 0 && master != 1)
        master = -1;
    beat_log(dk, master);

    /* The cells cover their rows every frame; the rest only changes with
     * the mode. DRIFT's text row is cleared in draw_drift. */
    if (painted[page] != mode) {
        fill_visual(base, MET_X, MET_Y, MET_W, MET_H, COL_BLACK);
        painted[page] = mode;
    }
    if (mode == BEAT_BARS) {
        draw_deck_bar(base, MET_ROW0, 0, &dk[0], master == 0);
        draw_deck_bar(base, MET_ROW1, 1, &dk[1], master == 1);
    } else {
        /* Reference: the sync master, else deck 1. */
        ref = master >= 0 && dk[master].ok ? master : 0;
        draw_drift(base, dk, ref);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    us = (int)((t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000);
    if (us > beat_paint_us)
        beat_paint_us = us;
}

/* COUNT BEATS: rbp draws a bar count as "NN.B Bars".  The count is passed as four times the beats, so the
 * two digits are the beats; cover the dot, the beat digit and the Bars label with the box's background and name
 * the unit.  Only while knobshim has just run the box update (it stamps deck_ms), i.e. while the player screen
 * is up.  rbp repaints the spot when a digit changes, so every frame (a few hundred pixels). */
static void count_label_paint(unsigned char *base)
{
    static const int y[2] = {185, 406};
    unsigned c;
    int i;
    if (!ov_shm)
        return;
    c = ov_shm->info_cfg;
    if ((c & (INFO_SET | INFO_OFF | INFO_BEATS | INFO_CNT)) != (INFO_SET | INFO_BEATS | INFO_CNT) ||
        (unsigned)mono_ms() - ov_shm->deck_ms > 300)
        return;
    for (i = 0; i < 2; i++) {
        fill_visual(base, 55, y[i], 122, 26, 0xff181818u);
        draw_text(base, 62, y[i] + 7, "BEATS", COL_TEXT);
    }
}

static void set_beat_mode(int m)
{
    if (!ov_shm)
        return;
    ov_shm->beat_mode = m;
    __sync_synchronize();
    olog(m == BEAT_OFF ? "overlay: beat OFF\n" :
         m == BEAT_BARS ? "overlay: beat BARS\n" : "overlay: beat DRIFT\n");
}

static int ensure_map(int fb_fd)
{
    struct {
        char id[16];
        unsigned long smem_start;
        unsigned int smem_len;
        unsigned int type, type_aux, visual;
        unsigned short xpanstep, ypanstep, ywrapstep;
        unsigned int line_length;
        unsigned long mmio_start;
        unsigned int mmio_len;
        unsigned int accel;
        unsigned short capabilities;
        unsigned short reserved[2];
    } fix;

    if (fb_map && fb_map_fd == fb_fd)
        return 1;
    if (fb_map) {
        munmap(fb_map, fb_map_len);
        fb_map = NULL;
    }
    memset(&fix, 0, sizeof(fix));
    if (syscall(SYS_ioctl, fb_fd, 0x4602 /* FBIOGET_FSCREENINFO */, &fix) != 0)
        return 0;
    if (fix.line_length < 800u * 4u || fix.smem_len < fix.line_length)
        return 0;
    fb_pitch = fix.line_length;
    fb_map_len = fix.smem_len;
    fb_map = (void *)syscall(SYS_mmap2, NULL, fb_map_len,
                             PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_map == (void *)-1) {
        fb_map = NULL;
        return 0;
    }
    fb_map_fd = fb_fd;
    return 1;
}


/* ---- Touch Cue and the preview playhead ------------------------------------------------------------
 * Touch Cue: while the MOD TCUE row is ON and a deck is playing, a touch on its overview waveform plays that
 * deck's track from the touched point in the headphones (the preview player, gated by LINK CUE); moving the
 * finger moves it, lifting stops it, and a pad pressed meanwhile sets that hot cue there.  A paused deck is
 * left to rbp's Needle Search.  The rectangles are the player's own Needle Search areas.
 * Each deck's Player keeps a copy of its StTrackInfo at +1988; DjEngineIF::loadPreview takes that type.
 * The copy's owned heap blocks (+804..+839: beat grid and VBR table with their sizes) were already freed,
 * so they are cleared; sharing them made two players free the same memory.
 * Playhead: rbp draws none for either preview, so a lime line is painted after the rotate, per fb page,
 * with the pixels under it saved and put back (the driver skips tiles that did not change). */
#define DJ_LOADPREVIEW   ((int (*)(void *, const void *, unsigned))0x0004c2fc)
#define DJ_SETPREVIEWPOS ((int (*)(void *, unsigned))0x0004c3b4)
#define DJ_UNLOAD        ((void (*)(void *, int, int, int))0x00044f5c)
#define DJ_ISLOADED      ((int (*)(void *, int))0x0004518c)
#define DJ_SETCUETIME    ((int (*)(void *, int, int, const void *))0x00048750)
#define PE_ISPLAYING     ((int (*)(void *, int))0x0005d880)
#define PP_TOTALLEN      ((int (*)(void *))0x00074064)
#define PLAYER_INFO_OFF  1988
#define PP_FRAME_OFF     1884      /* preview player's 44.1 kHz frame counter */
#define PREVIEW_CH       2
#define TC_AREA_Y        718
#define TC_AREA_H        61
#define TC_AREA_W        513
#define TC_MIN_MS        100
#define TC_MIN_DELTA     0.02f
#define PV_ROW_X         119       /* browse preview row windows: 200 x 38 at y = 110 + 50 * row */
#define PV_ROW_W         200
#define PV_ROW_H         38

extern int rb_preview_row(void) __attribute__((weak));   /* knobshim2: browse row being previewed, or -1 */
extern void *rb_ui_player(int ch) __attribute__((weak));    /* knobshim2: rbp's ui::Player of a deck (channel 1/2) */

static const int tc_area_x[2] = {106, 744};
static int tc_deck = -1;           /* deck with a Touch Cue running, else -1 */
static float tc_last;
static unsigned long long tc_ms;

static void *engine_vec_item(int off, int idx)
{
    char *pe = *(char **)PLAYENGINE_GLOBAL;
    char *vb;
    if (!pe)
        return NULL;
    vb = *(char **)(pe + off);
    return vb && *(char **)(pe + off + 4) > vb + 4 * idx ? *(void **)(vb + 4 * idx) : NULL;
}

/* Preview player position: frames played, and as 0..1 of the track.  0 when it is not known yet. */
static int preview_pos(int *frame, float *ratio)
{
    void *pp = engine_vec_item(24, 0);
    int cur, tot;
    if (!pp)
        return 0;
    cur = *(int *)((char *)pp + PP_FRAME_OFF);
    tot = PP_TOTALLEN(pp);
    if (frame)
        *frame = cur;
    if (ratio)
        *ratio = tot > 0 && cur >= 0 ? (cur >= tot ? 1.0f : (float)cur / (float)tot) : -1.0f;
    return tot > 0 && cur >= 0;
}

static unsigned fbits(float f)
{
    unsigned b;
    memcpy(&b, &f, sizeof(b));
    return b;
}

static float tc_ratio(int vx)
{
    float f = (float)(vx - tc_area_x[tc_deck]) / (float)TC_AREA_W;
    return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
}

/* Called from fbshim-tsc for every changed touch sample; returns 1 to keep the sample from rbp. */
int overlay_tcue_touch(int down, int lx, int ly)
{
    static int prev_down;
    int fresh = down && !prev_down, vx = 1279 - lx, vy = ly, deck;
    unsigned long long now;
    prev_down = down;

    if (tc_deck < 0) {
        unsigned char info[1680];
        void *pe, *pl;
        if (!fresh || (ov_shm && ov_shm->tcue_mode == TCUE_OFF) ||   /* TCUE off: nothing else runs */
            vy < TC_AREA_Y || vy >= TC_AREA_Y + TC_AREA_H)
            return 0;
        deck = vx >= tc_area_x[0] && vx < tc_area_x[0] + TC_AREA_W ? 0 :
               vx >= tc_area_x[1] && vx < tc_area_x[1] + TC_AREA_W ? 1 : -1;
        pe = *(void **)PLAYENGINE_GLOBAL;
        pl = deck >= 0 ? engine_vec_item(12, deck) : NULL;
        if (!pl || !pe || !PE_ISPLAYING(pe, deck) || (rb_preview_row && rb_preview_row() >= 0))
            return 0;
        tc_deck = deck;
        tc_last = tc_ratio(vx);
        memcpy(info, (char *)pl + PLAYER_INFO_OFF, sizeof(info));
        memset(info + 804, 0, 36);
        if (DJ_LOADPREVIEW(NULL, info, fbits(tc_last)) <= 0) {
            tc_deck = -1;
            return 0;
        }
        tc_ms = mono_ms();
        return 1;
    }
    if (!down) {
        DJ_UNLOAD(NULL, PREVIEW_CH, 0, 0);
        tc_deck = -1;
        return 1;
    }
    now = mono_ms();
    if (now - tc_ms >= TC_MIN_MS) {
        float f = tc_ratio(vx);
        if (f - tc_last >= TC_MIN_DELTA || tc_last - f >= TC_MIN_DELTA) {
            DJ_SETPREVIEWPOS(NULL, fbits(f));
            tc_last = f;
            tc_ms = now;
        }
    }
    return 1;
}

/* The engine call above records the cue, but the pad LED belongs to rbp's UI, which only lights it when its own
 * pad handler runs (that flow also pauses the deck while it records the cue).  So the UI half is replayed
 * here, on the paint thread like the other rbp UI calls: ui::Player::onHotCueEvent's steps after a record. */
#define UI_UPDATE_LED_STATE ((void (*)(void *, int, int, int))0x002fbc80)   /* (player, pad 1..8, state, interval) */
#define UI_OWN_COLOR        ((void (*)(void *, int, int))0x002fbdd4)        /* (player, pad 1..8, colour mode) */

static volatile int tcue_led_job;   /* ((deck + 1) << 4) | pad 1..8, 0 = none */

static void tcue_led_apply(void)
{
    int job = tcue_led_job, pad = job & 15;
    unsigned char *pl, *led;
    if (!job)
        return;
    tcue_led_job = 0;
    pl = rb_ui_player ? rb_ui_player(job >> 4) : NULL;
    if (!pl || pad > *(int *)(pl + 1204))
        return;
    UI_UPDATE_LED_STATE(pl, pad, 2, *(int *)(pl + 1224));
    UI_OWN_COLOR(pl, pad, (pl[541] >> 6) & 1);
    led = (*(unsigned char ***)(pl + 1196))[pad - 1];
    if (led[46]) {
        memcpy(led + 53, led + 50, 3);
        led[46] = 0;
    }
}

/* knobshim2 calls this for every pad note.  A pad pressed during Touch Cue sets that hot cue (EnCueType 1..8
 * = A..H) at the previewed time and is not passed to rbp; its release is swallowed with it. */
int rb_tcue_pad(int ch, int note, int on)
{
    static unsigned taken;
    int deck = ch - 4, pad = note - 15, frame;
    unsigned bit, was;
    if ((ch != 4 && ch != 5) || pad < 0 || pad > 7)
        return 0;
    bit = 1u << (deck * 8 + pad);
    if (!on) {
        was = taken & bit;
        taken &= ~bit;
        return was != 0;
    }
    if (tc_deck == deck && preview_pos(&frame, NULL)) {
        unsigned char info[824] = {0};   /* StCueInfo: +792 in-point ms, +804 loop end (0xffffffff = none) */
        *(unsigned *)(info + 792) = (unsigned)((unsigned long long)frame * 1000ull / 44100ull);
        *(unsigned *)(info + 804) = 0xffffffffu;
        DJ_SETCUETIME(NULL, deck, 1 + pad, info);
        tcue_led_job = ((deck + 1) << 4) | (pad + 1);
        taken |= bit;
        return 1;
    }
    return 0;
}

#define PH_HALF  3
#define PH_SPAN  (2 * PH_HALF + 1)
#define PH_CAP   3
#define PH_LIME  0xff00ff00u
#define PH_EDGE  0xff000000u

static struct { int valid, x, y0, h; unsigned saved[PH_SPAN * TC_AREA_H]; } ph[3];

static unsigned get_visual(unsigned char *base, int vx, int vy)
{
    int px = vy, py = 1279 - vx;
    if (px < 0 || py < 0 || px >= 800 || py >= 1280)
        return 0;
    return *(unsigned *)(base + (unsigned)py * fb_pitch + (unsigned)px * 4);
}

/* The line's colour at column c (0..PH_SPAN-1), row y of h; 0 = untouched. */
static unsigned ph_color(int c, int y, int h)
{
    int d = c - PH_HALF;
    if (y < PH_CAP || y >= h - PH_CAP)
        return d == -PH_HALF || d == PH_HALF ? PH_EDGE : PH_LIME;
    return d == -2 || d == 2 ? PH_EDGE : d >= -1 && d <= 1 ? PH_LIME : 0;
}

static void playhead_paint(unsigned char *base, int page)
{
    float f = -1.0f;
    int x = 0, y0 = 0, h = 0, row, c, y;

    if (tc_deck >= 0) {
        preview_pos(NULL, &f);
        if (f < 0.0f)
            f = tc_last;
        x = tc_area_x[tc_deck] + (int)(f * (TC_AREA_W - 1));
        y0 = TC_AREA_Y;
        h = TC_AREA_H;
    } else if (rb_preview_row && (row = rb_preview_row()) >= 0 && row < 12 && DJ_ISLOADED(NULL, PREVIEW_CH)) {
        preview_pos(NULL, &f);
        if (f >= 0.0f) {
            x = PV_ROW_X + (int)(f * (PV_ROW_W - 1));
            y0 = 110 + 50 * row;
            h = PV_ROW_H;
        }
    }
    if (!h && !ph[page].valid)
        return;
    if (ph[page].valid) {   /* put back what is still ours; the driver rewrote the rest */
        int x0 = ph[page].x - PH_HALF;
        for (c = 0; c < PH_SPAN; c++)
            for (y = 0; y < ph[page].h; y++) {
                unsigned col = ph_color(c, y, ph[page].h);
                if (col && get_visual(base, x0 + c, ph[page].y0 + y) == col)
                    put_visual(base, x0 + c, ph[page].y0 + y, ph[page].saved[y * PH_SPAN + c]);
            }
        ph[page].valid = 0;
    }
    if (!h)
        return;
    for (c = 0; c < PH_SPAN; c++)
        for (y = 0; y < h; y++) {
            unsigned col = ph_color(c, y, h);
            ph[page].saved[y * PH_SPAN + c] = get_visual(base, x - PH_HALF + c, y0 + y);
            if (col)
                put_visual(base, x - PH_HALF + c, y0 + y, col);
        }
    ph[page].x = x;
    ph[page].y0 = y0;
    ph[page].h = h;
    ph[page].valid = 1;
}

void overlay_paint(int fb_fd, unsigned yoffset)
{
    static unsigned last_hash[3];
    static unsigned char hash_valid[3];
    unsigned char *base;
    unsigned off;
    unsigned hash;
    int page;
    int mode;
    int open;
    int jog;
    int fps;
    int scr = 0, led = 0, cpu = -1;
    int beat;
    char label[24];
    char name1[32];
    char name2[32];
    int i;

    release_key();
    apply_pending();
    apply_wave();
    apply_quant();
    tcue_led_apply();
    apply_link();
    ov_tcue = !(ov_shm && ov_shm->tcue_mode == TCUE_OFF);
    ov_skip = ov_shm && ov_shm->skip_mode == SKIP_BEATS;
    ov_info = ov_shm && (ov_shm->info_cfg & INFO_SET) ? ov_shm->info_cfg : INFO_DEF;
    apply_track();
    open = ov_is_open();
    if (open) {
        refresh_mode();
        refresh_quant();
        if (!ov_wave_cur)
            ov_wave_cur = read_wave();
    }
    mode = ov_mode;
    jog = jog_milli();
    beat = beat_mode();
    /* Only while the panel is open: a closed tab repaints into the live
     * scanout buffer, so it must not change every second. */
    fps = open ? fps_x10() : 0;
    if (open) {
        scr = screen_pct();
        led = led_pct();
        cpu_sample();
        cpu = cpu_pct_v;
    }
    pull1 = 0;
    pull2 = 0;
    name1[0] = '\0';
    name2[0] = '\0';
    if (open) {
        pull1 = file_exists(USB_PULL1);
        pull2 = file_exists(USB_PULL2);
        read_file(USB_NAME1, name1, sizeof(name1));
        read_file(USB_NAME2, name2, sizeof(name2));
    }
    if (!ensure_map(fb_fd))
        return;

    off = yoffset * fb_pitch;
    if (off >= fb_map_len || fb_map_len - off < 1280u * fb_pitch)
        return;
    base = (unsigned char *)fb_map + off;

    /* The rotated DirectFB path normally renders into physical page 0. Drawing
     * the overlay on every flip therefore means drawing into the framebuffer
     * while it is being scanned out, which produces a moving partial-panel
     * tear. The rot16 driver preserves these pixels, so paint each page only
     * when visible overlay state changes. */
    page = (int)(yoffset / 1280u);
    if (page < 0 || page >= 3)
        page = 0;
    beat_paint(base, page);
    count_label_paint(base);
    playhead_paint(base, page);
    hash = 2166136261u;
    hash = hash_bytes(hash, &open, sizeof(open));
    hash = hash_bytes(hash, &mode, sizeof(mode));
    hash = hash_bytes(hash, &jog, sizeof(jog));
    hash = hash_bytes(hash, &beat, sizeof(beat));
    hash = hash_bytes(hash, &fps, sizeof(fps));
    hash = hash_bytes(hash, &scr, sizeof(scr));
    hash = hash_bytes(hash, &led, sizeof(led));
    hash = hash_bytes(hash, &cpu, sizeof(cpu));
    hash = hash_bytes(hash, &ov_wave_cur, sizeof(ov_wave_cur));
    hash = hash_bytes(hash, ov_quant, sizeof(ov_quant));
    hash = hash_bytes(hash, &ov_link, sizeof(ov_link));
    hash = hash_bytes(hash, &ov_tcue, sizeof(ov_tcue));
    hash = hash_bytes(hash, &ov_skip, sizeof(ov_skip));
    hash = hash_bytes(hash, &ov_info, sizeof(ov_info));
    hash = hash_bytes(hash, &ov_scroll, sizeof(ov_scroll));
    hash = hash_bytes(hash, &eject_arm, sizeof(eject_arm));
    hash = hash_bytes(hash, &pull1, sizeof(pull1));
    hash = hash_bytes(hash, &pull2, sizeof(pull2));
    hash = hash_bytes(hash, &power_arm, sizeof(power_arm));
    hash = hash_string(hash, name1);
    hash = hash_string(hash, name2);
    if (hash_valid[page] && last_hash[page] == hash)
        return;

    fill_visual(base, TAB_X, TAB_Y, TAB_W, TAB_H, COL_EDGE);
    fill_visual(base, TAB_X + 1, TAB_Y + 1, TAB_W - 2, TAB_H - 2, COL_TAB);
    draw_text_centered(base, TAB_X, TAB_Y, TAB_W, TAB_H, "MOD", COL_TEXT);
    if (!open) {
        last_hash[page] = hash;
        hash_valid[page] = 1;
        return;
    }

    fill_visual(base, PAN_X, PAN_Y, PAN_W, PAN_VIS_H, COL_EDGE);
    fill_visual(base, PAN_X + 2, PAN_Y + 2, PAN_W - 4, PAN_VIS_H - 4, COL_PANEL);
    if (SCROLL_MAX > 0)                  /* where the view sits in the whole list */
        fill_visual(base, PAN_X + PAN_W - 5, PAN_Y + 2 + ov_scroll * (PAN_VIS_H - 4) / PAN_H, 3,
                    (PAN_VIS_H - 4) * PAN_VIS_H / PAN_H, COL_TITLE);
    clip_y0 = PAN_Y + 2;
    clip_y1 = PAN_Y + PAN_VIS_H - 2;

    draw_label(base, MODE_Y, "MODE");
    fill_visual(base, VAL_X, MODE_Y, VAL_W, ROW_H, COL_ON);
    draw_text_centered(base, VAL_X, MODE_Y, VAL_W, ROW_H, mode_name(mode), COL_TEXT);

    jog_label(label, jog);
    draw_label(base, JOG_Y, "JOG");
    fill_visual(base, STEP_DN_X, JOG_Y, STEP_W, ROW_H, COL_BTN);
    draw_minus_mark(base, STEP_DN_X, JOG_Y, STEP_W, ROW_H);
    fill_visual(base, STEP_UP_X, JOG_Y, STEP_W, ROW_H, COL_BTN);
    draw_plus_mark(base, STEP_UP_X, JOG_Y, STEP_W, ROW_H);
    draw_text_centered(base, STEP_DN_X + STEP_W, JOG_Y,
                       STEP_UP_X - (STEP_DN_X + STEP_W), ROW_H, label, COL_TEXT);

    draw_label(base, WAVE_Y, "WAVE");
    {
        static const int wave_col[3] = {1, 3, 4};
        static const char *wave_name[3] = {"BLUE", "RGB", "3BAND"};
        int i;
        for (i = 0; i < 3; i++) {
            int x = WAVE_X + i * (WAVE_BTN + WAVE_GAP);
            fill_visual(base, x, WAVE_Y, WAVE_BTN, ROW_H,
                        ov_wave_cur == wave_col[i] ? COL_ON : COL_BTN);
            draw_text_centered(base, x, WAVE_Y, WAVE_BTN, ROW_H, wave_name[i], COL_TEXT);
        }
    }

    draw_label(base, BEAT_Y, "BEAT");
    {
        static const int beat_val[3] = {BEAT_OFF, BEAT_BARS, BEAT_DRIFT};
        static const char *beat_name[3] = {"OFF", "BARS", "DRIFT"};
        int i;
        for (i = 0; i < 3; i++) {
            int x = WAVE_X + i * (WAVE_BTN + WAVE_GAP);
            fill_visual(base, x, BEAT_Y, WAVE_BTN, ROW_H,
                        beat == beat_val[i] ? COL_ON : COL_BTN);
            draw_text_centered(base, x, BEAT_Y, WAVE_BTN, ROW_H, beat_name[i], COL_TEXT);
        }
    }

    draw_label(base, TRACK_Y, "TRACK");
    {
        static const char *track_name[3] = {"TAG", "TAGS", "FIND"};
        int i;
        for (i = 0; i < 3; i++) {
            int x = WAVE_X + i * (WAVE_BTN + WAVE_GAP);
            fill_visual(base, x, TRACK_Y, WAVE_BTN, ROW_H, COL_BTN);
            draw_text_centered(base, x, TRACK_Y, WAVE_BTN, ROW_H, track_name[i], COL_TEXT);
        }
    }

    draw_label(base, QUANT_Y, "QUANT");
    fill_visual(base, QUANT_ON_X, QUANT_Y, QUANT_HALF, ROW_H,
                (ov_quant[0] && ov_quant[1]) ? COL_ON : COL_BTN);
    draw_text_centered(base, QUANT_ON_X, QUANT_Y, QUANT_HALF, ROW_H, "ON", COL_TEXT);
    fill_visual(base, QUANT_OFF_X, QUANT_Y, QUANT_HALF, ROW_H,
                (!ov_quant[0] && !ov_quant[1]) ? COL_ON : COL_BTN);
    draw_text_centered(base, QUANT_OFF_X, QUANT_Y, QUANT_HALF, ROW_H, "OFF", COL_TEXT);

    draw_label(base, LINK_Y, "LINK");
    fill_visual(base, QUANT_ON_X, LINK_Y, QUANT_HALF, ROW_H, ov_link ? COL_ON : COL_BTN);
    draw_text_centered(base, QUANT_ON_X, LINK_Y, QUANT_HALF, ROW_H, "ON", COL_TEXT);
    fill_visual(base, QUANT_OFF_X, LINK_Y, QUANT_HALF, ROW_H, !ov_link ? COL_ON : COL_BTN);
    draw_text_centered(base, QUANT_OFF_X, LINK_Y, QUANT_HALF, ROW_H, "OFF", COL_TEXT);

    draw_label(base, TCUE_Y, "TCUE");
    fill_visual(base, QUANT_ON_X, TCUE_Y, QUANT_HALF, ROW_H, ov_tcue ? COL_ON : COL_BTN);
    draw_text_centered(base, QUANT_ON_X, TCUE_Y, QUANT_HALF, ROW_H, "ON", COL_TEXT);
    fill_visual(base, QUANT_OFF_X, TCUE_Y, QUANT_HALF, ROW_H, !ov_tcue ? COL_ON : COL_BTN);
    draw_text_centered(base, QUANT_OFF_X, TCUE_Y, QUANT_HALF, ROW_H, "OFF", COL_TEXT);

    draw_label(base, INFO_Y, "INFO");
    fill_visual(base, QUANT_ON_X, INFO_Y, QUANT_HALF, ROW_H, ov_info & INFO_OFF ? COL_ON : COL_BTN);
    draw_text_centered(base, QUANT_ON_X, INFO_Y, QUANT_HALF, ROW_H, "OFF", COL_TEXT);
    fill_visual(base, QUANT_OFF_X, INFO_Y, QUANT_HALF, ROW_H, !(ov_info & INFO_OFF) ? COL_ON : COL_BTN);
    draw_text_centered(base, QUANT_OFF_X, INFO_Y, QUANT_HALF, ROW_H, "ON", COL_TEXT);
    draw_label(base, ROWS_Y, "ROWS");
    for (i = 0; i < 4; i++) {
        static const char *info_name[4] = {"SRC", "KEY", "CUE", "LOOP"};
        int x = VAL_X + i * (INFO_BTN + INFO_GAP);
        fill_visual(base, x, ROWS_Y, INFO_BTN, ROW_H, ov_info & (INFO_SRC << i) ? COL_ON : COL_BTN);
        draw_text_centered(base, x, ROWS_Y, INFO_BTN, ROW_H, info_name[i], COL_TEXT);
    }
    draw_label(base, CNT_Y, "COUNT");
    fill_visual(base, QUANT_ON_X, CNT_Y, QUANT_HALF, ROW_H, !(ov_info & INFO_BEATS) ? COL_ON : COL_BTN);
    draw_text_centered(base, QUANT_ON_X, CNT_Y, QUANT_HALF, ROW_H, "BARS", COL_TEXT);
    fill_visual(base, QUANT_OFF_X, CNT_Y, QUANT_HALF, ROW_H, ov_info & INFO_BEATS ? COL_ON : COL_BTN);
    draw_text_centered(base, QUANT_OFF_X, CNT_Y, QUANT_HALF, ROW_H, "BEATS", COL_TEXT);

    draw_label(base, SKIP_Y, "SKIP");
    fill_visual(base, QUANT_ON_X, SKIP_Y, QUANT_HALF, ROW_H, !ov_skip ? COL_ON : COL_BTN);
    draw_text_centered(base, QUANT_ON_X, SKIP_Y, QUANT_HALF, ROW_H, "SEARCH", COL_TEXT);
    fill_visual(base, QUANT_OFF_X, SKIP_Y, QUANT_HALF, ROW_H, ov_skip ? COL_ON : COL_BTN);
    draw_text_centered(base, QUANT_OFF_X, SKIP_Y, QUANT_HALF, ROW_H, "16 BEATS", COL_TEXT);

    {
        int pw = PAN_W - 12;
        int th = 7 * SCALE;
        int ty = PWR_Y + (ROW_H - th) / 2;
        fill_visual(base, PAN_X + 6, PWR_Y, pw, ROW_H, power_arm ? COL_ON : COL_BTN);
        draw_text(base, LAB_X, ty, "POWER", COL_TEXT);
        if (power_arm) {
            int tw = text_width("YES");
            draw_text(base, VAL_X + VAL_W - tw - 8, ty, "YES", COL_TEXT);
        }
    }

    draw_label(base, SCR_Y, "SCREEN");
    fill_visual(base, STEP_DN_X, SCR_Y, STEP_W, ROW_H, COL_BTN);
    draw_minus_mark(base, STEP_DN_X, SCR_Y, STEP_W, ROW_H);
    fill_visual(base, STEP_UP_X, SCR_Y, STEP_W, ROW_H, COL_BTN);
    draw_plus_mark(base, STEP_UP_X, SCR_Y, STEP_W, ROW_H);
    pct_label(label, scr);
    draw_text_centered(base, STEP_DN_X + STEP_W, SCR_Y,
                       STEP_UP_X - (STEP_DN_X + STEP_W), ROW_H, label, COL_TEXT);

    draw_label(base, LED_Y, "LEDS");
    fill_visual(base, STEP_DN_X, LED_Y, STEP_W, ROW_H, COL_BTN);
    draw_minus_mark(base, STEP_DN_X, LED_Y, STEP_W, ROW_H);
    fill_visual(base, STEP_UP_X, LED_Y, STEP_W, ROW_H, COL_BTN);
    draw_plus_mark(base, STEP_UP_X, LED_Y, STEP_W, ROW_H);
    pct_label(label, led);
    draw_text_centered(base, STEP_DN_X + STEP_W, LED_Y,
                       STEP_UP_X - (STEP_DN_X + STEP_W), ROW_H, label, COL_TEXT);

    draw_label(base, USB_Y, "EJECT");
    fill_visual(base, USB_L_X, USB_Y, USB_HALF, ROW_H,
                (eject_arm == 1 || pull1) ? COL_ON : COL_BTN);
    if (eject_arm == 1 && !pull1)
        draw_text_centered(base, USB_L_X, USB_Y, USB_HALF, ROW_H, "YES", COL_TEXT);
    else
        draw_volume(base, USB_L_X, USB_Y, USB_HALF, ROW_H, name1, "USB 1", pull1);
    fill_visual(base, USB_R_X, USB_Y, USB_HALF, ROW_H,
                (eject_arm == 2 || pull2) ? COL_ON : COL_BTN);
    if (eject_arm == 2 && !pull2)
        draw_text_centered(base, USB_R_X, USB_Y, USB_HALF, ROW_H, "YES", COL_TEXT);
    else
        draw_volume(base, USB_R_X, USB_Y, USB_HALF, ROW_H, name2, "USB 2", pull2);

    draw_label(base, STATS_Y, "STATS");
    {
        /* "CPU 38%  FPS 60.4" */
        char *d = label;
        strcpy(d, "CPU ");
        d += 4;
        if (cpu >= 0) {   /* first sample lands a second after opening */
            d = put_uint(d, cpu);
            *d++ = '%';
        }
        strcpy(d, "  FPS ");
        d += 6;
        fps_label(d, fps);
    }
    fill_visual(base, VAL_X, STATS_Y, VAL_W, ROW_H, COL_BTN);
    draw_text_centered(base, VAL_X, STATS_Y, VAL_W, ROW_H, label, COL_TEXT);
    clip_y0 = 0;
    clip_y1 = 800;
    last_hash[page] = hash;
    hash_valid[page] = 1;
}

int overlay_touch(int down, int was_down, int lx, int ly)
{
    int vx = 1279 - lx;
    int vy = ly;
    int on_tab, on_panel, on_mode, on_jog_dn, on_jog_up;
    int on_usb1, on_usb2, on_blue, on_rgb, on_band, on_quant_on, on_quant_off, on_power;
    int on_tag, on_tags, on_find, on_link_on, on_link_off, on_tcue_on, on_tcue_off, on_skip_srch, on_skip_beats;
    int on_beat_off, on_beat_bars, on_beat_drift;
    int on_scr_dn, on_scr_up, on_led_dn, on_led_up, on_cnt_bars, on_cnt_beats, on_info_off, on_info_on;
    int info_hit = -1;
    static int press_x, press_y, press_scroll, pressed, dragging;
    int fresh;
    unsigned long long now;
    static unsigned long long last_ev_ms;

    /* A missed finger-up leaves the contact looking held. Reports keep
     * arriving during a real hold; a quiet gap means the finger left, so
     * the next report is a new tap even on the same button. */
    now = mono_ms();
    if (was_down < 0)
        was_down = 0;
    fresh = down && !was_down;
    if (down && was_down && ov_grab && last_ev_ms && now - last_ev_ms > 200)
        fresh = 1;
    last_ev_ms = now;

    /* A scrolling panel acts on release: a finger that moves first scrolls it instead. */
    if (SCROLL_MAX > 0 && ov_is_open()) {
        if (fresh && in_rect(vx, vy, PAN_X, PAN_Y, PAN_W, PAN_VIS_H)) {
            pressed = 1;
            dragging = 0;
            press_x = vx;
            press_y = vy;
            press_scroll = ov_scroll;
            ov_grab = 1;
            return 1;
        }
        if (pressed) {
            if (down) {
                int sc = press_scroll - (vy - press_y);
                if (dragging || vy - press_y > 8 || press_y - vy > 8) {
                    dragging = 1;
                    ov_scroll = sc < 0 ? 0 : sc > SCROLL_MAX ? SCROLL_MAX : sc;
                }
                return 1;
            }
            pressed = 0;
            if (dragging) {
                ov_grab = 0;
                return 1;
            }
            vx = press_x;
            vy = press_y;
            fresh = 1;
        }
    }

    if (fresh) {
        on_tab = in_rect(vx, vy, TAB_X, TAB_Y, TAB_W, TAB_H);
        on_panel = in_rect(vx, vy, PAN_X, PAN_Y, PAN_W, PAN_VIS_H);
        on_mode = in_rect(vx, vy, VAL_X, MODE_Y, VAL_W, ROW_H);
        on_jog_dn = in_rect(vx, vy, STEP_DN_X, JOG_Y, STEP_W, ROW_H);
        on_jog_up = in_rect(vx, vy, STEP_UP_X, JOG_Y, STEP_W, ROW_H);
        on_blue = in_rect(vx, vy, WAVE_X, WAVE_Y, WAVE_BTN, ROW_H);
        on_rgb = in_rect(vx, vy, WAVE_X + (WAVE_BTN + WAVE_GAP), WAVE_Y, WAVE_BTN, ROW_H);
        on_band = in_rect(vx, vy, WAVE_X + 2 * (WAVE_BTN + WAVE_GAP), WAVE_Y, WAVE_BTN, ROW_H);
        on_beat_off = in_rect(vx, vy, WAVE_X, BEAT_Y, WAVE_BTN, ROW_H);
        on_beat_bars = in_rect(vx, vy, WAVE_X + (WAVE_BTN + WAVE_GAP), BEAT_Y, WAVE_BTN, ROW_H);
        on_beat_drift = in_rect(vx, vy, WAVE_X + 2 * (WAVE_BTN + WAVE_GAP), BEAT_Y, WAVE_BTN, ROW_H);
        on_quant_on = in_rect(vx, vy, QUANT_ON_X, QUANT_Y, QUANT_HALF, ROW_H);
        on_quant_off = in_rect(vx, vy, QUANT_OFF_X, QUANT_Y, QUANT_HALF, ROW_H);
        on_link_on = in_rect(vx, vy, QUANT_ON_X, LINK_Y, QUANT_HALF, ROW_H);
        on_link_off = in_rect(vx, vy, QUANT_OFF_X, LINK_Y, QUANT_HALF, ROW_H);
        on_tcue_on = in_rect(vx, vy, QUANT_ON_X, TCUE_Y, QUANT_HALF, ROW_H);
        on_tcue_off = in_rect(vx, vy, QUANT_OFF_X, TCUE_Y, QUANT_HALF, ROW_H);
        on_skip_srch = in_rect(vx, vy, QUANT_ON_X, SKIP_Y, QUANT_HALF, ROW_H);
        on_skip_beats = in_rect(vx, vy, QUANT_OFF_X, SKIP_Y, QUANT_HALF, ROW_H);
        on_cnt_bars = in_rect(vx, vy, QUANT_ON_X, CNT_Y, QUANT_HALF, ROW_H);
        on_cnt_beats = in_rect(vx, vy, QUANT_OFF_X, CNT_Y, QUANT_HALF, ROW_H);
        on_info_off = in_rect(vx, vy, QUANT_ON_X, INFO_Y, QUANT_HALF, ROW_H);
        on_info_on = in_rect(vx, vy, QUANT_OFF_X, INFO_Y, QUANT_HALF, ROW_H);
        for (info_hit = 3; info_hit >= 0; info_hit--)
            if (in_rect(vx, vy, VAL_X + info_hit * (INFO_BTN + INFO_GAP), ROWS_Y, INFO_BTN, ROW_H))
                break;
        on_tag = in_rect(vx, vy, WAVE_X, TRACK_Y, WAVE_BTN, ROW_H);
        on_tags = in_rect(vx, vy, WAVE_X + (WAVE_BTN + WAVE_GAP), TRACK_Y, WAVE_BTN, ROW_H);
        on_find = in_rect(vx, vy, WAVE_X + 2 * (WAVE_BTN + WAVE_GAP), TRACK_Y, WAVE_BTN, ROW_H);
        on_usb1 = in_rect(vx, vy, USB_L_X, USB_Y, USB_HALF, ROW_H);
        on_usb2 = in_rect(vx, vy, USB_R_X, USB_Y, USB_HALF, ROW_H);
        on_power = in_rect(vx, vy, PAN_X + 6, PWR_Y, PAN_W - 12, ROW_H);
        on_scr_dn = in_rect(vx, vy, STEP_DN_X, SCR_Y, STEP_W, ROW_H);
        on_scr_up = in_rect(vx, vy, STEP_UP_X, SCR_Y, STEP_W, ROW_H);
        on_led_dn = in_rect(vx, vy, STEP_DN_X, LED_Y, STEP_W, ROW_H);
        on_led_up = in_rect(vx, vy, STEP_UP_X, LED_Y, STEP_W, ROW_H);
        if (!ov_is_open()) {
            ov_grab = on_tab;
            if (on_tab) {
                ov_set_open(1);
                ov_mode_known = 0;
                ov_quant_known = 0;
                ov_wave_cur = 0;
                eject_arm = 0;
                power_arm = 0;
            }
        } else {
            ov_grab = 1;
            if (on_tab) {
                ov_set_open(0);
                eject_arm = 0;
                power_arm = 0;
            }
            else if (on_mode) {
                int next = ov_mode + 1;
                if (next > 3)
                    next = 0;
                ov_mode = next;
                ov_mode_known = 1;
                ov_apply_mode = next;
                __sync_synchronize();
                ov_apply_seq++;
            } else if (on_jog_dn)
                nudge_jog(-1);
            else if (on_jog_up)
                nudge_jog(1);
            else if (on_scr_dn)
                nudge_screen(-1);
            else if (on_scr_up)
                nudge_screen(1);
            else if (on_led_dn)
                nudge_led(-1);
            else if (on_led_up)
                nudge_led(1);
            else if (on_blue)
                request_wave(1);
            else if (on_rgb)
                request_wave(3);
            else if (on_band)
                request_wave(4);
            else if (on_beat_off)
                set_beat_mode(BEAT_OFF);
            else if (on_beat_bars)
                set_beat_mode(BEAT_BARS);
            else if (on_beat_drift)
                set_beat_mode(BEAT_DRIFT);
            else if (on_quant_on)
                request_quant(1);
            else if (on_quant_off)
                request_quant(0);
            else if (on_link_on)
                request_link(1);
            else if (on_link_off)
                request_link(0);
            else if (on_tcue_on)
                request_tcue(1);
            else if (on_tcue_off)
                request_tcue(0);
            else if (on_skip_srch)
                request_skip(0);
            else if (on_skip_beats)
                request_skip(1);
            else if (on_info_off && !(ov_info & INFO_OFF))
                request_info(INFO_OFF);
            else if (on_info_on && (ov_info & INFO_OFF))
                request_info(INFO_OFF);
            else if (info_hit >= 0)
                request_info(INFO_SRC << info_hit);
            else if (on_cnt_bars && (ov_info & INFO_BEATS))
                request_info(INFO_BEATS);
            else if (on_cnt_beats && !(ov_info & INFO_BEATS))
                request_info(INFO_BEATS);
            else if (on_tag)
                request_track(1);
            else if (on_tags) {
                request_track(2);
                ov_set_open(0);
                eject_arm = 0;
                power_arm = 0;
            } else if (on_find) {
                request_track(3);
                ov_set_open(0);
                eject_arm = 0;
                power_arm = 0;
            } else if (on_usb1 && !pull1) {
                if (eject_arm == 1) {
                    eject_request(1);
                    eject_arm = 0;
                } else
                    eject_arm = 1;
            } else if (on_usb2 && !pull2) {
                if (eject_arm == 2) {
                    eject_request(2);
                    eject_arm = 0;
                } else
                    eject_arm = 2;
            } else if (on_power) {
                if (power_arm)
                    power_request();
                else
                    power_arm = 1;
            } else if (!on_panel) {
                ov_set_open(0);
                eject_arm = 0;
                power_arm = 0;
            }
        }
    }
    if (!ov_grab)
        return 0;
    if (!down)
        ov_grab = 0;
    return 1;
}
