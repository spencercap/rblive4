# Changelog

Differences from [erhan-/rblive4](https://github.com/erhan-/rblive4), forked at `6003702`.

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

### MOD menu: FPS

A read-only **FPS** row at the bottom of the panel shows displayed frames per
second, for example `60.4`. The same value ×10 is the last field of
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
