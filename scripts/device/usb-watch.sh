#!/bin/sh
# =============================================================================
# usb-watch.sh — SC Live 4 USB-A media ports -> rbp (rekordbox player)
#
# Two sticks. The first one seen is RX3 USB 1, the second is RX3 USB 2.
# They stay in those slots until ejected or unplugged.
#
#   slot 1: /dev/sdX1 -> /media/usb1/sda1  FIFO /tmp/udev_usb1
#   slot 2: /dev/sdY1 -> /media/usb4/sda1  FIFO /tmp/udev_usb2
#
# rbp's USB 2 mass-storage path is /media/usb4/sda1 (not /media/usb2).
# The MOD menu reads /tmp/usb-name-1 and /tmp/usb-name-2 (volume labels)
# and asks for an eject with /tmp/usb-eject-1 or /tmp/usb-eject-2.
# After a clean eject, /tmp/usb-pull-N appears until that stick is removed.
#
# Never write to /tmp/udev_usbctn* ("connect" there pops "USB Error").
#
# Usage:  sh /data/usb-watch.sh start|stop|status|run
# Env:    USBWATCH_BUSES="1 2 3"   USBWATCH_POLL=1
# =============================================================================

LOG=/data/usbwatch.log
PIDFILE=/tmp/usbwatch.pid
BUSES="${USBWATCH_BUSES:-1 2 3}"
POLL="${USBWATCH_POLL:-1}"
TIMEOUT=/data/timeout

dev1=""
dev2=""
ejected1=""
ejected2=""

log() { echo "$(date '+%F %T') $$ $*" >> "$LOG"; }

slot_mnt() {
  if [ "$1" = 1 ]; then echo /media/usb1/sda1; else echo /media/usb4/sda1; fi
}
slot_fifo() {
  if [ "$1" = 1 ]; then echo /tmp/udev_usb1; else echo /tmp/udev_usb2; fi
}
slot_name() { echo "/tmp/usb-name-$1"; }
slot_pull() { echo "/tmp/usb-pull-$1"; }
slot_req()  { echo "/tmp/usb-eject-$1"; }

# One kernel disk name per line, stable order (sysfs path).
list_media_sd() {
  for blk in /sys/block/sd*; do
    [ -e "$blk" ] || continue
    tgt=$(readlink "$blk" 2>/dev/null) || continue
    for b in $BUSES; do
      case "$tgt" in
        *"/usb$b/"*) echo "$tgt ${blk##*/}"; break ;;
      esac
    done
  done | sort | awk '{print $2}'
}

still_here() {
  list_media_sd | grep -x "$1" >/dev/null 2>&1
}

find_partition() {
  dev=$1
  i=0
  while [ $i -lt 40 ]; do
    [ -b "/dev/${dev}1" ] && { echo "${dev}1"; return 0; }
    i=$((i + 1)); sleep 0.1
  done
  if blkid "/dev/$dev" >/dev/null 2>&1; then echo "$dev"; return 0; fi
  return 1
}

notify_slot() {
  slot=$1
  msg=$2
  fifo=$(slot_fifo "$slot")
  if [ ! -p "$fifo" ]; then
    log "notify: $fifo missing (rbp down?) — skipping"
    return 1
  fi
  if "$TIMEOUT" 3 sh -c 'printf "%s" "$1" > "$2"' sh "$msg" "$fifo" 2>/dev/null; then
    log "notify slot$slot: $msg"
    return 0
  fi
  log "notify slot$slot: FAILED '$msg'"
  return 1
}

write_label() {
  slot=$1
  part=$2
  label=$(blkid -s LABEL -o value "/dev/$part" 2>/dev/null | tr -d '\r')
  printf '%s' "$label" > "$(slot_name "$slot")"
  log "slot$slot: label '${label:-<none>}'"
}

notify_mount_until_open() {
  slot=$1
  mnt=$(slot_mnt "$slot")
  n=0
  while [ $n -lt 12 ]; do
    sleep 4
    notify_slot "$slot" "umount $mnt"
    sleep 0.3
    notify_slot "$slot" "mount $mnt"
    sleep 4
    rbp=$(rbp_pid)
    if [ -n "$rbp" ] && ls -l /proc/$rbp/fd 2>/dev/null | grep -q "$mnt/PIONEER/rekordbox/export.pdb"; then
      log "slot$slot: rbp opened export.pdb (attempt $n)"
      return 0
    fi
    n=$((n + 1))
  done
  log "slot$slot: rbp never opened export.pdb"
  return 1
}

attach() {
  slot=$1
  dev=$2
  mnt=$(slot_mnt "$slot")
  chmnt=/data/rbx3-run$mnt
  part=$(find_partition "$dev") || { log "slot$slot: no partition on $dev"; return 1; }

  if mountpoint -q "$mnt"; then
    src=$(awk -v m="$mnt" '$2==m {print $1; exit}' /proc/mounts)
    case "$src" in
      "/dev/$part") log "slot$slot: $mnt already $part" ;;
      *)
        log "slot$slot: $mnt held by $src, replacing"
        umount -l "$chmnt" 2>/dev/null
        umount -l "$mnt" 2>/dev/null
        ;;
    esac
  fi

  if ! mountpoint -q "$mnt"; then
    fstype=$(blkid -s TYPE -o value "/dev/$part" 2>/dev/null)
    [ -n "$fstype" ] || fstype=vfat
    mkdir -p "$mnt"
    case "$fstype" in
      vfat)    mount -t vfat -o flush,rw,noatime,shortname=mixed,dmask=000,fmask=000,codepage=437,iocharset=iso8859-1,usefree,utf8 "/dev/$part" "$mnt" ;;
      exfat)   mount -t exfat -o rw,noatime "/dev/$part" "$mnt" ;;
      hfsplus) mount -t hfsplus -o force,rw,noatime "/dev/$part" "$mnt" ;;
      *)       mount "/dev/$part" "$mnt" ;;
    esac
    rc=$?
    if [ $rc -ne 0 ]; then
      log "slot$slot: mount /dev/$part -> $mnt failed rc=$rc"
      return 1
    fi
    log "slot$slot: mounted /dev/$part ($fstype) -> $mnt"
  fi

  mkdir -p "$chmnt"
  if ! mountpoint -q "$chmnt"; then
    if ! mount --bind "$mnt" "$chmnt"; then
      log "slot$slot: chroot bind failed"
      return 1
    fi
  fi

  write_label "$slot" "$part"
  rm -f "$(slot_pull "$slot")"
  notify_mount_until_open "$slot"
  return 0
}

