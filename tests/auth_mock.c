/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "auth_mock.h"
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
static char request[4096], response[512];
static size_t request_size, response_size;
static bool pending, accepted, closed;
static unsigned reads;
intptr_t auth_mock_listener(void) {
    request_size = response_size = reads = 0;
    pending = accepted = closed = false;
    return 100;
}
int auth_mock_connect(void) {
    request_size = response_size = reads = 0;
    pending = true; accepted = closed = false;
    return 101;
}
intptr_t auth_mock_accept(void) {
    if (!pending || accepted) { errno = EAGAIN; return -1; }
    accepted = true;
    return 101;
}
int auth_mock_write(const char *data, size_t len) {
    if (closed || len > sizeof request - request_size) abort();
    memcpy(request + request_size, data, len); request_size += len;
    return (int)len;
}
static int consume(char *buffer, size_t *used, char *out, size_t len) {
    if (len > *used) len = *used;
    if (len > 7) len = 7; /* every request/response is fragmented */
    memcpy(out, buffer, len); *used -= len;
    memmove(buffer, buffer + len, *used);
    return (int)len;
}
int auth_mock_receive(char *data, size_t len) {
    if (++reads % 3 == 1 || !request_size) { errno = EAGAIN; return -1; }
    return consume(request, &request_size, data, len);
}
int auth_mock_reply(const char *data, size_t len) {
    if (len > sizeof response - response_size) abort();
    memcpy(response + response_size, data, len); response_size += len;
    return (int)len;
}
int auth_mock_read(char *data, size_t len) {
    if (response_size) return consume(response, &response_size, data, len);
    if (closed) return 0;
    errno = EAGAIN; return -1;
}
int auth_mock_close(intptr_t fd) {
    if (fd == 101) { closed = true; pending = false; }
    return 0;
}
