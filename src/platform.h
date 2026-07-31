/* platform.h — processes, pipes and cross-platform helpers (POSIX / Windows) */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stddef.h>

#ifdef _WIN32
#include <windows.h>
typedef struct {
    HANDLE proc;
    HANDLE in_w;   /* we write the child's stdin here */
    HANDLE out_r;  /* we read the child's stdout here */
    bool alive;
} Proc;
#else
#include <sys/types.h>
typedef struct {
    pid_t pid;
    int in_w;
    int out_r;
    bool alive;
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

#endif
