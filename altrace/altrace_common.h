/**
 * MojoAL; a simple drop-in OpenAL implementation.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

// this is not a generic header that list declarations, it's shared code that
//  two source files use. We could clean this up if necessary later.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <dlfcn.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>

#include "../AL/al.h"
#include "../AL/alc.h"

#define ALTRACE_LOG_FILE_MAGIC  0x0104E5A1
#define ALTRACE_LOG_FILE_FORMAT 1

/* AL_EXT_FLOAT32 support... */
#ifndef AL_FORMAT_MONO_FLOAT32
#define AL_FORMAT_MONO_FLOAT32 0x10010
#endif

#ifndef AL_FORMAT_STEREO_FLOAT32
#define AL_FORMAT_STEREO_FLOAT32 0x10011
#endif

/* ALC_EXT_DISCONNECTED support... */
#ifndef ALC_CONNECTED
#define ALC_CONNECTED 0x313
#endif

typedef uint8_t uint8;
typedef int32_t int32;
typedef uint32_t uint32;
typedef uint64_t uint64;
typedef unsigned int uint;

#if defined(__clang__) || defined(__GNUC__)
#define NORETURN __attribute__((noreturn))
#else
#define NORETURN
#endif

#ifdef __linux__
#  include <endian.h>
#  if __BYTE_ORDER == 4321
#    define BIGENDIAN 1
#  endif
#elif defined(__hppa__) || defined(__m68k__) || defined(mc68000) || \
    defined(_M_M68K) || (defined(__MIPS__) && defined(__MISPEB__)) || \
    defined(__ppc__) || defined(__POWERPC__) || defined(_M_PPC) || \
    defined(__sparc__)
#  define BIGENDIAN 1
#endif

#ifdef BIGENDIAN
static uint32 swap32(const uint32 x)
{
    return (uint32) ( (x << 24) | ((x << 8) & 0x00FF0000) |
                      ((x >> 8) & 0x0000FF00) | (x >> 24) );
}

static uint64 swap64(uint64 x)
{
    uint32 hi, lo;
    lo = (Uint32) (x & 0xFFFFFFFF);
    x >>= 32;
    hi = (Uint32) (x & 0xFFFFFFFF);
    x = swap32(lo);
    x <<= 32;
    x |= swap32(hi);
    return x;
}
#else
#define swap32(x) (x)
#define swap64(x) (x)
#endif

typedef enum
{
    ALEE_EOS,
    ALEE_ALERROR_EVENT,
    ALEE_ALCERROR_EVENT,
    #define ENTRYPOINT(ret,name,params,args) ALEE_##name,
    #include "altrace_entrypoints.h"
    ALEE_MAX
} EntryEnum;


#define ENTRYPOINT(ret,name,params,args) static ret (*REAL_##name) params = NULL;
#include "altrace_entrypoints.h"

static struct timespec starttime;
static uint32 NOW(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) == -1) {
        fprintf(stderr, APPNAME ": Failed to get current clock time: %s\n", strerror(errno));
        return 0;
    }

    return (uint32)
        ( ( (((uint64) ts.tv_sec) * 1000) + (((uint64) ts.tv_nsec) / 1000000) ) -
          ( (((uint64) starttime.tv_sec) * 1000) + (((uint64) starttime.tv_nsec) / 1000000) ) );
}

static int init_clock(void)
{
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &starttime) == -1) {
        fprintf(stderr, APPNAME ": Failed to get current clock time: %s\n", strerror(errno));
        return 0;
    }
    usleep(1000);  // just so NOW() is (hopefully) never 0
    return 1;
}

static int logfd = -1;
static void *realdll = NULL;

// !!! FIXME: we should use al[c]GetProcAddress() and do it _per device_ and
// !!! FIXME:  _per context_.
static void *loadEntryPoint(void *dll, const char *fnname, const int extension, int *okay)
{
    void *fn = dlsym(dll, fnname);
    if (!fn && !extension) {
        fprintf(stderr, APPNAME ": Real OpenAL library doesn't have entry point '%s'!\n", fnname);
        *okay = 0;
    }
    return fn;
}

static int load_real_openal(void)
{
    int extensions = 0;
    int okay = 1;
    #ifdef __APPLE__
    const char *dllname = "libopenal.1.dylib";
    #elif defined(_WIN32)
    const char *dllname = "openal32.dll";
    #else
    const char *dllname = "libopenal.so.1";
    #endif

    realdll = dlopen(dllname, RTLD_NOW | RTLD_LOCAL);
    if (!realdll) {
        fprintf(stderr, APPNAME ": Failed to load real OpenAL: %s\n", dlerror());
        fflush(stderr);
        return 0;
    }

    #define ENTRYPOINT_EXTENSIONS_BEGIN() extensions = 1;
    #define ENTRYPOINT(ret,name,params,args) REAL_##name = (ret (*)params) loadEntryPoint(realdll, #name, extensions, &okay);
    #include "altrace_entrypoints.h"
    return okay;
}

static void close_real_openal(void)
{
    void *dll = realdll;

    realdll = NULL;

    #define ENTRYPOINT(ret,name,params,args) REAL_##name = NULL;
    #include "altrace_entrypoints.h"

    if (dll) {
        dlclose(dll);
    }
}

// end of altrace_common.h ...
