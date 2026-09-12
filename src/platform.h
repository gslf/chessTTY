/* platform.h — processes, pipes and cross-platform helpers (POSIX / Windows) */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
typedef struct {
    HANDLE proc;
    HANDLE in_w;   /* we write the child's stdin here */
    HANDLE out_r;  /* we read the child's stdout here */
    bool alive;
    int last_error; /* GetLastError() when the launch failed, else 0 */
} Proc;
#else
#include <sys/types.h>
typedef struct {
    pid_t pid;
    int in_w;
    int out_r;
    bool alive;
    int last_error; /* errno when the launch failed, else 0 */
} Proc;
#endif

bool proc_start(Proc *pr, const char *path);
/* Reads whatever is available (non-blocking). Returns bytes read,
   0 if nothing, -1 if the process died / EOF. */
int  proc_read(Proc *pr, char *buf, size_t n);
bool proc_write(Proc *pr, const char *data, size_t n);
void proc_kill(Proc *pr);

void msleep(int ms);
long long now_ms(void);
int  cpu_count(void);

/* Directory of the current executable (with trailing slash). False if unknown. */
bool exe_dir(char *buf, size_t n);
bool file_exists(const char *path);
bool dir_exists(const char *path);

/* ---------------- filesystem helpers used by the games database ----------------
   Deliberately allocation-free and bounded: the database must open instantly
   even on machines where malloc() is expensive. */

#define PATH_MAX_CT 512      /* longest path the database stores */
#define DIR_LIST_MAX 512     /* most files a single directory contributes */

/* Creates `path` and any missing parent, like `mkdir -p`.
   True if the directory exists afterwards. */
bool dir_make(const char *path);

/* Size and modification time of a regular file. False if it is not one. */
bool file_stat(const char *path, uint64_t *size, uint64_t *mtime);

/* Names (not full paths) of the files in `dir` whose name ends with `suffix`
   (case-insensitive, NULL = all). Sorted, so an index built from the listing is
   reproducible. Returns how many were written, or -1 if the directory is
   unreadable. Stops at `max`. */
int dir_list(const char *dir, const char *suffix, char (*out)[PATH_MAX_CT], int max);

/* Home directory with a trailing separator. False if it cannot be determined. */
bool home_dir(char *buf, size_t n);

/* Joins `dir` and `name` inserting a separator when needed. */
void path_join(char *out, size_t n, const char *dir, const char *name);

#endif
