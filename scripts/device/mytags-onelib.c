/* mytags-onelib.c: copy the My Tag edits made on the player into the stick's exportLibrary.db.
 *
 * rbp writes tag edits to PIONEER/rekordbox/exportExt.pdb only. rekordbox for Mac/Windows and newer players read
 * the other library on the stick, exportLibrary.db (OneLibrary, a SQLCipher database), which keeps the tags of the
 * last export. This makes the tags of every track that is in both libraries equal to the player's, matching tracks
 * by file path (the two libraries number their tracks differently).
 *
 *   mytags-onelib [-n] [-k KEYFILE] [-b BACKUPDIR] DIR
 *
 *   DIR      the stick's PIONEER/rekordbox folder (export.pdb, exportExt.pdb, exportLibrary.db)
 *   -k FILE  the database key, first line of FILE (default /data/onelibrary.key)
 *   -b DIR   copy exportLibrary.db to DIR before the first change (kept, one file per day)
 *   -n       only print what would change
 *
 * Exit 0 when the database matches or was updated, 1 on an error. Built with tools/build-sqlcipher/build.sh
 * (SQLCipher and a static OpenSSL; the unit has neither). Run by start-rb.sh a few seconds after exportExt.pdb
 * changes, see docs/11-runtime-launcher.md.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "sqlite3.h"

#define PG 4096

static uint32_t rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t rd16(const unsigned char *p) { return p[0] | p[1] << 8; }

/* ---- DeviceSQL .pdb: just enough to walk the rows of a table ---- */

struct pdb {
    unsigned char *d;
    size_t n;
    int ntab;
    struct { uint32_t type, first, last; } tab[64];
};

static int pdb_open(struct pdb *p, const char *path)
{
    FILE *f = fopen(path, "rb");
    long sz;
    int i;
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    rewind(f);
    p->d = malloc(sz);
    if (!p->d || fread(p->d, 1, sz, f) != (size_t)sz || sz < 28 + 16) {
        fclose(f);
        return -1;
    }
    fclose(f);
    p->n = sz;
    p->ntab = rd32(p->d + 8);
    if (p->ntab > 64 || 28 + 16 * p->ntab > sz)
        return -1;
    for (i = 0; i < p->ntab; i++) {
        p->tab[i].type = rd32(p->d + 28 + 16 * i);
        p->tab[i].first = rd32(p->d + 28 + 16 * i + 8);
        p->tab[i].last = rd32(p->d + 28 + 16 * i + 12);
    }
    return 0;
}

typedef void (*rowfn)(const unsigned char *row, size_t avail, void *ctx);

static void pdb_rows(const struct pdb *p, uint32_t type, rowfn fn, void *ctx)
{
    int i, t = -1;
    uint32_t pgn;
    unsigned guard = 0;
    for (i = 0; i < p->ntab; i++)
        if (p->tab[i].type == type)
            t = i;
    if (t < 0)
        return;
    pgn = p->tab[t].first;
    while ((size_t)(pgn + 1) * PG <= p->n && guard++ < 100000) {
        const unsigned char *pg = p->d + (size_t)pgn * PG;
        uint32_t nrow = rd32(pg + 0x18) & 0x1FFF, next = rd32(pg + 12);
        if (!(pg[0x1B] & 0x40)) {                         /* a data page */
            uint32_t r;
            for (r = 0; r < nrow; r++) {
                uint32_t g = r / 16, j = r % 16;
                size_t base;
                uint32_t off;
                if (PG < 0x24 * (g + 1))
                    break;
                base = PG - 0x24 * (g + 1);
                if (!((rd16(pg + base + 32) >> j) & 1))
                    continue;                             /* deleted row */
                off = rd16(pg + base + 2 * (15 - j));
                if (0x28 + off >= PG)
                    continue;
                fn(pg + 0x28 + off, PG - (0x28 + off), ctx);
            }
        }
        if (pgn == p->tab[t].last)
            break;
        pgn = next;
    }
}

