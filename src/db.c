/* db.c — games database: a folder of PGN files with a binary index */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "db.h"
#include "pgn.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The on-disk index is a raw dump of these structures, so their layout is part
   of the format.  A mismatch here would be silently read as garbage. */
_Static_assert(sizeof(DbRec) == 496, "DbRec layout changed");
_Static_assert(sizeof(DbFile) == PATH_MAX_CT + 24, "DbFile layout changed");

static void db_full_path(const Db *db, const char *name, char *out, size_t n) {
    path_join(out, n, db->dir, name);
}

#define IDX_MAGIC "CTTYDB\x01"
#define IDX_VERSION 2u

typedef struct {
    char     magic[8];
    uint32_t version;
    uint32_t rec_size;
    uint32_t file_size;
    uint32_t nfiles;
    uint32_t nrecs;
    uint32_t reserved;
} IdxHeader;

static void set_err(char *err, size_t errn, const char *msg) {
    if (err && errn) snprintf(err, errn, "%s", msg);
}

static void copy_field(char *dst, size_t n, const char *src) {
    size_t k = strlen(src);
    if (k >= n) k = n - 1;
    memcpy(dst, src, k);
    dst[k] = 0;
}

/* ------------------------------ default folder ------------------------------ */
/* Copies the sample collections shipped next to the binary into a freshly
   created database, so the first open is not an empty screen. */
static void seed_from(const char *src_dir, const char *dst_dir) {
    char (*names)[PATH_MAX_CT] = malloc(sizeof(*names) * 32);
    if (!names) return;
    int n = dir_list(src_dir, ".pgn", names, 32);
    for (int i = 0; i < n; i++) {
        char src[PATH_MAX_CT], dst[PATH_MAX_CT];
        path_join(src, sizeof src, src_dir, names[i]);
        path_join(dst, sizeof dst, dst_dir, names[i]);
        if (file_exists(dst)) continue;
        FILE *in = fopen(src, "rb");
        if (!in) continue;
        FILE *out = fopen(dst, "wb");
        if (!out) { fclose(in); continue; }
        char chunk[8192];
        size_t got;
        while (!feof(in) && !ferror(in) && (got = fread(chunk, 1, sizeof chunk, in)) > 0)
            if (fwrite(chunk, 1, got, out) != got) break;
        fclose(in);
        fclose(out);
    }
    free(names);
}

const char *db_default_dir(char *buf, size_t n) {
    char home[PATH_MAX_CT];
    const char *xdg = getenv("XDG_DATA_HOME");
    bool fresh;
    if (xdg && *xdg) {
        char base[PATH_MAX_CT];
        path_join(base, sizeof base, xdg, "chesstty");
        dir_make(base);
        path_join(buf, n, base, "games");
    } else if (home_dir(home, sizeof home)) {
        char base[PATH_MAX_CT];
        path_join(base, sizeof base, home, ".chesstty");
        dir_make(base);
        path_join(buf, n, base, "games");
    } else {
        snprintf(buf, n, "chesstty-games");
    }
    fresh = !dir_exists(buf);
    dir_make(buf);
    if (fresh) {
        /* Sample games live next to the executable in a release archive and one
           level up in a build tree. */
        char exe[PATH_MAX_CT], cand[PATH_MAX_CT];
        if (exe_dir(exe, sizeof exe)) {
            path_join(cand, sizeof cand, exe, "examples");
            if (dir_exists(cand)) seed_from(cand, buf);
            else {
                path_join(cand, sizeof cand, exe, "../examples");
                if (dir_exists(cand)) seed_from(cand, buf);
            }
        }
        if (dir_exists("examples")) seed_from("examples", buf);
    }
    return buf;
}

/* ------------------------------ record building ------------------------------ */
static uint8_t result_code(const char *s) {
    if (!strcmp(s, "1-0")) return DBR_WHITE;
    if (!strcmp(s, "0-1")) return DBR_BLACK;
    if (!strcmp(s, "1/2-1/2")) return DBR_DRAW;
    return DBR_UNKNOWN;
}

