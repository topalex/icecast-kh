/* Icecast
 *
 * This program is distributed under the GNU General Public License, version 2.
 * A copy of this license is included with this source.
 *
 * Copyright 2010-2023, Karl Heyes <karl@kheyes.plus.com>
 * Copyright 2000-2004, Jack Moffitt <jack@xiph.org>,
 *                      Michael Smith <msmith@xiph.org>,
 *                      oddsock <oddsock@xiph.org>,
 *                      Karl Heyes <karl@xiph.org>
 *                      and others (see AUTHORS for details).
 */

#ifndef __COMPAT_H__
#define __COMPAT_H__

/* compat.h
 *
 * This file contains most of the ugliness for header portability
 * and common types across various systems like Win32, Linux and
 * Solaris.
 */


#if defined(_WIN32) || defined(__MINGW32__)
#  define PATH_SEPARATOR "\\"
#  define filename_cmp stricmp
#else
#  define PATH_SEPARATOR "/"
#  define filename_cmp strcmp
#endif

#ifdef TIME_WITH_SYS_TIME
#  include <sys/time.h>
#  include <time.h>
#else
#  ifdef HAVE_SYS_TIME_H
#    include <sys/time.h>
#  else
#    include <time.h>
#  endif
#endif

#if defined(HAVE_INTTYPES_H)
#    include <inttypes.h>
#elif defined(HAVE_STDINT_H)
#    include <stdint.h>
#endif

#ifndef HAVE_UINT32_T
#  define uint32_t unsigned int
#endif
#ifndef HAVE_FSEEKO
#  define fseeko fseek
#endif

#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#endif

#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif

#ifndef HAVE_MEMMEM
void *memmem(const void *haystack, size_t haystacklen, const void *needle, size_t needlelen);
#endif

#define icefile_handle   int

#endif /* __COMPAT_H__ */

