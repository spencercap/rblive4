# 11 — Runtime launcher

[`scripts/device/start-rb.sh`](../scripts/device/start-rb.sh) runs on the device
and is the normal way to bring up the player.

## What it does

1. `systemctl stop engine.service edisksd.service` — `engine` owns the display/
   audio/MIDI; `edisksd` will bus-reset a USB drive it doesn't manage.
2. Kill stale `rbp`/`edb_streamd`, then paint the [boot screen](#boot-screen).
3. `sh /data/fix-dev.sh` — bind mounts + device stubs.
4. Copy `rbp-audio` + the shims into the chroot.
5. `rm -f /tmp/guard_LocalDBServer /tmp/req_LocalDBServer` — clear stale
   DeviceSQL locks (see [10](10-usb.md)).
6. Start `edb_streamd` (DeviceSQL) **before** rbp.
7. Start rbp, then `usb-watch.sh start`.

The rbp launch line (order matters):

```sh
chroot /data/rbx3-run env \
  PATH=/bin:/sbin:/usr/bin:/usr/sbin \
  DFB_ROTATE=left STARTUP_MUTE_MS=1500 STARTUP_FADE_MS=300 \
  LD_PRELOAD=/usr/lib/fbshim.so:/usr/lib/knobshim.so:/usr/lib/audioshim.so \
  /lib/ld-linux.so.3 /root/pdj/rbp -a
```

* `PATH` must include `/bin` (the device default `/usr/bin:/usr/sbin` misses the
  chroot's busybox tools → rbp logs `sh: ls: not found`).
* `knobshim.so` **before** `audioshim.so` so the shared `g_speaker_gain` data
  symbol resolves.
* the speaker/booth level is fixed via `SPEAKER_GAIN` (see [09](09-audio.md)).

## Boot screen

Until rbp draws its first frame (about 3 to 4 seconds after the old one is killed) the panel would keep whatever was on it last: Engine's final frame, or the frozen old rbp. After step 2 the launcher paints a logo instead.

![The boot screen](boot-screen.png)

* **What is painted.** `/data/splash.raw.gz` is one fb0 page as the panel stores it (800×1280, 32 bpp BGRA, the screen turned a quarter, see [06](06-display.md)), gzipped (about 40 KB). The launcher writes it to all three pages of `/dev/fb0` (`virtual_size` 800,3840), so it shows whichever page is displayed. rbp's first frame replaces it, tile by tile.
* **Why after the kill.** The old rbp and the overlay keep redrawing parts of the screen (the BEATS labels, the deck 2 box, the meters) until they are killed, and they painted over the logo when it was drawn earlier.
* **Skipped when it cannot be right.** No `/data/splash.raw.gz`, or `/sys/class/graphics/fb0` is not 32 bpp with a stride of 3200: the screen is left as it was. To turn the boot screen off, delete `/data/splash.raw.gz`.
* **Making your own.** `python3 tools/make-splash.py LOGO.png OUT_DIR` fits any image on a black 1280×800 screen (nearly black pixels become pure black, the margin is trimmed, the logo is 1000 px wide) and writes `splash.png` (the preview, kept in [`scripts/device/`](../scripts/device/)) and `splash.raw.gz`. Copy that to `/data/splash.raw.gz` and restart the launcher.

## My Tag sync

rbp writes My Tag edits to the stick's `PIONEER/rekordbox/exportExt.pdb` only (see [08 — My Tags](08-controls.md#my-tags-in-the-info-panel)). rekordbox for Mac/Windows, and newer players, read the stick's other library, `exportLibrary.db` (OneLibrary, formerly Device Library Plus), which keeps the tags from the last export. The launcher keeps the two equal, on the unit, with no computer involved.

* **When.** Step 10's wait loop polls `exportExt.pdb` of both slots every 2 s (`stat`, about nothing). When its time and size change and then stay the same for one more poll, it runs `/data/mytags-onelib` once, under `chrt -i 0` (idle priority, so the decks are never starved) and `/data/timeout 60`. An edit shows up in `exportLibrary.db` about 4 to 6 seconds later; the launcher also runs it once for each stick after it starts.
* **What it does.** [`mytags-onelib.c`](../scripts/device/mytags-onelib.c) reads the tag assignments from `exportExt.pdb` (type 4 rows) and the tracks' file paths from `export.pdb` (type 0 rows, string 20), opens `exportLibrary.db` and makes `myTag_content` match, for every track that is in both libraries (matched by file path: the two libraries number their tracks differently, Das Rite is 1168 in `export.pdb` and 1165 in `exportLibrary.db`). Tags are matched by id; `myTag` already holds the same ids rekordbox exported. Nothing else in the file is touched. The player wins over rekordbox for those tracks.
* **The database** is SQLCipher 4 (page size 4096, PBKDF2 256000 rounds, so opening takes about 3 s on the unit) in WAL mode. The helper opens it with `locking_mode=exclusive` (no `-shm` file on the FAT stick), changes the rows inside one transaction, runs `integrity_check` and checkpoints, so no `-wal` is left behind. Before its first change of a day it copies the file to `/data/onelib-backup/exportLibrary-YYYYMMDD.db`. Log: `/data/mytags-onelib.log`. `mytags-onelib -n DIR` prints the changes without writing (it opens the file immutable and leaves nothing on the stick).
* **Files.** `/data/mytags-onelib` is a static ARM build of the helper: `build-env/run.sh sh rblive4_sc/tools/build-sqlcipher/build.sh` builds SQLCipher 4 and OpenSSL 3 from source in the build container and writes `rblive4_sc/work/sqlcipher/mytags-onelib` (not committed). `/data/onelibrary.key` holds the OneLibrary key, one line. It is the key pyrekordbox uses and is not in this repo; to write it:

  ```python
  # pip install git+https://github.com/dylanljones/pyrekordbox.git   (PyPI's 0.4.4 lacks onelibrary)
  from pyrekordbox.onelibrary.database import BLOB
  from pyrekordbox.utils import deobfuscate
  open("onelibrary.key", "w").write(deobfuscate(BLOB) + "\n")
  ```
* **The My Tag master id.** rekordbox only imports or exports My Tags for a stick that belongs to its own library: the stick keeps the id of the library whose My Tags it holds (`property.myTagMasterDBID` in `exportLibrary.db`, and a u32 at row offset 24 of the single row in the type 7 table of `exportExt.pdb`, page 16 on this stick), and rekordbox compares it with `djmdProperty.DBID` of the computer's `master.db`. If they differ it shows "My Tag was not imported because this device is synced with another computer" and the tags never arrive, which is why edits made on the player did not show up in rekordbox. `mytags-onelib -m ID` sets the id in `exportLibrary.db` (once, backed up first); the `exportExt.pdb` one is four bytes, changed in place with the chroot's `dd` (`printf` the id little-endian into `chroot /data/rbx3-run /bin/dd of=... bs=1 seek=65600 count=4 conv=notrunc`; the host's BusyBox `dd` has no `conv=`). To find the id: `djmdProperty.DBID` in `~/Library/Pioneer/rekordbox/master.db`.
* **Skipped when it cannot be right.** No helper, no key file, or a stick without `exportLibrary.db`: nothing happens. A failed run (wrong key, a stick pulled out) is logged and tried again at the next change.
* **Not tested with rekordbox itself.** The stick's `exportLibrary.db` now holds the player's tags and passes SQLite's integrity check, and pyrekordbox reads them back. Pioneer documents rekordbox's **Update Collection** as bringing back cue points and beat grids, and says nothing about My Tags, so whether rekordbox imports them is something to check on a computer.

## Finding processes: not `ps w`

On Engine OS 5.x, `ps` is procps-ng (`/usr/bin/ps.procps`), not BusyBox. In
BSD syntax, `ps w` lists only processes on the caller's terminal. A launcher
started from a service, `setsid`, or `nohup` therefore saw no rbp at all,
and three things broke:

1. Step 2 did not kill the old rbp.
2. `RBP` stayed empty, so the launcher skipped its wait loop and ran its exit
   cleanup at once. That cleanup runs `usb-watch.sh stop`, so **USB sticks
   stopped being read**.
3. `fix-dev.sh` ran under the live chroot. `umount /data/rbx3-run/dev` failed
   with EBUSY, and the `rm -rf /data/rbx3-run/dev` after it deleted the
   **real** `/dev` nodes (`fb0`, `dri/*`, `sda*`), because that directory is a
   bind of `/dev`. A second rbp then started against the same DRM device, and
   the unit **rebooted** (`panic_on_oops=1`).

`start-rb.sh` and `usb-watch.sh` now scan `/proc/*/cmdline` with a `procs()`
helper that prints `pid cmdline`, the same shape `awk '{print $1}'` expects.
It works with procps and BusyBox alike. `fix-dev.sh` peels off stacked binds
and never deletes the directory while it is still a mount. If it stays busy,
the script prints `dev busy, kept existing bind`.

If `/dev/fb0` or `/dev/sda*` is missing on the host, only a reboot brings the
nodes back.

## Restarting

Run exactly one launcher. If one is already running, kill it first. If you
don't, the old launcher's exit cleanup kills the new rbp halfway through its
startup:

```sh
for d in /proc/[0-9]*; do
  case "$(tr '\0' ' ' < $d/cmdline 2>/dev/null)" in
    "sh /data/start-rb.sh "|"/bin/sh ./start-rb.sh ") kill ${d#/proc/};;
  esac
done
setsid nohup sh /data/start-rb.sh > /data/start-rb.log 2>&1 < /dev/null &
```

The new launcher stops the old rbp, `edb_streamd`, and `usb-watch` itself.
It is ready when the log shows `RBP=<pid> ready`. Keep the strings
`usb-watch`, `root/pdj/rbp`, and `edb_streamd` out of any SSH command line,
because the launcher's kill scan matches them and would kill the SSH session.

The launcher clears the DeviceSQL locks itself. When restarting rbp by hand,
clear them too, else the USB library shows the generic "USB1" label instead
of the volume name/track count:

```sh
for p in $(ps -eo pid,comm | awk '$2=="ld-linux.so.3" {print $1}'); do kill -9 $p; done
rm -f /tmp/guard_LocalDBServer /tmp/req_LocalDBServer
```

> The kill pattern matches the loader comm (`ld-linux.so.3`), which both rbp and
> `edb_streamd` use. Do **not** use an inline `ps | grep '/root/pdj/rbp'` kill
> loop over SSH — it matches the SSH shell's own command line and kills the
> session.

## Restore Engine OS

```sh
for p in $(ps -eo pid,comm | awk '$2=="ld-linux.so.3" {print $1}'); do kill -9 $p; done
systemctl start engine.service
```

## Autostart

There is no shipped autostart unit. A systemd unit under the `/etc` overlay
(`/data/system/etc/overlay/systemd/system/rb.service` + a
`multi-user.target.wants` symlink) can stop `engine` and run `start-rb.sh` at
boot; make it reversible (`systemctl stop rb; systemctl start engine`).
