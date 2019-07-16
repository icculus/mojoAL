/**
 * MojoAL; a simple drop-in OpenAL implementation.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

#define APPNAME "altrace_playback"
#include "altrace_common.h"

static int dump_calls = 1;
static int dump_callers = 0;
static int dump_state_changes = 0;
static int dump_errors = 0;
static int dumping = 1;
static int run_log = 0;

static int trace_scope = 0;

static void quit_altrace_playback(void);


// don't bother doing a full hash map for devices and contexts, since you'll
//  usually never have more than one or two and they live basically the entire
//  lifetime of your app.
SIMPLE_MAP(device, ALCdevice *, ALCdevice *)
SIMPLE_MAP(context, ALCcontext *, ALCcontext *)

static void free_hash_item_alname(ALuint from, ALuint to) { /* no-op */ }
static uint8 hash_alname(const ALuint name) {
    /* since these are usually small numbers that increment from 0, they distribute pretty well on their own. */
    return (uint8) (name & 0xFF);
}

#define free_hash_item_source free_hash_item_alname
#define hash_source hash_alname
HASH_MAP(source, ALuint, ALuint)

#define free_hash_item_buffer free_hash_item_alname
#define hash_buffer hash_alname
HASH_MAP(buffer, ALuint, ALuint)


static void free_hash_item_stackframe(void *from, char *to) { free(to); }
static uint8 hash_stackframe(void *from) {
    // everything is going to end in a multiple of pointer size, so flatten down.
    const size_t val = ((size_t) from) / (sizeof (void *));
    return (uint8) (val & 0xFF);  // good enough, I guess.
}
HASH_MAP(stackframe, void *, char *)

static uint32 next_mapped_threadid = 0;
SIMPLE_MAP(threadid, uint64, uint32);


#define MAX_IOBLOBS 32
static uint8 *ioblobs[MAX_IOBLOBS];
static size_t ioblobs_len[MAX_IOBLOBS];
static int next_ioblob = 0;

static void *get_ioblob(const size_t len)
{
    void *ptr = ioblobs[next_ioblob];
    if (len > ioblobs_len[next_ioblob]) {
        //printf("allocating ioblob of %llu bytes...\n", (unsigned long long) len);
        ptr = realloc(ptr, len);
        if (!ptr) {
            fprintf(stderr, APPNAME ": Out of memory!\n");
            quit_altrace_playback();
            _exit(42);
        }
        ioblobs[next_ioblob] = ptr;
        ioblobs_len[next_ioblob] = len;
    }
    next_ioblob++;
    if (next_ioblob >= ((sizeof (ioblobs) / sizeof (ioblobs[0])))) {
        next_ioblob = 0;
    }
    return ptr;
}


NORETURN static void IO_READ_FAIL(const int eof)
{
    fprintf(stderr, "Failed to read from log: %s\n", eof ? "end of file" : strerror(errno));
    quit_altrace_playback();
    _exit(42);
}

static uint32 readle32(void)
{
    uint32 retval;
    const ssize_t br = read(logfd, &retval, sizeof (retval));
    if (br != ((ssize_t) sizeof (retval))) {
        IO_READ_FAIL(br >= 0);
    }
    return swap32(retval);
}

static uint64 readle64(void)
{
    uint64 retval;
    const ssize_t br = read(logfd, &retval, sizeof (retval));
    if (br != ((ssize_t) sizeof (retval))) {
        IO_READ_FAIL(br >= 0);
    }
    return swap64(retval);
}

static int32 IO_INT32(void)
{
    union { int32 si32; uint32 ui32; } cvt;
    cvt.ui32 = readle32();
    return cvt.si32;
}

static uint32 IO_UINT32(void)
{
    return readle32();
}

static uint64 IO_UINT64(void)
{
    return readle64();
}

static ALCsizei IO_ALCSIZEI(void)
{
    return (ALCsizei) IO_UINT64();
}

static ALsizei IO_ALSIZEI(void)
{
    return (ALsizei) IO_UINT64();
}

static float IO_FLOAT(void)
{
    union { float f; uint32 ui32; } cvt;
    cvt.ui32 = readle32();
    return cvt.f;
}

static double IO_DOUBLE(void)
{
    union { double d; uint64 ui64; } cvt;
    cvt.ui64 = readle64();
    return cvt.d;
}

static uint8 *IO_BLOB(uint64 *_len)
{
    const uint64 len = IO_UINT64();
    const size_t slen = (size_t) len;
    uint8 *ptr;
    ssize_t br;

    if (len == 0xFFFFFFFFFFFFFFFFull) {
        *_len = 0;
        return NULL;
    }

    *_len = len;

    ptr = (uint8 *) get_ioblob(slen + 1);
    br = read(logfd, ptr, slen);
    if (br != ((ssize_t) slen)) {
        IO_READ_FAIL(br >= 0);
    }
    ptr[slen] = '\0';

    return ptr;
}

static const char *IO_STRING(void)
{
    uint64 len;
    return (const char *) IO_BLOB(&len);
}

static EventEnum IO_EVENTENUM(void)
{
    return (EventEnum) IO_UINT32();
}

static void *IO_PTR(void)
{
    return (void *) (size_t) IO_UINT64();
}

static ALCenum IO_ALCENUM(void)
{
    return (ALCenum) IO_UINT32();
}

static ALenum IO_ENUM(void)
{
    return (ALenum) IO_UINT32();
}

static ALCboolean IO_ALCBOOLEAN(void)
{
    return (ALCboolean) IO_UINT32();
}

static ALboolean IO_BOOLEAN(void)
{
    return (ALboolean) IO_UINT32();
}

static void IO_ENTRYINFO(void)
{
    const uint64 logthreadid = IO_UINT64();
    const uint32 frames = IO_UINT32();
    uint32 threadid = get_mapped_threadid(logthreadid);
    uint32 i, framei;

    if (!threadid) {
        threadid = ++next_mapped_threadid;
        add_threadid_to_map(logthreadid, threadid);
    }

    if (dump_callers) {
        for (i = 0; i < trace_scope; i++) { printf("    "); }
        printf("Call from threadid = %u, stack = {\n", (uint) threadid);
    }

    for (framei = 0; framei < frames; framei++) {
        void *ptr = IO_PTR();
        if (dump_callers) {
            char *str = get_mapped_stackframe(ptr);
            for (i = 0; i < trace_scope; i++) { printf("    "); }
            if (str) {
                printf("    %s\n", str);
            } else {
                printf("    %p\n", ptr);
            }
        }
    }

    if (dump_callers) {
        for (i = 0; i < trace_scope; i++) { printf("    "); }
        printf("}\n");
    }
}

