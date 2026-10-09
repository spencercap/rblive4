#ifndef OVERLAY_PLAYMODE_H
#define OVERLAY_PLAYMODE_H

/* Shared with the fbdev rotator so it can leave these pixels untouched.
 * The rotator otherwise repaints the whole panel every frame and the
 * button flickers. Layout is upright 1280x800, which is source (x, y)
 * under DFB_ROTATE=left. */
struct rb_overlay_shm {
    volatile int open;
    int tab_x, tab_y, tab_w, tab_h;
    int pan_x, pan_y, pan_w, pan_h;
    /* 1000 = the unscaled wheel. knobshim multiplies each jog step by
     * this / 1000. Kept across a player restart when /tmp still exists. */
    volatile int jog_gain_milli;
    /* Displayed frames per second, x10, measured at FBIOPAN by fbshim.
     * rot16 and knobshim map the older, shorter struct. */
    volatile int fps_x10;
    /* Screen backlight and panel LED brightness, percent. 0 = not set yet:
     * fbshim fills screen_pct from the current backlight, and LEDs run at
     * full. Kept in /tmp, so a player restart keeps them; a reboot does not. */
    volatile int screen_pct;
    volatile int led_pct;
    /* Beat meter in the top bar, right of MOD. 0 = not set yet = BARS. */
    volatile int beat_mode;
    /* LINK CUE for Track Preview (preview audio to the headphones).
     * 0 = not set yet = ON, 1 = ON, 2 = OFF. */
    volatile int link_cue;
    /* Touch Cue on the deck overview waveforms, applied by knobshim2.
     * 0 = not set yet = ON, 1 = ON, 2 = OFF. */
    volatile int tcue_mode;
    /* What the deck SEARCH < > buttons do: 0 = not set yet = SEARCH (scan), 1 = SEARCH, 2 = jump 16 beats.
     */
    volatile int skip_mode;
    /* What the two deck info boxes left of the waveforms show (INFO_*). 0 = not set yet = INFO_DEF.
     * Last field: rot16 and knobshim map the older, shorter struct. */
    volatile unsigned info_cfg;
    /* Written by knobshim2 each time the info boxes update: CLOCK_MONOTONIC ms, low 32 bits. */
    volatile unsigned deck_ms;
};

#define INFO_SET   0x80000000u   /* set once the MOD INFO row has been used */
#define INFO_SRC   1u            /* rows shown, in box order: source, key, count, loop size */
#define INFO_KEY   2u
#define INFO_CNT   4u
#define INFO_LOOP  8u
#define INFO_BEATS 16u           /* the count is in beats, not bars */
#define INFO_DEF   (INFO_KEY | INFO_CNT | INFO_LOOP)

#define LINK_ON  1
#define LINK_OFF 2
#define TCUE_ON  1
#define TCUE_OFF 2
#define SKIP_SEARCH 1
#define SKIP_BEATS   2

#define BEAT_OFF   1
#define BEAT_BARS  2
#define BEAT_DRIFT 3

#define JOG_GAIN_MIN  200   /* 20%  */
#define JOG_GAIN_MAX  2000  /* 200% */
#define JOG_GAIN_STEP 100   /* 10%  */
#define JOG_GAIN_DEF  400   /* 40%, shown on the MOD tab */

#define PCT_STEP        10
#define SCREEN_PCT_MIN  10   /* never fully dark: the panel is the only way back */
#define LED_PCT_MIN     10
#define PCT_MAX         100

#define RB_OVERLAY_SHM "/tmp/rb-overlay"

/* Touch sample in the same lx/ly space fbshim forwards to rbp
 * (lx = physical y, ly = physical x, DFB_ROTATE=left).
 * Returns 1 when the sample belongs to the overlay and must not be
 * forwarded. was_down is the previous sample's finger state. */
int overlay_touch(int down, int was_down, int lx, int ly);

/* Touch Cue on the deck overview waveforms: same sample, called after overlay_touch declined it.
 * Returns 1 when a Touch Cue took the sample. */
int overlay_tcue_touch(int down, int lx, int ly);

/* Draw the overlay onto the physical framebuffer that was just rotated.
 * yoffset is the FBIOPAN y offset of that buffer. */
void overlay_paint(int fb_fd, unsigned yoffset);

/* Count one displayed frame. Call once per FBIOPAN. */
void overlay_frame(void);

#endif