/* A DeviceSQL string as UTF-8 in out (size n); returns 0 on success. */
static int dsql_string(const unsigned char *row, size_t avail, size_t off, char *out, size_t n)
{
    unsigned b;
    size_t len, k = 0, i;
    if (off >= avail)
        return -1;
    b = row[off];
    if (b & 1) {                                          /* short ASCII */
        len = (b >> 1) - 1;
        if (off + 1 + len > avail || len >= n)
            return -1;
        memcpy(out, row + off + 1, len);
        out[len] = 0;
        return 0;
    }
    if (off + 4 > avail)
        return -1;
    len = rd16(row + off + 1);
    if (len < 4 || off + len > avail)
        return -1;
    len -= 4;
    if (b == 0x40) {                                      /* long ASCII */
        if (len >= n)
            return -1;
        memcpy(out, row + off + 4, len);
        out[len] = 0;
        return 0;
    }
    if (b != 0x90)
        return -1;
    for (i = 0; i + 1 < len; i += 2) {                    /* long UTF-16LE to UTF-8 */
        uint32_t c = rd16(row + off + 4 + i);
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < len) {
            uint32_t lo = rd16(row + off + 4 + i + 2);
            if (lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        if (k + 4 >= n)
            return -1;
        if (c < 0x80)
            out[k++] = c;
        else if (c < 0x800) {
            out[k++] = 0xC0 | c >> 6;
            out[k++] = 0x80 | (c & 63);
        } else if (c < 0x10000) {
            out[k++] = 0xE0 | c >> 12;
            out[k++] = 0x80 | ((c >> 6) & 63);
            out[k++] = 0x80 | (c & 63);
        } else {
            out[k++] = 0xF0 | c >> 18;
            out[k++] = 0x80 | ((c >> 12) & 63);
            out[k++] = 0x80 | ((c >> 6) & 63);
            out[k++] = 0x80 | (c & 63);
        }
    }
    out[k] = 0;
    return 0;
}

/* ---- the player's side ---- */

struct trk { uint32_t id; char *path; };
struct pair { uint32_t track, tag; };

static struct trk *trks;
static size_t ntrks, captrks;
static struct pair *pairs;
static size_t npairs, cappairs;

static void row_track(const unsigned char *row, size_t avail, void *ctx)
{
    char path[1024];
    (void)ctx;
    if (avail < 0x5E + 42)
        return;
    if (dsql_string(row, avail, rd16(row + 0x5E + 2 * 20), path, sizeof path))
        return;
    if (ntrks == captrks) {
        captrks = captrks ? captrks * 2 : 1024;
        trks = realloc(trks, captrks * sizeof *trks);
    }
    trks[ntrks].id = rd32(row + 0x48);
    trks[ntrks].path = strdup(path);
    ntrks++;
}

static void row_pair(const unsigned char *row, size_t avail, void *ctx)
{
    (void)ctx;
    if (avail < 12)
        return;
    if (npairs == cappairs) {
        cappairs = cappairs ? cappairs * 2 : 256;
        pairs = realloc(pairs, cappairs * sizeof *pairs);
    }
    pairs[npairs].track = rd32(row + 4);
    pairs[npairs].tag = rd32(row + 8);
    npairs++;
}

static int cmp_trk_path(const void *a, const void *b) { return strcmp(((const struct trk *)a)->path, ((const struct trk *)b)->path); }
static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* ---- the database ---- */

struct vec { uint64_t *v; size_t n, cap; };

static void vec_push(struct vec *a, uint64_t x)
{
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 256;
        a->v = realloc(a->v, a->cap * sizeof *a->v);
    }
    a->v[a->n++] = x;
}

static void vec_uniq(struct vec *a)
{
    size_t i, k = 0;
    qsort(a->v, a->n, sizeof *a->v, cmp_u64);
    for (i = 0; i < a->n; i++)
        if (!k || a->v[k - 1] != a->v[i])
            a->v[k++] = a->v[i];
    a->n = k;
}

static int has(const struct vec *a, uint64_t x) { return bsearch(&x, a->v, a->n, sizeof *a->v, cmp_u64) != NULL; }

static int run(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "sql: %s: %s\n", sql, err ? err : "?");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

static int copy_file(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb"), *b;
    char buf[65536];
    size_t n;
    int ok = 1;
    if (!a)
        return -1;
    b = fopen(to, "wb");
    if (!b) {
        fclose(a);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof buf, a)) > 0)
        if (fwrite(buf, 1, n, b) != n)
            ok = 0;
    fclose(a);
    if (fflush(b) || fsync(fileno(b)))
        ok = 0;
    fclose(b);
    return ok ? 0 : -1;
}

static void show(sqlite3 *db, int add, uint64_t key)
{
    sqlite3_stmt *s;
    uint32_t content = key >> 32, tag = (uint32_t)key;
    char name[128] = "?", title[256] = "?";
    if (sqlite3_prepare_v2(db, "select name from myTag where myTag_id=?", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, tag);
        if (sqlite3_step(s) == SQLITE_ROW)
            snprintf(name, sizeof name, "%s", (const char *)sqlite3_column_text(s, 0));
        sqlite3_finalize(s);
    }
    if (sqlite3_prepare_v2(db, "select title from content where content_id=?", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, content);
        if (sqlite3_step(s) == SQLITE_ROW)
            snprintf(title, sizeof title, "%s", (const char *)sqlite3_column_text(s, 0));
        sqlite3_finalize(s);
    }
    printf("%c %s  ->  %s (content %u)\n", add ? '+' : '-', name, title, content);
}

