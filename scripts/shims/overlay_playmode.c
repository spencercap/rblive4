/* On-screen MOD overlay for the SC Live 4 port.
 *
 * rbp owns the whole rekordbox UI, so this draws after the rotated frame
 * and steals taps before they reach rbp. A MOD tab at the top center of
 * the upright UI opens a panel. Each row names the setting on the left,
 * then the value: MODE (SINGLE, CONTINUE, REPEAT, ALL REPEAT), JOG
 * (− percent +), WAVE (BLUE, RGB, 3BAND), QUANT (ON, OFF), TRACK
 * (TAG, TAGS, FIND), SCREEN (− backlight percent +), LEDS (− panel LED
 * percent +, applied by knobshim), EJECT, STATS (read-only: CPU load percent
 * and display frames per second, counted at each FBIOPAN), and POWER.
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

/* Name on the left, value on the right. Ten rows under the MOD tab:
 * PAN_H = 2 * PAD + rows * ROW_H + (rows - 1) * ROW_GAP. */
#define PAN_W 340
#define PAN_H 452
#define PAN_X ((1280 - PAN_W) / 2)
#define PAN_Y 48

#define PAD 10
#define LAB_W 72
#define ROW_H 36
#define ROW_GAP 8
#define LAB_X (PAN_X + PAD)
#define VAL_X (LAB_X + LAB_W)
#define VAL_W (PAN_W - PAD - (VAL_X - PAN_X))
#define ROW_Y(i) (PAN_Y + PAD + (i) * (ROW_H + ROW_GAP))

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
#define QUANT_Y ROW_Y(3)
#define QUANT_GAP 8
#define QUANT_HALF ((VAL_W - QUANT_GAP) / 2)
#define QUANT_ON_X VAL_X
#define QUANT_OFF_X (VAL_X + QUANT_HALF + QUANT_GAP)
#define TRACK_Y ROW_Y(4)
#define SCR_Y  ROW_Y(5)
#define LED_Y  ROW_Y(6)
#define USB_Y  ROW_Y(7)
#define USB_GAP 8
#define USB_HALF ((VAL_W - USB_GAP) / 2)
#define USB_L_X VAL_X
#define USB_R_X (VAL_X + USB_HALF + USB_GAP)
#define STATS_Y ROW_Y(8)
/* POWER is always the last row; add new rows above it. */
#define PWR_Y  ROW_Y(9)

#define COL_TAB    0xff1c2128u
#define COL_PANEL  0xff121418u
#define COL_BTN    0xff2a3038u
#define COL_ON     0xff1b7a3au
#define COL_TEXT   0xfff2f2f2u
#define COL_TITLE  0xffb7bdc6u
#define COL_EDGE   0xff3a424cu

#define SCALE 2

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
    ov_shm->pan_h = PAN_H;
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
    if (px < 0 || py < 0 || px >= 800 || py >= 1280)
        return;
    p = base + (unsigned)py * fb_pitch + (unsigned)px * 4;
    *(unsigned *)p = color;
}

static void fill_visual(unsigned char *base, int x, int y, int w, int h, unsigned color)
{
    int iy, ix;
    for (iy = 0; iy < h; iy++)
        for (ix = 0; ix < w; ix++)
            put_visual(base, x + ix, y + iy, color);
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
    char label[24];
    char name1[32];
    char name2[32];

    release_key();
    apply_pending();
    apply_wave();
    apply_quant();
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
    hash = 2166136261u;
    hash = hash_bytes(hash, &open, sizeof(open));
    hash = hash_bytes(hash, &mode, sizeof(mode));
    hash = hash_bytes(hash, &jog, sizeof(jog));
    hash = hash_bytes(hash, &fps, sizeof(fps));
    hash = hash_bytes(hash, &scr, sizeof(scr));
    hash = hash_bytes(hash, &led, sizeof(led));
    hash = hash_bytes(hash, &cpu, sizeof(cpu));
    hash = hash_bytes(hash, &ov_wave_cur, sizeof(ov_wave_cur));
    hash = hash_bytes(hash, ov_quant, sizeof(ov_quant));
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

    fill_visual(base, PAN_X, PAN_Y, PAN_W, PAN_H, COL_EDGE);
    fill_visual(base, PAN_X + 2, PAN_Y + 2, PAN_W - 4, PAN_H - 4, COL_PANEL);

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
    last_hash[page] = hash;
    hash_valid[page] = 1;
}

int overlay_touch(int down, int was_down, int lx, int ly)
{
    int vx = 1279 - lx;
    int vy = ly;
    int on_tab, on_panel, on_mode, on_jog_dn, on_jog_up;
    int on_usb1, on_usb2, on_blue, on_rgb, on_band, on_quant_on, on_quant_off, on_power;
    int on_tag, on_tags, on_find;
    int on_scr_dn, on_scr_up, on_led_dn, on_led_up;
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

    if (fresh) {
        on_tab = in_rect(vx, vy, TAB_X, TAB_Y, TAB_W, TAB_H);
        on_panel = in_rect(vx, vy, PAN_X, PAN_Y, PAN_W, PAN_H);
        on_mode = in_rect(vx, vy, VAL_X, MODE_Y, VAL_W, ROW_H);
        on_jog_dn = in_rect(vx, vy, STEP_DN_X, JOG_Y, STEP_W, ROW_H);
        on_jog_up = in_rect(vx, vy, STEP_UP_X, JOG_Y, STEP_W, ROW_H);
        on_blue = in_rect(vx, vy, WAVE_X, WAVE_Y, WAVE_BTN, ROW_H);
        on_rgb = in_rect(vx, vy, WAVE_X + (WAVE_BTN + WAVE_GAP), WAVE_Y, WAVE_BTN, ROW_H);
        on_band = in_rect(vx, vy, WAVE_X + 2 * (WAVE_BTN + WAVE_GAP), WAVE_Y, WAVE_BTN, ROW_H);
        on_quant_on = in_rect(vx, vy, QUANT_ON_X, QUANT_Y, QUANT_HALF, ROW_H);
        on_quant_off = in_rect(vx, vy, QUANT_OFF_X, QUANT_Y, QUANT_HALF, ROW_H);
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
            else if (on_quant_on)
                request_quant(1);
            else if (on_quant_off)
                request_quant(0);
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
