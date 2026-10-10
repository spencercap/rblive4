#!/usr/bin/env python3
"""Bring the My Tag edits made on the player into rekordbox's library on the computer.

rekordbox 6 does not import them itself: Update Collection brings back cue points and beat grids only, and its
"Sync with My Tag" goes the other way (the library overwrites the tags stored on the stick). So edits made on the
player have to be added to the library's master.db, with rekordbox closed. This does that, for the tracks that
are on the stick and in the library, using the stick's own link to the library: exportLibrary.db keeps each
track's library id (content.masterContentId) and its tag ids are the library's (myTag.myTag_id = djmdMyTag.ID).

    tools/merge-mytags-into-rekordbox.py            # dry run: show what would change, writes nothing
    tools/merge-mytags-into-rekordbox.py --apply    # quit rekordbox first; backs master.db up beside itself

What counts as an edit: the stick's tags (exportExt.pdb, which is what the player writes) compared with the
stick's tags the last time this ran (kept in --state). Tags added in rekordbox in between are left alone. The
first run has no such record, so it only adds (a tag on the stick that the library lacks) and removes nothing.
Removing a tag marks the row deleted the way rekordbox does (rb_local_deleted), it is not erased.

Run "Sync with My Tag" in rekordbox only after this, never before: it replaces the stick's tags with the library's.

Needs pyrekordbox with OneLibrary support (not on PyPI yet):
    python3 -m venv .venv && .venv/bin/pip install git+https://github.com/dylanljones/pyrekordbox.git
"""
import argparse
import datetime
import glob
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid

PG = 4096


class Pdb:
    """Just enough of a DeviceSQL .pdb to walk the rows of one table."""

    def __init__(self, path):
        self.d = open(path, "rb").read()
        n = struct.unpack_from("<I", self.d, 8)[0]
        self.tabs = {}
        for i in range(n):
            t, _empty, first, last = struct.unpack_from("<IIII", self.d, 28 + 16 * i)
            self.tabs[t] = (first, last)

    def rows(self, ttype):
        if ttype not in self.tabs:
            return
        first, last = self.tabs[ttype]
        p, seen = first, set()
        while p not in seen:
            seen.add(p)
            pg = self.d[p * PG:(p + 1) * PG]
            nxt = struct.unpack_from("<I", pg, 12)[0]
            nrow = struct.unpack_from("<I", pg, 0x18)[0] & 0x1FFF
            if not pg[0x1B] & 0x40:
                for i in range(nrow):
                    g, j = divmod(i, 16)
                    base = PG - 0x24 * (g + 1)
                    if not (struct.unpack_from("<H", pg, base + 32)[0] >> j) & 1:
                        continue
                    off = struct.unpack_from("<H", pg, base + 2 * (15 - j))[0]
                    yield pg[0x28 + off:]
            if p == last:
                break
            p = nxt


def dsql_string(row, off):
    b = row[off]
    if b & 1:
        return row[off + 1:off + 1 + (b >> 1) - 1].decode("latin-1")
    n = struct.unpack_from("<H", row, off + 1)[0] - 4
    if b == 0x40:
        return row[off + 4:off + 4 + n].decode("latin-1")
    if b == 0x90:
        return row[off + 4:off + 4 + n].decode("utf-16le")
    raise ValueError("unknown string kind 0x%02x" % b)


def player_tags(rekordbox_dir):
    """{file path: set of tag ids} as the player has them (exportExt.pdb, with paths from export.pdb)."""
    exp = Pdb(os.path.join(rekordbox_dir, "export.pdb"))
    path_of = {}
    for r in exp.rows(0):
        tid = struct.unpack_from("<I", r, 0x48)[0]
        path_of[tid] = dsql_string(r, struct.unpack_from("<H", r, 0x5E + 2 * 20)[0])
    ext = Pdb(os.path.join(rekordbox_dir, "exportExt.pdb"))
    out = {p: set() for p in path_of.values()}
    for r in ext.rows(4):
        track, tag = struct.unpack_from("<II", r, 4)
        if track in path_of:
            out[path_of[track]].add(tag)
    return out


