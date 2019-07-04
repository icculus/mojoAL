/**
 * MojoAL; a simple drop-in OpenAL implementation.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

#define APPNAME "altrace_record"

#ifdef _MSC_VER
  #define AL_API __declspec(dllexport)
  #define ALC_API __declspec(dllexport)
#endif

#include "altrace_common.h"

static pthread_mutex_t _apilock;
static pthread_mutex_t *apilock;
//static ALCenum alcerrorlatch = AL_NO_ERROR;
//static ALenum alerrorlatch = AL_NO_ERROR;


static void quit_altrace_record(void) __attribute__((destructor));

NORETURN static void IO_WRITE_FAIL(void)
{
    fprintf(stderr, APPNAME ": failed to write to log: %s\n", strerror(errno));
    quit_altrace_record();
    _exit(42);
}

static void writele32(const uint32 x)
{
    const uint32 y = swap32(x);
    if (write(logfd, &y, sizeof (y)) != sizeof (y)) {
        IO_WRITE_FAIL();
    }
}

static void writele64(const uint64 x)
{
    const uint64 y = swap64(x);
    if (write(logfd, &y, sizeof (y)) != sizeof (y)) {
        IO_WRITE_FAIL();
    }
}

static void IO_INT32(const int32 x)
{
    union { int32 si32; uint32 ui32; } cvt;
    cvt.si32 = x;
    writele32(cvt.ui32);
}

static void IO_UINT32(const uint32 x)
{
    writele32(x);
}

static void IO_UINT64(const uint64 x)
{
    writele64(x);
}

static void IO_ALCSIZEI(const ALCsizei x)
{
    IO_UINT64((uint64) x);
}

static void IO_ALSIZEI(const ALsizei x)
{
    IO_UINT64((uint64) x);
}

static void IO_FLOAT(const float x)
{
    union { float f; uint32 ui32; } cvt;
    cvt.f = x;
    IO_UINT32(cvt.ui32);
}

static void IO_DOUBLE(const double x)
{
    union { double d; uint64 ui64; } cvt;
    cvt.d = x;
    IO_UINT64(cvt.ui64);
}

static void IO_STRING(const char *str)
{
    if (!str) {
        IO_UINT64(0xFFFFFFFFFFFFFFFFull);
    } else {
        const size_t len = strlen(str);
        IO_UINT64((uint64) len);
        if (write(logfd, str, len) != len) IO_WRITE_FAIL();
    }
}

static void IO_BLOB(const uint8 *data, const uint64 len)
{
    if (!data) {
        IO_UINT64(0xFFFFFFFFFFFFFFFFull);
    } else {
        const size_t slen = (size_t) len;
        IO_UINT64(len);
        if (write(logfd, data, slen) != slen) IO_WRITE_FAIL();
    }
}

static void IO_ENTRYENUM(const EntryEnum x)
{
    IO_UINT32((uint32) x);
}

static void IO_PTR(void *ptr)
{
    IO_UINT64((uint64) (size_t) ptr);
}

static void IO_ALCENUM(ALCenum e)
{
    IO_UINT32((uint32) e);
}

static void IO_ENUM(ALenum e)
{
    IO_UINT32((uint32) e);
}

static void IO_ALCBOOLEAN(ALCboolean b)
{
    IO_UINT32((uint32) b);
}

static void IO_BOOLEAN(ALboolean b)
{
    IO_UINT32((uint32) b);
}

static void APILOCK(void)
{
    const int rc = pthread_mutex_lock(apilock);
    if (rc != 0) {
        fprintf(stderr, APPNAME ": Failed to grab API lock: %s\n", strerror(rc));
        quit_altrace_record();
        _exit(42);
    }
}

static void APIUNLOCK(void)
{
    const int rc = pthread_mutex_unlock(apilock);
    if (rc != 0) {
        fprintf(stderr, APPNAME ": Failed to release API lock: %s\n", strerror(rc));
        quit_altrace_record();
        _exit(42);
    }
}

#if 0  // !!! FIXME: add this into IO_END, but we need to deal with the latch.
static void check_error_events(void)
{
    const ALenum alerr = REAL_alGetError();
    const ALCenum alcerr = REAL_alcGetError();
    if (alerr != AL_NO_ERROR) {
        IO_UINT32(NOW());
        IO_ENTRYENUM(ALEE_ALERROR_EVENT);
        IO_ENUM(alerr);
        alerrorlatch = alerr;
    }

    if (alcerr != ALC_NO_ERROR) {
        IO_UINT32(NOW());
        IO_ENTRYENUM(ALEE_ALCERROR_EVENT);
        IO_ALCENUM(alcerr);
        alcerrorlatch = alcerr;
    }
}
#endif

#define IO_START(e) \
    { \
        APILOCK(); \
        IO_UINT32(NOW()); \
        IO_ENTRYENUM(ALEE_##e);

#define IO_END() \
        /*check_error_events();*/ \
        APIUNLOCK(); \
    }

