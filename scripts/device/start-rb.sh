#!/bin/sh
# start-rb.sh — launch the XDJ-RX3 rekordbox player on the Denon SC Live 4.
# Adapted from PrimeBox (the SC Live 4 runs the same Buildroot/systemd base).

# 1. Stop Engine OS + disk service (releases audio, USB, controls)
systemctl stop engine.service edisksd.service 2>/dev/null
sleep 1

# 2. Kill any stale rb processes
for p in $(ps w | awk '$0 ~ /[s]trace|[r]oot\/pdj\/[r]bp|[e]db_streamd|[g]dbserver|[u]sb-watch/ {print $1}'); do
    kill -9 $p 2>/dev/null
done
sleep 1

# 3. Setup device binds, stubs, and FIFOs
sh /data/fix-dev.sh

# 4. Deploy binary and shims into the chroot
cp /data/rbp-audio /data/rbx3-run/root/pdj/rbp
chmod 755 /data/rbx3-run/root/pdj/rbp

cp /data/knobshim2.so /data/rbx3-run/root/pdj/knobshim.so
cp /data/knobshim2.so /data/rbx3-run/usr/lib/knobshim.so
chmod 755 /data/rbx3-run/usr/lib/knobshim.so /data/rbx3-run/root/pdj/knobshim.so

cp /data/audioshim.so /data/rbx3-run/root/pdj/audioshim.so
cp /data/audioshim.so /data/rbx3-run/usr/lib/audioshim.so
chmod 755 /data/rbx3-run/usr/lib/audioshim.so /data/rbx3-run/root/pdj/audioshim.so

cp /data/fbshim-tsc.so /data/rbx3-run/root/pdj/fbshim.so
cp /data/fbshim-tsc.so /data/rbx3-run/usr/lib/fbshim.so
chmod 755 /data/rbx3-run/usr/lib/fbshim.so /data/rbx3-run/root/pdj/fbshim.so

cp /data/libdirectfb_fbdev-rot16.so /data/rbx3-run/usr/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so
chmod 755 /data/rbx3-run/usr/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so

# 5. Clean stale IPC and logs
rm -f /tmp/guard_LocalDBServer /tmp/req_LocalDBServer /tmp/knobshim.log /tmp/audioshim.log /tmp/dfbdig*.log /tmp/rot_surface.dump
rm -f /data/rbp-p.log

# 6. Start EDB daemon inside chroot
export EDB_BIN=/usr/bin
nohup chroot /data/rbx3-run /lib/ld-linux.so.3 /usr/bin/edb_streamd > /data/edb_d.log 2>&1 &
sleep 1

# 7. Stop USB watcher during startup
sh /data/usb-watch.sh stop 2>/dev/null

# 8. Start rbp cleanly inside chroot
nohup chroot /data/rbx3-run env PATH=/bin:/sbin:/usr/bin:/usr/sbin DFB_ROTATE=left BEATLOOP=1 STARTUP_MUTE_MS=1500 STARTUP_FADE_MS=300 LD_PRELOAD=/usr/lib/fbshim.so:/usr/lib/knobshim.so:/usr/lib/audioshim.so /lib/ld-linux.so.3 /root/pdj/rbp -a </dev/null >/data/rbp-p.log 2>&1 &

echo "launched rbp, waiting for initialization..."
RBP=""
for i in $(seq 1 30); do
  RBP=$(ps w | awk '/\/root\/pdj\/rbp/ && !/sh -c/ && !/awk/ {print $1; exit}')
  if [ -n "$RBP" ] && ls -l /proc/$RBP/fd 2>/dev/null | grep -q udev_usb1; then
    echo "RBP=$RBP ready (udev_usb1 fd opened)"
    break
  fi
  sleep 0.5
done

# 9. Start USB watcher once rbp is ready
sh /data/usb-watch.sh start 2>/dev/null

# Host mounts and the chroot binds the player actually reads.
slots_busy() {
  mountpoint -q /media/usb1/sda1 && return 0
  mountpoint -q /media/usb4/sda1 && return 0
  mountpoint -q /data/rbx3-run/media/usb1/sda1 && return 0
  mountpoint -q /data/rbx3-run/media/usb4/sda1 && return 0
  return 1
}

# Ask the watcher for the same detach the MOD eject buttons use, then
# power off only after both sticks are unmounted.
poweroff_clean() {
  rm -f /tmp/rb-poweroff
  : > /tmp/usb-eject-1
  : > /tmp/usb-eject-2
  n=0
  while slots_busy && [ "$n" -lt 8 ]; do
    sleep 1
    n=$((n + 1))
  done
  if slots_busy; then
    sh /data/usb-watch.sh release
  fi
  sh /data/usb-watch.sh stop
  sync
  systemctl poweroff
  exit 0
}

# 10. Wait while rbp is running (so launcher does not redraw on top of rbp)
if [ -n "$RBP" ]; then
  while kill -0 $RBP 2>/dev/null; do
    if [ -f /tmp/rb-poweroff ]; then
      poweroff_clean
    fi
    sleep 2
  done
fi

# Cleanup on exit
sh /data/usb-watch.sh stop 2>/dev/null
for p in $(ps w | awk '$0 ~ /[r]oot\/pdj\/[r]bp|[e]db_streamd/ {print $1}'); do
    kill -9 $p 2>/dev/null
done
