/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef AUTH_H
#define AUTH_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {
    intptr_t listener, client;
    bool active;
    char verifier[65], state[65], redirect[128], input[4096];
    size_t used;
    int64_t deadline, client_deadline;
} OAuth;
bool oauth_begin(OAuth *a, char *url, size_t n, int64_t now);
/* Nonblocking: 0 = pending or rejected request, 1 = code, -1 = denial/timeout.
   Keep polling while active; even a successful client send may need more polls. */
int oauth_poll(OAuth *a, char *code, size_t n, int64_t now);
void oauth_close(OAuth *a);
bool oauth_challenge(const char *verifier, char out[44]);
void browser_reap(void);
bool open_browser(const char *url);
#endif