static void init_altrace_record(void) __attribute__((constructor));
static void init_altrace_record(void)
{
    int okay = 1;

    fprintf(stderr, "\n\n\n" APPNAME ": starting up...\n");
    fflush(stderr);

    if (!init_clock()) {
        fflush(stderr);
        _exit(42);
    }

    if (!load_real_openal()) {
        _exit(42);
    }

    if (okay) {
        const int rc = pthread_mutex_init(&_apilock, NULL);
        if (rc != 0) {
            fprintf(stderr, APPNAME ": Failed to create mutex: %s\n", strerror(rc));
            okay = 0;
        }
        apilock = &_apilock;
    }

    if (okay) {
        const char *filename = "altrace.trace";
        logfd = open(filename, O_WRONLY | O_TRUNC | O_CREAT, 0644);
        if (logfd == -1) {
            fprintf(stderr, APPNAME ": Failed to open OpenAL log file '%s': %s\n", filename, strerror(errno));
            okay = 0;
        } else {
            fprintf(stderr, APPNAME ": Recording OpenAL session to log file '%s'\n\n\n", filename);
        }
    }

    fflush(stderr);

    if (!okay) {
        quit_altrace_record();
        _exit(42);
    }

    IO_UINT32(ALTRACE_LOG_FILE_MAGIC);
    IO_UINT32(ALTRACE_LOG_FILE_FORMAT);
}

static void quit_altrace_record(void)
{
    const int io = logfd;
    pthread_mutex_t *mutex = apilock;

    logfd = -1;
    apilock = NULL;

    fprintf(stderr, APPNAME ": Shutting down...\n");
    fflush(stderr);

    if (io != -1) {
        const uint32 ticks = swap32(NOW());
        const uint32 eos = swap32((uint32) ALEE_EOS);
        if ((write(io, &ticks, 4) != 4) || (write(io, &eos, 4) != 4)) {
            fprintf(stderr, APPNAME ": Failed to write EOS to OpenAL log file: %s\n", strerror(errno));
        }
        if (close(io) < 0) {
            fprintf(stderr, APPNAME ": Failed to close OpenAL log file: %s\n", strerror(errno));
        }
    }

    if (mutex) {
        pthread_mutex_destroy(mutex);
    }

    #define ENTRYPOINT(ret,name,params,args) REAL_##name = NULL;
    #include "altrace_entrypoints.h"

    close_real_openal();

    fflush(stderr);
}



ALCcontext *alcGetCurrentContext(void)
{
    ALCcontext *retval;
    IO_START(alcGetCurrentContext);
    retval = REAL_alcGetCurrentContext();
    IO_PTR(retval);
    IO_END();
    return retval;
}

ALCdevice *alcGetContextsDevice(ALCcontext *context)
{
    ALCdevice *retval;
    IO_START(alcGetContextsDevice);
    IO_PTR(context);
    retval = REAL_alcGetContextsDevice(context);
    IO_PTR(retval);
    IO_END();
    return retval;
}

ALCboolean alcIsExtensionPresent(ALCdevice *device, const ALCchar *extname)
{
    ALCboolean retval;
    IO_START(alcIsExtensionPresent);
    IO_PTR(device);
    IO_STRING(extname);
    retval = REAL_alcIsExtensionPresent(device, extname);
    IO_ALCBOOLEAN(retval);
    IO_END();
    return retval;
}

void *alcGetProcAddress(ALCdevice *device, const ALCchar *funcname)
{
    void *retval = NULL;
    IO_START(alcGetProcAddress);
    IO_PTR(device);
    IO_STRING(funcname);

    // always return our entry points, so the app always calls through here.
    if (!funcname || ((funcname[0] != 'a') || (funcname[1] != 'l') || (funcname[2] != 'c'))) {
        // !!! FIXME: should set an error state.
        retval = NULL;
    }
    #define ENTRYPOINT(ret,name,params,args) else if (strcmp(funcname, #name) == 0) { retval = (void *) name; }
    #include "altrace_entrypoints.h"

    IO_PTR(retval);
    IO_END();
    return retval;

}

