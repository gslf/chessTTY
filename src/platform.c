/* platform.c — processes, pipes and cross-platform helpers */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "portable.h"   /* must come first: feature-test macros */

#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define PATH_SEP "\\"
#else
#define PATH_SEP "/"
#endif


/* ---------------- portable path helpers (shared) ---------------- */
void path_join(char *out, size_t n, const char *dir, const char *name) {
    size_t dl = strlen(dir);
    bool need_sep = dl > 0 && dir[dl - 1] != '/' && dir[dl - 1] != '\\';
    snprintf(out, n, "%s%s%s", dir, need_sep ? PATH_SEP : "", name);
}

/* Case-insensitive "does `name` end with `suffix`". */
static bool has_suffix_ci(const char *name, const char *suffix) {
    if (!suffix || !*suffix) return true;
    size_t nl = strlen(name), sl = strlen(suffix);
    if (sl > nl) return false;
    const char *p = name + nl - sl;
    for (size_t i = 0; i < sl; i++) {
        char a = p[i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return false;
    }
    return true;
}

static int name_cmp(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

/* Creates one directory whose parent already exists (platform primitive). */
static bool mkdir_one(const char *path);

/* Creates `path` and any missing parent, like `mkdir -p`: the collection folder
   may sit several levels below a data directory that does not exist yet. */
bool dir_make(const char *path) {
    if (dir_exists(path)) return true;
    char tmp[PATH_MAX_CT];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof tmp) return false;
    memcpy(tmp, path, n + 1);
    while (n > 1 && (tmp[n - 1] == '/' || tmp[n - 1] == '\\')) tmp[--n] = 0;
    /* From index 1, so a leading separator is not mistaken for a component. */
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] != '/' && tmp[i] != '\\') continue;
        char sep = tmp[i];
        tmp[i] = 0;
        if (!dir_exists(tmp)) mkdir_one(tmp);
        tmp[i] = sep;
    }
    mkdir_one(tmp);
    return dir_exists(path);
}

#ifdef _WIN32
/* ------------------------------- Windows ------------------------------- */

bool proc_start(Proc *pr, const char *path) {
    memset(pr, 0, sizeof *pr);
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE in_r = NULL, in_w = NULL, out_r = NULL, out_w = NULL;
    if (!CreatePipe(&in_r, &in_w, &sa, 0)) { pr->last_error = (int)GetLastError(); return false; }
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) {
        pr->last_error = (int)GetLastError();
        CloseHandle(in_r); CloseHandle(in_w);
        return false;
    }
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\"", path);
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                             NULL, NULL, &si, &pi);
    if (!ok) pr->last_error = (int)GetLastError();   /* before any other call */
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (!ok) {
        CloseHandle(in_w); CloseHandle(out_r);
        return false;
    }
    CloseHandle(pi.hThread);
    pr->proc = pi.hProcess;
    pr->in_w = in_w;
    pr->out_r = out_r;
    pr->alive = true;
    return true;
}

int proc_read(Proc *pr, char *buf, size_t n) {
    if (!pr->alive) return -1;
    DWORD avail = 0;
    if (!PeekNamedPipe(pr->out_r, NULL, 0, NULL, &avail, NULL)) {
        pr->alive = false;
        return -1;
    }
    if (avail == 0) {
        DWORD code;
        if (GetExitCodeProcess(pr->proc, &code) && code != STILL_ACTIVE) {
            pr->alive = false;
            return -1;
        }
        return 0;
    }
    DWORD got = 0;
    if (!ReadFile(pr->out_r, buf, (DWORD)(n < avail ? n : avail), &got, NULL)) {
        pr->alive = false;
        return -1;
    }
    return (int)got;
}

bool proc_write(Proc *pr, const char *data, size_t n) {
    if (!pr->alive) return false;
    DWORD written = 0;
    if (!WriteFile(pr->in_w, data, (DWORD)n, &written, NULL)) {
        pr->alive = false;
        return false;
    }
    return written == n;
}

void proc_kill(Proc *pr) {
    if (pr->in_w) { CloseHandle(pr->in_w); pr->in_w = NULL; }
    if (pr->proc) {
        if (WaitForSingleObject(pr->proc, 300) == WAIT_TIMEOUT)
            TerminateProcess(pr->proc, 0);
        CloseHandle(pr->proc);
        pr->proc = NULL;
    }
    if (pr->out_r) { CloseHandle(pr->out_r); pr->out_r = NULL; }
    pr->alive = false;
}

void msleep(int ms) { Sleep(ms); }

long long now_ms(void) {
    return (long long)GetTickCount64();
}

int cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
}

