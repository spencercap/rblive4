# 06 — Display

The rekordbox UI renders full-screen on the 800×1280 panel, correctly rotated.

## Facts (live)

* `/dev/fb0` = `rockchipdrmfb`, **800×1280 portrait**, 32 bpp, stride 3200,
  triple-buffered (`virtual_size = 800,3840`).
* DSI-1 connector, `modes: 800x1280`. Mali lib bind-mounted at
  `/usr/lib/libmali.so.14.0`.

## The stack

```
 rbp  ──renders RGB565 1280×800──►  DirectFB 1.4.16 (RX3 libs)
                                        │
                          patched libdirectfb_fbdev.so (rot16)
                          • forces the real fb format, serialises ioctls
                          • rotates 1280×800 RGB565 → 800×1280 RGB32
                            (3 threads, changed tiles only)
                                        ▼
                              /dev/fb0 (rockchipdrmfb), FBIOPAN waits for vblank
 LD_PRELOAD fbshim.so: reports a 1280×800 RGB565 logical fb, counts frames,
                       skips rbp's own 16 ms frame limiter
```

On device, `/tmp/dfbdig9.log` contains
`ROTINIT: real_fb=800x1280 pitch=3200 (orig_var=1280x800)`.

## Required: `directfbrc`

`usr/etc/directfbrc` (and `/etc/directfbrc`) **must** contain:

```
no-hardware
no-cursor
system=fbdev
fbdev=/dev/fb0
```

Without `no-hardware`, DirectFB takes the GPU/dri path and **oopses the
kernel**; the SC Live 4 boots with `panic_on_oops=1`, so the unit reboots.
`scripts/build-chroot.sh` writes this file.

## Artifacts

All supplied by the shared RK3288 display build (see
[`scripts/build-chroot.sh`](../scripts/build-chroot.sh)):

| Piece | Source |
|---|---|
| DirectFB 1.4.16 core libs | DirectFB build tree |
| patched fbdev module | `libdirectfb_fbdev-rot16.so` |
| `libdirectfb_linux_input.so`, `libdirectfbwm_default.so` | display build modules |

Rotation is selected at runtime with `DFB_ROTATE=left`.

## Notes

* Do **not** set `layer-size` or `layer-rotate` in `directfbrc`.
* DirectFB's layer runs `FRONTONLY` and keeps the real `yres_virtual`. The fb
  itself does pan (`ypanstep` 1, 3 pages of 1280 rows). The rotation path
  cycles through those pages itself (see Frame rate, item 6).
* Rotation direction: `DFB_ROTATE=left` (90° CCW). Wrong value = UI sideways.

## Frame rate

The display runs at **60 fps**, locked to the panel. The DSI mode is
`800x1280 73000 kHz, htotal 925, vtotal 1314`, which is 60.06 Hz. Until
2026-10-01 it ran at 15 fps, then 30, and the waveforms looked choppy.

### Where a frame's time goes

`gui_task` draws a frame, then DirectFB flips it. On this port the flip is a
software rotate into an fb page, then `FBIOPAN_DISPLAY`. On `rockchipdrmfb`
the pan is an atomic commit that **blocks until the next vblank**, so the pan
alone paces the loop at 60 Hz. Nothing else should wait.

| Step | Now | Before |
|---|---|---|
| rbp draws (`DS_HW_UpdateScreen`) | ~6 ms | ~7 ms, plus a sleep to 16 ms |
| rotate + RGB565→RGB32 | ~5 ms | 12 ms, plus a 6 MB debug dump |
| wait for vblank | the rest of the 16.7 ms | 2 vblank waits, plus a shim sleep |

### What was wrong

Each of these cost at least one refresh per frame:

1. **Debug writes in the fbdev driver.** Every flip did `fopen`/`fprintf`
   into `/tmp/dfbdig9.log` (one `flip:` and two `pool:` lines) and wrote the
   whole 6 MB `rot_surface` to `/tmp/rot_surface.dump`. All removed. Only the
   one-time `ROTINIT` line is left.
2. **Two vblank waits.** `primaryFlipRegion` called `FBIO_WAITFORVSYNC` for
   `DSFLIP_WAITFORSYNC`, and then the pan waited for a second vblank. With
   rotation on, the driver now skips the first wait.
3. **The shim's 60 fps sleep.** fbshim slept until 16.7 ms after the previous
   pan *started*. That time included the pan's own vblank wait, so the sleep
   ran past the next vblank. It is now a floor of 8 ms from when the previous
   pan *returned*. That only matters if a pan ever comes back without waiting.
4. **rbp's own frame limiter.** `DS_HW_UpdateScreen` (`0x1a6528`) runs
   `usleep(16000 − elapsed)` against the `gettimeofday` it took after the
   previous flip. On the RX3 the flip came after that. Here the rotate and
   pan come after the sleep, so every frame passed 16.7 ms and waited for the
   next vblank: 30 fps. fbshim overrides `usleep` and returns at once only
   when the caller is that one call site (return address `0x1a6920`,
   `rbp-audio` is not PIE). Every other `usleep` goes to the kernel.
