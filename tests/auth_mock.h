/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef AUTH_MOCK_H
#define AUTH_MOCK_H
#include <stddef.h>
#include <stdint.h>
int auth_mock_connect(void);
int auth_mock_write(const char *data, size_t len);
int auth_mock_read(char *data, size_t len);

#ifdef CHESSTTY_AUTH_IMPLEMENTATION
/* Replace only the OS transport, retaining the actual OAuth parser and crypto. */
intptr_t auth_mock_listener(void);
intptr_t auth_mock_accept(void);
int auth_mock_receive(char *data, size_t len);
int auth_mock_reply(const char *data, size_t len);
int auth_mock_close(intptr_t fd);
#define socket(d,t,p) ((void)(d), (void)(t), (void)(p), auth_mock_listener())
#define bind(s,a,n) ((void)(s), (void)(a), (void)(n), 0)
#define listen(s,n) ((void)(s), (void)(n), 0)
#define getsockname(s,a,n) ((void)(s), (void)(n), ((struct sockaddr_in *)(a))->sin_port = htons(12345), 0)
#define accept(s,a,n) ((void)(s), (void)(a), (void)(n), auth_mock_accept())
#define recv(s,b,n,f) ((void)(s), (void)(f), auth_mock_receive((b), (size_t)(n)))
#define send(s,b,n,f) ((void)(s), (void)(f), auth_mock_reply((b), (size_t)(n)))
#define setsockopt(s,l,o,v,n) ((void)(s), (void)(l), (void)(o), (void)(v), (void)(n))
#ifdef _WIN32
#define closesocket(s) auth_mock_close((intptr_t)(s))
#define ioctlsocket(s,c,m) ((void)(s), (void)(c), (void)(m), 0)
#define WSAStartup(v,d) ((void)(v), (void)(d), 0)
#else
#define close(s) auth_mock_close((intptr_t)(s))
#define fcntl(s,c,v) ((void)(s), (void)(c), (void)(v), 0)
#endif
#endif
#endif
