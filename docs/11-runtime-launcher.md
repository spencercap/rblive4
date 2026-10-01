# 11 — Runtime launcher

[`scripts/device/start-rb.sh`](../scripts/device/start-rb.sh) runs on the device
and is the normal way to bring up the player.

## What it does

1. `systemctl stop engine.service edisksd.service` — `engine` owns the display/
   audio/MIDI; `edisksd` will bus-reset a USB drive it doesn't manage.
2. Kill stale `rbp`/`edb_streamd`.
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