/* "1851.06.21", "1997.??.??", "1972" */
static void parse_date(const char *s, int16_t *year, uint8_t *mon, uint8_t *day) {
    int y = 0, m = 0, d = 0;
    *year = 0; *mon = 0; *day = 0;
    if (!isdigit((unsigned char)s[0])) return;
    y = atoi(s);
    if (y < 1 || y > 3000) return;
    *year = (int16_t)y;
    const char *p = strchr(s, '.');
    if (!p || !isdigit((unsigned char)p[1])) return;
    m = atoi(p + 1);
    if (m >= 1 && m <= 12) *mon = (uint8_t)m;
    p = strchr(p + 1, '.');
    if (!p || !isdigit((unsigned char)p[1])) return;
    d = atoi(p + 1);
    if (d >= 1 && d <= 31) *day = (uint8_t)d;
}

static void rec_from_ref(DbRec *r, const PgnRef *ref, uint32_t file_id, long file_end) {
    memset(r, 0, sizeof *r);
    r->file_id = file_id;
    r->off = (uint32_t)ref->rec_start;
    long end = ref->mt_end > 0 ? ref->mt_end : file_end;
    if (end < ref->rec_start) end = ref->rec_start;
    r->len = (uint32_t)(end - ref->rec_start);
    r->plies = ref->plies > 65535 ? 65535 : (uint16_t)ref->plies;
    r->result = result_code(ref->result);
    r->flags = ref->fen[0] ? DBF_SETUP : 0;
    parse_date(ref->date, &r->year, &r->month, &r->day);
    r->white_elo = (uint16_t)(ref->white_elo > 0 && ref->white_elo < 4000 ? ref->white_elo : 0);
    r->black_elo = (uint16_t)(ref->black_elo > 0 && ref->black_elo < 4000 ? ref->black_elo : 0);
    copy_field(r->eco, sizeof r->eco, ref->eco);
    copy_field(r->white, sizeof r->white, ref->white);
    copy_field(r->black, sizeof r->black, ref->black);
    copy_field(r->event, sizeof r->event, ref->event);
    copy_field(r->site, sizeof r->site, ref->site);
    copy_field(r->round, sizeof r->round, ref->round);
    copy_field(r->fen, sizeof r->fen, ref->fen);
}

static bool recs_reserve(Db *db, int want) {
    if (want <= db->cap) return true;
    int cap = db->cap ? db->cap : 256;
    while (cap < want) {
        if (cap > INT_MAX / 2) { cap = want; break; }
        cap *= 2;
    }
    DbRec *grown = realloc(db->recs, (size_t)cap * sizeof *db->recs);
    if (!grown) return false;
    db->recs = grown;
    db->cap = cap;
    return true;
}

/* Indexes one PGN file, appending its games to the record array.
 *
 * The file is walked in windows instead of being slurped, so indexing a
 * multi-gigabyte collection costs a few megabytes of RAM rather than its whole
 * size.  Each window is parsed by the normal PGN scanner; its *last* record is
 * then dropped and the next window restarts there, because that record is the
 * only one that can have been cut in half.  No boundary guessing is involved:
 * anything the scanner got wrong at the tail is simply re-read with its full
 * text present.
 */
#define SCAN_WINDOW (4u * 1024 * 1024)
#define SCAN_WINDOW_MAX (64u * 1024 * 1024)