bool exe_dir(char *buf, size_t n) {
    char tmp[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, tmp, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    char *slash = strrchr(tmp, '\\');
    if (!slash) return false;
    slash[1] = 0;
    if (strlen(tmp) + 1 > n) return false;
    strcpy(buf, tmp);
    return true;
}

bool file_exists(const char *path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dir_exists(const char *path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool mkdir_one(const char *path) {
    return CreateDirectoryA(path, NULL) != 0 || dir_exists(path);
}

bool file_stat(const char *path, uint64_t *size, uint64_t *mtime) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) return false;
    if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    if (size) *size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    if (mtime) *mtime = ((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32) |
                        d.ftLastWriteTime.dwLowDateTime;
    return true;
}

int dir_list(const char *dir, const char *suffix, char (*out)[PATH_MAX_CT], int max) {
    char pat[PATH_MAX_CT];
    path_join(pat, sizeof pat, dir, "*");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    int n = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!has_suffix_ci(fd.cFileName, suffix)) continue;
        if (n >= max) break;
        snprintf(out[n], PATH_MAX_CT, "%s", fd.cFileName);
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(out, (size_t)n, PATH_MAX_CT, name_cmp);
    return n;
}

bool home_dir(char *buf, size_t n) {
    const char *h = getenv("USERPROFILE");
    if (h && *h) { snprintf(buf, n, "%s\\", h); return true; }
    const char *drive = getenv("HOMEDRIVE"), *rest = getenv("HOMEPATH");
    if (drive && rest) { snprintf(buf, n, "%s%s\\", drive, rest); return true; }
    return false;
}

#else
/* -------------------------------- POSIX -------------------------------- */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char **environ;

bool proc_start(Proc *pr, const char *path) {
    memset(pr, 0, sizeof *pr);
    pr->in_w = pr->out_r = -1;   /* so that a failed start cannot close fd 0 */
    int inpipe[2], outpipe[2];
    if (pipe(inpipe) != 0) { pr->last_error = errno; return false; }
    if (pipe(outpipe) != 0) {
        pr->last_error = errno;
        close(inpipe[0]); close(inpipe[1]);
        return false;
    }

    /* A second engine must not inherit the first engine's pipes. */
    int fds[] = {inpipe[0], inpipe[1], outpipe[0], outpipe[1]};
    for (int i = 0; i < 4; i++) {
        if (fcntl(fds[i], F_SETFD, FD_CLOEXEC) < 0) {
            pr->last_error = errno;
            for (int j = 0; j < 4; j++) close(fds[j]);
            return false;
        }
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, inpipe[0], 0);
    posix_spawn_file_actions_adddup2(&fa, outpipe[1], 1);
    posix_spawn_file_actions_addclose(&fa, inpipe[1]);
    posix_spawn_file_actions_addclose(&fa, outpipe[0]);

    char *argv[] = { (char *)path, NULL };
    pid_t pid;
    int rc = posix_spawnp(&pid, path, &fa, NULL, argv, environ); /* also searches PATH */
    posix_spawn_file_actions_destroy(&fa);
    close(inpipe[0]);
    close(outpipe[1]);
    if (rc != 0) {
        pr->last_error = rc;   /* posix_spawnp returns the error directly */
        close(inpipe[1]); close(outpipe[0]);
        return false;
    }
    fcntl(outpipe[0], F_SETFL, O_NONBLOCK);
    pr->pid = pid;
    pr->in_w = inpipe[1];
    pr->out_r = outpipe[0];
    pr->alive = true;
    return true;
}

int proc_read(Proc *pr, char *buf, size_t n) {
    if (!pr->alive) return -1;
    ssize_t got = read(pr->out_r, buf, n);
    if (got > 0) return (int)got;
    if (got == 0) { pr->alive = false; return -1; } /* EOF */
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    pr->alive = false;
    return -1;
}

bool proc_write(Proc *pr, const char *data, size_t n) {
    if (!pr->alive) return false;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(pr->in_w, data + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            pr->alive = false;
            return false;
        }
        off += (size_t)w;
    }
    return true;
}

void proc_kill(Proc *pr) {
    if (pr->in_w >= 0) { close(pr->in_w); pr->in_w = -1; }
    if (pr->pid > 0) {
        int status;
        long long t0 = now_ms();
        for (;;) {
            pid_t r = waitpid(pr->pid, &status, WNOHANG);
            if (r == pr->pid || r < 0) break;
            if (now_ms() - t0 > 300) { kill(pr->pid, SIGKILL); waitpid(pr->pid, &status, 0); break; }
            msleep(10);
        }
        pr->pid = 0;
    }
    if (pr->out_r >= 0) { close(pr->out_r); pr->out_r = -1; }
    pr->alive = false;
}

void msleep(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int cpu_count(void) {
#ifdef _SC_NPROCESSORS_ONLN
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#else
    return 1;   /* not a POSIX standard query: assume a single core */
#endif
}

bool exe_dir(char *buf, size_t n) {
    char tmp[4096];
#ifdef __APPLE__
    uint32_t size = sizeof tmp;
    if (_NSGetExecutablePath(tmp, &size) != 0) return false;
#else
    ssize_t len = readlink("/proc/self/exe", tmp, sizeof tmp - 1);
    if (len <= 0) return false;
    tmp[len] = 0;
#endif
    char *slash = strrchr(tmp, '/');
    if (!slash) return false;
    slash[1] = 0;
    if (strlen(tmp) + 1 > n) return false;
    strcpy(buf, tmp);
    return true;
}

bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool mkdir_one(const char *path) {
    return mkdir(path, 0755) == 0 || dir_exists(path);
}

bool file_stat(const char *path, uint64_t *size, uint64_t *mtime) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    if (size) *size = (uint64_t)st.st_size;
    if (mtime) {
#ifdef __APPLE__
        *mtime = (uint64_t)st.st_mtimespec.tv_sec * 1000000000 + st.st_mtimespec.tv_nsec;
#else
        *mtime = (uint64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
#endif
    }
    return true;
}

int dir_list(const char *dir, const char *suffix, char (*out)[PATH_MAX_CT], int max) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;            /* hidden files and . .. */
        if (!has_suffix_ci(e->d_name, suffix)) continue;
        char full[PATH_MAX_CT];
        path_join(full, sizeof full, dir, e->d_name);
        if (!file_exists(full)) continue;             /* skip directories, sockets… */
        if (n >= max) break;
        snprintf(out[n], PATH_MAX_CT, "%s", e->d_name);
        n++;
    }
    closedir(d);
    qsort(out, (size_t)n, PATH_MAX_CT, name_cmp);
    return n;
}

bool home_dir(char *buf, size_t n) {
    const char *h = getenv("HOME");
    if (!h || !*h) return false;
    snprintf(buf, n, "%s/", h);
    return true;
}

#endif