ALCenum alcGetEnumValue(ALCdevice *device, const ALCchar *enumname)
{
    ALCenum retval;
    IO_START(alcGetEnumValue);
    IO_PTR(device);
    IO_STRING(enumname);
    retval = REAL_alcGetEnumValue(device, enumname);
    IO_ALCENUM(retval);
    IO_END();
    return retval;
}

const ALCchar *alcGetString(ALCdevice *device, ALCenum param)
{
    const ALCchar *retval;
    IO_START(alcGetString);
    IO_PTR(device);
    IO_ALCENUM(param);
    retval = REAL_alcGetString(device, param);
    IO_STRING(retval);
    IO_END();
    return retval;
}

ALCdevice *alcCaptureOpenDevice(const ALCchar *devicename, ALCuint frequency, ALCenum format, ALCsizei buffersize)
{
    ALCdevice *retval;
    IO_START(alcCaptureOpenDevice);
    IO_STRING(devicename);
    IO_UINT32(frequency);
    IO_ALCENUM(format);
    IO_ALSIZEI(buffersize);
    retval = REAL_alcCaptureOpenDevice(devicename, frequency, format, buffersize);
    IO_PTR(retval);
    IO_END();
    return retval;
}

ALCboolean alcCaptureCloseDevice(ALCdevice *device)
{
    ALCboolean retval;
    IO_START(alcCaptureCloseDevice);
    IO_PTR(device);
    retval = REAL_alcCaptureCloseDevice(device);
    IO_ALCBOOLEAN(retval);
    IO_END();
    return retval;
}

ALCdevice *alcOpenDevice(const ALCchar *devicename)
{
    ALCdevice *retval;
    IO_START(alcOpenDevice);
    IO_STRING(devicename);
    retval = REAL_alcOpenDevice(devicename);
    IO_PTR(retval);
    IO_END();
    return retval;
}

ALCboolean alcCloseDevice(ALCdevice *device)
{
    ALCboolean retval;
    IO_START(alcCloseDevice);
    IO_PTR(device);
    retval = REAL_alcCloseDevice(device);
    IO_ALCBOOLEAN(retval);
    IO_END();
    return retval;
}

ALCcontext *alcCreateContext(ALCdevice *device, const ALCint* attrlist)
{
    ALCcontext *retval;
    uint32 attrcount = 0;
    uint32 i;
    IO_START(alcCreateContext);
    IO_PTR(device);
    if (attrlist) {
        while (attrlist[attrcount] != 0) { attrcount += 2; }
        attrcount++;
    }
    IO_UINT32(attrcount);
    if (attrlist) {
        for (i = 0; i < attrcount; i++) {
            IO_INT32(attrlist[i]);
        }
    }
    retval = REAL_alcCreateContext(device, attrlist);
    IO_PTR(retval);
    IO_END();
    return retval;
    
}

ALCboolean alcMakeContextCurrent(ALCcontext *ctx)
{
    ALCboolean retval;
    IO_START(alcMakeContextCurrent);
    IO_PTR(ctx);
    retval = REAL_alcMakeContextCurrent(ctx);
    IO_ALCBOOLEAN(retval);
    IO_END();
    return retval;
}

void alcProcessContext(ALCcontext *ctx)
{
    IO_START(alcProcessContext);
    IO_PTR(ctx);
    REAL_alcProcessContext(ctx);
    IO_END();
}

void alcSuspendContext(ALCcontext *ctx)
{
    IO_START(alcSuspendContext);
    IO_PTR(ctx);
    REAL_alcSuspendContext(ctx);
    IO_END();
}

void alcDestroyContext(ALCcontext *ctx)
{
    IO_START(alcDestroyContext);
    IO_PTR(ctx);
    REAL_alcDestroyContext(ctx);
    IO_END();
}

ALCenum alcGetError(ALCdevice *device)
{
    ALCenum retval;
    IO_START(alcGetError);
    IO_PTR(device);
    retval = REAL_alcGetError(device);
    IO_ALCENUM(retval);
    IO_END();
    return retval;
}

void alcGetIntegerv(ALCdevice *device, ALCenum param, ALCsizei size, ALCint *values)
{
    IO_START(alcGetIntegerv);
    IO_PTR(device);
    IO_ALCENUM(param);
    IO_ALCSIZEI(size);
    REAL_alcGetIntegerv(device, param, size, values);
    IO_END();
}

void alcCaptureStart(ALCdevice *device)
{
    IO_START(alcCaptureStart);
    IO_PTR(device);
    REAL_alcCaptureStart(device);
    IO_END();
}

void alcCaptureStop(ALCdevice *device)
{
    IO_START(alcCaptureStop);
    IO_PTR(device);
    REAL_alcCaptureStop(device);
    IO_END();
}

