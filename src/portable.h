/* portable.h — feature-test macros for the POSIX layer
   SPDX-License-Identifier: GPL-3.0-or-later

   MUST be the first include of every file that touches POSIX APIs.

   On glibc/musl, compiling with -std=c11 defines __STRICT_ANSI__, which makes
   the headers expose ISO C only: kill(), nanosleep(), readlink(), SIGKILL and
   friends disappear, and GCC >= 14 turns the resulting implicit declarations
   into hard errors.  macOS and the BSDs expose everything by default and are
   left untouched on purpose: defining _POSIX_C_SOURCE there would *hide*
   declarations instead of adding them. */
#ifndef PORTABLE_H
#define PORTABLE_H

#if defined(__linux__) && !defined(_GNU_SOURCE)
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#  ifndef _DEFAULT_SOURCE      /* BSD/misc extras: struct winsize, sysconf ids */
#    define _DEFAULT_SOURCE 1
#  endif
#endif

#endif
