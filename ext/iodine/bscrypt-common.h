/*
(un)copyright: Boaz segev, 2016
License: Public Domain except for any non-public-domain algorithms, which are
subject to their own licenses.

Feel free to copy, use and enjoy in accordance with to the license(s).
*/
#ifndef BSCRYPT_COMMON_H
#define BSCRYPT_COMMON_H
/* *****************************************************************************
Environment - you can safely ignore this part... probably.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* check for __uint128_t support */
#if defined(__SIZEOF_INT128__) || defined(__SIZEOF_INT128__)
#define HAS_UINT128
#endif

/* check for features used by lib-bscrypt using include file methology */
#ifdef __has_include

/* check for unix support */
#if __has_include(<unistd.h>)&&__has_include(<pthread.h>)
#define HAS_UNIX_FEATURES
#endif

/* include intrinsics if supported */
#if __has_include(<x86intrin.h>)
#include <x86intrin.h>
#define HAVE_X86Intrin
/*
see: https://software.intel.com/en-us/node/513411
and: https://software.intel.com/sites/landingpage/IntrinsicsGuide/
*/
#endif /* __has_include(<x86intrin.h>) */

#endif

#if !defined(__BIG_ENDIAN__) && !defined(__LITTLE_ENDIAN__)
#if defined(__has_include) && __has_include(<endian.h>)
#include <endian.h>
#endif
#if !defined(__BIG_ENDIAN__) && !defined(__LITTLE_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define __BIG_ENDIAN__
#endif
#endif

#ifndef __unused
#define __unused __attribute__((unused))
#endif

#ifndef BSCRYPT_TEST
#define BSCRYPT_TEST 1
#endif

/* *****************************************************************************
C++ extern
*/
#if defined(__cplusplus)
extern "C" {
#endif

/* ***************************************************************************
Types commonly used by the bscrypt libraries
*/

typedef union {
#ifdef HAVE_X86Intrin
  __m128i mm;
#endif
#ifdef HAS_UINT128
  __uint128_t i;
#endif
  uint8_t bytes[16];
  uint8_t matrix[4][4];
  uint32_t words_small[4];
  uint64_t words[2];
} bits128_u;

typedef union {
#ifdef HAVE_X86Intrin
  __m256i mm;
#endif
#ifdef HAS_UINT128
  __uint128_t huge[2];
#endif
  uint8_t bytes[32];
  uint8_t matrix[8][4];
  uint32_t ints[8];
  uint64_t words[4];
} bits256_u;

/* *****************************************************************************
C++ extern finish
*/
#if defined(__cplusplus)
}
#endif

#endif
