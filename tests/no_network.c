/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Remote game states are fixtures. Fail if a unit test tries real HTTP. */
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
CURLcode test_http_forbidden(CURL *easy) {
    (void)easy;
    fputs("Unexpected real HTTP in unit test: use a response fixture.\n", stderr);
    abort();
}
CURLMcode test_stream_forbidden(CURLM *multi, CURL *easy) {
    (void)multi; (void)easy;
    fputs("Unexpected real HTTP stream in unit test: use a response fixture.\n", stderr);
    abort();
}