static int scan_file(Db *db, uint32_t file_id, const char *path) {
    uint64_t fsize = 0;
    if (!file_stat(path, &fsize, NULL)) return -1;
    if (fsize == 0) return 0;
    /* Record offsets are 32-bit, which caps a single file at 4 GB.  A
       collection is not capped: it may hold DB_FILES_MAX of them. */
    if (fsize > 0xFFFFFFFFull || fsize > LONG_MAX) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    int added = 0;
    uint64_t base = 0;
    size_t window = SCAN_WINDOW;
    while (base < fsize) {
        size_t want = window;
        if ((uint64_t)want > fsize - base) want = (size_t)(fsize - base);
        char *buf = malloc(want + 1);
        if (!buf) break;
        if (fseek(f, (long)base, SEEK_SET) != 0) { free(buf); break; }
        size_t got = fread(buf, 1, want, f);
        if (got == 0) { free(buf); break; }
        buf[got] = 0;
        bool at_eof = base + got >= fsize;

        char err[128];
        PgnList *L = pgn_scan_mem(buf, got, err, sizeof err);   /* takes over buf */
        if (!L) break;
        /* Everything but the tail is complete; at EOF the tail is complete too. */
        int take = at_eof ? L->n : L->n - 1;
        if (take <= 0) {
            pgn_list_free(L);
            if (at_eof) break;
            if (window >= SCAN_WINDOW_MAX) break;   /* one game larger than 64 MB */
            window *= 2;                            /* retry with a wider window */
            continue;
        }
        if (!recs_reserve(db, db->nrecs + take)) { pgn_list_free(L); break; }
        for (int i = 0; i < take; i++) {
            rec_from_ref(&db->recs[db->nrecs], &L->games[i], file_id, (long)L->len);
            db->recs[db->nrecs].off += (uint32_t)base;
            db->nrecs++;
        }
        added += take;
        uint64_t next = at_eof ? fsize : base + (uint64_t)L->games[L->n - 1].rec_start;
        pgn_list_free(L);
        if (next <= base) break;                    /* no progress: stop safely */
        base = next;
        window = SCAN_WINDOW;
    }
    fclose(f);
    return base == fsize ? added : -1;
}

/* ------------------------------ index I/O ------------------------------ */
static bool index_read(Db *db, DbFile *old_files, int *old_nfiles,
                       DbRec **old_recs, int *old_nrecs) {
    *old_nfiles = 0;
    *old_recs = NULL;
    *old_nrecs = 0;
    FILE *f = fopen(db->index_path, "rb");
    if (!f) return false;
    IdxHeader h;
    if (fread(&h, sizeof h, 1, f) != 1 ||
        memcmp(h.magic, IDX_MAGIC, 8) != 0 ||
        h.version != IDX_VERSION ||
        h.rec_size != sizeof(DbRec) ||
        h.file_size != sizeof(DbFile) ||
        h.nfiles > DB_FILES_MAX || h.nrecs > INT_MAX ||
        (uint64_t)h.nrecs * sizeof(DbRec) > SIZE_MAX) {
        fclose(f);
        return false;                       /* foreign or stale format: rebuild */
    }
    uint64_t bytes;
    uint64_t expected = sizeof h + (uint64_t)h.nfiles * sizeof(DbFile) +
                        (uint64_t)h.nrecs * sizeof(DbRec);
    if (!file_stat(db->index_path, &bytes, NULL) || bytes != expected) {
        fclose(f);
        return false;
    }
    if (h.nfiles && fread(old_files, sizeof(DbFile), h.nfiles, f) != h.nfiles) {
        fclose(f);
        return false;
    }
    for (uint32_t i = 0; i < h.nfiles; i++) {
        const char *name = old_files[i].name;
        if (!memchr(name, 0, sizeof old_files[i].name) || !*name ||
            strchr(name, '/') || strchr(name, '\\') || strchr(name, ':') ||
            !strcmp(name, ".") || !strcmp(name, "..")) {
            fclose(f);
            return false;
        }
    }
    DbRec *recs = NULL;
    if (h.nrecs) {
        recs = malloc((size_t)h.nrecs * sizeof *recs);
        if (!recs || fread(recs, sizeof *recs, h.nrecs, f) != h.nrecs) {
            free(recs);
            fclose(f);
            return false;
        }
    }
    fclose(f);
    for (uint32_t i = 0; i < h.nrecs; i++) {
        const DbRec *r = &recs[i];
        if (r->file_id >= h.nfiles || !r->len ||
            (uint64_t)r->off + r->len > old_files[r->file_id].size ||
            !memchr(r->white, 0, sizeof r->white) ||
            !memchr(r->black, 0, sizeof r->black) ||
            !memchr(r->event, 0, sizeof r->event) ||
            !memchr(r->eco, 0, sizeof r->eco) ||
            !memchr(r->site, 0, sizeof r->site) ||
            !memchr(r->round, 0, sizeof r->round) ||
            !memchr(r->fen, 0, sizeof r->fen)) {
            free(recs);
            return false;
        }
    }
    *old_nfiles = (int)h.nfiles;
    *old_recs = recs;
    *old_nrecs = (int)h.nrecs;
    return true;
}