5. **A slow rotate.** It was one thread, one pixel at a time, with
   `ov_keep()` per pixel and column-strided writes. The RGB16 left-rotate
   path (`fbdev_rotate_left16`) now:
   * works in 32×32 tiles, so the strided writes stay in cache;
   * converts with two 256-entry tables (`lo[px & 0xff] | hi[px >> 8]`);
   * tests the MOD tab and panel rects once per tile, and skips tiles that lie
     wholly inside them (those pixels are the overlay's). With the panel open,
     this took FPS from about 53 back to 60;
   * splits the screen into bands on 3 threads. Helpers run at nice 5 so
     `JuceALSA` (SCHED_OTHER, nice 0) still gets a core. `DFB_ROT_THREADS`
     (1 to 4) overrides the count;
   * keeps a copy of the last source shown on each fb page and skips tiles
     that did not change. A page is redrawn in full the first time, and
     whenever the MOD rects change, so the area under a closed panel comes
     back.
6. **Drawing into the page on screen (tearing).** DirectFB hands the driver
   the same layer buffer every frame (offset 0), so every rotate went into fb
   page 0 while the panel was scanning it out. The panel scans in physical
   portrait order, which on screen runs from the right edge to the left.
   The rotate starts about 6 ms after vblank, when the scan has already
   passed the right-hand part. So only the middle and left tore: waveforms
   wiggled near and after the playhead, but not at the right edge. The fb has
   3 pages (`yres_virtual` 3840, `ypanstep` 1). The driver now cycles through
   them itself, rotating into a page that is not on screen and panning to it
   once it is complete. `cat /sys/class/graphics/fb0/pan` should show
   `0,0`, `0,1280`, and `0,2560` over a few reads.

### Measuring it

* **MOD panel → STATS row** shows displayed frames per second next to CPU load, for example
  `FPS 60.4`. fbshim counts every `FBIOPAN_DISPLAY` over a 1 s window. The row
  only updates while the panel is open, because repainting the closed tab
  every second would draw into the scanout buffer.
* Over SSH, the same number ×10 is the 11th word of the overlay shm (`fps_x10`):

  ```sh
  hexdump -e '17/4 "%d " "\n"' /tmp/rb-overlay | cut -d' ' -f11   # 603 -> 60.3 fps
  ```

* `/tmp/rb-rot` is rewritten about every 2 s by the fbdev driver:
  `frames rot_avg_us rot_max_us render_avg_us threads`. A healthy line is
  `121 3818 5746 5758 3`: 121 frames in 2 s, a 3.8 ms rotate, 5.8 ms of rbp
  drawing. *render* is the time from the previous pan's return to the next
  rotate.
* What `gui_task` is blocked on, from 500 samples:

  ```sh
  T=$(grep -l gui_task /proc/<rbp pid>/task/*/comm | head -n 1 | xargs dirname)
  for i in $(seq 500); do echo "$(cut -d' ' -f3 $T/stat) $(cat $T/wchan)"; done | sort | uniq -c
  ```

  `drm_atomic_helper_wait_for_vblanks` is the pan, and it is expected.
  `drm_wait_one_vblank` is an extra `FBIO_WAITFORVSYNC`. `hrtimer_nanosleep`
  is a sleep, from rbp or a shim.

## Deck info panel

The two boxes left of the waveforms (DECK 1 and DECK 2) have four rows: the source, the key, a **Bars**
countdown, and the loop size. `knobshim2` changes two of them. Both changes are always on.

* **Source row is blank.** It showed where the track came from ("USB1"). `ui_Deck_Update` picks the
  device icon from the media type and hides it when the deck has no media. The `bne` at `0x28fc14` that
  leads to the icon is replaced with a no-op, so rbp always takes its own empty-deck path. The row's
  background stays.
* **Bars counts to the next hot cue (A–H), not the next memory cue.** rbp's
  `CmnFunc_CmnInfo_GetLocalNowPlay_CountDownNum(deck)` (`0x184b10`) reads a sorted list of up to 10 memory
  cue beat numbers per deck (at `0x0322ab70 + deck × 0x12fd8 + 0x10ef4`, `-1` ends it). rbp fills it on
  track load and reads it nowhere else. A hook refills it before each call: hot cue IN times from
  `UiGetHotCueINtime(deck, 0..7)` (ms, `-1` = empty), turned into beats with
  `DJcont_searchBeatNo_forMemCue`, the same call rbp uses for memory cues. rbp's own code then does the
  rest: the format (bars.beats), the colours, `--.-` past 400 beats or when no hot cue is ahead, and slip
  mode. A hot cue that is set or deleted shows up on the next frame. The cost is up to 8 lookups per deck
  per frame. `TotalCnt_SentinelTASK` reads the same value, so it changes there too.