def snapshot(src_db, dest_dir):
    """Copy a (possibly open) SQLite file with its -wal and -shm so it reads consistently."""
    dst = os.path.join(dest_dir, os.path.basename(src_db))
    for suffix in ("", "-wal", "-shm"):
        if os.path.exists(src_db + suffix):
            shutil.copy2(src_db + suffix, dst + suffix)
    return dst


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stick", help="the stick's PIONEER/rekordbox folder (default: the one mounted under /Volumes)")
    ap.add_argument("--library", default=os.path.expanduser("~/Library/Pioneer/rekordbox/master.db"))
    ap.add_argument("--state", default=os.path.expanduser("~/Library/Pioneer/rekordbox/mytag-merge-state.json"),
                    help="where the stick's tags as of the last run are kept")
    ap.add_argument("--apply", action="store_true", help="write master.db (rekordbox must be closed)")
    ap.add_argument("--allow-running", action="store_true", help="skip the rekordbox check (only for a copy of master.db)")
    a = ap.parse_args()
    try:
        import sqlalchemy as sa
        from pyrekordbox.masterdb import MasterDatabase, models
        from pyrekordbox.onelibrary import OneLibrary
    except ImportError:
        sys.exit("needs pyrekordbox with OneLibrary support, see the top of this file")

    stick = a.stick
    if not stick:
        found = glob.glob("/Volumes/*/PIONEER/rekordbox/exportLibrary.db")
        if len(found) != 1:
            sys.exit("found %d sticks under /Volumes; give --stick" % len(found))
        stick = os.path.dirname(found[0])
    if a.apply and not a.allow_running and subprocess.run(["pgrep", "-fi", "rekordbox"], capture_output=True).stdout.strip():
        sys.exit("rekordbox is running: quit it (and eject the stick from it) first")

    tmp = tempfile.mkdtemp(prefix="mytag-merge-")
    dlp = OneLibrary(snapshot(os.path.join(stick, "exportLibrary.db"), tmp))
    dq = lambda s: dlp.session.execute(sa.text(s)).fetchall()
    prop = dq("select deviceName, createdDate, myTagMasterDBID from property")[0]
    stick_key = "%s|%s" % (prop[0], prop[1])

    # the stick, in the library's numbering
    lib_copy = a.library if a.apply else snapshot(a.library, tmp)
    mdb = MasterDatabase(path=lib_copy)
    mq = lambda s, **k: mdb.session.execute(sa.text(s), k).fetchall()
    dbid = int(mq("select DBID from djmdProperty")[0][0])
    link = {r[0]: int(r[1]) for r in dq("select path, masterContentId from content where masterDbId=%d" % dbid)}
    if not link:
        sys.exit("no track on this stick links to this library (DBID %d)" % dbid)
    names = {int(r[0]): r[1] for r in mq("select ID, Name from djmdMyTag")}
    title = {int(r[0]): r[1] for r in mq("select ID, Title from djmdContent")}
    pt = player_tags(stick)
    device = {link[p]: {t for t in ts if t in names} for p, ts in pt.items() if p in link}
    dlp_tags = {cid: set() for cid in link.values()}
    for t, c in dq("select m.myTag_id, c.masterContentId from myTag_content m join content c on c.content_id=m.content_id "
                   "where c.masterDbId=%d" % dbid):
        dlp_tags.setdefault(int(c), set()).add(int(t))
    behind = sum(1 for c in device if device[c] != dlp_tags.get(c, set()))
    if behind:
        print("note: exportLibrary.db differs from exportExt.pdb on %d track(s); using exportExt.pdb, which is what the "
              "player wrote" % behind)

    live = {}
    for t, c in mq("select MyTagID, ContentID from djmdSongMyTag where rb_local_deleted=0"):
        live.setdefault(int(c), set()).add(int(t))
    state = {}
    if os.path.exists(a.state):
        state = json.load(open(a.state))
    base = state.get(stick_key)
    first = base is None
    base = {int(c): set(ts) for c, ts in (base or {}).get("tags", {}).items()}

    adds, rems = [], []
    for c, d in sorted(device.items()):
        if first:
            adds += [(c, t) for t in sorted(d - live.get(c, set()))]
        else:
            b = base.get(c, set())
            adds += [(c, t) for t in sorted((d - b) - live.get(c, set()))]
            rems += [(c, t) for t in sorted((b - d) & live.get(c, set()))]
    print("stick %s: %d tracks link to this library%s" % (stick_key, len(link),
          "; first run, so only adding" if first else ""))
    for c, t in adds:
        print("  + %-18s %s" % (names[t], title.get(c, c)))
    for c, t in rems:
        print("  - %-18s %s" % (names[t], title.get(c, c)))
    if not adds and not rems:
        print("the library already has the player's tags")
    if not a.apply:
        print("dry run: nothing written." + (" Add --apply to write." if adds or rems else ""))
        return
    if adds or rems:
        backup = os.path.join(os.path.dirname(a.library), "master.backup-before-mytag-merge-%s.db" %
                              datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
        shutil.copy2(a.library, backup)
        for suffix in ("-wal", "-shm"):
            if os.path.exists(a.library + suffix):
                shutil.copy2(a.library + suffix, backup + suffix)
        print("backup:", backup)
        now = datetime.datetime.now(datetime.timezone.utc)
        usn = mdb.get_local_usn()
        S = models.DjmdSongMyTag
        for c, t in adds:
            usn += 1
            row = mdb.session.query(S).filter_by(MyTagID=str(t), ContentID=str(c)).first()
            if row is not None:                           # a row marked deleted earlier: bring it back
                row.rb_local_deleted, row.rb_local_usn, row.updated_at = 0, usn, now
            else:
                mdb.session.add(S(ID=str(uuid.uuid4()), MyTagID=str(t), ContentID=str(c), TrackNo=None, UUID=str(uuid.uuid4()),
                                  rb_local_usn=usn, created_at=now, updated_at=now))
        for c, t in rems:
            usn += 1
            row = mdb.session.query(S).filter_by(MyTagID=str(t), ContentID=str(c), rb_local_deleted=0).first()
            row.rb_local_deleted, row.rb_local_usn, row.updated_at = 1, usn, now
        mdb.set_local_usn(usn)
        mdb.session.commit()
        ok = mq("pragma integrity_check")[0][0]
        if ok != "ok":
            sys.exit("integrity_check says %r; restore %s" % (ok, backup))
        print("library updated: %d added, %d removed" % (len(adds), len(rems)))
    state[stick_key] = {"tags": {str(c): sorted(ts) for c, ts in device.items()},
                        "saved": datetime.datetime.now().isoformat(timespec="seconds")}
    json.dump(state, open(a.state, "w"))
    print("remembered the stick's tags in", a.state)


if __name__ == "__main__":
    main()
