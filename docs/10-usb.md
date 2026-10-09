# 10 — USB stick + rekordbox database

A rekordbox-exported stick in the SC Live 4's rear USB-A port is detected
natively: it mounts, `export.pdb` is opened and analysed by DeviceSQL, and the
drive shows as **USB 1** in rb.

The flow uses `usb-watch.sh` with two SC Live 4 specifics:

1. **The media port is on `usb1` (DWC OTG)**, not `usb3/usb4` as on the Prime
   GO. A stick in the back enumerates as:
   ```
   /sys/block/sda -> ../platform/ff540000.usb/usb1/1-1/1-1:1.0/host0/.../block/sda
   ```
   The control surface is a MIDI UART (card 0), *not* USB, so `usb1` is free
   for media. `USBWATCH_BUSES="1 2 3"` (the shipped default) watches all ports.

2. **`PATH` must include `/bin`** when launching rbp. The SC Live 4's default
   `PATH` is `/usr/bin:/usr/sbin`, but the chroot's busybox tools (`ls`, …) live
   in `/bin`. Without it rbp logs `sh: ls: not found` and the mount-handler
   helpers fail. `start-rb.sh` sets
   `PATH=/bin:/sbin:/usr/bin:/usr/sbin`.

## Working chain

```
stick (usb1, 1-1) → sda/sda1
  usb-watch.sh:  mount /dev/sda1 → /media/usb1/sda1 (vfat)
                 mount --bind → /data/rbx3-run/media/usb1/sda1
                 FIFO /tmp/udev_usb1: "umount …" then "mount /media/usb1/sda1"
  rbp UsbMountManager → DbProxy → DbIF::mount(type=3)
  DeviceSQL (edb_streamd) opens export.pdb + exportExt.pdb and analyses the
  library → USB 1 appears in Source / Browse
```

## SD card slot

The SD slot is mmc host 1 (`ff0c0000.dwmmc`), and a card in it enumerates as
`/dev/mmcblk1` with partitions `mmcblk1p1`, … (a `p` before the number). It is
not on a USB bus, so `usb-watch.sh` also lists any `mmcblk*` whose
`/sys/block/<dev>/device/type` is `SD`. The internal eMMC (`mmcblk0`, the `/data`
and rootfs disk) reports `MMC` and is never picked up.

An SD card is attached like a stick, with one rule: it takes **USB 2** (USB 1 if
USB 2 is busy), and a USB stick takes USB 1 (USB 2 if USB 1 is busy). A library
stick plus an SD card always show as USB 1 and USB 2, whatever order you put them
in. The unit has only two slots, so with an SD card in, one USB stick at a time.
The MOD eject buttons, the power-off release, and removal handling work the same
as for a stick.

rbp records a set to `<mount>/PIONEER REC/REC###.WAV`, and only ever to **USB 2**:
its REC key goes through the USB 2 slot manager, which has to be in the "database
attached" state. That needs a rekordbox `export.pdb`, so on a plain SD card the
key does nothing. The MOD **REC** row therefore calls the recorder itself
([14](14-using-the-extras.md#recording-a-set)), and the watcher's job is to keep
the card mounted at `/media/usb4/sda1` as USB 2.
exFAT (what SDXC cards ship with) mounts read-write with the kernel's `exfat`
driver; vfat and HFS+ use the same branches as sticks. The card does not need a
rekordbox library: with no `PIONEER/rekordbox/export.pdb` the watcher sends rbp
one `umount` / `mount` pair and moves on, instead of re-mounting every 8 s while
it waits for a library that never comes (that would stall the watcher and could
cut a recording).

If a card in the slot does not appear at all, check
`grep 'gpio-206' /sys/kernel/debug/gpio` and `dmesg | grep mmc1`. `gpio-206` is
the card-detect line: `lo` means the kernel thinks the slot is empty and it never
powers the card, so there is no `mmcblk1` for the watcher to find. A card that is
not pushed in until it clicks does that. Eject it with a push, pull it out, and
push it back in; `mmc1: new SDXC card` appears in `dmesg` when it is seen.

## Files

* `scripts/device/usb-watch.sh` — hotplug watcher (SC Live 4 buses), shipped to
  `/data/usb-watch.sh`.
* `/data/timeout` — a small ARM `timeout(1)` helper required by
  `usb-watch.sh notify()`.
* `fix-dev.sh` creates the `/tmp/udev_*` FIFOs and the `/etc/mtab` symlink.

## Run

```sh
sh /data/usb-watch.sh start    # one-shot; start-rb.sh does this automatically
sh /data/usb-watch.sh status   # mounted? chroot bind? media sd?
sh /data/usb-watch.sh stop
```

## Notes

* **`edisksd.service`** must stay stopped — it will bus-reset and unmount a
  storage device it doesn't manage (~30 s later).
* The stick is re-notified automatically when rbp restarts (the watcher tracks
  the rbp pid).
* A stick without a rekordbox export (no `PIONEER/rekordbox/export.pdb`) still
  mounts, but shows empty — folder browsing needs the native DB import.
