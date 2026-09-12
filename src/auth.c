/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "portable.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#define close_socket(s) closesocket((SOCKET)(s))
#define SOCK(s) ((SOCKET)(s))
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#define close_socket(s) close((int)(s))
#define SOCK(s) ((int)(s))
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/evp.h>
#endif
#endif
#include "auth.h"
#include <stdio.h>
#include <string.h>

bool oauth_challenge(const char *verifier, char out[44]) {
    unsigned char digest[32];
#ifdef _WIN32
    BCRYPT_ALG_HANDLE alg = NULL; BCRYPT_HASH_HANDLE hash = NULL;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) return false;
    bool ok = BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) == 0 &&
              BCryptHashData(hash, (PUCHAR)verifier, (ULONG)strlen(verifier), 0) == 0 &&
              BCryptFinishHash(hash, digest, sizeof digest, 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return false;
#elif defined(__APPLE__)
    if (!CC_SHA256(verifier, (CC_LONG)strlen(verifier), digest)) return false;
#else
    unsigned length;
    if (!EVP_Digest(verifier, strlen(verifier), digest, &length, EVP_sha256(), NULL) || length != 32) return false;
#endif
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    unsigned bits = 0, acc = 0; size_t used = 0;
    for (size_t i = 0; i < sizeof digest; i++) {
        acc = (acc << 8) | digest[i]; bits += 8;
        while (bits >= 6) { bits -= 6; out[used++] = alphabet[(acc >> bits) & 63]; }
    }
    if (bits) out[used++] = alphabet[(acc << (6 - bits)) & 63];
    out[used] = 0; return true;
}
static bool random_hex(char out[65]) {
    unsigned char bytes[32];
#ifdef _WIN32
    if (BCryptGenRandom(NULL, bytes, sizeof bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return false;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return false;
    bool ok = fread(bytes, 1, sizeof bytes, f) == sizeof bytes;
    fclose(f); if (!ok) return false;
#endif
    for (int i = 0; i < 32; i++) { out[2*i] = "0123456789abcdef"[bytes[i] >> 4]; out[2*i+1] = "0123456789abcdef"[bytes[i] & 15]; }
    out[64] = 0; return true;
}
static bool nonblocking(intptr_t sock) {
#ifdef _WIN32
    u_long mode = 1; return ioctlsocket((SOCKET)sock, FIONBIO, &mode) == 0;
#else
    return fcntl((int)sock, F_SETFL, O_NONBLOCK) == 0 && fcntl((int)sock, F_SETFD, FD_CLOEXEC) == 0;
#endif
}
void oauth_close(OAuth *a) {
    if (a->active) {
        if (a->listener != -1) close_socket(a->listener);
        if (a->client != -1) close_socket(a->client);
    }
    memset(a, 0, sizeof *a); a->listener = a->client = -1;
}
bool oauth_begin(OAuth *a, char *url, size_t n, int64_t now) {
    oauth_close(a);
    char challenge[44];
    if (!random_hex(a->verifier) || !random_hex(a->state) || !oauth_challenge(a->verifier, challenge)) return false;
#ifdef _WIN32
    static bool winsock_ready;
    if (!winsock_ready) { WSADATA data; if (WSAStartup(MAKEWORD(2,2), &data) != 0) return false; winsock_ready = true; }
#endif
    a->listener = (intptr_t)socket(AF_INET, SOCK_STREAM, 0);
    if (a->listener == -1) return false;
    a->active = true;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr); addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(SOCK(a->listener), (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(SOCK(a->listener), 2) != 0 || !nonblocking(a->listener)) { oauth_close(a); return false; }
#ifdef _WIN32
    int len = sizeof addr;
#else
    socklen_t len = sizeof addr;
#endif
    if (getsockname(SOCK(a->listener), (struct sockaddr *)&addr, &len) != 0) { oauth_close(a); return false; }
    snprintf(a->redirect, sizeof a->redirect, "http://127.0.0.1:%u/callback", ntohs(addr.sin_port));
    int written = snprintf(url, n, "https://lichess.org/oauth?response_type=code&client_id=chesstty&redirect_uri=http%%3A%%2F%%2F127.0.0.1%%3A%u%%2Fcallback&scope=board%%3Aplay&code_challenge_method=S256&code_challenge=%s&state=%s", ntohs(addr.sin_port), challenge, a->state);
    if (written < 0 || (size_t)written >= n) { oauth_close(a); return false; }
    a->deadline = now + 180000;
    return true;
}
static bool query(const char *url, const char *key, char *out, size_t n) {
    const char *p = strchr(url, '?');
    if (!p) return false;
    p++;
    while (*p) {
        const char *end = strchr(p, '&'); if (!end) end = p + strlen(p);
        size_t k = strlen(key);
        if ((size_t)(end - p) > k && !memcmp(p, key, k) && p[k] == '=') {
            p += k + 1; size_t len = (size_t)(end - p);
            if (len >= n) return false;
            /* Lichess codes and our random state are URL-safe alphanumeric. */
            for (size_t i = 0; i < len; i++)
                if (!((p[i] >= 'a' && p[i] <= 'z') || (p[i] >= 'A' && p[i] <= 'Z') ||
                      (p[i] >= '0' && p[i] <= '9') || p[i] == '_' || p[i] == '-')) return false;
            memcpy(out, p, len); out[len] = 0; return true;
        }
        p = *end ? end + 1 : end;
    }
    return false;
}
int oauth_poll(OAuth *a, char *code, size_t n, int64_t now) {
    if (!a->active) return 0;
    if (now >= a->deadline) { oauth_close(a); return -1; }
    if (a->client == -1) {
        a->client = (intptr_t)accept(SOCK(a->listener), NULL, NULL);
        if (a->client == -1) return 0;
        if (!nonblocking(a->client)) { close_socket(a->client); a->client = -1; return 0; }
#ifdef SO_NOSIGPIPE
        int no_sigpipe = 1;
        setsockopt(SOCK(a->client), SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof no_sigpipe);
#endif
        a->used = 0; a->input[0] = 0; a->client_deadline = now + 2000;
    }
    int got = (int)recv(SOCK(a->client), a->input + a->used, (int)(sizeof a->input - 1 - a->used), 0);
    if (got > 0) { a->used += (size_t)got; a->input[a->used] = 0; }
    if (strstr(a->input, "\r\n\r\n")) {
        char method[8], path[3072], state[128], error[128];
        bool valid = sscanf(a->input, "%7s %3071s", method, path) == 2 && !strcmp(method, "GET") &&
                     !strncmp(path, "/callback?", 10) && query(path, "state", state, sizeof state) && !strcmp(state, a->state);
        int result = valid && query(path, "code", code, n) && *code ? 1 :
                     valid && query(path, "error", error, sizeof error) ? -1 : 0;
        const char *reply = result == 1 ? "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nLogin received. Return to ChessTTY.\n" :
                                        "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nLogin was not accepted. Return to ChessTTY.\n";
#ifdef MSG_NOSIGNAL
        send(SOCK(a->client), reply, strlen(reply), MSG_NOSIGNAL);
#else
        send(SOCK(a->client), reply, (int)strlen(reply), 0);
#endif
        close_socket(a->client); a->client = -1; a->used = 0; a->input[0] = 0;
        /* Keep verifier/redirect until the caller has constructed token POST. */
        if (result) { close_socket(a->listener); a->listener = -1; a->active = false; }
        return result;
    }
    if (got == 0 || now > a->client_deadline || a->used >= sizeof a->input - 1) {
        close_socket(a->client); a->client = -1; a->used = 0; a->input[0] = 0;
    }
    return 0;
}
#ifndef _WIN32
static pid_t browser_children[8];
#endif
void browser_reap(void) {
#ifndef _WIN32
    for (int i = 0; i < 8; i++)
        if (browser_children[i] > 0 && waitpid(browser_children[i], NULL, WNOHANG) != 0) browser_children[i] = 0;
#endif
}
bool open_browser(const char *url) {
#ifdef _WIN32
    return (intptr_t)ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL) > 32;
#else
    browser_reap();
    int slot = 0;
    while (slot < 8 && browser_children[slot]) slot++;
    if (slot == 8) return false;
    extern char **environ;
#ifdef __APPLE__
    char *args[] = {"open", (char *)url, NULL};
#else
    char *args[] = {"xdg-open", (char *)url, NULL};
#endif
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    int rc = posix_spawnp(&pid, args[0], &actions, NULL, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    /* The desktop opener normally exits immediately; it must not stall clocks. */
    if (rc == 0) browser_children[slot] = pid;
    return rc == 0;
#endif
}