void alcCaptureSamples(ALCdevice *device, ALCvoid *buffer, ALCsizei samples)
{
    IO_START(alcCaptureSamples);
    IO_PTR(device);
    IO_ALCSIZEI(samples);
    // !!! FIXME: IO_BLOB the results.
    REAL_alcCaptureSamples(device, buffer, samples);
    IO_END();
}

void alDopplerFactor(ALfloat value)
{
    IO_START(alDopplerFactor);
    IO_FLOAT(value);
    REAL_alDopplerFactor(value);
    IO_END();
}

void alDopplerVelocity(ALfloat value)
{
    IO_START(alDopplerVelocity);
    IO_FLOAT(value);
    REAL_alDopplerVelocity(value);
    IO_END();
}

void alSpeedOfSound(ALfloat value)
{
    IO_START(alSpeedOfSound);
    IO_FLOAT(value);
    REAL_alSpeedOfSound(value);
    IO_END();
}

void alDistanceModel(ALenum model)
{
    IO_START(alDistanceModel);
    IO_ENUM(model);
    REAL_alDistanceModel(model);
    IO_END();
}

void alEnable(ALenum capability)
{
    IO_START(alEnable);
    IO_ENUM(capability);
    REAL_alEnable(capability);
    IO_END();
}

void alDisable(ALenum capability)
{
    IO_START(alDisable);
    IO_ENUM(capability);
    REAL_alDisable(capability);
    IO_END();
}

ALboolean alIsEnabled(ALenum capability)
{
    ALboolean retval;
    IO_START(alIsEnabled);
    IO_ENUM(capability);
    retval = REAL_alIsEnabled(capability);
    IO_BOOLEAN(retval);
    IO_END();
    return retval;
}

const ALchar *alGetString(const ALenum param)
{
    const ALchar *retval;
    IO_START(alGetString);
    IO_ENUM(param);
    retval = REAL_alGetString(param);
    IO_STRING(retval);
    IO_END();
    return retval;
}

void alGetBooleanv(ALenum param, ALboolean *values)
{
    IO_START(alGetBooleanv);
    IO_ENUM(param);
    REAL_alGetBooleanv(param, values);
    IO_END();
}

void alGetIntegerv(ALenum param, ALint *values)
{
    IO_START(alGetIntegerv);
    IO_ENUM(param);
    REAL_alGetIntegerv(param, values);
    IO_END();
}

void alGetFloatv(ALenum param, ALfloat *values)
{
    IO_START(alGetFloatv);
    IO_ENUM(param);
    REAL_alGetFloatv(param, values);
    IO_END();
}

void alGetDoublev(ALenum param, ALdouble *values)
{
    IO_START(alGetDoublev);
    IO_ENUM(param);
    REAL_alGetDoublev(param, values);
    IO_END();
}

ALboolean alGetBoolean(ALenum param)
{
    ALboolean retval;
    IO_START(alGetBoolean);
    IO_ENUM(param);
    retval = REAL_alGetBoolean(param);
    IO_BOOLEAN(retval);
    IO_END();
    return retval;
}

ALint alGetInteger(ALenum param)
{
    ALint retval;
    IO_START(alGetInteger);
    IO_ENUM(param);
    retval = REAL_alGetInteger(param);
    IO_INT32(retval);
    IO_END();
    return retval;
}

ALfloat alGetFloat(ALenum param)
{
    ALfloat retval;
    IO_START(alGetFloat);
    IO_ENUM(param);
    retval = REAL_alGetFloat(param);
    IO_FLOAT(retval);
    IO_END();
    return retval;
}

ALdouble alGetDouble(ALenum param)
{
    ALdouble retval;
    IO_START(alGetDouble);
    IO_ENUM(param);
    retval = REAL_alGetDouble(param);
    IO_DOUBLE(retval);
    IO_END();
    return retval;
}

ALboolean alIsExtensionPresent(const ALchar *extname)
{
    ALboolean retval;
    IO_START(alIsExtensionPresent);
    IO_STRING(extname);
    retval = REAL_alIsExtensionPresent(extname);
    IO_BOOLEAN(retval);
    IO_END();
    return retval;
}

ALenum alGetError(void)
{
    ALenum retval;
    IO_START(alGetError);
    retval = REAL_alGetError();
    IO_ENUM(retval);
    IO_END();
    return retval;
}

