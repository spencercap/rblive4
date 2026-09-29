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
};

#define JOG_GAIN_MIN  200   /* 20%  */
#define JOG_GAIN_MAX  2000  /* 200% */
#define JOG_GAIN_STEP 100   /* 10%  */
#define JOG_GAIN_DEF  400   /* 40%, shown on the MOD tab */

#define RB_OVERLAY_SHM "/tmp/rb-overlay"

/* Touch sample in the same lx/ly space fbshim forwards to rbp
 * (lx = physical y, ly = physical x, DFB_ROTATE=left).
 * Returns 1 when the sample belongs to the overlay and must not be
 * forwarded. was_down is the previous sample's finger state. */
int overlay_touch(int down, int was_down, int lx, int ly);

/* Draw the overlay onto the physical framebuffer that was just rotated.
 * yoffset is the FBIOPAN y offset of that buffer. */
void overlay_paint(int fb_fd, unsigned yoffset);

#endif
