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
    ALEE_ALERROR_TRIGGERED,
    ALEE_ALCERROR_TRIGGERED,
    ALEE_NEW_CALLSTACK_SYMS,
    #define ENTRYPOINT(ret,name,params,args) ALEE_##name,
    #include "altrace_entrypoints.h"
    ALEE_MAX
} EventEnum;


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

#define SIMPLE_MAP(maptype, fromctype, toctype) \
    typedef struct SimpleMap_##maptype { \
        fromctype from; \
        toctype to; \
    } SimpleMap_##maptype; \
    static SimpleMap_##maptype *simplemap_##maptype = NULL; \
    static uint32 simplemap_##maptype##_map_size = 0; \
    static void add_##maptype##_to_map(fromctype from, toctype to) { \
        void *ptr; uint32 i; \
        for (i = 0; i < simplemap_##maptype##_map_size; i++) { \
            if (simplemap_##maptype[i].from == from) { \
                simplemap_##maptype[i].to = to; \
                return; \
            } \
        } \
        ptr = realloc(simplemap_##maptype, (simplemap_##maptype##_map_size + 1) * sizeof (SimpleMap_##maptype)); \
        if (!ptr) { \
            fprintf(stderr, APPNAME ": Out of memory!\n"); \
            _exit(42); \
        } \
        simplemap_##maptype = (SimpleMap_##maptype *) ptr; \
        simplemap_##maptype[simplemap_##maptype##_map_size].from = from; \
        simplemap_##maptype[simplemap_##maptype##_map_size].to = to; \
        simplemap_##maptype##_map_size++; \
    } \
    static toctype get_mapped_##maptype(fromctype from) { \
        uint32 i; \
        for (i = 0; i < simplemap_##maptype##_map_size; i++) { \
            if (simplemap_##maptype[i].from == from) { \
                return simplemap_##maptype[i].to; \
            } \
        } \
        return (toctype) 0; \
    } \
    static void free_##maptype##_map(void) { \
        free(simplemap_##maptype); \
        simplemap_##maptype = NULL; \
    }


#define HASH_MAP(maptype, fromctype, toctype) \
    typedef struct HashMap_##maptype { \
        fromctype from; \
        toctype to; \
        struct HashMap_##maptype *next; \
    } HashMap_##maptype; \
    static HashMap_##maptype *hashmap_##maptype[256]; \
    static HashMap_##maptype *get_hashitem_##maptype(fromctype from, uint8 *_hash) { \
        const uint8 hash = hash_##maptype(from); \
        HashMap_##maptype *prev = NULL; \
        HashMap_##maptype *item = hashmap_##maptype[hash]; \
        if (_hash) { *_hash = hash; } \
        while (item) { \
            if (item->from == from) { \
                if (prev) { /* move to front of list */ \
                    prev->next = item->next; \
                    item->next = hashmap_##maptype[hash]; \
                    hashmap_##maptype[hash] = item; \
                } \
                return item; \
            } \
            prev = item; \
            item = item->next; \
        } \
        return NULL; \
    } \
    static void add_##maptype##_to_map(fromctype from, toctype to) { \
        uint8 hash; HashMap_##maptype *item = get_hashitem_##maptype(from, &hash); \
        if (item) { \
            item->to = to; \
        } else { \
            item = (HashMap_##maptype *) calloc(1, sizeof (HashMap_##maptype)); \
            if (!item) { \
                fprintf(stderr, APPNAME ": Out of memory!\n"); \
                _exit(42); \
            } \
            item->from = from; \
            item->to = to; \
            item->next = hashmap_##maptype[hash]; \
            hashmap_##maptype[hash] = item; \
        } \
    } \
    static toctype get_mapped_##maptype(fromctype from) { \
        HashMap_##maptype *item = get_hashitem_##maptype(from, NULL); \
        return item ? item->to : (toctype) 0; \
    } \
    static void free_##maptype##_map(void) { \
        int i; \
        for (i = 0; i < 256; i++) { \
            HashMap_##maptype *item; HashMap_##maptype *next; \
            for (item = hashmap_##maptype[i]; item; item = next) { \
                free_hash_item_##maptype(item->from, item->to); \
                next = item->next; \
                free(item); \
            } \
            hashmap_##maptype[i] = NULL; \
        } \
    }

// end of altrace_common.h ...