void *alGetProcAddress(const ALchar *funcname)
{
    void *retval = NULL;
    IO_START(alGetProcAddress);
    IO_STRING(funcname);

    // always return our entry points, so the app always calls through here.
    if (!funcname || ((funcname[0] != 'a') || (funcname[1] != 'l') || (funcname[2] == 'c'))) {
        // !!! FIXME: should set an error state.
        retval = NULL;
    }
    #define ENTRYPOINT(ret,name,params,args) else if (strcmp(funcname, #name) == 0) { retval = (void *) name; }
    #include "altrace_entrypoints.h"

    IO_PTR(retval);
    IO_END();
    return retval;
}

ALenum alGetEnumValue(const ALchar *enumname)
{
    ALenum retval;
    IO_START(alGetEnumValue);
    IO_STRING(enumname);
    retval = REAL_alGetEnumValue(enumname);
    IO_ENUM(retval);
    IO_END();
    return retval;
}

void alListenerfv(ALenum param, const ALfloat *values)
{
    uint32 numvals = 1;
    uint32 i;
    IO_START(alListenerfv);
    IO_ENUM(param);
    switch(param) {
        case AL_GAIN: break;
        case AL_POSITION: numvals = 3; break;
        case AL_VELOCITY: numvals = 3; break;
        case AL_ORIENTATION: numvals = 6; break;
        default: break;   /* uhoh. */
    }

    IO_UINT32(numvals);
    for (i = 0; i < numvals; i++) {
        IO_FLOAT(values[i]);
    }

    REAL_alListenerfv(param, values);
    IO_END();
}

void alListenerf(ALenum param, ALfloat value)
{
    IO_START(alListenerf);
    IO_ENUM(param);
    IO_FLOAT(value);
    REAL_alListenerf(param, value);
    IO_END();
}

void alListener3f(ALenum param, ALfloat value1, ALfloat value2, ALfloat value3)
{
    IO_START(alListener3f);
    IO_ENUM(param);
    IO_FLOAT(value1);
    IO_FLOAT(value2);
    IO_FLOAT(value3);
    REAL_alListener3f(param, value1, value2, value3);
    IO_END();
}

void alListeneriv(ALenum param, const ALint *values)
{
    uint32 numvals = 1;
    uint32 i;
    IO_START(alListeneriv);
    IO_ENUM(param);
    switch(param) {
        case AL_POSITION: numvals = 3; break;
        case AL_VELOCITY: numvals = 3; break;
        case AL_ORIENTATION: numvals = 6; break;
        default: break;   /* uhoh. */
    }

    IO_UINT32(numvals);
    for (i = 0; i < numvals; i++) {
        IO_INT32(values[i]);
    }

    REAL_alListeneriv(param, values);
    IO_END();
}

void alListeneri(ALenum param, ALint value)
{
    IO_START(alListeneri);
    IO_ENUM(param);
    IO_INT32(value);
    REAL_alListeneri(param, value);
    IO_END();
}

void alListener3i(ALenum param, ALint value1, ALint value2, ALint value3)
{
    IO_START(alListener3i);
    IO_ENUM(param);
    IO_INT32(value1);
    IO_INT32(value2);
    IO_INT32(value3);
    REAL_alListener3i(param, value1, value2, value3);
    IO_END();
}

void alGetListenerfv(ALenum param, ALfloat *values)
{
    IO_START(alGetListenerfv);
    IO_ENUM(param);
    REAL_alGetListenerfv(param, values);
    IO_END();
}

void alGetListenerf(ALenum param, ALfloat *value)
{
    IO_START(alGetListenerf);
    IO_ENUM(param);
    REAL_alGetListenerf(param, value);
    IO_END();
}

void alGetListener3f(ALenum param, ALfloat *value1, ALfloat *value2, ALfloat *value3)
{
    IO_START(alGetListener3f);
    IO_ENUM(param);
    REAL_alGetListener3f(param, value1, value2, value3);
    IO_END();
}

void alGetListeneriv(ALenum param, ALint *values)
{
    IO_START(alGetListeneriv);
    IO_ENUM(param);
    REAL_alGetListeneriv(param, values);
    IO_END();
}

void alGetListeneri(ALenum param, ALint *value)
{
    IO_START(alGetListeneri);
    IO_ENUM(param);
    REAL_alGetListeneri(param, value);
    IO_END();
}

void alGetListener3i(ALenum param, ALint *value1, ALint *value2, ALint *value3)
{
    IO_START(alGetListener3i);
    IO_ENUM(param);
    REAL_alGetListener3i(param, value1, value2, value3);
    IO_END();
}

