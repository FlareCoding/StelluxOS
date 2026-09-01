#ifndef STLX_TCC_CONFIG_H
#define STLX_TCC_CONFIG_H

/* Version of the vendored upstream snapshot (mob branch). */
#define TCC_VERSION "0.9.28rc"
#define TCC_GITHASH "mob:2ba12e83"

/* Compiler home, holds tcc's private headers and libtcc1.a.
   Default search paths derive from it: {B}/include:/usr/include
   for headers and {B}:/usr/lib for libraries. */
#define CONFIG_TCCDIR "/usr/lib/tcc"

/* Stellux has no dynamic linking, so dlopen support is stubbed out. */
#define CONFIG_TCC_STATIC 1

/* Single-threaded compiler, no semaphore locking needed. */
#define CONFIG_TCC_SEMLOCK 0

/* The backtrace and bounds-check runtimes are not shipped. */
#define CONFIG_TCC_BACKTRACE 0
#define CONFIG_TCC_BCHECK 0

#endif /* STLX_TCC_CONFIG_H */
