# Changelog

Differences from [erhan-/rblive4](https://github.com/erhan-/rblive4), forked at `6003702`.

## 2026-10-08

### Track Preview, Touch Cue and two touch fixes

* **Track Preview works.** Touching a browse row's mini waveform plays it in the headphones. rbp has it, but four things stopped it here: its audio stays muted until the RX3's LINK CUE button turns it on (new MOD row **LINK**, default ON), the startup patch at `0x3664b4` made the player refuse every load, a noise filter in its touch reader kept slow drags from reaching the middle of the waveform, and it draws no playhead (now a lime line that follows the real position). Details in [docs/08 — Track Preview](docs/08-controls.md#track-preview).
* **Touch Cue.** Touch and hold a playing deck's overview waveform to hear that point in the headphones while the deck keeps playing. Move to move the point, lift to stop. While held, a pad sets the matching hot cue there, and its LED lights as for any hot cue. New MOD row **TCUE** (default ON); OFF removes the touch area completely. A paused deck still uses Needle Search. See [docs/08 — Touch Cue](docs/08-controls.md#touch-cue). The audio is rough for now (TODO, noted there).
* **Scrubbing no longer skips the middle.** rbp's `TouchAdValueHysteresis` filter has bands of 50 and 100 ADC counts, but this port feeds it pixels. A slow drag crept one pixel per five samples and then jumped about 100 px. The bands are now divided by 4. It affects every drag in the player.
* **Touch screen found by name.** It was read from `/dev/input/event0`. On some boots Linux gives `event0` to `gpio-keys` and the ILI2117 becomes `event1`, which left touch dead while the knobs worked. `fbshim-tsc` now finds the device by its name.

### Beat meter

rbp marks the beat only with the small red bar ticks over each waveform. A beat meter now sits in the top bar, right of the MOD tab, with one row per deck. It reads each deck's beat grid straight from the engine. A new MOD row, **BEAT**, picks the view:

* **BARS** (default): 4 beat cells per deck. The current beat lights up and fills as it passes. Matching fill edges mean the decks are in phase.
* **DRIFT**: a center-zero gauge of the other deck against the sync master, the offset in ms, the BPM difference, and the downbeat offset.
* **OFF**.

See [docs/08 — Beat meter](docs/08-controls.md#beat-meter).

## 2026-10-01

### Display: 60 fps

The screen ran at about 15 fps, and the waveforms looked choppy. It now runs
at 60 fps, locked to the panel's 60.06 Hz. The full breakdown is in
[docs/06 — Frame rate](docs/06-display.md#frame-rate).

| Cause | Fix | FPS after |
|---|---|---|
| The fbdev driver logged 3 lines and wrote a 6 MB `/tmp/rot_surface.dump` on every flip | removed | |
| `FBIO_WAITFORVSYNC` before a pan that already waits for vblank | skipped when rotating | 30 |
| fbshim slept to 16.7 ms from the previous pan's start | now an 8 ms floor from the previous pan's return | |
| rbp's `DS_HW_UpdateScreen` sleeps to 16 ms, and the rotate comes after that | fbshim's `usleep` returns at once for that one caller (`0x1a6920`) | 30–54 |
| One-thread, per-pixel rotate (12 ms) | 32×32 tiles, a lookup table, 3 threads, unchanged tiles skipped (~5 ms) | **60** |
| Every frame was rotated into fb page 0 while it was on screen, so waveforms wiggled near and after the playhead | the driver cycles through the fb's 3 pages and pans to each one once it is complete | 60, no tearing |

The driver changes are in
[`tools/build-directfb/directfb-full.diff`](tools/build-directfb/directfb-full.diff),
regenerated against DirectFB `2199f40b1`. `/tmp/rb-rot` reports the rotate and
draw times about every 2 s.

### Audio: no more clicks at 60 fps

The display now uses more CPU, and occasional clicks showed up. They were
underruns. The buffer is 128 frames (2.9 ms), the writer thread `JuceALSA`
ran at normal priority, and JUCE's stop threshold turns an underrun into
silent gaps instead of an error. audioshim now runs only that thread at
SCHED_FIFO 40 (below the RT kernel's IRQ threads). Over 60 s of playback,
underrun windows went from 6 to 0, and the fullest the buffer got to empty
went from 183 frames to 62. See
[docs/09](docs/09-audio.md#underruns-and-clicks).

### MOD menu: SCREEN, LEDS, STATS

The panel order is now MODE, JOG, WAVE, QUANT, TRACK, **SCREEN**, **LEDS**,
EJECT, **STATS**, POWER. POWER always stays last.

- **SCREEN** is backlight brightness, − percent +, 10% to 100% in 10% steps.
  It starts from what Engine OS left it at.
- **LEDS** is the controller's panel LED brightness, − percent +, 10% to 100%.
  knobshim scales each LED's Note On velocity (pads: each colour channel) and
  re-sends every LED when it changes.
- **STATS** is read-only, for example `CPU 38%  FPS 60.4`.

Both brightness values are kept in `/tmp/rb-overlay` (`screen_pct`,
`led_pct`), so they survive a player restart but not a reboot.

The label column is 8 px wider so SCREEN clears its − button. With the
panel open the display dropped to about 53 fps, because every tile under
the panel was converted pixel by pixel. The fbdev driver now skips tiles
that lie wholly inside the MOD rects, and the display holds 60 fps with the
panel open. The menu image in the docs is a fresh 3× capture from the unit.

### MOD menu: FPS

Displayed frames per second, for example `60.4`, now part of the **STATS**
row. The same value ×10 is the last field of
`/tmp/rb-overlay` (`fps_x10`, appended to `struct rb_overlay_shm`).

### Launcher: restarts no longer wipe `/dev`

On Engine OS 5.x, `ps` is procps, and `ps w` lists only processes on the
caller's terminal. A launcher started without that terminal never saw rbp.
It skipped the kill, stopped `usb-watch` in its exit cleanup (USB sticks
stopped being read), and ran `fix-dev.sh` under the live chroot. There
`umount` failed, and `rm -rf /data/rbx3-run/dev` deleted the real `/dev`
nodes through the bind mount, then the unit rebooted.

- `start-rb.sh` and `usb-watch.sh` find processes through `/proc/*/cmdline`
  (`procs()`).
- `fix-dev.sh` only deletes and re-binds `dev` once it is no longer a mount.
- The safe restart procedure is in
  [docs/11](docs/11-runtime-launcher.md#restarting).

## 2026-09-29

### MOD menu

A **MOD** tab sits at the top center of the screen. Taps on the tab and its panel stay in the overlay and are not passed to the player. Each row names the setting on the left, then the value. Green marks the current play mode and waveform color.

<img src="docs/mod-menu.png" alt="MOD menu" width="360"/>

- **MODE** is the play mode. Each tap cycles **SINGLE**, **CONTINUE**, **REPEAT**, and **ALL REPEAT**, using the same `UiSetUtilAutoPlayMode` call as the RX3 utility screen. The choice is written back to `XdjSettings.dat`.
- **JOG** is jog sensitivity for both decks, shown as −, the percent, and +. It starts at 40% of the original calibration, steps by 10% between 20% and 200%, and is kept in `/tmp/rb-overlay` until the device reboots.
- **WAVE** is **BLUE**, **RGB**, or **3 BAND**, and it recolors the waveform that is already on screen. On this player, tapping the waveform does not open the RX3 shortcut, so the choice is on the MOD panel.
- **QUANT** is deck quantize for both decks, **ON** or **OFF**. This is the QUANT button (`UiSetQuantizeOnOff`), so **OFF** lets cue land off the beat grid. The settings entry "quantize beat value" only changes the grid size and leaves snapping on.
- **TRACK** sits above EJECT. **TAG** adds the highlighted track to the Tag List (`0x420e`). **TAGS** opens the Tag List (`0x0203`). **FIND** opens Search (`0x0205`). TAGS and FIND close the panel so that screen is visible.
- **EJECT** names the row. The two USB slots are already shown, each labeled with a shortened volume name. Tapping a slot turns that half into **YES**. Only the **YES** tap ejects that stick. Closing the menu clears a pending **YES**.
- **POWER** sits on the last row. The first tap turns the row green and shows **YES**. The second tap asks the launcher to eject both sticks, then power the unit off. Closing the menu before **YES** cancels it.

### USB

`usb-watch.sh` keeps two sticks in fixed slots until each one is ejected or unplugged.

| Slot | Mount | Player FIFO |
|---|---|---|
| 1 | `/media/usb1/sda1` | `/tmp/udev_usb1` |
| 2 | `/media/usb4/sda1` | `/tmp/udev_usb2` |

Slot 2 uses `/media/usb4/sda1` because that is the player's USB 2 mass-storage path. Volume labels are written to `/tmp/usb-name-1` and `/tmp/usb-name-2` for the MOD buttons. An eject request is `/tmp/usb-eject-1` or `/tmp/usb-eject-2`. After a clean eject, that slot stays released until the disk disappears, so the stick is not mounted again while it is still plugged in.

`knobshim` treats USB 2 as present only when `/media/usb4/sda1` contains a rekordbox `export.pdb`. Otherwise it clears the phantom USB 2 flag that was hiding the USB 1 label.

### Browse

The unused LIGHTING button (global note 39, under MENU) opens Tag List on a short press (`0x0203`) and Search on a hold of about 600 ms (`0x0205`). Search is the browse screen with the on-screen keyboard.

A short tap of FWD still opens Source. Holding FWD for about 600 ms sends `0x420e` (`UiKey_AddTag`), which adds the highlighted browse track to the Tag List.

### Beat FX

The FX assign knob drives both the on-screen channel (`K_BFXCH`) and
`DjEngineIF::setBeatEffectSelectChannel`, so Ch1 / Ch2 / Main actually change
the audio route. A short push of TIME cycles **BEAT → TIME → BPM** (default
BEAT). Hold TIME and turn still sends BEAT `<` / `>`. A short tap of FX SELECT calls `DjEngineIF::triggerTapTiming()`. Hold FX SELECT
(~600 ms) returns Beat FX BPM to AUTO/quantize, restoring live pitch-adjusted BPM and
the on-screen QUANTIZE state. A qualifying release also fires the action,
avoiding a race at the hold threshold. MAIN falls back to the sync-master
deck's analyzed BPM plus live tempo offset when the missing RX3 mixer hardware
state prevents rbp's native master-source lookup.

### Beat loop

`BEATLOOP=1` is on by default. The per-deck encoder uses latched
`setAutoBeatLoop` (mode 0), not pad keys. Push toggles with `exitLoop` so
playback does not jump. Default length is 16 beats; the range is 128 down to
1/32.

### Launch

`start-rb.sh` starts the player with `BEATLOOP=1`. MIDI/jog/tempo debug
logging is off unless `KNOB_VERBOSE`, `JOG_VERBOSE`, or `TEMPO_VERBOSE` is set.

### Build

`fbshim-tsc.so` is built with `overlay_playmode.c`. The rotated DirectFB path
normally presents physical framebuffer page 0, so repainting the overlay on
every flip caused visible partial-panel tearing. The overlay now hashes its
visible state and repaints each framebuffer page only when that state changes.
`knobshim2` reads the jog gain from the same `/tmp/rb-overlay` mapping. The
shim build defines `O_TMPFILE` because the glibc 2.13 headers predate it.