void alGenSources(ALsizei n, ALuint *names)
{
    ALsizei i;
    IO_START(alGenSources);
    IO_ALSIZEI(n);
    memset(names, 0, n * sizeof (ALuint));
    REAL_alGenSources(n, names);
    for (i = 0; i < n; i++) {
        IO_UINT32(names[i]);
    }
    IO_END();
}

void alDeleteSources(ALsizei n, const ALuint *names)
{
    ALsizei i;
    IO_START(alDeleteSources);
    IO_ALSIZEI(n);
    for (i = 0; i < n; i++) {
        IO_UINT32(names[i]);
    }
    REAL_alDeleteSources(n, names);
    IO_END();
}

ALboolean alIsSource(ALuint name)
{
    ALboolean retval;
    IO_START(alIsSource);
    IO_UINT32(name);
    retval = REAL_alIsSource(name);
    IO_BOOLEAN(retval);
    IO_END();
    return retval;
}

void alSourcefv(ALuint name, ALenum param, const ALfloat *values)
{
    uint32 numvals = 1;
    uint32 i;
    IO_START(alSourcefv);
    IO_UINT32(name);
    IO_ENUM(param);
    switch(param) {
        case AL_GAIN: break;
        case AL_POSITION: numvals = 3; break;
        case AL_VELOCITY: numvals = 3; break;
        case AL_DIRECTION: numvals = 3; break;
        case AL_MIN_GAIN: break;
        case AL_MAX_GAIN: break;
        case AL_REFERENCE_DISTANCE: break;
        case AL_ROLLOFF_FACTOR: break;
        case AL_MAX_DISTANCE: break;
        case AL_PITCH: break;
        case AL_CONE_INNER_ANGLE: break;
        case AL_CONE_OUTER_ANGLE: break;
        case AL_CONE_OUTER_GAIN: break;
        default: break;   /* uhoh. */
    }

    IO_UINT32(numvals);
    for (i = 0; i < numvals; i++) {
        IO_FLOAT(values[i]);
    }

    REAL_alSourcefv(name, param, values);
    IO_END();
}

void alSourcef(ALuint name, ALenum param, ALfloat value)
{
    IO_START(alSourcef);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_FLOAT(value);
    REAL_alSourcef(name, param, value);
    IO_END();
}

void alSource3f(ALuint name, ALenum param, ALfloat value1, ALfloat value2, ALfloat value3)
{
    IO_START(alSource3f);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_FLOAT(value1);
    IO_FLOAT(value2);
    IO_FLOAT(value3);
    REAL_alSource3f(name, param, value1, value2, value3);
    IO_END();
}

void alSourceiv(ALuint name, ALenum param, const ALint *values)
{
    uint32 numvals = 1;
    uint32 i;
    IO_START(alSourceiv);
    IO_UINT32(name);
    IO_ENUM(param);

    switch(param) {
        case AL_BUFFER: break;
        case AL_SOURCE_RELATIVE: break;
        case AL_LOOPING: break;
        case AL_REFERENCE_DISTANCE: break;
        case AL_ROLLOFF_FACTOR: break;
        case AL_MAX_DISTANCE: break;
        case AL_CONE_INNER_ANGLE: break;
        case AL_CONE_OUTER_ANGLE: break;
        case AL_DIRECTION: numvals = 3; break;
        case AL_SEC_OFFSET: break;
        case AL_SAMPLE_OFFSET: break;
        case AL_BYTE_OFFSET: break;
        default: break;   /* uhoh. */
    }

    IO_UINT32(numvals);
    for (i = 0; i < numvals; i++) {
        IO_INT32(values[i]);
    }

    REAL_alSourceiv(name, param, values);
    IO_END();
}

void alSourcei(ALuint name, ALenum param, ALint value)
{
    IO_START(alSourcei);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_INT32(value);
    REAL_alSourcei(name, param, value);
    IO_END();
}

void alSource3i(ALuint name, ALenum param, ALint value1, ALint value2, ALint value3)
{
    IO_START(alSource3i);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_INT32(value1);
    IO_INT32(value2);
    IO_INT32(value3);
    REAL_alSource3i(name, param, value1, value2, value3);
    IO_END();
}

void alGetSourcefv(ALuint name, ALenum param, ALfloat *values)
{
    IO_START(alGetSourcefv);
    IO_UINT32(name);
    IO_ENUM(param);
    REAL_alGetSourcefv(name, param, values);
    IO_END();
}

void alGetSourcef(ALuint name, ALenum param, ALfloat *value)
{
    IO_START(alGetSourcef);
    IO_UINT32(name);
    IO_ENUM(param);
    REAL_alGetSourcef(name, param, value);
    IO_END();
}

