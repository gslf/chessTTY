/* db.h — games database: a folder of PGN files with a binary index */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DB_H
#define DB_H

#include "game.h"
#include "platform.h"

/*
 * Design
 * ------
 * A database is a *folder of ordinary PGN files* plus one binary sidecar,
 * `index.ctdb`.  Nothing is copied into a proprietary container: the games stay
 * in the files the user (or another program) put there.
 *
 * The index holds one fixed-size 496-byte record per game — the listing columns
 * plus the byte range of the record inside its file.  Consequences:
 *
 *   · Opening the database is a single sequential read of the index.  No PGN is
 *     parsed; startup scales with the compact index rather than PGN text.
 *   · Scrolling, sorting and filtering touch only the flat record array; sorting
 *     permutes 4-byte indices, never the records.
 *   · Opening one game seeks straight to its byte range and reads just that,
 *     so it costs the size of a single game, not of the collection.
 *   · Saving appends to a PGN file and appends one record: no rewrite.
 *
 * A single file is limited to 4 GB by the 32-bit offsets (2 GB on platforms
 * with 32-bit long/fseek); a collection is not,
 * since it may hold up to DB_FILES_MAX of them.
 *
 * The index is a cache, never the source of truth.  Every file's size and
 * modification time are stored with it; at open time only files whose stamp
 * changed are re-scanned, and a corrupt or foreign index is simply rebuilt.
 */

#define DB_NAME_MAX 80
#define DB_EVENT_MAX 80
#define DB_FILES_MAX 256
#define DB_FILTER_MAX 128

enum { DBR_UNKNOWN = 0, DBR_WHITE = 1, DBR_BLACK = 2, DBR_DRAW = 3 };
enum { DBF_SETUP = 1 };          /* the game starts from a FEN */

/* Fixed-size metadata, including all tags understood by the PGN reader. */
typedef struct {
    uint32_t file_id;
    uint32_t off;                /* byte offset of the record in its file */
    uint32_t len;                /* byte length of the record */
    uint16_t plies;
    uint8_t  result;
    uint8_t  flags;
    int16_t  year;               /* 0 = unknown */
    uint8_t  month, day;
    uint16_t white_elo, black_elo;
    char     eco[8];
    char     white[DB_NAME_MAX];
    char     black[DB_NAME_MAX];
    char     event[DB_EVENT_MAX];
    char     site[80], round[16], fen[128];
} DbRec;

/* Only the file *name* is stored: the folder comes from the Db, so the index
   stays valid when the collection is moved, renamed, or simply opened through
   a different spelling of the same path. */
typedef struct {
    char     name[PATH_MAX_CT];
    uint64_t size, mtime;        /* staleness stamp */
    uint32_t count;              /* games contributed (display only) */
    uint32_t pad;
} DbFile;

/* Sort keys for the listing. */
enum { DBS_NATURAL = 0, DBS_WHITE, DBS_BLACK, DBS_EVENT, DBS_DATE, DBS_PLIES, DBS_COUNT };

typedef struct {
    char   dir[PATH_MAX_CT];     /* the collection folder */
    char   index_path[PATH_MAX_CT];
    char   write_name[PATH_MAX_CT];  /* file new games are appended to */
    bool   single_file;          /* the collection is one PGN, not a folder */

    DbFile  *files;              /* DB_FILES_MAX entries, allocated on open */
    int      nfiles;
    DbRec   *recs;
    int      nrecs, cap;

    uint32_t *view;              /* filtered + sorted permutation of recs */
    int       nview, view_cap;

    char   filter[DB_FILTER_MAX];
    int    sort;
    bool   sort_desc;
    bool   open;
    int    scanned;              /* files re-parsed on the last open */
    long long open_ms;           /* how long the last open took */
} Db;

/* Default collection folder (created on demand, seeded with the bundled
   sample games the first time).  Returns `buf`. */
const char *db_default_dir(char *buf, size_t n);

/* Opens (and, if needed, rebuilds) the collection at `path`, which may be a
   folder or a single PGN file.  Safe to call on an already-open Db. */
bool db_open(Db *db, const char *path, char *err, size_t errn);
void db_close(Db *db);

/* Listing.  `i` indexes the current view, not the underlying array. */
const DbRec *db_at(const Db *db, int i);
void         db_path_of(const Db *db, const DbRec *r, char *out, size_t n);
bool db_load(const Db *db, int i, Game *g, char *err, size_t errn);

/* Appends a game (annotations included) and indexes it in place. */
bool db_add(Db *db, const Game *g, char *err, size_t errn);

void db_set_filter(Db *db, const char *text);
void db_set_sort(Db *db, int key);       /* same key again flips the direction */
const char *db_sort_name(int key);

size_t db_memory_bytes(const Db *db);

#endif
