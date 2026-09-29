# Changelog

Differences from [erhan-/rblive4](https://github.com/erhan-/rblive4), forked at `6003702`.

## 2026-09-29

### MOD menu

A **MOD** tab sits at the top center of the screen. Taps on the tab and its panel stay in the overlay and are not passed to the player. The open panel looks like this. Green marks the current play mode and waveform color.

![MOD menu](docs/mod-menu.png)

- **Play mode** is one button. Each tap cycles **SINGLE**, **CONTINUE**, **REPEAT**, and **ALL REPEAT**, using the same `UiSetUtilAutoPlayMode` call as the RX3 utility screen. The choice is written back to `XdjSettings.dat`.
- **Jog sensitivity** is one setting for both decks. It starts at 40% of the original calibration, steps by 10% between 20% and 200%, and is kept in `/tmp/rb-overlay` until the device reboots.
- **Waveform color** is **BLUE**, **RGB**, or **3 BAND**, and it recolors the waveform that is already on screen. On this player, tapping the waveform does not open the RX3 shortcut, so the choice is on the MOD panel.
- **Eject** starts as one full-width button. A tap splits that row into the two USB slots, each labeled with a shortened volume name. Tapping a slot turns that half into **YES**. Only the **YES** tap ejects that stick. Closing the menu returns the row to the single **EJECT** button.
- **Power** sits under eject. The first tap turns that button into **YES**. The second tap asks the launcher to eject both sticks, then power the unit off. Closing the menu before **YES** cancels it.

### USB

`usb-watch.sh` keeps two sticks in fixed slots until each one is ejected or unplugged.

| Slot | Mount | Player FIFO |
|---|---|---|
| 1 | `/media/usb1/sda1` | `/tmp/udev_usb1` |
| 2 | `/media/usb4/sda1` | `/tmp/udev_usb2` |

Slot 2 uses `/media/usb4/sda1` because that is the player's USB 2 mass-storage path. Volume labels are written to `/tmp/usb-name-1` and `/tmp/usb-name-2` for the MOD buttons. An eject request is `/tmp/usb-eject-1` or `/tmp/usb-eject-2`. After a clean eject, that slot stays released until the disk disappears, so the stick is not mounted again while it is still plugged in.

`knobshim` treats USB 2 as present only when `/media/usb4/sda1` contains a rekordbox `export.pdb`. Otherwise it clears the phantom USB 2 flag that was hiding the USB 1 label.

### Beat FX

The FX assign knob drives both the on-screen channel (`K_BFXCH`) and
`DjEngineIF::setBeatEffectSelectChannel`, so Ch1 / Ch2 / Main actually change
the audio route. A short push of TIME cycles **BEAT → TIME → BPM** (default
BEAT). Hold TIME and turn still sends BEAT `<` / `>`. Hold FX SELECT (~600 ms)
puts Beat FX BPM detect back in AUTO so it follows the master/source deck.

### Beat loop

`BEATLOOP=1` is on by default. The per-deck encoder uses latched
`setAutoBeatLoop` (mode 0), not pad keys. Push toggles with `exitLoop` so
playback does not jump. Default length is 16 beats; the range is 128 down to
1/32.

### Launch

`start-rb.sh` starts the player with `BEATLOOP=1`. MIDI/jog/tempo debug
logging is off unless `KNOB_VERBOSE`, `JOG_VERBOSE`, or `TEMPO_VERBOSE` is set.

### Build

`fbshim-tsc.so` is built with `overlay_playmode.c` and paints the MOD panel on each frame. `knobshim2` reads the jog gain from the same `/tmp/rb-overlay` mapping. The shim build defines `O_TMPFILE` because the glibc 2.13 headers predate it.