void alGetSource3f(ALuint name, ALenum param, ALfloat *value1, ALfloat *value2, ALfloat *value3)
{
    IO_START(alGetSource3f);
    IO_UINT32(name);
    IO_ENUM(param);
    REAL_alGetSource3f(name, param, value1, value2, value3);
    IO_END();
}

void alGetSourceiv(ALuint name, ALenum param, ALint *values)
{
    IO_START(alGetSourceiv);
    IO_UINT32(name);
    IO_ENUM(param);
    REAL_alGetSourceiv(name, param, values);
    IO_END();
}

void alGetSourcei(ALuint name, ALenum param, ALint *value)
{
    IO_START(alGetSourcei);
    IO_UINT32(name);
    IO_ENUM(param);
    REAL_alGetSourcei(name, param, value);
    IO_END();
}

void alGetSource3i(ALuint name, ALenum param, ALint *value1, ALint *value2, ALint *value3)
{
    IO_START(alGetSource3i);
    IO_UINT32(name);
    IO_ENUM(param);
    REAL_alGetSource3i(name, param, value1, value2, value3);
    IO_END();
}

void alSourcePlay(ALuint name)
{
    IO_START(alSourcePlay);
    IO_UINT32(name);
    REAL_alSourcePlay(name);
    IO_END();
}

void alSourcePlayv(ALsizei n, const ALuint *names)
{
    ALsizei i;
    IO_START(alSourcePlayv);
    IO_ALSIZEI(n);
    for (i = 0; i < n; i++) {
        IO_UINT32(names[i]);
    }
    REAL_alSourcePlayv(n, names);
    IO_END();
}

void alSourcePause(ALuint name)
{
    IO_START(alSourcePause);
    IO_UINT32(name);
    REAL_alSourcePause(name);
    IO_END();
}

void alSourcePausev(ALsizei n, const ALuint *names)
{
    ALsizei i;
    IO_START(alSourcePausev);
    IO_ALSIZEI(n);
    for (i = 0; i < n; i++) {
        IO_UINT32(names[i]);
    }
    REAL_alSourcePausev(n, names);
    IO_END();
}

void alSourceRewind(ALuint name)
{
    IO_START(alSourceRewind);
    IO_UINT32(name);
    REAL_alSourceRewind(name);
    IO_END();
}

void alSourceRewindv(ALsizei n, const ALuint *names)
{
    ALsizei i;
    IO_START(alSourceRewindv);
    IO_ALSIZEI(n);
    for (i = 0; i < n; i++) {
        IO_UINT32(names[i]);
    }
    REAL_alSourceRewindv(n, names);
    IO_END();
}

void alSourceStop(ALuint name)
{
    IO_START(alSourceStop);
    IO_UINT32(name);
    REAL_alSourceStop(name);
    IO_END();
}

void alSourceStopv(ALsizei n, const ALuint *names)
{
    ALsizei i;
    IO_START(alSourceStopv);
    IO_ALSIZEI(n);
    for (i = 0; i < n; i++) {
        IO_UINT32(names[i]);
    }
    REAL_alSourceStopv(n, names);
    IO_END();
}

void alSourceQueueBuffers(ALuint name, ALsizei nb, const ALuint *bufnames)
{
    ALsizei i;
    IO_START(alSourceQueueBuffers);
    IO_UINT32(name);
    IO_ALSIZEI(nb);
    for (i = 0; i < nb; i++) {
        IO_UINT32(bufnames[i]);
    }
    REAL_alSourceQueueBuffers(name, nb, bufnames);
    IO_END();
}

void alSourceUnqueueBuffers(ALuint name, ALsizei nb, ALuint *bufnames)
{
    ALsizei i;
    IO_START(alSourceUnqueueBuffers);
    IO_UINT32(name);
    IO_ALSIZEI(nb);
    memset(bufnames, 0, nb * sizeof (ALuint));
    REAL_alSourceUnqueueBuffers(name, nb, bufnames);
    for (i = 0; i < nb; i++) {
        IO_UINT32(bufnames[i]);
    }
    IO_END();
}

void alGenBuffers(ALsizei n, ALuint *names)
{
    ALsizei i;
    IO_START(alGenBuffers);
    IO_ALSIZEI(n);
    memset(names, 0, n * sizeof (ALuint));
    REAL_alGenBuffers(n, names);
    for (i = 0; i < n; i++) {
        IO_UINT32(names[i]);
    }
    IO_END();
}