static void index_write(const Db *db) {
    FILE *f = fopen(db->index_path, "wb");
    if (!f) return;                          /* a read-only collection still works */
    IdxHeader h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, IDX_MAGIC, 8);
    h.version = IDX_VERSION;
    h.rec_size = sizeof(DbRec);
    h.file_size = sizeof(DbFile);
    h.nfiles = (uint32_t)db->nfiles;
    h.nrecs = (uint32_t)db->nrecs;
    bool ok = fwrite(&h, sizeof h, 1, f) == 1;
    if (ok && db->nfiles)
        ok = fwrite(db->files, sizeof(DbFile), (size_t)db->nfiles, f) == (size_t)db->nfiles;
    if (ok && db->nrecs)
        ok = fwrite(db->recs, sizeof(DbRec), (size_t)db->nrecs, f) == (size_t)db->nrecs;
    if (fclose(f) != 0) ok = false;
    if (!ok) remove(db->index_path);         /* never leave a half-written index */
}

/* ------------------------------ view (filter + sort) ------------------------------ */
/* The search terms, lowercased once per keystroke instead of once per record. */
#define FILTER_WORDS (DB_FILTER_MAX / 2)
typedef struct {
    char w[FILTER_WORDS][DB_FILTER_MAX];
    int  len[FILTER_WORDS];
    bool numeric[FILTER_WORDS];
    int  n;
} Filter;

