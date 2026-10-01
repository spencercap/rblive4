# 13 — Engine OS 4.3.1 → 5.0.4

What actually happened when a rooted SC Live 4 (`JP21`) running the rekordbox
player was moved from Engine OS 4.3.1 to 5.0.4, and what has to be true before
doing it again.

The player itself did not need a rebuild. 5.0.4 still runs the soft-float
chroot. The upgrade wipes `/data`, drops root SSH, and changes a few host
tools the installer depends on. Restore root first, then copy the player back.

Confirmed after this upgrade: Engine **5.0.4** is running, dm-verity is still
enforcing a signed rootfs, root SSH survives a reboot, and `start-rb.sh`
brings the player up.

## What changed on the unit

| | 4.3.1, before | 5.0.4, after |
|---|---|---|
| Engine version | 4.3.1 | 5.0.4, in `/usr/Engine/Scripts/publish_manifest.yml` |
| Userspace | Buildroot 2023.02.11 | `az0x 5.0.14 (scarthgap)`, Yocto |
| `/etc/os-release` | `Buildroot 2023.02.11` | does **not** say 5.0.4. Read the manifest |
| Kernel | `6.1.111-inmusic` | `6.6.119-az01-2025-12-17-rt67` |
| U-Boot | 2024.07 | `2026.01-inmusic-20260206` |
| glibc | older Buildroot glibc | 2.39, hard-float, `/usr/lib/ld-linux-armhf.so.3` |
| Root SSH | overlay preload | gone until `/data` is flashed again |
| Player files | `/data/start-rb.sh`, `/data/rbx3-run` | gone. `/data` is a new filesystem |
| `chown -R` | usable on the chroot | follows symlinks into the read-only rootfs and aborts |

The signed boot chain is the same idea as 4.3.1. Root is still
`/dev/dm-0` over `mmcblk0p10` with `verity`. Do not patch the rootfs, kernel
FIT, or U-Boot. A modified rootfs boot-loops back to fastboot.

`/data` is still `mmcblk0p11`, plain ext4, about 6.2 GB. `df` after the
working flash reports 5.9 GB. The fastboot size variable is wrong; see below.

## Before any write

1. Boot the unit in Engine, with nothing mounted under `/data`. `start-rb.sh`
   bind-mounts `/dev`, `/proc`, `/sys`, and USB sticks inside the chroot, and
   `device-tools/backup.sh` refuses to run in that state.
2. Take a full backup:
   ```sh
   device-tools/backup.sh <device>
   ```
   Check `SHA256SUMS`, that `data.tar` lists cleanly, and that `emmc.img` is
   exactly `7,820,083,200` bytes on this unit. The live `md5sum` of
   `/dev/mmcblk0` will usually differ from the file, because `/data` is
   mounted read-write while it is being copied. `data.tar` is the copy that
   matters for the library.
3. Keep `firmware/SCLIVE4-4.3.1-Update.img` and the 5.0.4 image. The 5.0.4
   file used here hashes to
   `2933d26440f0b6a29d1f3fa6c9e07e2f188630f41f5cd4124065c7d46dddb060`.
   Its AZ0x header lists `JP21`. The header version string is
   `SNAPSHOT-20260724102220`; the Engine release inside is still 5.0.4.
4. Build the recovery `/data` image **before** updating, at `--size 6240`.
   After the update there is no SSH left to build it from the device.

The backup from this run is `backup/20261001-000630/`.

## Apply the vendor image

Use the image's own updater. Do not fastboot the `.img` as a partition.

On 4.3.1 the supported file path is Engine's `UpdateFromFile` quit reason,
which runs:

```sh
/usr/sbin/az01-update /path/to/SCLIVE4-5.0.4-Update.img
```

That binary's own usage is `az01-update [--file|--net] <file|url>`. The 4.3.1
Engine script calls it with the path only. The 5.0.4 script calls it with
`--file`. Match the script on the version that is about to start the update.

What worked:

1. Put only `SCLIVE4-5.0.4-Update.img` at the root of an exFAT stick the unit
   already has mounted. Move any older `SCLIVE4-*-Update.img` out of the root
   first (a `firmware-rollback/` directory on the same stick is fine).
2. Confirm the on-stick SHA-256 matches the file on the Mac.
3. From the rooted 4.3.1 shell, write the stick path to
   `/tmp/update-file-path`, write `UpdateFromFile` to
   `/tmp/engine-quit-reason`, and `SIGTERM` the `Engine` process. The engine
   wrapper then execs `az01-update`.
4. Leave it alone. SSH drops, then the port refuses connections, then the
   unit boots 5.0.4 on its own. The screen shows the vendor update text while
   it is writing.