void alDeleteBuffers(ALsizei n, const ALuint *names)
{
    ALsizei i;
    IO_START(alDeleteSources);
    IO_ALSIZEI(n);
    for (i = 0; i < n; i++) {
        IO_UINT32(names[i]);
    }
    REAL_alDeleteBuffers(n, names);
    IO_END();
}

ALboolean alIsBuffer(ALuint name)
{
    ALboolean retval;
    IO_START(alIsBuffer);
    IO_UINT32(name);
    retval = REAL_alIsBuffer(name);
    IO_BOOLEAN(retval);
    IO_END();
    return retval;
}

void alBufferData(ALuint name, ALenum alfmt, const ALvoid *data, ALsizei size, ALsizei freq)
{
    IO_START(alBufferData);
    IO_UINT32(name);
    IO_ENUM(alfmt);
    IO_ALSIZEI(freq);
    IO_BLOB(data, size);
    REAL_alBufferData(name, alfmt, data, size, freq);
    IO_END();
}

void alBufferfv(ALuint name, ALenum param, const ALfloat *values)
{
    uint32 numvals = 1;
    uint32 i;
    IO_START(alBufferfv);
    IO_UINT32(name);
    IO_ENUM(param);
    /* nothing uses this at the moment. */
    IO_UINT32(numvals);
    for (i = 0; i < numvals; i++) {
        IO_FLOAT(values[i]);
    }
    REAL_alBufferfv(name, param, values);
    IO_END();
}

void alBufferf(ALuint name, ALenum param, ALfloat value)
{
    IO_START(alBufferf);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_FLOAT(value);
    REAL_alBufferf(name, param, value);
    IO_END();
}

void alBuffer3f(ALuint name, ALenum param, ALfloat value1, ALfloat value2, ALfloat value3)
{
    IO_START(alBuffer3f);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_FLOAT(value1);
    IO_FLOAT(value2);
    IO_FLOAT(value3);
    REAL_alBuffer3f(name, param, value1, value2, value3);
    IO_END();
}

void alBufferiv(ALuint name, ALenum param, const ALint *values)
{
    uint32 numvals = 1;
    uint32 i;
    IO_START(alBufferiv);
    IO_UINT32(name);
    IO_ENUM(param);
    /* nothing uses this at the moment. */
    IO_UINT32(numvals);
    for (i = 0; i < numvals; i++) {
        IO_INT32(values[i]);
    }
    REAL_alBufferiv(name, param, values);
    IO_END();
}

void alBufferi(ALuint name, ALenum param, ALint value)
{
    IO_START(alBufferi);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_INT32(value);
    REAL_alBufferi(name, param, value);
    IO_END();
}

void alBuffer3i(ALuint name, ALenum param, ALint value1, ALint value2, ALint value3)
{
    IO_START(alBuffer3i);
    IO_UINT32(name);
    IO_ENUM(param);
    IO_INT32(value1);
    IO_INT32(value2);
    IO_INT32(value3);
    REAL_alBuffer3i(name, param, value1, value2, value3);
    IO_END();
}

void alGetBufferfv(ALuint name, ALenum param, ALfloat *values)
{
    IO_START(alGetBufferfv);
    IO_UINT32(name);
    IO_ENUM(param);
    REAL_alGetBufferfv(name, param, values);
    IO_END();
}

void alGetBufferf(ALuint name, ALenum param, ALfloat *value)
{
    IO_START(alGetBufferf);
    IO_UINT32(name);
    IO_ENUM(param);
    REAL_alGetBufferf(name, param, value);
    IO_END();
}

void alGetBuffer3f(ALuint name, ALenum param, ALfloat *value1, ALfloat *value2, ALfloat *value3)
{
    IO_START(alGetBuffer3f);
    IO_UINT32(name);
    IO_ENUM(param);
    REAL_alGetBuffer3f(name, param, value1, value2, value3);
    IO_END();
}

void alGetBufferi(ALuint name, ALenum param, ALint *value)
{
    IO_START(alGetBufferi);
    IO_UINT32(name);
    IO_ENUM(param);
    REAL_alGetBufferi(name, param, value);
    IO_END();
}

void alGetBuffer3i(ALuint name, ALenum param, ALint *value1, ALint *value2, ALint *value3)
{
    IO_START(alGetBuffer3i);
    IO_UINT32(name);
    IO_ENUM(param);
    REAL_alGetBuffer3i(name, param, value1, value2, value3);
    IO_END();
}

void alGetBufferiv(ALuint name, ALenum param, ALint *values)
{
    IO_START(alGetBufferiv);
    IO_UINT32(name);
    IO_ENUM(param);
    REAL_alGetBufferiv(name, param, values);
    IO_END();
}

// end of altrace_record.c ...