int main(int argc, char **argv)
{
    const char *keyfile = "/data/onelibrary.key", *backup = NULL, *dir = NULL;
    int dry = 0, i;
    char path[512], key[256] = "";
    struct pdb ext, exp;
    sqlite3 *db = NULL;
    sqlite3_stmt *s;
    struct vec known = {0}, onplayer = {0}, want = {0}, have = {0};
    struct { uint32_t dev, content; } *cmap = NULL;
    size_t ncmap = 0, capmap = 0, a, b;
    FILE *kf;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n"))
            dry = 1;
        else if (!strcmp(argv[i], "-k") && i + 1 < argc)
            keyfile = argv[++i];
        else if (!strcmp(argv[i], "-b") && i + 1 < argc)
            backup = argv[++i];
        else if (argv[i][0] != '-')
            dir = argv[i];
        else {
            fprintf(stderr, "usage: %s [-n] [-k KEYFILE] [-b BACKUPDIR] DIR\n", argv[0]);
            return 1;
        }
    }
    if (!dir) {
        fprintf(stderr, "usage: %s [-n] [-k KEYFILE] [-b BACKUPDIR] DIR\n", argv[0]);
        return 1;
    }
    snprintf(path, sizeof path, "%s/exportLibrary.db", dir);
    if (access(path, F_OK)) {
        printf("no exportLibrary.db in %s: nothing to do\n", dir);
        return 0;
    }
    kf = fopen(keyfile, "r");
    if (!kf || !fgets(key, sizeof key, kf)) {
        fprintf(stderr, "cannot read the key from %s\n", keyfile);
        return 1;
    }
    fclose(kf);
    key[strcspn(key, "\r\n")] = 0;

    snprintf(path, sizeof path, "%s/exportExt.pdb", dir);
    if (pdb_open(&ext, path)) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    snprintf(path, sizeof path, "%s/export.pdb", dir);
    if (pdb_open(&exp, path)) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }
    pdb_rows(&exp, 0, row_track, NULL);                   /* tracks: id and path */
    pdb_rows(&ext, 4, row_pair, NULL);                    /* (track, tag) pairs */
    qsort(trks, ntrks, sizeof *trks, cmp_trk_path);

    snprintf(path, sizeof path, "%s/exportLibrary.db", dir);
    if (dry) {                                            /* immutable: no -wal or -shm file is left on the stick */
        char uri[640];
        snprintf(uri, sizeof uri, "file:%s?immutable=1", path);
        i = sqlite3_open_v2(uri, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL);
    } else
        i = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL);
    if (i != SQLITE_OK) {
        fprintf(stderr, "cannot open %s: %s\n", path, sqlite3_errmsg(db));
        return 1;
    }
    sqlite3_key(db, key, strlen(key));
    memset(key, 0, sizeof key);
    if (!dry)
        run(db, "pragma locking_mode=exclusive");         /* no -shm file on the stick */
    if (sqlite3_prepare_v2(db, "select count(*) from sqlite_master", -1, &s, NULL) != SQLITE_OK ||
        sqlite3_step(s) != SQLITE_ROW) {
        fprintf(stderr, "cannot unlock %s (wrong key?): %s\n", path, sqlite3_errmsg(db));
        return 1;
    }
    sqlite3_finalize(s);

    /* which tags exist there */
    if (sqlite3_prepare_v2(db, "select myTag_id from myTag where attribute=0", -1, &s, NULL) != SQLITE_OK) {
        fprintf(stderr, "no myTag table: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    while (sqlite3_step(s) == SQLITE_ROW)
        vec_push(&known, (uint64_t)sqlite3_column_int64(s, 0));
    sqlite3_finalize(s);
    vec_uniq(&known);

    /* which of its tracks the player has too (by path), and the player's id for each */
    if (sqlite3_prepare_v2(db, "select content_id, path from content", -1, &s, NULL) != SQLITE_OK) {
        fprintf(stderr, "no content table: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    while (sqlite3_step(s) == SQLITE_ROW) {
        struct trk key_ = { 0, (char *)sqlite3_column_text(s, 1) }, *hit;
        if (!key_.path)
            continue;
        hit = bsearch(&key_, trks, ntrks, sizeof *trks, cmp_trk_path);
        if (!hit)
            continue;
        if (ncmap == capmap) {
            capmap = capmap ? capmap * 2 : 1024;
            cmap = realloc(cmap, capmap * sizeof *cmap);
        }
        cmap[ncmap].dev = hit->id;
        cmap[ncmap].content = (uint32_t)sqlite3_column_int64(s, 0);
        ncmap++;
        vec_push(&onplayer, (uint32_t)sqlite3_column_int64(s, 0));
    }
    sqlite3_finalize(s);
    vec_uniq(&onplayer);

    /* what the player says, in that library's numbering */
    for (a = 0; a < npairs; a++)
        for (b = 0; b < ncmap; b++)                       /* a few hundred pairs at most: no need for more */
            if (cmap[b].dev == pairs[a].track && has(&known, pairs[a].tag))
                vec_push(&want, (uint64_t)cmap[b].content << 32 | pairs[a].tag);
    vec_uniq(&want);

    /* what it says now, for the same tracks */
    if (sqlite3_prepare_v2(db, "select myTag_id, content_id from myTag_content", -1, &s, NULL) != SQLITE_OK) {
        fprintf(stderr, "no myTag_content table: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    while (sqlite3_step(s) == SQLITE_ROW) {
        uint64_t c = (uint32_t)sqlite3_column_int64(s, 1), t = (uint32_t)sqlite3_column_int64(s, 0);
        if (has(&onplayer, c))
            vec_push(&have, c << 32 | t);
    }
    sqlite3_finalize(s);
    vec_uniq(&have);

    {   /* the difference */
        struct vec add = {0}, rem = {0};
        for (a = 0; a < want.n; a++)
            if (!has(&have, want.v[a]))
                vec_push(&add, want.v[a]);
        for (a = 0; a < have.n; a++)
            if (!has(&want, have.v[a]))
                vec_push(&rem, have.v[a]);
        if (!add.n && !rem.n) {
            printf("exportLibrary.db already has the player's tags (%zu tracks in both)\n", onplayer.n);
            sqlite3_close(db);
            return 0;
        }
        for (a = 0; a < add.n; a++)
            show(db, 1, add.v[a]);
        for (a = 0; a < rem.n; a++)
            show(db, 0, rem.v[a]);
        if (dry) {
            printf("dry run: %zu to add, %zu to remove\n", add.n, rem.n);
            sqlite3_close(db);
            return 0;
        }
        if (backup) {
            char day[16], to[600];
            time_t now = time(NULL);
            strftime(day, sizeof day, "%Y%m%d", gmtime(&now));
            mkdir(backup, 0755);
            snprintf(to, sizeof to, "%s/exportLibrary-%s.db", backup, day);
            if (access(to, F_OK) && copy_file(path, to)) {   /* the first change of the day keeps the original */
                fprintf(stderr, "cannot back up to %s: %s\n", to, strerror(errno));
                return 1;
            }
        }
        if (run(db, "begin"))
            return 1;
        for (a = 0; a < rem.n; a++) {
            sqlite3_prepare_v2(db, "delete from myTag_content where myTag_id=? and content_id=?", -1, &s, NULL);
            sqlite3_bind_int64(s, 1, (uint32_t)rem.v[a]);
            sqlite3_bind_int64(s, 2, rem.v[a] >> 32);
            if (sqlite3_step(s) != SQLITE_DONE) {
                fprintf(stderr, "delete failed: %s\n", sqlite3_errmsg(db));
                sqlite3_finalize(s);
                run(db, "rollback");
                return 1;
            }
            sqlite3_finalize(s);
        }
        for (a = 0; a < add.n; a++) {
            sqlite3_prepare_v2(db, "insert into myTag_content(myTag_id, content_id) values (?, ?)", -1, &s, NULL);
            sqlite3_bind_int64(s, 1, (uint32_t)add.v[a]);
            sqlite3_bind_int64(s, 2, add.v[a] >> 32);
            if (sqlite3_step(s) != SQLITE_DONE) {
                fprintf(stderr, "insert failed: %s\n", sqlite3_errmsg(db));
                sqlite3_finalize(s);
                run(db, "rollback");
                return 1;
            }
            sqlite3_finalize(s);
        }
        if (run(db, "commit"))
            return 1;
        if (sqlite3_prepare_v2(db, "pragma integrity_check", -1, &s, NULL) != SQLITE_OK || sqlite3_step(s) != SQLITE_ROW ||
            strcmp((const char *)sqlite3_column_text(s, 0), "ok")) {
            fprintf(stderr, "integrity_check failed after the write; the original is in %s\n", backup ? backup : "(no backup)");
            return 1;
        }
        sqlite3_finalize(s);
        run(db, "pragma wal_checkpoint(truncate)");
        printf("exportLibrary.db updated: %zu added, %zu removed\n", add.n, rem.n);
    }
    sqlite3_close(db);
    return 0;
}
