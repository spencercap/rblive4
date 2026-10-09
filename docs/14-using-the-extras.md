# 14 — Using the extras

A how-to for everything this fork adds on top of the stock port. Each item says what it does, how to use it on the unit, and where the details are. Nothing here needs SSH once the player is running.

**Jump to:** [MOD menu](#the-mod-menu) · [Recording](#recording-a-set) · [My Tags](#my-tags-in-the-track-info-panel) · [Memory cues](#memory-cues-slip-and-the-pad-page-arrows) · [Hot cue countdown](#the-hot-cue-countdown-deck-info-boxes) · [Beat meter](#beat-meter) · [SHIFT combos](#shift-combos) · [Touch Cue](#touch-cue) · [Track Preview](#track-preview) · [Boot screen](#boot-screen)

## The MOD menu

Tap **MOD** at the top center of the screen. Tap anywhere outside the panel to close it.

* **Scroll:** the panel is taller than the screen. Drag your finger up or down inside it. The thin bar on its right edge shows where you are.
* **Tap a button** to use it. Buttons act when you lift your finger, so a drag never presses one by accident.
* **Green** means on or current.
* Settings marked "until reboot" in [08](08-controls.md#mod-menu) reset when the unit restarts.

## Recording a set

The master mix can be recorded to the SD card (or whatever is in **USB 2**). A library stick takes USB 1 and the SD card takes USB 2, so the two work together ([10](10-usb.md#sd-card-slot)).

1. Put the card in and push it until it clicks. It shows as USB 2 in the Source screen.
2. Tap **MOD**, drag the panel to the bottom, and tap **REC**. It turns red and reads **STOP**.
3. To finish, tap **STOP**, then **YES**.

The file is `PIONEER REC/REC001.WAV` on the card (a later recording takes the next free number), 16-bit stereo 44.1 kHz. rbp's own limit is 3 hours per file, and it wants roughly 32 MB free to start. The card does not need a rekordbox library.

* **While recording the file is called `RECTMP.WAV`.** It becomes `REC001.WAV` when you stop, and the header then holds the right length.
* **If REC says NO USB 2**, the card is not mounted: reseat it.
* **Stop before you restart the player or the unit.** Ejecting USB 2 or tapping POWER in the MOD menu stops it for you. A restart from SSH or a power cut mid-recording leaves `RECTMP.WAV` behind, and its length header may be wrong.
* Listen to a first recording to check the level.

## Memory cues: SLIP and the pad page arrows

Store and jump between memory cues without leaving the deck. rbp saves them to the stick like the RX3's MEMORY button, and draws a red ▼ marker on the waveform.

| Do this | Result |
|---|---|
| Press **SLIP** | Stores a memory cue at the playhead |
| Press the pad page **►** | Jumps to the next memory cue |
| Press the pad page **◄** | Jumps to the previous memory cue |
| Hold **SHIFT**, press **◄** | Deletes the memory cue at the playhead |
| Hold **SHIFT**, press **SLIP** | Slip mode (what SLIP used to do) |

Tips: jumping past the last or first cue does nothing. Call and delete work on the playhead, so jump to a cue first, then delete it. Details: [08 — Memory cues](08-controls.md#memory-cues).

## The hot cue countdown (deck info boxes)

The two boxes left of the waveforms (DECK 1 and DECK 2) show source, key, a countdown and the loop size. The countdown now counts to the **next hot cue (A–H)**, not the next memory cue. In the MOD menu, scroll to the last rows:

| Row | Buttons | What it does |
|---|---|---|
| **INFO** | OFF / ON | **ON** (default) is this fork's boxes. **OFF** gives them back to stock rbp: "USB1" shows, and the countdown goes back to memory cues. Use OFF to rule the fork out when something looks odd. |
| **ROWS** | SRC, KEY, CUE, LOOP | Show or hide each row. Green = shown. SRC (where the track came from, "USB1") starts hidden. A hidden row leaves an empty strip. |
| **COUNT** | BARS / BEATS | **BARS** reads like `02.3` (bars.beats). **BEATS** reads like `11 BEATS`. Over 99 beats it shows `--`. |

The countdown turns red in the last 16 beats (4 beats in BEATS mode). It shows `--.-` when no hot cue is ahead. Details: [06 — Deck info panel](06-display.md#deck-info-panel).

## My Tags in the track INFO panel

See and change a track's **My Tags** (the genre, mood and other tags you set in rekordbox) without leaving the player. **Tap a tag to tag or untag the loaded track.**

![My Tags in the INFO panel: every tag as a button, the loaded track's in orange](my-tags.png)

**How to use it:**

1. Load a track and tap the **INFO** button (top right of the screen). The right half of the panel, where the artwork would be, now lists **every My Tag** as a button, grouped by category (Genre, Mood, and so on). The loaded track's tags are **orange**; the rest are grey.
2. **Drag the list up or down** to scroll. The thin bar on its right edge shows where you are.
3. **Tap a tag** to give the track that tag, or tap an orange one to take it off. The header shows **SAVING** while it is written to the stick, then the button lights or dims. If it says **NOT SAVED**, the write failed and nothing changed.
4. **Switch decks** by tapping **DECK 1** or **DECK 2** on the left. The INFO panel and the tag list follow the selected deck (the one with the white frame).

**Settings and behaviour:**

* **Turn the list off or on** with the **TAGS ON / OFF** button at the top right of the list, or with MOD row **TAGS**. They are the same setting. With it off, rbp shows the artwork area as usual and a small TAGS OFF button stays to bring the list back.
* The MOD menu stays on top; the list moves to its right while the menu is open.
* The list stays on top of rbp's scrolling comment line and the "TRACK n/m" text, and it costs no measurable CPU or frame rate (about 60 fps with the panel open, same as with the list off).
* Tags are shown in capitals.
* If the stick has no My Tags (rekordbox writes them to `PIONEER/rekordbox/exportExt.pdb` when it exports), the area keeps rbp's artwork and there is nothing to show.
* **Careful:** this edits your stick's library. A copy of the tag file is saved on the unit before the first change of each run (`/data/rbx3-run/mytags-backup/`). rekordbox on your computer does not read these changes, and its next export of the stick replaces them.

Details: [08 — My Tags](08-controls.md#my-tags-in-the-info-panel).

## Beat meter

A small meter in the top bar, right of MOD, with one row per deck. MOD row **BEAT**: **BARS** (default, shows where each deck is in its bar), **DRIFT** (how far the other deck is from the sync master) or **OFF**. Details: [08 — Beat meter](08-controls.md#beat-meter).

## SHIFT combos

Hold **SHIFT** and:

| Press | Result |
|---|---|
| a hot cue pad | deletes that hot cue |
| the jog wheel | searches through the track |
| the Beat FX TIME encoder | steps BEAT `<` / `>` in any encoder mode |
| the pad page **◄** | deletes the memory cue at the playhead |
| **SLIP** | slip mode |

### Beat jump on SEARCH < >

MOD row **SKIP** has two settings: **SEARCH** (the normal scan) and **LOOP SIZE**. With **LOOP SIZE**, each press of SEARCH `<` or `>` jumps the deck back or forward by the size the **loop encoder** is set to:

1. Turn the deck's loop encoder to a size (for example 4, 16 or 64 beats). The loop does not have to be running.
2. Press SEARCH `>` to jump forward that many beats, `<` to jump back.

Sizes: 128, 64, 32, 16, 8, 4, 2, 1 beats. At 1/2 beat and smaller the jump is 1/2 a beat. Details: [08 — SHIFT](08-controls.md#shift).

## Beat jump pads

Press the deck's **SLICER** pad-mode button (the beat jump mode). It opens **BEAT JUMP 2** first (pads jump 1/2, 2, 4 and 16 beats). Press it again for **BEAT JUMP** page 1 (1, 2, 4 and 8 beats); each press flips the page.

## Touch Cue

While a deck plays, **touch and hold its overview waveform** (the small full-track waveform in the deck panel at the bottom) to hear that point in the headphones. Slide to move the point, lift to stop. While holding, press a **pad** to set that hot cue at the previewed point. Turn it off with MOD row **TCUE**.

![Touch Cue: the lime line in deck 1's overview is the point being heard](touch-cue.png) Details: [08 — Touch Cue](08-controls.md#touch-cue).

## Track Preview

In the browse list, **touch a track's mini waveform** to hear it from that point in the headphones. A lime line shows the position. It needs MOD row **LINK** on (the default).

![Track Preview: the lime line on the second row is the playhead](track-preview.png) Details: [08 — Track Preview](08-controls.md#track-preview).

## Safe to try

* **Memory cues and hot cues are written to your stick**, as with any rekordbox player. SHIFT + ◄ removes a memory cue you did not mean to make; SHIFT + pad removes a hot cue.
* **MOD menu settings last until reboot** unless the table in [08](08-controls.md#mod-menu) says otherwise.
* If the screen ever looks wrong, set **INFO** to **OFF** and open and close the MOD panel to force a full redraw.

## Boot screen

When you start the launcher (or restart it), the screen shows a logo while the player starts, instead of freezing on the last frame, and then the player's own screen takes over.

![The boot screen](boot-screen.png)

* **Nothing to do:** it appears by itself every time `start-rb.sh` runs.
* **Change the picture:** `python3 tools/make-splash.py your-logo.png` on your computer (white on black works best), copy the `splash.raw.gz` it writes to `/data/splash.raw.gz` on the unit, and start the launcher again.
* **Turn it off:** delete `/data/splash.raw.gz`.

Details: [11 — Boot screen](11-runtime-launcher.md#boot-screen).