#define IO_START(e) \
    { \
        IO_ENTRYINFO(); \
        if (dump_calls) { \
            int i; for (i = 0; i < trace_scope; i++) { printf("    "); }  \
            printf("%s", #e); \
        } { \

#define IO_END() \
        if (dumping) { fflush(stdout); } } \
    }

static void init_altrace_playback(const char *filename)
{
    int okay = 1;

    fprintf(stderr, APPNAME ": starting up...\n");
    fflush(stderr);

    if (run_log) {
        if (!init_clock()) {
            fflush(stderr);
            _exit(42);
        }

        if (!load_real_openal()) {
            _exit(42);
        }
    }

    if (okay) {
        logfd = open(filename, O_RDONLY);
        if (logfd == -1) {
            fprintf(stderr, APPNAME ": Failed to open OpenAL log file '%s': %s\n", filename, strerror(errno));
            okay = 0;
        } else {
            fprintf(stderr, "\n\n\n" APPNAME ": Playback OpenAL session from log file '%s'\n\n\n", filename);
        }
    }

    fflush(stderr);

    if (okay) {
        if (IO_UINT32() != ALTRACE_LOG_FILE_MAGIC) {
            fprintf(stderr, APPNAME ": File '%s' does not appear to be an OpenAL log file.\n", filename);
            okay = 0;
        } else if (IO_UINT32() != ALTRACE_LOG_FILE_FORMAT) {
            fprintf(stderr, APPNAME ": File '%s' is an unsupported log file format version.\n", filename);
            okay = 0;
        }
    }

    if (!okay) {
        quit_altrace_playback();
        _exit(42);
    }
}

static void quit_altrace_playback(void)
{
    const int io = logfd;
    size_t i;

    logfd = -1;

    fflush(stdout);

    fprintf(stderr, APPNAME ": Shutting down...\n");
    fflush(stderr);

    if ((io != -1) && (close(io) < 0)) {
        fprintf(stderr, APPNAME ": Failed to close OpenAL log file: %s\n", strerror(errno));
    }

    close_real_openal();

    for (i = 0; i < (sizeof (ioblobs) / sizeof (ioblobs[0])); i++) {
        free(ioblobs[i]);
        ioblobs[i] = NULL;
    }
    next_ioblob = 0;

    free_device_map();
    free_context_map();
    free_source_map();
    free_buffer_map();
    free_stackframe_map();
    free_threadid_map();

    fflush(stderr);
}

static const char *alcboolString(const ALCboolean x)
{
    char *str;
    switch (x) {
        case ALC_TRUE: return "ALC_TRUE";
        case ALC_FALSE: return "ALC_FALSE";
        default: break;
    }

    str = (char *) get_ioblob(32);
    snprintf(str, 32, "0x%X", (uint) x);
    return str;
}

static const char *alboolString(const ALCboolean x)
{
    char *str;
    switch (x) {
        case AL_TRUE: return "AL_TRUE";
        case AL_FALSE: return "AL_FALSE";
        default: break;
    }

    str = (char *) get_ioblob(32);
    snprintf(str, 32, "0x%X", (uint) x);
    return str;
}

static const char *alcenumString(const ALCenum x)
{
    char *str;
    switch (x) {
        #define ENUM_TEST(e) case e: return #e
        ENUM_TEST(ALC_FREQUENCY);
        ENUM_TEST(ALC_REFRESH);
        ENUM_TEST(ALC_SYNC);
        ENUM_TEST(ALC_MONO_SOURCES);
        ENUM_TEST(ALC_STEREO_SOURCES);
        ENUM_TEST(ALC_NO_ERROR);
        ENUM_TEST(ALC_INVALID_DEVICE);
        ENUM_TEST(ALC_INVALID_CONTEXT);
        ENUM_TEST(ALC_INVALID_ENUM);
        ENUM_TEST(ALC_INVALID_VALUE);
        ENUM_TEST(ALC_OUT_OF_MEMORY);
        ENUM_TEST(ALC_MAJOR_VERSION);
        ENUM_TEST(ALC_MINOR_VERSION);
        ENUM_TEST(ALC_ATTRIBUTES_SIZE);
        ENUM_TEST(ALC_ALL_ATTRIBUTES);
        ENUM_TEST(ALC_DEFAULT_DEVICE_SPECIFIER);
        ENUM_TEST(ALC_DEVICE_SPECIFIER);
        ENUM_TEST(ALC_EXTENSIONS);
        ENUM_TEST(ALC_CAPTURE_DEVICE_SPECIFIER);
        ENUM_TEST(ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER);
        ENUM_TEST(ALC_CAPTURE_SAMPLES);
        ENUM_TEST(ALC_DEFAULT_ALL_DEVICES_SPECIFIER);
        ENUM_TEST(ALC_ALL_DEVICES_SPECIFIER);
        ENUM_TEST(ALC_CONNECTED);
        #undef ENUM_TEST
        default: break;
    }

    str = (char *) get_ioblob(32);
    snprintf(str, 32, "0x%X", (uint) x);
    return str;
}

static const char *alenumString(const ALCenum x)
{
    char *str;
    switch (x) {
        #define ENUM_TEST(e) case e: return #e
        ENUM_TEST(AL_NONE);
        ENUM_TEST(AL_SOURCE_RELATIVE);
        ENUM_TEST(AL_CONE_INNER_ANGLE);
        ENUM_TEST(AL_CONE_OUTER_ANGLE);
        ENUM_TEST(AL_PITCH);
        ENUM_TEST(AL_POSITION);
        ENUM_TEST(AL_DIRECTION);
        ENUM_TEST(AL_VELOCITY);
        ENUM_TEST(AL_LOOPING);
        ENUM_TEST(AL_BUFFER);
        ENUM_TEST(AL_GAIN);
        ENUM_TEST(AL_MIN_GAIN);
        ENUM_TEST(AL_MAX_GAIN);
        ENUM_TEST(AL_ORIENTATION);
        ENUM_TEST(AL_SOURCE_STATE);
        ENUM_TEST(AL_INITIAL);
        ENUM_TEST(AL_PLAYING);
        ENUM_TEST(AL_PAUSED);
        ENUM_TEST(AL_STOPPED);
        ENUM_TEST(AL_BUFFERS_QUEUED);
        ENUM_TEST(AL_BUFFERS_PROCESSED);
        ENUM_TEST(AL_REFERENCE_DISTANCE);
        ENUM_TEST(AL_ROLLOFF_FACTOR);
        ENUM_TEST(AL_CONE_OUTER_GAIN);
        ENUM_TEST(AL_MAX_DISTANCE);
        ENUM_TEST(AL_SEC_OFFSET);
        ENUM_TEST(AL_SAMPLE_OFFSET);
        ENUM_TEST(AL_BYTE_OFFSET);
        ENUM_TEST(AL_SOURCE_TYPE);
        ENUM_TEST(AL_STATIC);
        ENUM_TEST(AL_STREAMING);
        ENUM_TEST(AL_UNDETERMINED);
        ENUM_TEST(AL_FORMAT_MONO8);
        ENUM_TEST(AL_FORMAT_MONO16);
        ENUM_TEST(AL_FORMAT_STEREO8);
        ENUM_TEST(AL_FORMAT_STEREO16);
        ENUM_TEST(AL_FREQUENCY);
        ENUM_TEST(AL_BITS);
        ENUM_TEST(AL_CHANNELS);
        ENUM_TEST(AL_SIZE);
        ENUM_TEST(AL_UNUSED);
        ENUM_TEST(AL_PENDING);
        ENUM_TEST(AL_PROCESSED);
        ENUM_TEST(AL_INVALID_NAME);
        ENUM_TEST(AL_INVALID_ENUM);
        ENUM_TEST(AL_INVALID_VALUE);
        ENUM_TEST(AL_INVALID_OPERATION);
        ENUM_TEST(AL_OUT_OF_MEMORY);
        ENUM_TEST(AL_VENDOR);
        ENUM_TEST(AL_VERSION);
        ENUM_TEST(AL_RENDERER);
        ENUM_TEST(AL_EXTENSIONS);
        ENUM_TEST(AL_DOPPLER_FACTOR);
        ENUM_TEST(AL_DOPPLER_VELOCITY);
        ENUM_TEST(AL_SPEED_OF_SOUND);
        ENUM_TEST(AL_DISTANCE_MODEL);
        ENUM_TEST(AL_INVERSE_DISTANCE);
        ENUM_TEST(AL_INVERSE_DISTANCE_CLAMPED);
        ENUM_TEST(AL_LINEAR_DISTANCE);
        ENUM_TEST(AL_LINEAR_DISTANCE_CLAMPED);
        ENUM_TEST(AL_EXPONENT_DISTANCE);
        ENUM_TEST(AL_EXPONENT_DISTANCE_CLAMPED);
        ENUM_TEST(AL_FORMAT_MONO_FLOAT32);
        ENUM_TEST(AL_FORMAT_STEREO_FLOAT32);
        #undef ENUM_TEST
        default: break;
    }

    str = (char *) get_ioblob(32);
    snprintf(str, 32, "0x%X", (uint) x);
    return str;
}

static const char *litString(const char *str)
{
    if (str) {
        const size_t slen = (strlen(str) + 1) * 2;
        char *retval = (char *) get_ioblob(slen);
        char *ptr = retval;
        *(ptr++) = '"';
        while (1) {
            const char ch = *(str++);
            if (ch == '\0') {
                *(ptr++) = '"';
                *(ptr++) = '\0';
                break;
            } else if (ch == '"') {
                *(ptr++) = '\\';
                *(ptr++) = '"';
            } else {
                *(ptr++) = ch;
            }
        }
        return retval;
    }
    return "NULL";
}

static void dump_alcGetCurrentContext(void)
{
    IO_START(alcGetCurrentContext);
    ALCcontext *retval = (ALCcontext *) IO_PTR();
    if (dump_calls) { printf("() => %p\n", retval); }
    if (run_log) { REAL_alcGetCurrentContext(); }
    IO_END();
}

static void dump_alcGetContextsDevice(void)
{
    IO_START(alcGetContextsDevice);
    ALCcontext *context = (ALCcontext *) IO_PTR();
    ALCdevice *retval = (ALCdevice *) IO_PTR();
    if (dump_calls) { printf("(%p) => %p\n", context, retval); }
    if (run_log) { REAL_alcGetContextsDevice(context); }
    IO_END();
}

static void dump_alcIsExtensionPresent(void)
{
    IO_START(alcIsExtensionPresent);
    ALCdevice *device = (ALCdevice *) IO_PTR();
    const ALCchar *extname = (const ALCchar *) IO_STRING();
    const ALCboolean retval = IO_ALCBOOLEAN();
    if (dump_calls) { printf("(%p, %s) => %s\n", device, litString(extname), alcboolString(retval)); }
    if (run_log) { REAL_alcIsExtensionPresent(device, extname); }
    IO_END();
}

static void dump_alcGetProcAddress(void)
{
    IO_START(alcGetProcAddress);
    ALCdevice *device = (ALCdevice *) IO_PTR();
    const ALCchar *funcname = (const ALCchar *) IO_STRING();
    void *retval = IO_PTR();
    if (dump_calls) { printf("(%p, %s) => %p\n", device, litString(funcname), retval); }
    if (run_log) { REAL_alcGetProcAddress(get_mapped_device(device), funcname); }
    IO_END();

}

static void dump_alcGetEnumValue(void)
{
    IO_START(alcGetEnumValue);
    ALCdevice *device = (ALCdevice *) IO_PTR();
    const ALCchar *enumname = (const ALCchar *) IO_STRING();
    const ALCenum retval = IO_ALCENUM();
    if (dump_calls) { printf("(%p, %s) => %s\n", device, litString(enumname), alcenumString(retval)); }
    if (run_log) { REAL_alcGetEnumValue(device, enumname); }
    IO_END();
}

static void dump_alcGetString(void)
{
    IO_START(alcGetEnumValue);
    ALCdevice *device = (ALCdevice *) IO_PTR();
    const ALCenum param = IO_ALCENUM();
    const ALCchar *retval = (const ALCchar *) IO_STRING();
    if (dump_calls) { printf("(%p, %s) => %s\n", device, alcenumString(param), litString(retval)); }
    if (run_log) { REAL_alcGetString(device, param); }
    IO_END();
}

static void dump_alcCaptureOpenDevice(void)
{
    IO_START(alcCaptureOpenDevice);
    const ALCchar *devicename = (const ALCchar *) IO_STRING();
    const ALCuint frequency = IO_UINT32();
    const ALCenum format = IO_ALCENUM();
    const ALCsizei buffersize = IO_ALSIZEI();
    ALCdevice *retval = (ALCdevice *) IO_PTR();
    const ALint major_version = retval ? IO_INT32() : 0;
    const ALint minor_version = retval ? IO_INT32() : 0;
    const ALCchar *devspec = (const ALCchar *) (retval ? IO_STRING() : NULL);
    const ALCchar *extensions = (const ALCchar *) (retval ? IO_STRING() : NULL);

    if (dump_calls) { printf("(%s, %u, %s, %u) => %p\n", litString(devicename), (uint) frequency, alcenumString(format), (uint) buffersize, retval); }

    if (dump_state_changes) {
        printf("<<< CAPTURE DEVICE STATE: alc_version=%d.%d device_specifier=%s extensions=%s >>>\n", (int) major_version, (int) minor_version, litString(devspec), litString(extensions));
    }

    if (run_log) {
        ALCdevice *dev = REAL_alcCaptureOpenDevice(devicename, frequency, format, buffersize);
        if (!dev && retval) {
            fprintf(stderr, "Uhoh, failed to open capture device when log did!\n");
            if (devicename) {
                fprintf(stderr, "Trying NULL device...\n");
                dev = REAL_alcCaptureOpenDevice(NULL, frequency, format, buffersize);
                if (!dev) {
                    fprintf(stderr, "Still no luck. This is probably going to go wrong.\n");
                } else {
                    fprintf(stderr, "That worked. Carrying on.\n");
                }
            }
        }
        if (dev) {
            add_device_to_map(retval, dev);
        }
    }

    IO_END();
}

static void dump_alcCaptureCloseDevice(void)
{
    IO_START(alcCaptureCloseDevice);
    ALCdevice *device = (ALCdevice *) IO_PTR();
    const ALCboolean retval = IO_ALCBOOLEAN();
    if (dump_calls) { printf("(%p) => %s\n", device, alcboolString(retval)); }
    if (run_log) { REAL_alcCaptureCloseDevice(get_mapped_device(device)); }
    IO_END();
}

static void dump_alcOpenDevice(void)
{
    IO_START(alcOpenDevice);
    const ALCchar *devicename = IO_STRING();
    ALCdevice *retval = (ALCdevice *) IO_PTR();
    const ALint major_version = retval ? IO_INT32() : 0;
    const ALint minor_version = retval ? IO_INT32() : 0;
    const ALCchar *devspec = (const ALCchar *) (retval ? IO_STRING() : NULL);
    const ALCchar *extensions = (const ALCchar *) (retval ? IO_STRING() : NULL);

    if (dump_calls) { printf("(%s) => %p\n", litString(devicename), retval); }

    if (dump_state_changes) {
        printf("<<< PLAYBACK DEVICE STATE: alc_version=%d.%d device_specifier=%s extensions=%s >>>\n", (int) major_version, (int) minor_version, litString(devspec), litString(extensions));
    }

    if (run_log) {
        ALCdevice *dev = REAL_alcOpenDevice(devicename);
        if (!dev && retval) {
            fprintf(stderr, "Uhoh, failed to open playback device when log did!\n");
            if (devicename) {
                fprintf(stderr, "Trying NULL device...\n");
                dev = REAL_alcOpenDevice(NULL);
                if (!dev) {
                    fprintf(stderr, "Still no luck. This is probably going to go wrong.\n");
                } else {
                    fprintf(stderr, "That worked. Carrying on.\n");
                }
            }
        }
        if (dev) {
            add_device_to_map(retval, dev);
        }
    }

    IO_END();
}

static void dump_alcCloseDevice(void)
{
    IO_START(alcCloseDevice);
    ALCdevice *device = (ALCdevice *) IO_PTR();
    const ALCboolean retval = IO_ALCBOOLEAN();
    if (dump_calls) { printf("(%p) => %s\n", device, alcboolString(retval)); }
    if (run_log) { REAL_alcCloseDevice(get_mapped_device(device)); }
    IO_END();
}

static void dump_alcCreateContext(void)
{
    IO_START(alcCreateContext);
    ALCcontext *retval;
    ALCint *attrlist = NULL;
    ALCdevice *device = (ALCdevice *) IO_PTR();
    const uint32 attrcount = IO_UINT32();
    if (attrcount) {
        ALCint i;
        attrlist = get_ioblob(sizeof (ALCint) * attrcount);
        for (i = 0; i < attrcount; i++) {
            attrlist[i] = (ALCint) IO_INT32();
        }
    }
    retval = (ALCcontext *) IO_PTR();
    if (dump_calls) {
        printf("(%p, ", device);
        if (!attrlist) {
            printf("NULL");
        } else {
            ALCint i;
            printf("{");
            for (i = 0; i < attrcount; i += 2) {
                printf(" %s, %u,", alcenumString(attrlist[i]), (uint) attrlist[i+1]);
            }
            printf(" 0 }");
        }
        printf(") => %p\n", retval);
    }

    if (run_log) {
        ALCcontext *ctx = REAL_alcCreateContext(get_mapped_device(device), attrlist);
        if (!ctx && retval) {
            fprintf(stderr, "Uhoh, failed to create context when log did!\n");
            if (attrlist) {
                fprintf(stderr, "Trying default context...\n");
                ctx = REAL_alcCreateContext(get_mapped_device(device), NULL);
                if (!ctx) {
                    fprintf(stderr, "Still no luck. This is probably going to go wrong.\n");
                } else {
                    fprintf(stderr, "That worked. Carrying on.\n");
                }
            }
        }
        if (ctx) {
            add_context_to_map(retval, ctx);
        }
    }

    IO_END();

}

static void dump_alcMakeContextCurrent(void)
{
    IO_START(alcMakeContextCurrent);
    ALCcontext *ctx = (ALCcontext *) IO_PTR();
    const ALCboolean retval = IO_ALCBOOLEAN();
    if (dump_calls) { printf("(%p) => %s\n", ctx, alcboolString(retval)); }
    if (run_log) { REAL_alcMakeContextCurrent(get_mapped_context(ctx)); }
    IO_END();
}

static void dump_alcProcessContext(void)
{
    IO_START(alcProcessContext);
    ALCcontext *ctx = (ALCcontext *) IO_PTR();
    if (dump_calls) { printf("(%p)\n", ctx); }
    if (run_log) { REAL_alcProcessContext(get_mapped_context(ctx)); }
    IO_END();
}

static void dump_alcSuspendContext(void)
{
    IO_START(alcSuspendContext);
    ALCcontext *ctx = (ALCcontext *) IO_PTR();
    if (dump_calls) { printf("(%p)\n", ctx); }
    if (run_log) { REAL_alcSuspendContext(get_mapped_context(ctx)); }
    IO_END();
}

static void dump_alcDestroyContext(void)
{
    IO_START(alcDestroyContext);
    ALCcontext *ctx = (ALCcontext *) IO_PTR();
    if (dump_calls) { printf("(%p)\n", ctx); }
    if (run_log) { REAL_alcDestroyContext(get_mapped_context(ctx)); }
    IO_END();
}

static void dump_alcGetError(void)
{
    IO_START(alcGetError);
    ALCdevice *device = (ALCdevice *) IO_PTR();
    const ALCenum retval = IO_ALCENUM();
    if (dump_calls) { printf("(%p) => %s\n", device, alcboolString(retval)); }
    if (run_log) { REAL_alcGetError(get_mapped_device(device)); }
    IO_END();
}

static void dump_alcGetIntegerv(void)
{
    IO_START(alcGetIntegerv);
    ALCdevice *device = (ALCdevice *) IO_PTR();
    const ALCenum param = IO_ALCENUM();
    const ALCsizei size = IO_ALCSIZEI();
    ALint *origvalues = (ALint *) IO_PTR();
    ALint *values = origvalues ? get_ioblob(size * sizeof (ALCint)) : NULL;
    ALCsizei i;

    if (origvalues) {
        for (i = 0; i < size; i++) {
            values[i] = IO_INT32();
        }
    }

    if (dump_calls) {
        printf("(%p, %s, %u, %p) => {", device, alcenumString(param), (uint) size, origvalues);
        if (values) {
            for (i = 0; i < size; i++) {
                printf("%s %d", i > 0 ? "," : "", values[i]);
            }
            printf("%s", size > 0 ? " " : "");
        }
        printf("}\n");
    }

    if (run_log) { REAL_alcGetIntegerv(get_mapped_device(device), param, size, values); }

    IO_END();
}

static void dump_alcCaptureStart(void)
{
    IO_START(alcCaptureStart);
    ALCdevice *device = (ALCdevice *) IO_PTR();
    if (dump_calls) { printf("(%p)\n", device); }
    if (run_log) { REAL_alcCaptureStart(get_mapped_device(device)); }
    IO_END();
}

static void dump_alcCaptureStop(void)
{
    IO_START(alcCaptureStop);
    ALCdevice *device = (ALCdevice *) IO_PTR();
    if (dump_calls) { printf("(%p)\n", device); }
    if (run_log) { REAL_alcCaptureStop(get_mapped_device(device)); }
    IO_END();
}

static void dump_alcCaptureSamples(void)
{
    IO_START(alcCaptureSamples);
    ALCdevice *device = (ALCdevice *) IO_PTR();
    const ALCsizei samples = IO_ALCSIZEI();
    uint64 bloblen;
    uint8 *blob = IO_BLOB(&bloblen); (void) bloblen;
    if (dump_calls) { printf("(%p, &buffer, %u)\n", device, (uint) samples); }
    if (run_log) { REAL_alcCaptureSamples(get_mapped_device(device), blob, samples); }
    IO_END();
}

static void dump_alDopplerFactor(void)
{
    IO_START(alDopplerFactor);
    const ALfloat value = IO_FLOAT();
    if (dump_calls) { printf("(%f)\n", value); }
    if (run_log) { REAL_alDopplerFactor(value); }
    IO_END();
}

static void dump_alDopplerVelocity(void)
{
    IO_START(alDopplerVelocity);
    const ALfloat value = IO_FLOAT();
    if (dump_calls) { printf("(%f)\n", value); }
    if (run_log) { REAL_alDopplerVelocity(value); }
    IO_END();
}

static void dump_alSpeedOfSound(void)
{
    IO_START(alSpeedOfSound);
    const ALfloat value = IO_FLOAT();
    if (dump_calls) { printf("(%f)\n", value); }
    if (run_log) { REAL_alSpeedOfSound(value); }
    IO_END();
}

static void dump_alDistanceModel(void)
{
    IO_START(alDistanceModel);
    const ALenum model = IO_ENUM();
    if (dump_calls) { printf("(%s)\n", alenumString(model)); }
    if (run_log) { REAL_alDistanceModel(model); }
    IO_END();
}

static void dump_alEnable(void)
{
    IO_START(alEnable);
    const ALenum capability = IO_ENUM();
    if (dump_calls) { printf("(%s)\n", alenumString(capability)); }
    if (run_log) { REAL_alEnable(capability); }
    IO_END();
}

static void dump_alDisable(void)
{
    IO_START(alDisable);
    const ALenum capability = IO_ENUM();
    if (dump_calls) { printf("(%s)\n", alenumString(capability)); }
    if (run_log) { REAL_alDisable(capability); }
    IO_END();
}

static void dump_alIsEnabled(void)
{
    IO_START(alIsEnabled);
    const ALenum capability = IO_ENUM();
    const ALboolean retval = IO_BOOLEAN();
    if (dump_calls) { printf("(%s) => %s\n", alenumString(capability), alboolString(retval)); }
    if (run_log) { REAL_alIsEnabled(capability); }
    IO_END();
}

static void dump_alGetString(void)
{
    IO_START(alGetString);
    const ALenum param = IO_ENUM();
    const ALchar *retval = (const ALchar *) IO_STRING();
    if (dump_calls) { printf("(%s) => %s\n", alenumString(param), litString(retval)); }
    if (run_log) { REAL_alGetString(param); }
    IO_END();
}

static void dump_alGetBooleanv(void)
{
    IO_START(alGetBooleanv);
    const ALenum param = IO_ENUM();
    ALboolean *origvalues = (ALboolean *) IO_PTR();
    const ALsizei numvals = IO_ALSIZEI();
    ALboolean *values = numvals ? get_ioblob(numvals * sizeof (ALboolean)) : NULL;
    ALsizei i;
    for (i = 0; i < numvals; i++) {
        values[i] = IO_BOOLEAN();
    }

    if (dump_calls) {
        printf("(%s, %p) => {", alenumString(param), origvalues);
        for (i = 0; i < numvals; i++) {
            printf("%s %s", i > 0 ? "," : "", alboolString(values[i]));
        }
        printf("%s}\n", numvals > 0 ? " " : "");
    }

    if (run_log) { REAL_alGetBooleanv(param, values); }

    IO_END();
}

static void dump_alGetIntegerv(void)
{
    IO_START(alGetIntegerv);
    const ALenum param = IO_ENUM();
    ALint *origvalues = (ALint *) IO_PTR();
    const ALsizei numvals = IO_ALSIZEI();
    ALint *values = numvals ? get_ioblob(numvals * sizeof (ALint)) : NULL;
    ALsizei i;
    ALboolean isenum = AL_FALSE;

    for (i = 0; i < numvals; i++) {
        values[i] = IO_INT32();
    }

    switch (param) {
        case AL_DISTANCE_MODEL: isenum = AL_TRUE; break;
        default: break;
    }

    if (dump_calls) {
        printf("(%s, %p) => {", alenumString(param), origvalues);
        for (i = 0; i < numvals; i++) {
            if (isenum) {
                printf("%s %s", i > 0 ? "," : "", alenumString((ALenum) values[i]));
            } else {
                printf("%s %d", i > 0 ? "," : "", (int) values[i]);
            }
        }
        printf("%s}\n", numvals > 0 ? " " : "");
    }

    if (run_log) { REAL_alGetIntegerv(param, values); }

    IO_END();
}

static void dump_alGetFloatv(void)
{
    IO_START(alGetFloatv);
    const ALenum param = IO_ENUM();
    ALfloat *origvalues = (ALfloat *) IO_PTR();
    const ALsizei numvals = IO_ALSIZEI();
    ALfloat *values = numvals ? get_ioblob(numvals * sizeof (ALfloat)) : NULL;
    ALsizei i;
    for (i = 0; i < numvals; i++) {
        values[i] = IO_FLOAT();
    }

    if (dump_calls) {
        printf("(%s, %p) => {", alenumString(param), origvalues);
        for (i = 0; i < numvals; i++) {
            printf("%s %f", i > 0 ? "," : "", values[i]);
        }
        printf("%s}\n", numvals > 0 ? " " : "");
    }

    if (run_log) { REAL_alGetFloatv(param, values); }

    IO_END();
}

static void dump_alGetDoublev(void)
{
    IO_START(alGetDoublev);
    const ALenum param = IO_ENUM();
    ALdouble *origvalues = (ALdouble *) IO_PTR();
    const ALsizei numvals = IO_ALSIZEI();
    ALdouble *values = numvals ? get_ioblob(numvals * sizeof (ALdouble)) : NULL;
    ALsizei i;
    for (i = 0; i < numvals; i++) {
        values[i] = IO_DOUBLE();
    }

    if (dump_calls) {
        printf("(%s, %p) => {", alenumString(param), origvalues);
        for (i = 0; i < numvals; i++) {
            printf("%s %f", i > 0 ? "," : "", values[i]);
        }
        printf("%s}\n", numvals > 0 ? " " : "");
    }

    if (run_log) { REAL_alGetDoublev(param, values); }

    IO_END();
}

static void dump_alGetBoolean(void)
{
    IO_START(alGetBoolean);
    const ALenum param = IO_ENUM();
    const ALboolean retval = IO_BOOLEAN();
    if (dump_calls) { printf("(%s) => %s\n", alenumString(param), alboolString(retval)); }
    if (run_log) { REAL_alGetBoolean(param); }
    IO_END();
}

static void dump_alGetInteger(void)
{
    IO_START(alGetInteger);
    const ALenum param = IO_ENUM();
    const ALint retval = IO_INT32();
    if (dump_calls) { printf("(%s) => %d\n", alenumString(param), (int) retval); }
    if (run_log) { REAL_alGetInteger(param); }
    IO_END();
}

static void dump_alGetFloat(void)
{
    IO_START(alGetFloat);
    const ALenum param = IO_ENUM();
    const ALfloat retval = IO_FLOAT();
    if (dump_calls) { printf("(%s) => %f\n", alenumString(param), retval); }
    if (run_log) { REAL_alGetFloat(param); }
    IO_END();
}

static void dump_alGetDouble(void)
{
    IO_START(alGetDouble);
    const ALenum param = IO_ENUM();
    const ALdouble retval = IO_DOUBLE();
    if (dump_calls) { printf("(%s) => %f\n", alenumString(param), (float) retval); }
    if (run_log) { REAL_alGetDouble(param); }
    IO_END();
}

static void dump_alIsExtensionPresent(void)
{
    IO_START(alIsExtensionPresent);
    const ALchar *extname = (const ALchar *) IO_STRING();
    const ALboolean retval = IO_BOOLEAN();
    if (dump_calls) { printf("(%s) => %s\n", litString(extname), alboolString(retval)); }
    if (run_log) { REAL_alIsExtensionPresent(extname); }
    IO_END();
}

static void dump_alGetError(void)
{
    IO_START(alGetError);
    const ALenum retval = IO_ENUM();
    if (dump_calls) { printf("() => %s\n", alenumString(retval)); }
    if (run_log) { REAL_alGetError(); }
    IO_END();
}

static void dump_alGetProcAddress(void)
{
    IO_START(alGetProcAddress);
    const ALchar *funcname = (const ALchar *) IO_STRING();
    void *retval = IO_PTR();
    if (dump_calls) { printf("(%s) => %p\n", litString(funcname), retval); }
    if (run_log) { REAL_alGetProcAddress(funcname); }
    IO_END();
}

static void dump_alGetEnumValue(void)
{
    IO_START(alGetProcAddress);
    const ALchar *enumname = (const ALchar *) IO_STRING();
    const ALenum retval = IO_ENUM();
    if (dump_calls) { printf("(%s) => %s\n", litString(enumname), alenumString(retval)); }
    if (run_log) {  REAL_alGetEnumValue(enumname); }
    IO_END();
}

static void dump_alListenerfv(void)
{
    IO_START(alListenerfv);
    const ALenum param = IO_ENUM();
    ALfloat *origvalues = (ALfloat *) IO_PTR();
    const uint32 numvals = IO_UINT32();
    ALfloat *values = (ALfloat *) (numvals ? get_ioblob(sizeof (ALfloat) * numvals) : NULL);
    uint32 i;

    for (i = 0; i < numvals; i++) {
        values[i] = IO_FLOAT();
    }

    if (dump_calls) {
        printf("(%s, ", alenumString(param));
        if (!origvalues) {
            printf("NULL)\n");
        } else {
            printf("{");
            for (i = 0; i < numvals; i++) {
                printf("%s %f", i > 0 ? "," : "", values[i]);
            }
            printf("%s})\n", numvals > 0 ? " " : "");
        }
    }

    if (run_log) { REAL_alListenerfv(param, values); }

    IO_END();
}

static void dump_alListenerf(void)
{
    IO_START(alListenerf);
    const ALenum param = IO_ENUM();
    const ALfloat value = IO_FLOAT();
    if (dump_calls) { printf("(%s, %f)\n", alenumString(param), value); }
    if (run_log) { REAL_alListenerf(param, value); }
    IO_END();
}

static void dump_alListener3f(void)
{
    IO_START(alListener3f);
    const ALenum param = IO_ENUM();
    const ALfloat value1 = IO_FLOAT();
    const ALfloat value2 = IO_FLOAT();
    const ALfloat value3 = IO_FLOAT();
    if (dump_calls) { printf("(%s, %f, %f, %f)\n", alenumString(param), value1, value2, value3); }
    if (run_log) { REAL_alListener3f(param, value1, value2, value3); }
    IO_END();
}

static void dump_alListeneriv(void)
{
    IO_START(alListeneriv);
    const ALenum param = IO_ENUM();
    ALint *origvalues = (ALint *) IO_PTR();
    const uint32 numvals = IO_UINT32();
    ALint *values = (ALint *) (numvals ? get_ioblob(sizeof (ALint) * numvals) : NULL);
    uint32 i;

    for (i = 0; i < numvals; i++) {
        values[i] = IO_INT32();
    }

    if (dump_calls) {
        printf("(%s, ", alenumString(param));
        if (!origvalues) {
            printf("NULL)\n");
        } else {
            printf("{");
            for (i = 0; i < numvals; i++) {
                printf("%s %d", i > 0 ? "," : "", (int) values[i]);
            }
            printf("%s})\n", numvals > 0 ? " " : "");
        }
    }

    if (run_log) { REAL_alListeneriv(param, values); }

    IO_END();
}

static void dump_alListeneri(void)
{
    IO_START(alListeneri);
    const ALenum param = IO_ENUM();
    const ALint value = IO_INT32();
    if (dump_calls) { printf("(%s, %d)\n", alenumString(param), value); }
    if (run_log) { REAL_alListeneri(param, value); }
    IO_END();
}

static void dump_alListener3i(void)
{
    IO_START(alListener3i);
    const ALenum param = IO_ENUM();
    const ALint value1 = IO_INT32();
    const ALint value2 = IO_INT32();
    const ALint value3 = IO_INT32();
    if (dump_calls) { printf("(%s, %d, %d, %d)\n", alenumString(param), (int) value1, (int) value2, (int) value3); }
    if (run_log) { REAL_alListener3i(param, value1, value2, value3); }
    IO_END();
}

static void dump_alGetListenerfv(void)
{
    IO_START(alGetListenerfv);
    const ALenum param = IO_ENUM();
    ALfloat *origvalues = (ALfloat *) IO_PTR();
    const uint32 numvals = IO_UINT32();
    ALfloat *values = (ALfloat *) (numvals ? get_ioblob(sizeof (ALfloat) * numvals) : NULL);
    uint32 i;

    for (i = 0; i < numvals; i++) {
        values[i] = IO_FLOAT();
    }

    if (dump_calls) {
        printf("(%s, %p)", alenumString(param), origvalues);
        if (origvalues) {
            printf(" => {");
            for (i = 0; i < numvals; i++) {
                printf("%s %f", i > 0 ? "," : "", values[i]);
            }
            printf("%s}", numvals > 0 ? " " : "");
        }
        printf("\n");
    }

    if (run_log) { REAL_alGetListenerfv(param, values); }

    IO_END();
}

static void dump_alGetListenerf(void)
{
    IO_START(alGetListenerf);
    const ALenum param = IO_ENUM();
    ALfloat *origvalue = (ALfloat *) IO_PTR();
    const ALfloat value = IO_FLOAT();
    if (dump_calls) { printf("(%s, %p) => { %f }\n", alenumString(param), origvalue, value); }
    if (run_log) { ALfloat f; REAL_alGetListenerf(param, &f); }
    IO_END();
}

static void dump_alGetListener3f(void)
{
    IO_START(alGetListener3f);
    const ALenum param = IO_ENUM();
    ALfloat *origvalue1 = (ALfloat *) IO_PTR();
    ALfloat *origvalue2 = (ALfloat *) IO_PTR();
    ALfloat *origvalue3 = (ALfloat *) IO_PTR();
    const ALfloat value1 = IO_FLOAT();
    const ALfloat value2 = IO_FLOAT();
    const ALfloat value3 = IO_FLOAT();
    if (dump_calls) { printf("(%s, %p, %p, %p) => { %f, %f %f }\n", alenumString(param), origvalue1, origvalue2, origvalue3, value1, value2, value3); }
    if (run_log) { ALfloat f1, f2, f3; REAL_alGetListener3f(param, &f1, &f2, &f3); }
    IO_END();
}

static void dump_alGetListeneriv(void)
{
    IO_START(alGetListeneriv);
    const ALenum param = IO_ENUM();
    ALint *origvalues = (ALint *) IO_PTR();
    const uint32 numvals = IO_UINT32();
    ALint *values = (ALint *) (numvals ? get_ioblob(sizeof (ALint) * numvals) : NULL);
    uint32 i;

    for (i = 0; i < numvals; i++) {
        values[i] = IO_INT32();
    }

    if (dump_calls) {
        printf("(%s, %p)", alenumString(param), origvalues);
        if (origvalues) {
            printf(" => {");
            for (i = 0; i < numvals; i++) {
                printf("%s %d", i > 0 ? "," : "", (int) values[i]);
            }
            printf("%s}", numvals > 0 ? " " : "");
        }
        printf("\n");
    }

    if (run_log) { REAL_alGetListeneriv(param, values); }

    IO_END();
}

static void dump_alGetListeneri(void)
{
    IO_START(alGetListeneri);
    const ALenum param = IO_ENUM();
    ALint *origvalue = (ALint *) IO_PTR();
    const ALint value = IO_INT32();
    if (dump_calls) { printf("(%s, %p) => { %d }\n", alenumString(param), origvalue, (int) value); }

    if (run_log) { ALint i; REAL_alGetListeneri(param, &i); }

    IO_END();
}

static void dump_alGetListener3i(void)
{
    IO_START(alGetListener3i);
    const ALenum param = IO_ENUM();
    ALint *origvalue1 = (ALint *) IO_PTR();
    ALint *origvalue2 = (ALint *) IO_PTR();
    ALint *origvalue3 = (ALint *) IO_PTR();
    const ALint value1 = IO_INT32();
    const ALint value2 = IO_INT32();
    const ALint value3 = IO_INT32();
    if (dump_calls) { printf("(%s, %p, %p, %p) => { %d, %d %d }\n", alenumString(param), origvalue1, origvalue2, origvalue3, (int) value1, (int) value2, (int) value3); }
    if (run_log) { ALint i1, i2, i3; REAL_alGetListener3i(param, &i1, &i2, &i3); }
    IO_END();
}

static void dump_alGenSources(void)
{
    IO_START(alGenSources);
    const ALsizei n = IO_ALSIZEI();
    ALuint *names = (ALuint *) get_ioblob(sizeof (ALuint) * n);
    ALsizei i;

    for (i = 0; i < n; i++) {
        names[i] = IO_UINT32();
    }

    if (dump_calls) {
        printf("(%u) => {", (uint) n);
        for (i = 0; i < n; i++) {
            printf("%s %u", i > 0 ? "," : "", (uint) names[i]);
        }
        printf("%s}\n", n > 0 ? " " : "");
    }

    if (run_log) {
        ALuint *realnames = (ALuint *) get_ioblob(sizeof (ALuint) * n);
        memset(realnames, '\0', sizeof (ALuint) * n);
        REAL_alGenSources(n, realnames);
        for (i = 0; i < n; i++) {
            if (!realnames[i] && names[i]) {
                fprintf(stderr, "Uhoh, we didn't generate enough sources!\n");
                fprintf(stderr, "This is probably going to cause playback problems.\n");
            } else {
                add_source_to_map(names[i], realnames[i]);
            }
        }
    }

    IO_END();
}

static void dump_alDeleteSources(void)
{
    IO_START(alDeleteSources);
    const ALsizei n = IO_ALSIZEI();
    ALuint *names = (ALuint *) get_ioblob(sizeof (ALuint) * n);
    ALsizei i;

    for (i = 0; i < n; i++) {
        names[i] = IO_UINT32();
    }

    if (dump_calls) {
        printf("(%u, {", (uint) n);
        for (i = 0; i < n; i++) {
            printf("%s %u", i > 0 ? "," : "", (uint) names[i]);
        }
        printf("%s})\n", n > 0 ? " " : "");
    }

    if (run_log) {
        ALuint *realnames = (ALuint *) get_ioblob(sizeof (ALuint) * n);
        for (i = 0; i < n; i++) {
            realnames[i] = get_mapped_source(names[i]);
        }
        REAL_alDeleteSources(n, realnames);
    }

    IO_END();
}

static void dump_alIsSource(void)
{
    IO_START(alIsSource);
    const ALuint name = IO_UINT32();
    const ALboolean retval = IO_BOOLEAN();
    if (dump_calls) { printf("(%u) => %s\n", (uint) name, alboolString(retval)); }
    if (run_log) { REAL_alIsSource(get_mapped_source(name)); }
    IO_END();
}

static void dump_alSourcefv(void)
{
    IO_START(alSourcefv);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const uint32 numvals = IO_UINT32();
    ALfloat *values = (ALfloat *) get_ioblob(sizeof (ALfloat) * numvals);
    uint32 i;

    for (i = 0; i < numvals; i++) {
        values[i] = IO_FLOAT();
    }

    if (dump_calls) {
        printf("(%u, %s, {", (uint) name, alenumString(param));
        for (i = 0; i < numvals; i++) {
            printf("%s %f", i > 0 ? "," : "", values[i]);
        }
        printf("%s})\n", numvals > 0 ? " " : "");
    }

    if (run_log) { REAL_alSourcefv(get_mapped_source(name), param, values); }

    IO_END();
}

static void dump_alSourcef(void)
{
    IO_START(alSourcef);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALfloat value = IO_FLOAT();
    if (dump_calls) { printf("(%u, %s, %f)\n", (uint) name, alenumString(param), value); }
    if (run_log) { REAL_alSourcef(get_mapped_source(name), param, value); }
    IO_END();
}

static void dump_alSource3f(void)
{
    IO_START(alSource3f);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALfloat value1 = IO_FLOAT();
    const ALfloat value2 = IO_FLOAT();
    const ALfloat value3 = IO_FLOAT();
    if (dump_calls) { printf("(%u, %s, %f, %f, %f)\n", (uint) name, alenumString(param), value1, value2, value3); }
    if (run_log) { REAL_alSource3f(get_mapped_source(name), param, value1, value2, value3); }
    IO_END();
}

static void dump_alSourceiv(void)
{
    IO_START(alSourceiv);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const uint32 numvals = IO_UINT32();
    ALint *values = (ALint *) get_ioblob(sizeof (ALint) * numvals);
    uint32 i;

    for (i = 0; i < numvals; i++) {
        values[i] = IO_INT32();
    }

    if (dump_calls) {
        printf("(%u, %s, {", (uint) name, alenumString(param));
        for (i = 0; i < numvals; i++) {
            printf("%s %d", i > 0 ? "," : "", (int) values[i]);
        }
        printf("%s})\n", numvals > 0 ? " " : "");
    }

    if (run_log) { REAL_alSourceiv(get_mapped_source(name), param, values); }

    IO_END();
}

static void dump_alSourcei(void)
{
    IO_START(alSourcei);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALint value = IO_INT32();
    if (dump_calls) { printf("(%u, %s, %d)\n", (uint) name, alenumString(param), (int) value); }
    if (run_log) { REAL_alSourcei(get_mapped_source(name), param, value); }
    IO_END();
}

static void dump_alSource3i(void)
{
    IO_START(alSource3i);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALint value1 = IO_INT32();
    const ALint value2 = IO_INT32();
    const ALint value3 = IO_INT32();
    if (dump_calls) { printf("(%u, %s, %d, %d, %d)\n", (uint) name, alenumString(param), (int) value1, (int) value2, (int) value3); }
    if (run_log) { REAL_alSource3i(get_mapped_source(name), param, value1, value2, value3); }
    IO_END();
}

static void dump_alGetSourcefv(void)
{
    IO_START(alGetSourcefv);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    ALfloat *origvalues = (ALfloat *) IO_PTR();
    const uint32 numvals = IO_UINT32();
    ALfloat *values = (ALfloat *) (numvals ? get_ioblob(sizeof (ALfloat) * numvals) : NULL);
    uint32 i;

    for (i = 0; i < numvals; i++) {
        values[i] = IO_FLOAT();
    }

    if (dump_calls) {
        printf("(%u, %s, %p)", (uint) name, alenumString(param), origvalues);
        if (origvalues) {
            printf(" => {");
            for (i = 0; i < numvals; i++) {
                printf("%s %f", i > 0 ? "," : "", values[i]);
            }
            printf("%s}", numvals > 0 ? " " : "");
        }
        printf("\n");
    }

    if (run_log) { REAL_alGetSourcefv(get_mapped_source(name), param, values); }

    IO_END();
}

static void dump_alGetSourcef(void)
{
    IO_START(alGetSourcef);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    ALfloat *origvalue = (ALfloat *) IO_PTR();
    const ALfloat value = IO_FLOAT();
    if (dump_calls) { printf("(%u, %s, %p) => { %f }\n", (uint) name, alenumString(param), origvalue, value); }
    if (run_log) { ALfloat f; REAL_alGetSourcef(get_mapped_source(name), param, &f); }
    IO_END();
}

static void dump_alGetSource3f(void)
{
    IO_START(alGetSource3f);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    ALfloat *origvalue1 = (ALfloat *) IO_PTR();
    ALfloat *origvalue2 = (ALfloat *) IO_PTR();
    ALfloat *origvalue3 = (ALfloat *) IO_PTR();
    const ALfloat value1 = IO_FLOAT();
    const ALfloat value2 = IO_FLOAT();
    const ALfloat value3 = IO_FLOAT();
    if (dump_calls) { printf("(%u, %s, %p, %p, %p) => { %f, %f, %f }\n", (uint) name, alenumString(param), origvalue1, origvalue2, origvalue3, value1, value2, value3); }
    if (run_log) { ALfloat f1, f2, f3; REAL_alGetSource3f(get_mapped_source(name), param, &f1, &f2, &f3); }
    IO_END();
}

static void dump_alGetSourceiv(void)
{
    IO_START(alGetSourceiv);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    ALint *origvalues = (ALint *) IO_PTR();
    const uint32 numvals = IO_UINT32();
    ALint *values = (ALint *) (numvals ? get_ioblob(sizeof (ALfloat) * numvals) : NULL);
    ALboolean isenum = AL_FALSE;
    uint32 i;

    for (i = 0; i < numvals; i++) {
        values[i] = IO_INT32();
    }

    switch (param) {
        case AL_SOURCE_STATE: isenum = AL_TRUE;
        default: break;
    }

    if (dump_calls) {
        printf("(%u, %s, %p)", (uint) name, alenumString(param), origvalues);
        if (origvalues) {
            printf(" => {");
            for (i = 0; i < numvals; i++) {
                if (isenum) {
                    printf("%s %s", i > 0 ? "," : "", alenumString((ALenum) values[i]));
                } else {
                    printf("%s %d", i > 0 ? "," : "", (int) values[i]);
                }
            }
            printf("%s}", numvals > 0 ? " " : "");
        }
        printf("\n");
    }

    if (run_log) { REAL_alGetSourceiv(get_mapped_source(name), param, values); }

    IO_END();
}

static void dump_alGetSourcei(void)
{
    IO_START(alGetSourcei);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    ALint *origvalue = (ALint *) IO_PTR();
    const ALint value = IO_INT32();
    ALboolean isenum = AL_FALSE;

    switch (param) {
        case AL_SOURCE_STATE: isenum = AL_TRUE;
        default: break;
    }

    if (dump_calls) {
        if (isenum) {
            printf("(%u, %s, %p) => { %s }\n", (uint) name, alenumString(param), origvalue, alenumString((ALenum) value));
        } else {
            printf("(%u, %s, %p) => { %d }\n", (uint) name, alenumString(param), origvalue, (int) value);
        }
    }

    if (run_log) { ALint i; REAL_alGetSourcei(get_mapped_source(name), param, &i); }

    IO_END();
}

static void dump_alGetSource3i(void)
{
    IO_START(alGetSource3i);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    ALint *origvalue1 = (ALint *) IO_PTR();
    ALint *origvalue2 = (ALint *) IO_PTR();
    ALint *origvalue3 = (ALint *) IO_PTR();
    const ALint value1 = IO_INT32();
    const ALint value2 = IO_INT32();
    const ALint value3 = IO_INT32();
    if (dump_calls) { printf("(%u, %s, %p, %p, %p) => { %d, %d, %d }\n", (uint) name, alenumString(param), origvalue1, origvalue2, origvalue3, (int) value1, (int) value2, (int) value3); }
    if (run_log) { ALint i1, i2, i3; REAL_alGetSource3i(get_mapped_source(name), param, &i1, &i2, &i3); }
    IO_END();
}

static void dump_alSourcePlay(void)
{
    IO_START(alSourcePlay);
    const ALuint name = IO_UINT32();
    if (dump_calls) { printf("(%u)\n", (uint) name); }
    if (run_log) { REAL_alSourcePlay(get_mapped_source(name)); }
    IO_END();
}

static void dump_alSourcePlayv(void)
{
    IO_START(alSourcePlayv);
    const ALsizei n = IO_ALSIZEI();
    ALuint *names = (ALuint *) get_ioblob(sizeof (ALuint) * n);
    ALsizei i;

    for (i = 0; i < n; i++) {
        names[i] = IO_UINT32();
    }

    if (dump_calls) {
        printf("(%u, {", (uint) n);
        for (i = 0; i < n; i++) {
            printf("%s %u", i > 0 ? "," : "", (uint) names[i]);
        }
        printf("%s})\n", n > 0 ? " " : "");
    }

    if (run_log) {
        ALuint *realnames = (ALuint *) get_ioblob(sizeof (ALuint) * n);
        for (i = 0; i < n; i++) {
            realnames[i] = get_mapped_source(names[i]);
        }
        REAL_alSourcePlayv(n, realnames);
    }

    IO_END();
}

static void dump_alSourcePause(void)
{
    IO_START(alSourcePause);
    const ALuint name = IO_UINT32();
    if (dump_calls) { printf("(%u)\n", (uint) name); }
    if (run_log) { REAL_alSourcePause(get_mapped_source(name)); }
    IO_END();
}

static void dump_alSourcePausev(void)
{
    IO_START(alSourcePausev);
    const ALsizei n = IO_ALSIZEI();
    ALuint *names = (ALuint *) get_ioblob(sizeof (ALuint) * n);
    ALsizei i;

    for (i = 0; i < n; i++) {
        names[i] = IO_UINT32();
    }

    if (dump_calls) {
        printf("(%u, {", (uint) n);
        for (i = 0; i < n; i++) {
            printf("%s %u", i > 0 ? "," : "", (uint) names[i]);
        }
        printf("%s})\n", n > 0 ? " " : "");
    }

    if (run_log) {
        ALuint *realnames = (ALuint *) get_ioblob(sizeof (ALuint) * n);
        for (i = 0; i < n; i++) {
            realnames[i] = get_mapped_source(names[i]);
        }
        REAL_alSourcePausev(n, realnames);
    }

    IO_END();
}

static void dump_alSourceRewind(void)
{
    IO_START(alSourceRewind);
    const ALuint name = IO_UINT32();
    if (dump_calls) { printf("(%u)\n", (uint) name); }
    if (run_log) { REAL_alSourceRewind(get_mapped_source(name)); }
    IO_END();
}

static void dump_alSourceRewindv(void)
{
    IO_START(alSourceRewindv);
    const ALsizei n = IO_ALSIZEI();
    ALuint *names = (ALuint *) get_ioblob(sizeof (ALuint) * n);
    ALsizei i;

    for (i = 0; i < n; i++) {
        names[i] = IO_UINT32();
    }

    if (dump_calls) {
        printf("(%u, {", (uint) n);
        for (i = 0; i < n; i++) {
            printf("%s %u", i > 0 ? "," : "", (uint) names[i]);
        }
        printf("%s})\n", n > 0 ? " " : "");
    }

    if (run_log) {
        ALuint *realnames = (ALuint *) get_ioblob(sizeof (ALuint) * n);
        for (i = 0; i < n; i++) {
            realnames[i] = get_mapped_source(names[i]);
        }
        REAL_alSourceRewindv(n, realnames);
    }

    IO_END();
}

static void dump_alSourceStop(void)
{
    IO_START(alSourceStop);
    const ALuint name = IO_UINT32();
    if (dump_calls) { printf("(%u)\n", (uint) name); }
    if (run_log) { REAL_alSourceStop(get_mapped_source(name)); }
    IO_END();
}

static void dump_alSourceStopv(void)
{
    IO_START(alSourceStopv);
    const ALsizei n = IO_ALSIZEI();
    ALuint *names = (ALuint *) get_ioblob(sizeof (ALuint) * n);
    ALsizei i;

    for (i = 0; i < n; i++) {
        names[i] = IO_UINT32();
    }

    if (dump_calls) {
        printf("(%u, {", (uint) n);
        for (i = 0; i < n; i++) {
            printf("%s %u", i > 0 ? "," : "", (uint) names[i]);
        }
        printf("%s})\n", n > 0 ? " " : "");
    }

    if (run_log) {
        ALuint *realnames = (ALuint *) get_ioblob(sizeof (ALuint) * n);
        for (i = 0; i < n; i++) {
            realnames[i] = get_mapped_source(names[i]);
        }
        REAL_alSourceStopv(n, realnames);
    }

    IO_END();
}

static void dump_alSourceQueueBuffers(void)
{
    IO_START(alSourceQueueBuffers);
    const ALuint name = IO_UINT32();
    const ALsizei nb = IO_ALSIZEI();
    ALuint *names = (ALuint *) get_ioblob(sizeof (ALuint) * nb);
    ALsizei i;

    for (i = 0; i < nb; i++) {
        names[i] = IO_UINT32();
    }

    if (dump_calls) {
        printf("(%u, %u, {", (uint) name, (uint) nb);
        for (i = 0; i < nb; i++) {
            printf("%s %u", i > 0 ? "," : "", (uint) names[i]);
        }
        printf("%s})\n", nb > 0 ? " " : "");
    }

    if (run_log) {
        ALuint *realnames = (ALuint *) get_ioblob(sizeof (ALuint) * nb);
        for (i = 0; i < nb; i++) {
            realnames[i] = get_mapped_buffer(names[i]);
        }
        REAL_alSourceQueueBuffers(get_mapped_source(name), nb, realnames);
    }

    IO_END();
}

static void dump_alSourceUnqueueBuffers(void)
{
    IO_START(alSourceUnqueueBuffers);
    const ALuint name = IO_UINT32();
    const ALsizei nb = IO_ALSIZEI();
    ALuint *names = (ALuint *) get_ioblob(sizeof (ALuint) * nb);
    ALsizei i;

    for (i = 0; i < nb; i++) {
        names[i] = IO_UINT32();
    }

    if (dump_calls) {
        printf("(%u, %u, &names) => {", (uint) name, (uint) nb);
        for (i = 0; i < nb; i++) {
            printf("%s %u", i > 0 ? "," : "", (uint) names[i]);
        }
        printf("%s})\n", nb > 0 ? " " : "");
    }

    if (run_log) { REAL_alSourceUnqueueBuffers(get_mapped_source(name), nb, names); }

    IO_END();
}

static void dump_alGenBuffers(void)
{
    IO_START(alGenBuffers);
    const ALsizei n = IO_ALSIZEI();
    ALuint *names = (ALuint *) get_ioblob(sizeof (ALuint) * n);
    ALsizei i;

    for (i = 0; i < n; i++) {
        names[i] = IO_UINT32();
    }

    if (dump_calls) {
        printf("(%u) => {", (uint) n);
        for (i = 0; i < n; i++) {
            printf("%s %u", i > 0 ? "," : "", (uint) names[i]);
        }
        printf("%s}\n", n > 0 ? " " : "");
    }

    if (run_log) {
        ALuint *realnames = (ALuint *) get_ioblob(sizeof (ALuint) * n);
        memset(realnames, '\0', sizeof (ALuint) * n);
        REAL_alGenBuffers(n, realnames);
        for (i = 0; i < n; i++) {
            if (!realnames[i] && names[i]) {
                fprintf(stderr, "Uhoh, we didn't generate enough buffers!\n");
                fprintf(stderr, "This is probably going to cause playback problems.\n");
            } else {
                add_buffer_to_map(names[i], realnames[i]);
            }
        }
    }

    IO_END();
}

static void dump_alDeleteBuffers(void)
{
    IO_START(alDeleteBuffers);
    const ALsizei n = IO_ALSIZEI();
    ALuint *names = (ALuint *) get_ioblob(sizeof (ALuint) * n);
    ALsizei i;

    for (i = 0; i < n; i++) {
        names[i] = IO_UINT32();
    }

    if (dump_calls) {
        printf("(%u, {", (uint) n);
        for (i = 0; i < n; i++) {
            printf("%s %u", i > 0 ? "," : "", (uint) names[i]);
        }
        printf("%s})\n", n > 0 ? " " : "");
    }

    if (run_log) {
        ALuint *realnames = (ALuint *) get_ioblob(sizeof (ALuint) * n);
        for (i = 0; i < n; i++) {
            realnames[i] = get_mapped_buffer(names[i]);
        }
        REAL_alDeleteBuffers(n, realnames);
    }

    IO_END();
}

static void dump_alIsBuffer(void)
{
    IO_START(alIsBuffer);
    const ALuint name = IO_UINT32();
    const ALboolean retval = IO_BOOLEAN();
    if (dump_calls) { printf("(%u) => %s\n", (uint) name, alboolString(retval)); }
    if (run_log) { REAL_alIsBuffer(get_mapped_buffer(name)); }
    IO_END();
}

static void dump_alBufferData(void)
{
    IO_START(alBufferData);
    uint64 size = 0;
    const ALuint name = IO_UINT32();
    const ALenum alfmt = IO_ENUM();
    const ALsizei freq = IO_ALSIZEI();
    const ALvoid *data = (const ALvoid *) IO_BLOB(&size); (void) data;
    if (dump_calls) { printf("(%u, %s, &data, %u, %u)\n", (uint) name, alenumString(alfmt), (uint) size, (uint) freq); }
    if (run_log) { REAL_alBufferData(get_mapped_buffer(name), alfmt, data, (ALsizei) size, freq); }
    IO_END();
}

static void dump_alBufferfv(void)
{
    IO_START(alBufferfv);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const uint32 numvals = IO_UINT32();
    ALfloat *values = (ALfloat *) get_ioblob(sizeof (ALfloat) * numvals);
    uint32 i;

    for (i = 0; i < numvals; i++) {
        values[i] = IO_INT32();
    }

    if (dump_calls) {
        printf("(%u, %s, {", (uint) name, alenumString(param));
        for (i = 0; i < numvals; i++) {
            printf("%s %d", i > 0 ? "," : "", (int) values[i]);
        }
        printf("%s})\n", numvals > 0 ? " " : "");
    }

    if (run_log) { REAL_alBufferfv(get_mapped_buffer(name), param, values); }

    IO_END();
}

static void dump_alBufferf(void)
{
    IO_START(alBufferf);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALfloat value = IO_FLOAT();
    if (dump_calls) { printf("(%u, %s, %f)\n", (uint) name, alenumString(param), value); }
    if (run_log) { REAL_alBufferf(get_mapped_buffer(name), param, value); }
    IO_END();
}

static void dump_alBuffer3f(void)
{
    IO_START(alBuffer3f);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALfloat value1 = IO_FLOAT();
    const ALfloat value2 = IO_FLOAT();
    const ALfloat value3 = IO_FLOAT();
    if (dump_calls) { printf("(%u, %s, %f, %f, %f)\n", (uint) name, alenumString(param), value1, value2, value3); }
    if (run_log) { REAL_alBuffer3f(get_mapped_buffer(name), param, value1, value2, value3); }
    IO_END();
}

static void dump_alBufferiv(void)
{
    IO_START(alBufferiv);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const uint32 numvals = IO_UINT32();
    ALint *values = (ALint *) get_ioblob(sizeof (ALint) * numvals);
    uint32 i;

    for (i = 0; i < numvals; i++) {
        values[i] = IO_INT32();
    }

    if (dump_calls) {
        printf("(%u, %s, {", (uint) name, alenumString(param));
        for (i = 0; i < numvals; i++) {
            printf("%s %d", i > 0 ? "," : "", (int) values[i]);
        }
        printf("%s})\n", numvals > 0 ? " " : "");
    }

    if (run_log) { REAL_alBufferiv(get_mapped_buffer(name), param, values); }

    IO_END();
}

static void dump_alBufferi(void)
{
    IO_START(alBufferi);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALint value = IO_INT32();
    if (dump_calls) { printf("(%u, %s, %d)\n", (uint) name, alenumString(param), (int) value); }
    if (run_log) { REAL_alBufferi(get_mapped_buffer(name), param, value); }
    IO_END();
}

static void dump_alBuffer3i(void)
{
    IO_START(alBuffer3i);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALint value1 = IO_INT32();
    const ALint value2 = IO_INT32();
    const ALint value3 = IO_INT32();
    if (dump_calls) { printf("(%u, %s, %d, %d, %d)\n", (uint) name, alenumString(param), (int) value1, (int) value2, (int) value3); }
    if (run_log) { REAL_alBuffer3i(get_mapped_buffer(name), param, value1, value2, value3); }
    IO_END();
}

static void dump_alGetBufferfv(void)
{
    IO_START(alGetBufferfv);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    ALfloat *origvalues = (ALfloat *) IO_PTR();
    const uint32 numvals = IO_UINT32();
    ALfloat *values = (ALfloat *) (numvals ? get_ioblob(sizeof (ALfloat) * numvals) : NULL);
    uint32 i;

    for (i = 0; i < numvals; i++) {
        values[i] = IO_FLOAT();
    }

    if (dump_calls) {
        printf("(%u, %s, %p)", (uint) name, alenumString(param), origvalues);
        if (origvalues) {
            printf(" => {");
            for (i = 0; i < numvals; i++) {
                printf("%s %f", i > 0 ? "," : "", values[i]);
            }
            printf("%s}", numvals > 0 ? " " : "");
        }
        printf("\n");
    }

    if (run_log) { REAL_alGetBufferfv(get_mapped_buffer(name), param, values); }

    IO_END();
}

static void dump_alGetBufferf(void)
{
    IO_START(alGetBufferf);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    ALfloat *origvalue = (ALfloat *) IO_PTR();
    const ALfloat value = IO_FLOAT();
    if (dump_calls) { printf("(%u, %s, %p) => { %f }\n", (uint) name, alenumString(param), origvalue, value); }
    if (run_log) { ALfloat f; REAL_alGetBufferf(get_mapped_buffer(name), param, &f); }
    IO_END();
}

static void dump_alGetBuffer3f(void)
{
    IO_START(alGetBuffer3f);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    ALfloat *origvalue1 = (ALfloat *) IO_PTR();
    ALfloat *origvalue2 = (ALfloat *) IO_PTR();
    ALfloat *origvalue3 = (ALfloat *) IO_PTR();
    const ALfloat value1 = IO_FLOAT();
    const ALfloat value2 = IO_FLOAT();
    const ALfloat value3 = IO_FLOAT();
    if (dump_calls) { printf("(%u, %s, %p, %p, %p) => { %f, %f, %f }\n", (uint) name, alenumString(param), origvalue1, origvalue2, origvalue3, value1, value2, value3); }
    if (run_log) { ALfloat f1, f2, f3; REAL_alGetBuffer3f(get_mapped_buffer(name), param, &f1, &f2, &f3); }
    IO_END();
}

static void dump_alGetBufferi(void)
{
    IO_START(alGetBufferi);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    ALint *origvalue = (ALint *) IO_PTR();
    const ALint value = IO_INT32();
    if (dump_calls) { printf("(%u, %s, %p) => { %d }\n", (uint) name, alenumString(param), origvalue, (int) value); }
    if (run_log) { ALint i; REAL_alGetBufferi(get_mapped_buffer(name), param, &i); }
    IO_END();
}

static void dump_alGetBuffer3i(void)
{
    IO_START(alGetBuffer3i);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    ALint *origvalue1 = (ALint *) IO_PTR();
    ALint *origvalue2 = (ALint *) IO_PTR();
    ALint *origvalue3 = (ALint *) IO_PTR();
    const ALint value1 = IO_INT32();
    const ALint value2 = IO_INT32();
    const ALint value3 = IO_INT32();
    if (dump_calls) { printf("(%u, %s, %p, %p, %p) => { %d, %d, %d }\n", (uint) name, alenumString(param), origvalue1, origvalue2, origvalue3, (int) value1, (int) value2, (int) value3); }
    if (run_log) { ALint i1, i2, i3; REAL_alGetBuffer3i(get_mapped_buffer(name), param, &i1, &i2, &i3); }
    IO_END();
}

static void dump_alGetBufferiv(void)
{
    IO_START(alGetBufferiv);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    ALint *origvalues = (ALint *) IO_PTR();
    const uint32 numvals = IO_UINT32();
    ALint *values = (ALint *) (numvals ? get_ioblob(sizeof (ALfloat) * numvals) : NULL);
    uint32 i;

    for (i = 0; i < numvals; i++) {
        values[i] = IO_INT32();
    }

    if (dump_calls) {
        printf("(%u, %s, %p)", (uint) name, alenumString(param), origvalues);
        if (origvalues) {
            printf(" => {");
            for (i = 0; i < numvals; i++) {
                printf("%s %d", i > 0 ? "," : "", (int) values[i]);
            }
            printf("%s}", numvals > 0 ? " " : "");
        }
        printf("\n");
    }

    if (run_log) { REAL_alGetBufferiv(get_mapped_buffer(name), param, values); }

    IO_END();
}

static void dump_alTracePushScope(void)
{
    IO_START(alTracePushScope);
    const ALchar *str = IO_STRING();
    if (dump_calls) { printf("(%s)\n", litString(str)); }
    trace_scope++;
    if (run_log) { if (REAL_alTracePushScope) REAL_alTracePushScope(str); }
    IO_END();
}

static void dump_alTracePopScope(void)
{
    trace_scope--;
    IO_START(alTracePopScope);
    if (run_log) { if (REAL_alTracePopScope) REAL_alTracePopScope(); }
    IO_END();
}

static void dump_alTraceMessage(void)
{
    IO_START(alTraceMessage);
    const ALchar *str = IO_STRING();
    if (dump_calls) { printf("(%s)\n", litString(str)); }
    if (run_log) { if (REAL_alTraceMessage) REAL_alTraceMessage(str); }
    IO_END();
}

static void dump_alTraceBufferLabel(void)
{
    IO_START(alTraceBufferLabel);
    const ALuint name = IO_UINT32();
    const ALchar *str = IO_STRING();
    if (dump_calls) { printf("(%u, %s)\n", (uint) name, litString(str)); }
    if (run_log) { if (REAL_alTraceBufferLabel) REAL_alTraceBufferLabel(get_mapped_buffer(name), str); }
    IO_END();
}

static void dump_alTraceSourceLabel(void)
{
    IO_START(alTraceSourceLabel);
    const ALuint name = IO_UINT32();
    const ALchar *str = IO_STRING();
    if (dump_calls) { printf("(%u, %s)\n", (uint) name, litString(str)); }
    if (run_log) { if (REAL_alTraceSourceLabel) REAL_alTraceSourceLabel(get_mapped_source(name), str); }
    IO_END();
}

static void dump_alcTraceDeviceLabel(void)
{
    IO_START(alcTraceDeviceLabel);
    ALCdevice *device = (ALCdevice *) IO_PTR();
    const ALchar *str = IO_STRING();
    if (dump_calls) { printf("(%p, %s)\n", device, litString(str)); }
    if (run_log) { if (REAL_alcTraceDeviceLabel) REAL_alcTraceDeviceLabel(get_mapped_device(device), str); }
    IO_END();
}

static void dump_alcTraceContextLabel(void)
{
    IO_START(alcTraceContextLabel);
    ALCcontext *ctx = (ALCcontext *) IO_PTR();
    const ALchar *str = IO_STRING();
    if (dump_calls) { printf("(%p, %s)\n", ctx, litString(str)); }
    if (run_log) { if (REAL_alcTraceContextLabel) REAL_alcTraceContextLabel(get_mapped_context(ctx), str); }
    IO_END();
}



static void dump_al_error_event(void)
{
    const ALenum err = IO_ENUM();
    if (dump_errors) {
        printf("<<< AL ERROR SET HERE: %s >>>\n", alenumString(err));
    }
}

static void dump_alc_error_event(void)
{
    ALCdevice *device = (ALCdevice *) IO_PTR();
    const ALCenum err = IO_ALCENUM();
    if (dump_errors) {
        printf("<<< ALC ERROR SET HERE: device=%p %s >>>\n", device, alcenumString(err));
    }
}

static void dump_callstack_syms_event(void)
{
    const uint32 num_new_strings = IO_UINT32();
    uint32 i;
    for (i = 0; i < num_new_strings; i++) {
        void *ptr = IO_PTR();
        const char *str = IO_STRING();
        if (str) {
            char *dup = strdup(str);
            if (ptr && dup) {
                add_stackframe_to_map(ptr, dup);
            }
        }
    }
}

static void dump_context_state_changed_enum(void)
{
    void *ctx = IO_PTR();
    const ALenum param = IO_ENUM();
    const ALenum newval = IO_ENUM();
    if (dump_state_changes) {
        printf("<<< CONTEXT STATE CHANGE: ctx=%p param=%s value=%s >>>\n", ctx, alenumString(param), alenumString(newval));
    }
}

static void dump_context_state_changed_float(void)
{
    void *ctx = IO_PTR();
    const ALenum param = IO_ENUM();
    const ALfloat newval = IO_FLOAT();
    if (dump_state_changes) {
        printf("<<< CONTEXT STATE CHANGE: ctx=%p param=%s value=%f >>>\n", ctx, alenumString(param), newval);
    }
}

static void dump_listener_state_changed_floatv(void)
{
    void *ctx = IO_PTR();
    const ALenum param = IO_ENUM();
    const int32 numfloats = IO_INT32();
    int32 i;

    if (dump_state_changes) {
        printf("<<< LISTENER STATE CHANGE: ctx=%p param=%s values={", ctx, alenumString(param));
    }

    for (i = 0; i < numfloats; i++) {
        const ALfloat newval = IO_FLOAT();
        if (dump_state_changes) {
            printf("%s %f", i > 0 ? "," : "", newval);
        }
    }

    if (dump_state_changes) {
        printf("%s} >>>\n", numfloats > 0 ? " " : "");
    }
}

static void dump_source_state_changed_bool(void)
{
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALboolean newval = IO_BOOLEAN();
    if (dump_state_changes) {
        printf("<<< SOURCE STATE CHANGE: name=%u param=%s value=%s >>>\n", (uint) name, alenumString(param), alboolString(newval));
    }
}

static void dump_source_state_changed_enum(void)
{
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALenum newval = IO_ENUM();
    if (dump_state_changes) {
        printf("<<< SOURCE STATE CHANGE: name=%u param=%s value=%s >>>\n", (uint) name, alenumString(param), alenumString(newval));
    }
}

static void dump_source_state_changed_int(void)
{
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALint newval = IO_INT32();
    if (dump_state_changes) {
        printf("<<< SOURCE STATE CHANGE: name=%u param=%s value=%d >>>\n", (uint) name, alenumString(param), (int) newval);
    }
}

static void dump_source_state_changed_uint(void)
{
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALuint newval = IO_UINT32();
    if (dump_state_changes) {
        printf("<<< SOURCE STATE CHANGE: name=%u param=%s value=%u >>>\n", (uint) name, alenumString(param), (uint) newval);
    }
}

static void dump_source_state_changed_float(void)
{
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALfloat newval = IO_FLOAT();
    if (dump_state_changes) {
        printf("<<< SOURCE STATE CHANGE: name=%u param=%s value=%f >>>\n", (uint) name, alenumString(param), newval);
    }
}

static void dump_source_state_changed_float3(void)
{
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALfloat newval1 = IO_FLOAT();
    const ALfloat newval2 = IO_FLOAT();
    const ALfloat newval3 = IO_FLOAT();
    if (dump_state_changes) {
        printf("<<< SOURCE STATE CHANGE: name=%u param=%s value={ %f, %f, %f } >>>\n", (uint) name, alenumString(param), newval1, newval2, newval3);
    }
}

static void dump_buffer_state_changed_int(void)
{
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALint newval = IO_INT32();
    if (dump_state_changes) {
        printf("<<< BUFFER STATE CHANGE: name=%u param=%s value=%d >>>\n", (uint) name, alenumString(param), (int) newval);
    }
}


static void process_log(void)
{
    int eos = 0;
    while (!eos) {
        const uint32 wait_until = IO_UINT32();
        if (run_log) {
            while (NOW() < wait_until) {
                usleep(1000);  /* keep the pace of the original run */
            }
        }

        switch (IO_EVENTENUM()) {
            #define ENTRYPOINT(ret,name,params,args) case ALEE_##name: dump_##name(); break;
            #include "altrace_entrypoints.h"

            case ALEE_NEW_CALLSTACK_SYMS:
                dump_callstack_syms_event();
                break;

            case ALEE_ALERROR_TRIGGERED:
                dump_al_error_event();
                break;

            case ALEE_ALCERROR_TRIGGERED:
                dump_alc_error_event();
                break;

            case ALEE_CONTEXT_STATE_CHANGED_ENUM:
                dump_context_state_changed_enum();
                break;

            case ALEE_CONTEXT_STATE_CHANGED_FLOAT:
                dump_context_state_changed_float();
                break;

            case ALEE_LISTENER_STATE_CHANGED_FLOATV:
                dump_listener_state_changed_floatv();
                break;

            case ALEE_SOURCE_STATE_CHANGED_BOOL:
                dump_source_state_changed_bool();
                break;

            case ALEE_SOURCE_STATE_CHANGED_ENUM:
                dump_source_state_changed_enum();
                break;

            case ALEE_SOURCE_STATE_CHANGED_INT:
                dump_source_state_changed_int();
                break;

            case ALEE_SOURCE_STATE_CHANGED_UINT:
                dump_source_state_changed_uint();
                break;

            case ALEE_SOURCE_STATE_CHANGED_FLOAT:
                dump_source_state_changed_float();
                break;

            case ALEE_SOURCE_STATE_CHANGED_FLOAT3:
                dump_source_state_changed_float3();
                break;

            case ALEE_BUFFER_STATE_CHANGED_INT:
                dump_buffer_state_changed_int();
                break;

            case ALEE_EOS:
                if (dump_calls) { printf("\n<<< END OF LOG FILE >>>\n"); }
                eos = 1;
                break;

            default:
                printf("\n<<< UNEXPECTED LOG ENTRY. BUG? NEW LOG VERSION? CORRUPT FILE? >>>\n");
                eos = 1;
                break;
        }
    }
}

int main(int argc, char **argv)
{
    const char *fname = NULL;
    int usage = 0;
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--dump-calls") == 0) {
            dump_calls = 1;
        } else if (strcmp(arg, "--no-dump-calls") == 0) {
            dump_calls = 0;
        } else if (strcmp(arg, "--dump-callers") == 0) {
            dump_callers = 1;
        } else if (strcmp(arg, "--no-dump-callers") == 0) {
            dump_callers = 0;
        } else if (strcmp(arg, "--dump-errors") == 0) {
            dump_errors = 1;
        } else if (strcmp(arg, "--no-dump-errors") == 0) {
            dump_errors = 0;
        } else if (strcmp(arg, "--dump-state-changes") == 0) {
            dump_state_changes = 1;
        } else if (strcmp(arg, "--no-dump-state-changes") == 0) {
            dump_state_changes = 0;
        } else if (strcmp(arg, "--dump-all") == 0) {
            dump_calls = dump_callers = dump_errors = dump_state_changes = 1;
        } else if (strcmp(arg, "--no-dump-all") == 0) {
            dump_calls = dump_callers = dump_errors = dump_state_changes = 0;
        } else if (strcmp(arg, "--run") == 0) {
            run_log = 1;
        } else if (strcmp(arg, "--no-run") == 0) {
            run_log = 0;
        } else if (strcmp(arg, "--help") == 0) {
            usage = 1;
        } else if (fname == NULL) {
            fname = arg;
        } else {
            usage = 1;
        }
    }

    if (fname == NULL) {
        usage = 1;
    }

    if (usage) {
        fprintf(stderr, "USAGE: %s [args] <altrace.trace>\n", argv[0]);
        fprintf(stderr, "  args:\n");
        fprintf(stderr, "   --[no-]dump-calls\n");
        fprintf(stderr, "   --[no-]dump-callers\n");
        fprintf(stderr, "   --[no-]dump-errors\n");
        fprintf(stderr, "   --[no-]dump-state-changes\n");
        fprintf(stderr, "   --[no-]dump-all\n");
        fprintf(stderr, "   --[no-]run\n");
        fprintf(stderr, "\n");
        return 1;
    }

    dumping = dump_calls || dump_callers || dump_errors || dump_state_changes;

    init_altrace_playback(fname);
    process_log();
    quit_altrace_playback();
    return 0;
}

// end of altrace_playback.c ...