static char lower_ch(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static void filter_parse(const char *text, Filter *f) {
    f->n = 0;
    const char *p = text;
    while (*p && f->n < FILTER_WORDS) {
        while (*p == ' ') p++;
        if (!*p) break;
        int k = 0;
        while (*p && *p != ' ' && k < (int)sizeof f->w[0] - 1) f->w[f->n][k++] = lower_ch(*p++);
        while (*p && *p != ' ') p++;
        f->w[f->n][k] = 0;
        f->len[f->n] = k;
        f->numeric[f->n] = false;
        for (int i = 0; i < k; i++) {
            unsigned char c = (unsigned char)f->w[f->n][i];
            if ((c >= '0' && c <= '9') || c == '*' || c >= 0x80)
                f->numeric[f->n] = true;
        }
        f->n++;
    }
}

/* Substring search with an already-lowercased needle. */
static bool ci_find(const char *hay, const char *needle, int nlen) {
    if (nlen <= 0) return true;
    char first = needle[0];
    for (const char *h = hay; *h; h++) {
        if (lower_ch(*h) != first) continue;
        int i = 1;
        while (i < nlen && h[i] && lower_ch(h[i]) == needle[i]) i++;
        if (i == nlen) return true;
        if (!h[i]) return false;          /* the rest is shorter than the needle */
    }
    return false;
}

/* Fixed-capacity scalar metadata fits in 192 bytes. Avoid printf's format
   parsing for every row when filtering hundreds of thousands of games. */
static char *search_number(char *out, unsigned value, int width) {
    char digits[10];
    int n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (width-- > n) *out++ = '0';
    while (n) *out++ = digits[--n];
    return out;
}

static void search_scalars(const DbRec *r, char out[192]) {
    char *p = out;
    if (r->year > 0) {
        const char *separators = ".-/";
        for (int i = 0; i < 3; i++) {
            p = search_number(p, (unsigned)r->year, 4); *p++ = separators[i];
            p = search_number(p, r->month, 2); *p++ = separators[i];
            p = search_number(p, r->day, 2); *p++ = ' ';
        }
    }
    if (r->white_elo) { p = search_number(p, r->white_elo, 0); *p++ = ' '; }
    if (r->black_elo) { p = search_number(p, r->black_elo, 0); *p++ = ' '; }
    p = search_number(p, (r->plies + 1) / 2, 0); *p++ = ' ';
    p = search_number(p, r->plies, 0); *p++ = ' ';
    strcpy(p, r->result == DBR_WHITE ? "1-0" : r->result == DBR_BLACK ? "0-1" :
              r->result == DBR_DRAW ? "1/2-1/2 ½-½" : "*");
}

/* Every search word must appear in one of the indexed columns; their order
   does not matter, so "fischer spassky" and "spassky fischer" agree. */
static bool rec_matches(const Db *db, const DbRec *r, const Filter *f) {
    char numbers[192];
    bool have_numbers = false;
    for (int i = 0; i < f->n; i++) {
        const char *w = f->w[i];
        int n = f->len[i];
        if (ci_find(r->white, w, n) || ci_find(r->black, w, n) ||
            ci_find(r->event, w, n) || ci_find(r->eco, w, n) ||
            ci_find(r->site, w, n) || ci_find(r->round, w, n) ||
            ci_find(r->fen, w, n) ||
            (r->file_id < (uint32_t)db->nfiles &&
             ci_find(db->files[r->file_id].name, w, n))) continue;
        if (ci_find(r->flags & DBF_SETUP ? "setup fen moves plies" : "standard moves plies", w, n))
            continue;
        if (!f->numeric[i]) return false;
        /* Format at most once, and only for words that can match numbers. */
        if (!have_numbers) {
            search_scalars(r, numbers);
            have_numbers = true;
        }
        if (!ci_find(numbers, w, n)) return false;
    }
    return true;
}

static const Db *g_sort_db;      /* qsort() has no user-data parameter */

static long rec_date_key(const DbRec *r) {
    return (long)r->year * 10000 + (long)r->month * 100 + r->day;
}

static int view_cmp(const void *pa, const void *pb) {
    const Db *db = g_sort_db;
    uint32_t ia = *(const uint32_t *)pa, ib = *(const uint32_t *)pb;
    const DbRec *a = &db->recs[ia], *b = &db->recs[ib];
    int c = 0;
    switch (db->sort) {
        case DBS_WHITE: c = strcmp(a->white, b->white); break;
        case DBS_BLACK: c = strcmp(a->black, b->black); break;
        case DBS_EVENT: c = strcmp(a->event, b->event); break;
        case DBS_DATE: {
            /* Unknown dates remain at the bottom in either direction. */
            if (!a->year != !b->year) return a->year ? -1 : 1;
            long ka = rec_date_key(a), kb = rec_date_key(b);
            c = ka < kb ? -1 : ka > kb ? 1 : 0;
            break;
        }
        case DBS_PLIES: c = (int)a->plies - (int)b->plies; break;
        default: break;
    }
    if (c == 0) c = ia < ib ? -1 : ia > ib ? 1 : 0;   /* stable, no ties */
    return db->sort_desc ? -c : c;
}

static void view_rebuild(Db *db) {
    if (db->view_cap < db->nrecs) {
        int cap = db->nrecs ? db->nrecs : 1;
        uint32_t *grown = realloc(db->view, (size_t)cap * sizeof *db->view);
        if (!grown) { db->nview = 0; return; }
        db->view = grown;
        db->view_cap = cap;
    }
    Filter f;
    filter_parse(db->filter, &f);
    db->nview = 0;
    for (int i = 0; i < db->nrecs; i++)
        if (rec_matches(db, &db->recs[i], &f))
            db->view[db->nview++] = (uint32_t)i;
    if (db->nview > 1 && (db->sort != DBS_NATURAL || db->sort_desc)) {
        g_sort_db = db;
        qsort(db->view, (size_t)db->nview, sizeof *db->view, view_cmp);
    }
}

/* Typing one more character can only ever remove matches, so the new view is
   sieved out of the old one.  After the first couple of letters this costs a
   handful of comparisons instead of a full pass over the collection — which is
   what keeps the search responsive on a slow machine with a large database. */
static void view_narrow(Db *db) {
    Filter f;
    filter_parse(db->filter, &f);
    int k = 0;
    for (int i = 0; i < db->nview; i++)
        if (rec_matches(db, &db->recs[db->view[i]], &f)) db->view[k++] = db->view[i];
    db->nview = k;                        /* relative order, hence the sort, is kept */
}

void db_set_filter(Db *db, const char *text) {
    const char *next = text ? text : "";
    size_t oldn = strlen(db->filter);
    /* Extending the current search narrows it; anything else starts over. */
    bool narrowing = oldn > 0 && strlen(next) > oldn && !strncmp(next, db->filter, oldn);
    copy_field(db->filter, sizeof db->filter, next);
    if (narrowing) view_narrow(db);
    else view_rebuild(db);
}

void db_set_sort(Db *db, int key) {
    if (key < 0 || key >= DBS_COUNT) return;
    if (db->sort == key) db->sort_desc = !db->sort_desc;
    else { db->sort = key; db->sort_desc = key == DBS_DATE; }
    /* Filtering has already selected the rows; only their order changes. */
    if (db->nview > 1) {
        g_sort_db = db;
        qsort(db->view, (size_t)db->nview, sizeof *db->view, view_cmp);
    }
}

const char *db_sort_name(int key) {
    switch (key) {
        case DBS_WHITE: return "white";
        case DBS_BLACK: return "black";
        case DBS_EVENT: return "event";
        case DBS_DATE: return "date";
        case DBS_PLIES: return "length";
        default: return "file order";
    }
}

/* ------------------------------ open / close ------------------------------ */
void db_close(Db *db) {
    free(db->recs);
    free(db->view);
    free(db->files);
    memset(db, 0, sizeof *db);
}

bool db_open(Db *db, const char *path, char *err, size_t errn) {
    long long t0 = now_ms();
    char dir[PATH_MAX_CT];
    copy_field(dir, sizeof dir, path);
    size_t dl = strlen(dir);
    while (dl > 1 && (dir[dl - 1] == '/' || dir[dl - 1] == '\\')) dir[--dl] = 0;
    if (!*dir) { set_err(err, errn, "Empty path"); return false; }

    bool single = !dir_exists(dir) && file_exists(dir);
    if (!single && !dir_exists(dir) && !dir_make(dir)) {
        set_err(err, errn, "No such folder, and it could not be created");
        return false;
    }

    /* Names of the PGN files that make up the collection. */
    char (*names)[PATH_MAX_CT] = malloc(sizeof(*names) * DB_FILES_MAX);
    if (!names) { set_err(err, errn, "Out of memory"); return false; }
    int nn = 0;
    char base_dir[PATH_MAX_CT];
    if (single) {
        const char *slash = strrchr(dir, '/');
#ifdef _WIN32
        const char *bslash = strrchr(dir, '\\');
        if (!slash || (bslash && bslash > slash)) slash = bslash;
#endif
        if (slash) {
            size_t k = (size_t)(slash - dir);
            if (k == 0) k = 1;
#ifdef _WIN32
            if (k == 2 && dir[1] == ':') k++;
#endif
            if (k >= sizeof base_dir) k = sizeof base_dir - 1;
            memcpy(base_dir, dir, k);
            base_dir[k] = 0;
            copy_field(names[0], sizeof names[0], slash + 1);
        } else {
            snprintf(base_dir, sizeof base_dir, ".");
            copy_field(names[0], sizeof names[0], dir);
        }
        nn = 1;
    } else {
        copy_field(base_dir, sizeof base_dir, dir);
        nn = dir_list(dir, ".pgn", names, DB_FILES_MAX);
        if (nn < 0) { free(names); set_err(err, errn, "Cannot read collection folder"); return false; }
    }

    /* Take over the caller's Db only once the listing succeeded. */
    DbFile *old_files = malloc(sizeof(DbFile) * DB_FILES_MAX);
    DbFile *new_files = calloc(DB_FILES_MAX, sizeof(DbFile));
    if (!old_files || !new_files) {
        free(names); free(old_files); free(new_files);
        set_err(err, errn, "Out of memory");
        return false;
    }
    int old_nfiles = 0, old_nrecs = 0;
    DbRec *old_recs = NULL;
    char index_path[PATH_MAX_CT];
    int w = single ? snprintf(index_path, sizeof index_path, "%s.ctdb", dir) : -1;
    /* A truncated sidecar name could collide with another collection's, so a
       path with no room for the suffix keeps its index in the folder instead. */
    if (!single || w < 0 || (size_t)w >= sizeof index_path)
        path_join(index_path, sizeof index_path, single ? base_dir : dir, "index.ctdb");

    db_close(db);
    db->files = new_files;
    db->single_file = single;
    copy_field(db->dir, sizeof db->dir, single ? base_dir : dir);
    copy_field(db->index_path, sizeof db->index_path, index_path);
    copy_field(db->write_name, sizeof db->write_name,
               single ? names[0] : "chesstty-saved.pgn");

    bool have_index = index_read(db, old_files, &old_nfiles, &old_recs, &old_nrecs);
    bool changed = !have_index || old_nfiles != nn;

    /* Fast path — the usual one.  When every file is where the index left it,
       with the same size and timestamp, the records are adopted as they were
       read: no per-file matching, no copy, no rewrite. */
    bool identical = have_index && old_nfiles == nn && nn <= DB_FILES_MAX;
    for (int i = 0; identical && i < nn; i++) {
        char full[PATH_MAX_CT];
        uint64_t size = 0, mtime = 0;
        db_full_path(db, names[i], full, sizeof full);
        if (!file_stat(full, &size, &mtime) || strcmp(old_files[i].name, names[i]) ||
            old_files[i].size != size || old_files[i].mtime != mtime)
            identical = false;
    }
    if (identical) {
        memcpy(db->files, old_files, sizeof(DbFile) * (size_t)nn);
        db->nfiles = nn;
        db->recs = old_recs;
        db->nrecs = db->cap = old_nrecs;
        old_recs = NULL;
        changed = false;
        nn = 0;                      /* skip the per-file pass below */
    }

    for (int i = 0; i < nn && i < DB_FILES_MAX; i++) {
        DbFile *fe = &db->files[db->nfiles];
        memset(fe, 0, sizeof *fe);
        copy_field(fe->name, sizeof fe->name, names[i]);
        char full[PATH_MAX_CT];
        db_full_path(db, fe->name, full, sizeof full);
        if (!file_stat(full, &fe->size, &fe->mtime)) continue;

        /* Reuse the indexed records when the file has not been touched. */
        int reuse = -1;
        for (int j = 0; j < old_nfiles; j++)
            if (!strcmp(old_files[j].name, fe->name) &&
                old_files[j].size == fe->size && old_files[j].mtime == fe->mtime) {
                reuse = j;
                break;
            }
        if (reuse >= 0 && old_recs) {
            int first = db->nrecs;
            for (int k = 0; k < old_nrecs; k++) {
                if (old_recs[k].file_id != (uint32_t)reuse) continue;
                if (!recs_reserve(db, db->nrecs + 1)) goto scan_failed;
                db->recs[db->nrecs] = old_recs[k];
                db->recs[db->nrecs].file_id = (uint32_t)db->nfiles;
                db->nrecs++;
            }
            fe->count = (uint32_t)(db->nrecs - first);
            if (reuse != db->nfiles) changed = true;
        } else {
            int count = scan_file(db, (uint32_t)db->nfiles, full);
            if (count < 0) goto scan_failed;
            fe->count = (uint32_t)count;
            db->scanned++;
            changed = true;
        }
        db->nfiles++;
    }
    free(names);
    free(old_files);
    free(old_recs);

    db->open = true;
    db->sort = DBS_NATURAL;
    view_rebuild(db);
    if (changed) index_write(db);
    db->open_ms = now_ms() - t0;
    if (db->nfiles == 0) {
        set_err(err, errn, "No PGN files in this folder");
        return true;                          /* an empty collection is not an error */
    }
    return true;

scan_failed:
    free(names);
    free(old_files);
    free(old_recs);
    db_close(db);
    set_err(err, errn, "Cannot completely index PGN files (read error, size limit or out of memory)");
    return false;
}

/* ------------------------------ access ------------------------------ */
const DbRec *db_at(const Db *db, int i) {
    if (!db->open || i < 0 || i >= db->nview) return NULL;
    return &db->recs[db->view[i]];
}

void db_path_of(const Db *db, const DbRec *r, char *out, size_t n) {
    if (!r || r->file_id >= (uint32_t)db->nfiles) { snprintf(out, n, "%s", ""); return; }
    db_full_path(db, db->files[r->file_id].name, out, n);
}

bool db_load(const Db *db, int i, Game *g, char *err, size_t errn) {
    const DbRec *r = db_at(db, i);
    if (!r) { set_err(err, errn, "No such game"); return false; }
    char full[PATH_MAX_CT];
    db_path_of(db, r, full, sizeof full);
    return pgn_load_at(full, (long)r->off, (long)r->len, g, err, errn);
}

bool db_add(Db *db, const Game *g, char *err, size_t errn) {
    if (!db->open) { set_err(err, errn, "No collection is open"); return false; }
    char full[PATH_MAX_CT];
    db_full_path(db, db->write_name, full, sizeof full);
    /* Validate capacity before appending: a failed save must not add a
       duplicate game on the user's next retry. */
    int fid = -1;
    for (int i = 0; i < db->nfiles; i++)
        if (!strcmp(db->files[i].name, db->write_name)) { fid = i; break; }
    if (fid < 0 && db->nfiles >= DB_FILES_MAX) {
        set_err(err, errn, "Too many files in the collection"); return false;
    }
    if (!recs_reserve(db, db->nrecs + 1)) { set_err(err, errn, "Out of memory"); return false; }
    uint64_t size = 0;
    file_stat(full, &size, NULL);
    /* Reserve a conservative upper bound for this fixed-capacity Game. */
    uint64_t limit = LONG_MAX < 0xFFFFFFFFull ? LONG_MAX : 0xFFFFFFFFull;
    if (size > limit - sizeof(Game)) {
        set_err(err, errn, "That PGN file is too large: start a new one"); return false;
    }
    long off = 0, len = 0;
    if (!pgn_append(g, full, &off, &len, err, errn)) return false;

    if (fid < 0) {
        fid = db->nfiles++;
        memset(&db->files[fid], 0, sizeof db->files[fid]);
        copy_field(db->files[fid].name, sizeof db->files[fid].name, db->write_name);
    }

    DbRec *r = &db->recs[db->nrecs];
    memset(r, 0, sizeof *r);
    r->file_id = (uint32_t)fid;
    r->off = (uint32_t)off;
    r->len = (uint32_t)len;
    r->plies = g->n > 65535 ? 65535 : (uint16_t)g->n;
    r->result = result_code(result_str(g->result));
    r->flags = g->custom_start ? DBF_SETUP : 0;
    parse_date(g->tag_date, &r->year, &r->month, &r->day);
    copy_field(r->white, sizeof r->white, g->tag_white);
    copy_field(r->black, sizeof r->black, g->tag_black);
    copy_field(r->event, sizeof r->event, g->tag_event);
    copy_field(r->site, sizeof r->site, g->tag_site);
    copy_field(r->round, sizeof r->round, g->tag_round);
    copy_field(r->eco, sizeof r->eco, g->tag_eco);
    copy_field(r->fen, sizeof r->fen, g->custom_start ? g->start_fen : "");
    r->white_elo = g->tag_white_elo;
    r->black_elo = g->tag_black_elo;
    db->nrecs++;
    db->files[fid].count++;
    /* Re-stamp the file so the next open reuses the index instead of re-parsing. */
    file_stat(full, &db->files[fid].size, &db->files[fid].mtime);
    view_rebuild(db);
    index_write(db);
    return true;
}

size_t db_memory_bytes(const Db *db) {
    return (size_t)db->cap * sizeof(DbRec) + (size_t)db->view_cap * sizeof(uint32_t) +
           (db->files ? sizeof(DbFile) * DB_FILES_MAX : 0);
}
