/* platform.c — processes, pipes and cross-platform helpers */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "portable.h"   /* must come first: feature-test macros */

#include "platform.h"
#include <stdio.h>
#include <string.h>

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

#else
/* -------------------------------- POSIX -------------------------------- */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/time.h>
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
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
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

#endif