The same stick at the root of a USB port is also what the on-screen
About / Update reboot uses. While that file sits in the root of a connected
stick, a later boot into update mode can pick it up again. Move it off the
root when the update is finished.

## Root does not survive the update

The first 5.0.4 boot answered on the network and had no listener on port 22.
That is expected. Plan on flashing `/data` again.

5.0.4's `/etc/az01/overlayfs.conf` still builds `/etc` from
`/data/system/etc/overlay`, and it does **not** delete
`ld.so.preload`. It does delete overlay copies of `passwd`, `shadow`,
`group`, `gshadow`, and the whole overlay `ssh` directory, on every boot.
`/usr/sbin/az0x-data-mkfs` (the old `az01-data-mkfs`) still runs
`fsck.ext4 -y`, then exits with the status from `tune2fs -O encrypt`,
not from fsck. On the 5.0.4 `tune2fs`, setting `encrypt` when it is
already set returns 0, so a later boot does not reformat `/data` for
that reason. A filesystem that does not fill the partition is still
the image to avoid: the 2303 MiB image flashed and never started SSH.

So the 4.3.1 method still applies, with the same rule: one ext4 image, label
`az01-data`, feature `encrypt`, sized to the real partition, containing:

* `/ssh/freemymprime.so`
* `/system/etc/overlay/ld.so.preload` pointing at that `.so`
* `/ssh/setup.sh`, `/ssh/sshd_config`, `/ssh/authorized_keys`

Build the `.so` with `arm-linux-gnueabihf-gcc -shared -fPIC -O2`. It must
stay on `GLIBC_2.4`. 5.0.4's loader is
`/lib/ld-linux-armhf.so.3` from glibc 2.39, and this `.so` loads there.
Check with `qemu-arm` and that loader before flashing. `sshd` is
`/usr/sbin/sshd`, and `sftp-server` is still `/usr/libexec/sftp-server`.
There is no busybox `telnetd` on 5.0.4; the setup log will say
`no busybox telnetd`. That is not a failure. `sshd rc=0` is the success line.

`setup.sh` generates a new SSH host key. After the first login, remove the
old `known_hosts` line for the unit.

## Fastboot lies about the size, and macOS fastboot cannot send the raw image

Update mode is still **BACK + FWD + Browse Encoder**, held while powering on,
until the USB computer-update screen is up. `fastboot` then reports
`product: rk3288-az05-jp21`.

`fastboot getvar partition-size:data` returns `0x8ff4be00`
(2,415,181,312 bytes). That value is a 32-bit truncation of the real
partition, which is still about 6.2 GB. An image built to the reported size
(2303 MiB) flashes cleanly and then never starts SSH: the filesystem does not
fill the partition, `az0x-data-mkfs` fails its fsck, and `/data` never
mounts, so the preload never runs. There is no shell on that boot to confirm
the journal. The 6240 MiB image is the one that came up.

Build it the way FreeMyPrime already says:

```sh
tools/make_data_overlay.sh --pubkey <device-key>.pub \
    --out work/data-ssh.img --size 6240
e2fsck -fn work/data-ssh.img
```

Do not `resize2fs` it afterwards.

On this Mac, `fastboot flash data` of that raw file fails immediately with
`Failed reading from data`, before the unit writes anything:

* `mke2fs` leaves a sparse file. `du` shows a couple of megabytes while
  `stat` shows 6.2 GB. macOS fastboot cannot read the holes.
* Copying it into a fully allocated file (`dd`) still fails the same way.

Convert the checked image to an Android sparse image and flash that:

```sh
img2simg work/data-ssh.img work/data-ssh.simg
fastboot flash data work/data-ssh.simg
fastboot reboot
```

The sparse file is a few megabytes. The unit expands it. A good flash looks
like `Sending 'data' (3304 KB)` followed by `Writing 'data' OKAY` after about
three minutes. The warning `skip copying data image avb footer due to sparse
image` is from the host fastboot client and did not affect the write.
`fastboot -S` does not avoid the raw-image read failure.

The sparse header must expand back to 6,543,114,240 bytes (`6240 MiB`).
If `Writing` returns an error, stop. Do not erase boot or rootfs from
fastboot to "retry".

First boot after this flash takes longer than a normal boot. SSH is up when
`/data/ssh/setup.log` contains `sshd rc=0` and `/etc/ld.so.preload` still
reads `/data/ssh/freemymprime.so`. Reboot once more. A second setup stanza in
that log is the persistence check. `systemctl is-active engine` should be
`active`, and `/proc/cmdline` should still contain `verity`.

## Put the player back

The `/data` flash deletes `start-rb.sh`, the shims, `rbp-audio`, and
`rbx3-run`. The copies that belong on the unit are the `deploy/` set from
`device-tools/deploy.sh`:

```
rbx3-run.tgz  rbp-audio  knobshim2.so  audioshim.so  fbshim-tsc.so
libdirectfb_fbdev-rot16.so  fix-dev.sh  start-rb.sh  usb-watch.sh  timeout
```

`deploy/start-rb.sh` can lag `rblive4_sc/scripts/device/start-rb.sh`. Copy the
source script into `deploy/` first when they differ, then:

```sh
device-tools/deploy.sh <device>
```

That copies the files, checks MD5, and unpacks `/data/rbx3-run`. It does not
start the player.

On 5.0.4, `chown -R root:root /data/rbx3-run` follows chroot symlinks such as
`sbin/e2label -> /sbin/findfs` into the read-only signed rootfs, prints
`Read-only file system`, and the script exits before `chroot ok`. The tree is
already extracted. Finish with:

```sh
chown -h -R root:root /data/rbx3-run
chroot /data/rbx3-run /bin/sh -c 'echo chroot ok'
```

Launch only after that, with the master volume down:

```sh
ssh root@<device> 'sh /data/start-rb.sh'
```

Ready is the line `ready` in `/data/start-rb.log`.

While that launcher is running, keep `usb-watch`, `root/pdj/rbp`, and
`edb_streamd` out of the SSH command line. Its process scan matches those
strings and will kill the SSH session.

## SSH dies when the player starts

After the player was launched on 5.0.4, the unit stayed on the network and
port 22 refused connections. Engine's screen was up, and `scx4.local` still
resolved to the unit. The SD card was out, so this was not the update file
being installed again. Reflashing the same `/data` image and rebooting did
not bring `sshd` back either.

The preload runs inside whichever process loads it. That process is Engine.
The old code forked `setup.sh` from there and set
`/run/freemymprime.started` before `sshd` was actually up. Two consequences:

* The child stays in `engine.service`'s cgroup. `start-rb.sh` stops that
  service, and systemd kills every process in the cgroup, including `sshd`.
  The player is running, the network is up, and nothing is listening on
  port 22.
* The marker is per boot. One early attempt, including one that loses the
  race with a short-lived service, is the only attempt until the next reboot.

`/data/ssh/setup.sh` is now started with `systemd-run --unit=freemymprime-setup`,
so pid 1 places it in its own unit. The script records its pid in
`/run/freemymprime.pid` and loops, starting `sshd` again if it exits. The
preload tries again on a later process when that pid is not alive, instead
of giving up for the rest of the boot.

After the corrected image was flashed, with Engine active:

```text
systemctl is-active freemymprime-setup
active
# /proc/<sshd-pid>/cgroup
0::/system.slice/freemymprime-setup.service
```

`sshd` is no longer inside `engine.service`. Stopping Engine to launch the
player does not take SSH with it.

A `/data` flash also removes the saved Wi-Fi network. One boot after a
reflash stopped on the password screen, and the unit was not reachable until
that password was entered again. The player files are removed by the same
flash; copy them back with `device-tools/deploy.sh` and the `chown -h` step
above.

## If it goes wrong

Stop on a hash mismatch, a product id other than `JP21`, an updater error, a
boot loop, or a fastboot write error.

* Update mode still accepts a signed vendor image. The 4.3.1 image is the
  rollback for the firmware itself.
* A `/data` image that is too small fails closed: Engine boots, SSH does not.
  Flash the 6240 MiB sparse image. Do not grow the small one with `resize2fs`.
* Do not write `rootfs`, `kernel`, or the U-Boot partitions from a hand-cut
  image. The AZ0x container hashes each compressed payload with SHA-256 and
  the bootloader checks FIT signatures. Inspection is safe; replacement is not
  part of this upgrade.

## What the 5.0.4 file actually is

`SCLIVE4-5.0.4-Update.img` is an `AZ0x` container, not a FIT and not a raw
disk. Cleartext at the start is four boot FIT images plus `PART` records.
The OS payloads are two XZ streams:

| Stream | Offset | Compressed size | Unpacked |
|---|---|---|---|
| first | 18,863,368 | 5,908,048 | small ext4, ~6.3 MB |
| rootfs | 24,771,416 | 449,663,180 | ext4 plus a trailing verity hash tree, 812,508,160 bytes total |

The rootfs `PART` record stores that offset, that length, and the SHA-256 of
the compressed bytes. The unpacked ext4 is the scarthgap rootfs
(`UUID=e1e953be-9bdf-43af-a20a-666d5a95f452`). Reading it is how the overlay
rules, `sshd` paths, and glibc ABI above were checked. Nothing in that
inspection was written back into the image that got flashed.