detach() {
  slot=$1
  mnt=$(slot_mnt "$slot")
  chmnt=/data/rbx3-run$mnt
  log "slot$slot: detach"
  notify_slot "$slot" "umount $mnt"
  sleep 1
  sync
  if mountpoint -q "$chmnt"; then umount -l "$chmnt"; fi
  if mountpoint -q "$mnt"; then umount -l "$mnt"; fi
  rmdir "$chmnt" 2>/dev/null
  rmdir "$mnt" 2>/dev/null
  rm -f "$(slot_name "$slot")"
}

rbp_pid() { ps w | awk '/\/root\/pdj\/rbp/ && !/sh -c/ && !/strace/ && !/awk/ {print $1; exit}'; }

held() {
  dev=$1
  [ -n "$dev" ] || return 1
  [ "$dev" = "$ejected1" ] || [ "$dev" = "$ejected2" ]
}

clear_eject_if_gone() {
  if [ -n "$ejected1" ] && ! still_here "$ejected1"; then
    log "slot1: stick removed"
    ejected1=""
    rm -f /tmp/usb-pull-1
  fi
  if [ -n "$ejected2" ] && ! still_here "$ejected2"; then
    log "slot2: stick removed"
    ejected2=""
    rm -f /tmp/usb-pull-2
  fi
}

take_eject() {
  slot=$1
  req=$(slot_req "$slot")
  [ -f "$req" ] || return 0
  rm -f "$req"
  eval "cur=\$dev$slot"
  if [ -n "$cur" ] || mountpoint -q "$(slot_mnt "$slot")"; then
    detach "$slot"
    eval "ejected$slot=\$cur"
    eval "dev$slot="
    printf 'pull\n' > "$(slot_pull "$slot")"
    log "slot$slot: released, pull the stick"
  else
    rm -f "$(slot_pull "$slot")"
    log "slot$slot: eject with nothing mounted"
  fi
}

fill_free_slots() {
  for dev in $(list_media_sd); do
    held "$dev" && continue
    if [ "$dev" = "$dev1" ] || [ "$dev" = "$dev2" ]; then
      continue
    fi
    if [ -z "$dev1" ]; then
      if attach 1 "$dev"; then dev1=$dev; else log "slot1: attach $dev failed"; fi
    elif [ -z "$dev2" ]; then
      if attach 2 "$dev"; then dev2=$dev; else log "slot2: attach $dev failed"; fi
    fi
  done
}

drop_if_gone() {
  slot=$1
  eval "cur=\$dev$slot"
  [ -n "$cur" ] || return 0
  if ! still_here "$cur"; then
    log "slot$slot: $cur disappeared"
    detach "$slot"
    eval "dev$slot="
  fi
}

run() {
  log "=== usb-watch run: two slots, buses [$BUSES], poll ${POLL}s ==="
  dev1=""
  dev2=""
  ejected1=""
  ejected2=""
  last_rbp=$(rbp_pid)

  while :; do
    rbp=$(rbp_pid)
    clear_eject_if_gone
    take_eject 1
    take_eject 2
    drop_if_gone 1
    drop_if_gone 2

    if [ -n "$rbp" ] && [ "$rbp" != "$last_rbp" ]; then
      log "rbp restarted ($last_rbp -> $rbp), re-notifying"
      [ -n "$dev1" ] && notify_mount_until_open 1
      [ -n "$dev2" ] && notify_mount_until_open 2
    fi

    fill_free_slots
    last_rbp=$rbp
    sleep "$POLL"
  done
}

start() {
  if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    echo "already running (pid $(cat "$PIDFILE"))"
    return 0
  fi
  log "=== usb-watch start ==="
  nohup sh "$0" run >/dev/null 2>&1 &
  echo $! > "$PIDFILE"
  sleep 1
  echo "started pid $(cat "$PIDFILE")"
}

stop() {
  if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    kill "$(cat "$PIDFILE")" 2>/dev/null
    rm -f "$PIDFILE"
    echo "stopped"
  else
    echo "not running"
  fi
}

status() {
  echo "pid: $(cat "$PIDFILE" 2>/dev/null || echo none)"
  echo "disks: $(list_media_sd | tr '\n' ' ')"
  echo "slot1 $(slot_mnt 1): $(mountpoint -q "$(slot_mnt 1)" && echo mounted || echo no) name=$(cat "$(slot_name 1)" 2>/dev/null)"
  echo "slot2 $(slot_mnt 2): $(mountpoint -q "$(slot_mnt 2)" && echo mounted || echo no) name=$(cat "$(slot_name 2)" 2>/dev/null)"
  echo "--- log tail ---"
  tail -15 "$LOG" 2>/dev/null
}

case "$1" in
  start)  start ;;
  stop)   stop ;;
  status) status ;;
  run)    run ;;
  *) echo "usage: $0 start|stop|status"; exit 1 ;;
esac
