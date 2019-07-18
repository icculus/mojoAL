/**
 * alTrace; a debugging tool for OpenAL.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

#include "altrace_playback.c"

static int dump_calls = 1;
static int dump_callers = 0;
static int dump_state_changes = 0;
static int dump_errors = 0;
static int dumping = 1;
static int run_calls = 0;


// Visitors for logging OpenAL calls to stdout...

static void dump_alcGetCurrentContext(const CallerInfo *callerinfo, ALCcontext *retval)
{
    printf("() => %s\n", ptrString(retval));
}

static void dump_alcGetContextsDevice(const CallerInfo *callerinfo, ALCdevice *retval, ALCcontext *context)
{
    printf("(%s) => %s\n", ptrString(context), ptrString(retval));
}

static void dump_alcIsExtensionPresent(const CallerInfo *callerinfo, ALCboolean retval, ALCdevice *device, const ALCchar *extname)
{
    printf("(%s, %s) => %s\n", ptrString(device), litString(extname), alcboolString(retval));
}

static void dump_alcGetProcAddress(const CallerInfo *callerinfo, void *retval, ALCdevice *device, const ALCchar *funcname)
{
    printf("(%s, %s) => %s\n", ptrString(device), litString(funcname), ptrString(retval));
}

static void dump_alcGetEnumValue(const CallerInfo *callerinfo, ALCenum retval, ALCdevice *device, const ALCchar *enumname)
{
    printf("(%s, %s) => %s\n", ptrString(device), litString(enumname), alcenumString(retval));
}

static void dump_alcGetString(const CallerInfo *callerinfo, const ALCchar *retval, ALCdevice *device, ALCenum param)
{
    printf("(%s, %s) => %s\n", ptrString(device), alcenumString(param), litString(retval));
}

static void dump_alcCaptureOpenDevice(const CallerInfo *callerinfo, ALCdevice *retval, const ALCchar *devicename, ALCuint frequency, ALCenum format, ALCsizei buffersize)
{
    printf("(%s, %u, %s, %u) => %s\n", litString(devicename), (uint) frequency, alcenumString(format), (uint) buffersize, ptrString(retval));
}

static void dump_alcCaptureCloseDevice(const CallerInfo *callerinfo, ALCboolean retval, ALCdevice *device)
{
    printf("(%s) => %s\n", ptrString(device), alcboolString(retval));
}

static void dump_alcOpenDevice(const CallerInfo *callerinfo, ALCdevice *retval, const ALCchar *devicename)
{
    printf("(%s) => %s\n", litString(devicename), ptrString(retval));
}

static void dump_alcCloseDevice(const CallerInfo *callerinfo, ALCboolean retval, ALCdevice *device)
{
    printf("(%s) => %s\n", ptrString(device), alcboolString(retval));
}

static void dump_alcCreateContext(const CallerInfo *callerinfo, ALCcontext *retval, ALCdevice *device, const ALCint *origattrlist, uint32 attrcount, const ALCint *attrlist)
{
    printf("(%s, %s", ptrString(device), ptrString(origattrlist));
    if (origattrlist) {
        ALCint i;
        printf(" {");
        for (i = 0; i < attrcount; i += 2) {
            printf(" %s, %u,", alcenumString(attrlist[i]), (uint) attrlist[i+1]);
        }
        printf(" 0 }");
    }
    printf(") => %s\n", ptrString(retval));
}

static void dump_alcMakeContextCurrent(const CallerInfo *callerinfo, ALCboolean retval, ALCcontext *ctx)
{
    printf("(%s) => %s\n", ptrString(ctx), alcboolString(retval));
}

static void dump_alcProcessContext(const CallerInfo *callerinfo, ALCcontext *ctx)
{
    printf("(%s)\n", ptrString(ctx));
}

static void dump_alcSuspendContext(const CallerInfo *callerinfo, ALCcontext *ctx)
{
    printf("(%s)\n", ptrString(ctx));
}

static void dump_alcDestroyContext(const CallerInfo *callerinfo, ALCcontext *ctx)
{
    printf("(%s)\n", ptrString(ctx));
}

static void dump_alcGetError(const CallerInfo *callerinfo, ALCenum retval, ALCdevice *device)
{
    printf("(%s) => %s\n", ptrString(device), alcboolString(retval));
}

static void dump_alcGetIntegerv(const CallerInfo *callerinfo, ALCdevice *device, ALCenum param, ALCsizei size, ALCint *origvalues, ALCint *values)
{
    ALCsizei i;
    printf("(%s, %s, %u, %s)", ptrString(device), alcenumString(param), (uint) size, ptrString(origvalues));
    if (origvalues) {
        printf(" => {");
        for (i = 0; i < size; i++) {
            printf("%s %d", i > 0 ? "," : "", values[i]);
        }
        printf("%s}", size > 0 ? " " : "");
    }
    printf("\n");
}

static void dump_alcCaptureStart(const CallerInfo *callerinfo, ALCdevice *device)
{
    printf("(%s)\n", ptrString(device));
}

static void dump_alcCaptureStop(const CallerInfo *callerinfo, ALCdevice *device)
{
    printf("(%s)\n", ptrString(device));
}

static void dump_alcCaptureSamples(const CallerInfo *callerinfo, ALCdevice *device, ALCvoid *origbuffer, ALCvoid *buffer, ALCsizei samples)
{
    printf("(%s, %s, %u)\n", ptrString(device), ptrString(origbuffer), (uint) samples);
}

static void dump_alDopplerFactor(const CallerInfo *callerinfo, ALfloat value)
{
    printf("(%f)\n", value);
}

static void dump_alDopplerVelocity(const CallerInfo *callerinfo, ALfloat value)
{
    printf("(%f)\n", value);
}

static void dump_alSpeedOfSound(const CallerInfo *callerinfo, ALfloat value)
{
    printf("(%f)\n", value);
}

static void dump_alDistanceModel(const CallerInfo *callerinfo, ALenum model)
{
    printf("(%s)\n", alenumString(model));
}

static void dump_alEnable(const CallerInfo *callerinfo, ALenum capability)
{
    printf("(%s)\n", alenumString(capability));
}

static void dump_alDisable(const CallerInfo *callerinfo, ALenum capability)
{
    printf("(%s)\n", alenumString(capability));
}

static void dump_alIsEnabled(const CallerInfo *callerinfo, ALboolean retval, ALenum capability)
{
    printf("(%s) => %s\n", alenumString(capability), alboolString(retval));
}

static void dump_alGetString(const CallerInfo *callerinfo, const ALchar *retval, const ALenum param)
{
    printf("(%s) => %s\n", alenumString(param), litString(retval));
}

static void dump_alGetBooleanv(const CallerInfo *callerinfo, ALenum param, ALboolean *origvalues, uint32 numvals, ALboolean *values)
{
    uint32 i;
    printf("(%s, %s) => {", alenumString(param), ptrString(origvalues));
    for (i = 0; i < numvals; i++) {
        printf("%s %s", i > 0 ? "," : "", alboolString(values[i]));
    }
    printf("%s}\n", numvals > 0 ? " " : "");
}

static void dump_alGetIntegerv(const CallerInfo *callerinfo, ALenum param, ALint *origvalues, uint32 numvals, ALboolean isenum, ALint *values)
{
    uint32 i;
    printf("(%s, %s) => {", alenumString(param), ptrString(origvalues));
    for (i = 0; i < numvals; i++) {
        if (isenum) {
            printf("%s %s", i > 0 ? "," : "", alenumString((ALenum) values[i]));
        } else {
            printf("%s %d", i > 0 ? "," : "", (int) values[i]);
        }
    }
    printf("%s}\n", numvals > 0 ? " " : "");
}

static void dump_alGetFloatv(const CallerInfo *callerinfo, ALenum param, ALfloat *origvalues, uint32 numvals, ALfloat *values)
{
    uint32 i;
    printf("(%s, %s) => {", alenumString(param), ptrString(origvalues));
    for (i = 0; i < numvals; i++) {
        printf("%s %f", i > 0 ? "," : "", values[i]);
    }
    printf("%s}\n", numvals > 0 ? " " : "");
}

static void dump_alGetDoublev(const CallerInfo *callerinfo, ALenum param, ALdouble *origvalues, uint32 numvals, ALdouble *values)
{
    uint32 i;
    printf("(%s, %s) => {", alenumString(param), ptrString(origvalues));
    for (i = 0; i < numvals; i++) {
        printf("%s %f", i > 0 ? "," : "", values[i]);
    }
    printf("%s}\n", numvals > 0 ? " " : "");
}

static void dump_alGetBoolean(const CallerInfo *callerinfo, ALboolean retval, ALenum param)
{
    printf("(%s) => %s\n", alenumString(param), alboolString(retval));
}

static void dump_alGetInteger(const CallerInfo *callerinfo, ALint retval, ALenum param)
{
    printf("(%s) => %d\n", alenumString(param), (int) retval);
}

static void dump_alGetFloat(const CallerInfo *callerinfo, ALfloat retval, ALenum param)
{
    printf("(%s) => %f\n", alenumString(param), retval);
}

static void dump_alGetDouble(const CallerInfo *callerinfo, ALdouble retval, ALenum param)
{
    printf("(%s) => %f\n", alenumString(param), retval);
}

static void dump_alIsExtensionPresent(const CallerInfo *callerinfo, ALboolean retval, const ALchar *extname)
{
    printf("(%s) => %s\n", litString(extname), alboolString(retval));
}

static void dump_alGetError(const CallerInfo *callerinfo, ALenum retval)
{
    printf("() => %s\n", alenumString(retval));
}

static void dump_alGetProcAddress(const CallerInfo *callerinfo, void *retval, const ALchar *funcname)
{
    printf("(%s) => %s\n", litString(funcname), ptrString(retval));
}

static void dump_alGetEnumValue(const CallerInfo *callerinfo, ALenum retval, const ALchar *enumname)
{
    printf("(%s) => %s\n", litString(enumname), alenumString(retval));
}

static void dump_alListenerfv(const CallerInfo *callerinfo, ALenum param, const ALfloat *origvalues, uint32 numvals, const ALfloat *values)
{
    uint32 i;
    printf("(%s, %s", alenumString(param), ptrString(origvalues));
    if (origvalues) {
        printf(" {");
        for (i = 0; i < numvals; i++) {
            printf("%s %f", i > 0 ? "," : "", values[i]);
        }
        printf("%s}", numvals > 0 ? " " : "");
    }
    printf(")\n");
}

static void dump_alListenerf(const CallerInfo *callerinfo, ALenum param, ALfloat value)
{
    printf("(%s, %f)\n", alenumString(param), value);
}

static void dump_alListener3f(const CallerInfo *callerinfo, ALenum param, ALfloat value1, ALfloat value2, ALfloat value3)
{
    printf("(%s, %f, %f, %f)\n", alenumString(param), value1, value2, value3);
}

static void dump_alListeneriv(const CallerInfo *callerinfo, ALenum param, const ALint *origvalues, uint32 numvals, const ALint *values)
{
    uint32 i;
    printf("(%s, %s", alenumString(param), ptrString(origvalues));
    if (origvalues) {
        printf(" {");
        for (i = 0; i < numvals; i++) {
            printf("%s %d", i > 0 ? "," : "", (int) values[i]);
        }
        printf("%s}", numvals > 0 ? " " : "");
    }
    printf(")\n");
}

static void dump_alListeneri(const CallerInfo *callerinfo, ALenum param, ALint value)
{
    printf("(%s, %d)\n", alenumString(param), (int) value);
}

static void dump_alListener3i(const CallerInfo *callerinfo, ALenum param, ALint value1, ALint value2, ALint value3)
{
    printf("(%s, %d, %d, %d)\n", alenumString(param), (int) value1, (int) value2, (int) value3);
}

static void dump_alGetListenerfv(const CallerInfo *callerinfo, ALenum param, ALfloat *origvalues, uint32 numvals, ALfloat *values)
{
    uint32 i;
    printf("(%s, %s)", alenumString(param), ptrString(origvalues));
    if (origvalues) {
        printf(" => {");
        for (i = 0; i < numvals; i++) {
            printf("%s %f", i > 0 ? "," : "", values[i]);
        }
        printf("%s}", numvals > 0 ? " " : "");
    }
    printf("\n");
}

static void dump_alGetListenerf(const CallerInfo *callerinfo, ALenum param, ALfloat *origvalue, ALfloat value)
{
    printf("(%s, %s) => { %f }\n", alenumString(param), ptrString(origvalue), value);
}

static void dump_alGetListener3f(const CallerInfo *callerinfo, ALenum param, ALfloat *origvalue1, ALfloat *origvalue2, ALfloat *origvalue3, ALfloat value1, ALfloat value2, ALfloat value3)
{
    printf("(%s, %s, %s, %s) => { %f, %f, %f }\n", alenumString(param), ptrString(origvalue1), ptrString(origvalue2), ptrString(origvalue3), value1, value2, value3);
}

static void dump_alGetListeneri(const CallerInfo *callerinfo, ALenum param, ALint *origvalue, ALint value)
{
    printf("(%s, %s) => { %d }\n", alenumString(param), ptrString(origvalue), (int) value);
}

static void dump_alGetListeneriv(const CallerInfo *callerinfo, ALenum param, ALint *origvalues, uint32 numvals, ALint *values)
{
    uint32 i;
    printf("(%s, %s)", alenumString(param), ptrString(origvalues));
    if (origvalues) {
        printf(" => {");
        for (i = 0; i < numvals; i++) {
            printf("%s %d", i > 0 ? "," : "", (int) values[i]);
        }
        printf("%s}", numvals > 0 ? " " : "");
    }
    printf("\n");
}

static void dump_alGetListener3i(const CallerInfo *callerinfo, ALenum param, ALint *origvalue1, ALint *origvalue2, ALint *origvalue3, ALint value1, ALint value2, ALint value3)
{
    printf("(%s, %s, %s, %s) => { %d, %d, %d }\n", alenumString(param), ptrString(origvalue1), ptrString(origvalue2), ptrString(origvalue3), (int) value1, (int) value2, (int) value3);
}

static void dump_alGenSources(const CallerInfo *callerinfo, ALsizei n, ALuint *orignames, ALuint *names)
{
    ALsizei i;
    printf("(%u, %s)", (uint) n, ptrString(orignames));
    if (orignames) {
        printf(" => {");
        for (i = 0; i < n; i++) {
            printf("%s %u", i > 0 ? "," : "", (uint) names[i]);
        }
        printf("%s}", n > 0 ? " " : "");
    }
    printf("\n");
}

static void dump_alDeleteSources(const CallerInfo *callerinfo, ALsizei n, const ALuint *orignames, const ALuint *names)
{
    ALsizei i;
    printf("(%u, %s", (uint) n, ptrString(orignames));
    if (orignames) {
        printf(" {");
        for (i = 0; i < n; i++) {
            printf("%s %u", i > 0 ? "," : "", (uint) names[i]);
        }
        printf("%s}", n > 0 ? " " : "");
    }
    printf(")\n");
}

static void dump_alIsSource(const CallerInfo *callerinfo, ALboolean retval, ALuint name)
{
    printf("(%u) => %s\n", (uint) name, alboolString(retval));
}

static void dump_alSourcefv(const CallerInfo *callerinfo, ALuint name, ALenum param, const ALfloat *origvalues, uint32 numvals, const ALfloat *values)
{
    uint32 i;
    printf("(%u, %s, %s", (uint) name, alenumString(param), ptrString(origvalues));
    if (origvalues) {
        printf(" {");
        for (i = 0; i < numvals; i++) {
            printf("%s %f", i > 0 ? "," : "", values[i]);
        }
        printf("%s}", numvals > 0 ? " " : "");
    }
    printf(")\n");
}

static void dump_alSourcef(const CallerInfo *callerinfo, ALuint name, ALenum param, ALfloat value)
{
    printf("(%u, %s, %f)\n", (uint) name, alenumString(param), value);
}

static void dump_alSource3f(const CallerInfo *callerinfo, ALuint name, ALenum param, ALfloat value1, ALfloat value2, ALfloat value3)
{
    printf("(%u, %s, %f, %f, %f)\n", (uint) name, alenumString(param), value1, value2, value3);
}

static void dump_alSourceiv(const CallerInfo *callerinfo, ALuint name, ALenum param, const ALint *origvalues, uint32 numvals, const ALint *values)
{
    uint32 i;
    printf("(%u, %s, %s", (uint) name, alenumString(param), ptrString(origvalues));
    if (origvalues) {
        printf(" {");
        for (i = 0; i < numvals; i++) {
            printf("%s %d", i > 0 ? "," : "", (int) values[i]);
        }
        printf("%s}", numvals > 0 ? " " : "");
    }
    printf(")\n");
}

static void dump_alSourcei(const CallerInfo *callerinfo, ALuint name, ALenum param, ALint value)
{
    printf("(%u, %s, %d)\n", (uint) name, alenumString(param), (int) value);
}

static void dump_alSource3i(const CallerInfo *callerinfo, ALuint name, ALenum param, ALint value1, ALint value2, ALint value3)
{
    printf("(%u, %s, %d, %d, %d)\n", (uint) name, alenumString(param), (int) value1, (int) value2, (int) value3);
}

static void dump_alGetSourcefv(const CallerInfo *callerinfo, ALuint name, ALenum param, ALfloat *origvalues, uint32 numvals, ALfloat *values)
{
    uint32 i;
    printf("(%u, %s, %s)", (uint) name, alenumString(param), ptrString(origvalues));
    if (origvalues) {
        printf(" => {");
        for (i = 0; i < numvals; i++) {
            printf("%s %f", i > 0 ? "," : "", values[i]);
        }
        printf("%s}", numvals > 0 ? " " : "");
    }
    printf("\n");
}

static void dump_alGetSourcef(const CallerInfo *callerinfo, ALuint name, ALenum param, ALfloat *origvalue, ALfloat value)
{
    printf("(%u, %s, %s) => { %f }\n", (uint) name, alenumString(param), ptrString(origvalue), value);
}

static void dump_alGetSource3f(const CallerInfo *callerinfo, ALuint name, ALenum param, ALfloat *origvalue1, ALfloat *origvalue2, ALfloat *origvalue3, ALfloat value1, ALfloat value2, ALfloat value3)
{
    printf("(%u, %s, %s, %s, %s) => { %f, %f, %f }\n", (uint) name, alenumString(param), ptrString(origvalue1), ptrString(origvalue2), ptrString(origvalue3), value1, value2, value3);
}

static void dump_alGetSourceiv(const CallerInfo *callerinfo, ALuint name, ALenum param, ALboolean isenum, ALint *origvalues, uint32 numvals, ALint *values)
{
    uint32 i;
    printf("(%u, %s, %s)", (uint) name, alenumString(param), ptrString(origvalues));
    if (origvalues) {
        printf(" => {");
        for (i = 0; i < numvals; i++) {
            printf("%s %d", i > 0 ? "," : "", (int) values[i]);
        }
        printf("%s}", numvals > 0 ? " " : "");
    }
    printf("\n");
}

static void dump_alGetSourcei(const CallerInfo *callerinfo, ALuint name, ALenum param, ALboolean isenum, ALint *origvalue, ALint value)
{
    printf("(%u, %s, %s) => { %d }\n", (uint) name, alenumString(param), ptrString(origvalue), (int) value);
}

static void dump_alGetSource3i(const CallerInfo *callerinfo, ALuint name, ALenum param, ALint *origvalue1, ALint *origvalue2, ALint *origvalue3, ALint value1, ALint value2, ALint value3)
{
    printf("(%u, %s, %s, %s, %s) => { %d, %d, %d }\n", (uint) name, alenumString(param), ptrString(origvalue1), ptrString(origvalue2), ptrString(origvalue3), (int) value1, (int) value2, (int) value3);
}

static void dump_alSourcePlay(const CallerInfo *callerinfo, ALuint name)
{
    printf("(%u)\n", (uint) name);
}

static void dump_alSourcePlayv(const CallerInfo *callerinfo, ALsizei n, const ALuint *orignames, const ALuint *names)
{
    ALsizei i;
    printf("(%u, %s", (uint) n, ptrString(orignames));
    if (orignames) {
        printf(" {");
        for (i = 0; i < n; i++) {
            printf("%s %u", i > 0 ? "," : "", (uint) names[i]);
        }
        printf("%s}", n > 0 ? " " : "");
    }
    printf(")\n");
}

static void dump_alSourcePause(const CallerInfo *callerinfo, ALuint name)
{
    printf("(%u)\n", (uint) name);
}

static void dump_alSourcePausev(const CallerInfo *callerinfo, ALsizei n, const ALuint *orignames, const ALuint *names)
{
    ALsizei i;
    printf("(%u, %s", (uint) n, ptrString(orignames));
    if (orignames) {
        printf(" {");
        for (i = 0; i < n; i++) {
            printf("%s %u", i > 0 ? "," : "", (uint) names[i]);
        }
        printf("%s}", n > 0 ? " " : "");
    }
    printf(")\n");
}

static void dump_alSourceRewind(const CallerInfo *callerinfo, ALuint name)
{
    printf("(%u)\n", (uint) name);
}

static void dump_alSourceRewindv(const CallerInfo *callerinfo, ALsizei n, const ALuint *orignames, const ALuint *names)
{
    ALsizei i;
    printf("(%u, %s", (uint) n, ptrString(orignames));
    if (orignames) {
        printf(" {");
        for (i = 0; i < n; i++) {
            printf("%s %u", i > 0 ? "," : "", (uint) names[i]);
        }
        printf("%s}", n > 0 ? " " : "");
    }
    printf(")\n");
}

static void dump_alSourceStop(const CallerInfo *callerinfo, ALuint name)
{
    printf("(%u)\n", (uint) name);
}

static void dump_alSourceStopv(const CallerInfo *callerinfo, ALsizei n, const ALuint *orignames, const ALuint *names)
{
    ALsizei i;
    printf("(%u, %s", (uint) n, ptrString(orignames));
    if (orignames) {
        printf(" {");
        for (i = 0; i < n; i++) {
            printf("%s %u", i > 0 ? "," : "", (uint) names[i]);
        }
        printf("%s}", n > 0 ? " " : "");
    }
    printf(")\n");
}

static void dump_alSourceQueueBuffers(const CallerInfo *callerinfo, ALuint name, ALsizei nb, const ALuint *origbufnames, const ALuint *bufnames)
{
    ALsizei i;
    printf("(%u, %u, %s", (uint) name, (uint) nb, ptrString(origbufnames));
    if (origbufnames) {
        printf(" {");
        for (i = 0; i < nb; i++) {
            printf("%s %u", i > 0 ? "," : "", (uint) bufnames[i]);
        }
        printf("%s}", nb > 0 ? " " : "");
    }
    printf(")\n");
}

static void dump_alSourceUnqueueBuffers(const CallerInfo *callerinfo, ALuint name, ALsizei nb, ALuint *origbufnames, ALuint *bufnames)
{
    ALsizei i;
    printf("(%u, %u, %s", (uint) name, (uint) nb, ptrString(origbufnames));
    if (origbufnames) {
        printf(" {");
        for (i = 0; i < nb; i++) {
            printf("%s %u", i > 0 ? "," : "", (uint) bufnames[i]);
        }
        printf("%s}", nb > 0 ? " " : "");
    }
    printf(")\n");
}

static void dump_alGenBuffers(const CallerInfo *callerinfo, ALsizei n, ALuint *orignames, ALuint *names)
{
    ALsizei i;
    printf("(%u, %s)", (uint) n, ptrString(orignames));
    if (orignames) {
        printf(" => {");
        for (i = 0; i < n; i++) {
            printf("%s %u", i > 0 ? "," : "", (uint) names[i]);
        }
        printf("%s}", n > 0 ? " " : "");
    }
    printf("\n");
}

static void dump_alDeleteBuffers(const CallerInfo *callerinfo, ALsizei n, const ALuint *orignames, const ALuint *names)
{
    ALsizei i;
    printf("(%u, %s", (uint) n, ptrString(orignames));
    if (orignames) {
        printf(" {");
        for (i = 0; i < n; i++) {
            printf("%s %u", i > 0 ? "," : "", (uint) names[i]);
        }
        printf("%s}", n > 0 ? " " : "");
    }
    printf(")\n");
}

static void dump_alIsBuffer(const CallerInfo *callerinfo, ALboolean retval, ALuint name)
{
    printf("(%u) => %s\n", (uint) name, alboolString(retval));
}

static void dump_alBufferData(const CallerInfo *callerinfo, ALuint name, ALenum alfmt, const ALvoid *origdata, const ALvoid *data, ALsizei size, ALsizei freq)
{
    printf("(%u, %s, %s, %u, %u)\n", (uint) name, alenumString(alfmt), ptrString(origdata), (uint) size, (uint) freq);
}

static void dump_alBufferfv(const CallerInfo *callerinfo, ALuint name, ALenum param, const ALfloat *origvalues, uint32 numvals, const ALfloat *values)
{
    uint32 i;
    printf("(%u, %s, %s", (uint) name, alenumString(param), ptrString(origvalues));
    if (origvalues) {
        printf(" {");
        for (i = 0; i < numvals; i++) {
            printf("%s %f", i > 0 ? "," : "", values[i]);
        }
        printf("%s}", numvals > 0 ? " " : "");
    }
    printf(")\n");
}

static void dump_alBufferf(const CallerInfo *callerinfo, ALuint name, ALenum param, ALfloat value)
{
    printf("(%u, %s, %f)\n", (uint) name, alenumString(param), value);
}

static void dump_alBuffer3f(const CallerInfo *callerinfo, ALuint name, ALenum param, ALfloat value1, ALfloat value2, ALfloat value3)
{
    printf("(%u, %s, %f, %f, %f)\n", (uint) name, alenumString(param), value1, value2, value3);
}

static void dump_alBufferiv(const CallerInfo *callerinfo, ALuint name, ALenum param, const ALint *origvalues, uint32 numvals, const ALint *values)
{
    uint32 i;
    printf("(%u, %s, %s", (uint) name, alenumString(param), ptrString(origvalues));
    if (origvalues) {
        printf(" {");
        for (i = 0; i < numvals; i++) {
            printf("%s %d", i > 0 ? "," : "", (int) values[i]);
        }
        printf("%s}", numvals > 0 ? " " : "");
    }
    printf(")\n");
}

static void dump_alBufferi(const CallerInfo *callerinfo, ALuint name, ALenum param, ALint value)
{
    printf("(%u, %s, %d)\n", (uint) name, alenumString(param), (int) value);
}

static void dump_alBuffer3i(const CallerInfo *callerinfo, ALuint name, ALenum param, ALint value1, ALint value2, ALint value3)
{
    printf("(%u, %s, %d, %d, %d)\n", (uint) name, alenumString(param), (int) value1, (int) value2, (int) value3);
}

static void dump_alGetBufferfv(const CallerInfo *callerinfo, ALuint name, ALenum param, ALfloat *origvalues, uint32 numvals, ALfloat *values)
{
    uint32 i;
    printf("(%u, %s, %s)", (uint) name, alenumString(param), ptrString(origvalues));
    if (origvalues) {
        printf(" => {");
        for (i = 0; i < numvals; i++) {
            printf("%s %f", i > 0 ? "," : "", values[i]);
        }
        printf("%s}", numvals > 0 ? " " : "");
    }
    printf("\n");
}

static void dump_alGetBufferf(const CallerInfo *callerinfo, ALuint name, ALenum param, ALfloat *origvalue, ALfloat value)
{
    printf("(%u, %s, %s) => { %f }\n", (uint) name, alenumString(param), ptrString(origvalue), value);
}

static void dump_alGetBuffer3f(const CallerInfo *callerinfo, ALuint name, ALenum param, ALfloat *origvalue1, ALfloat *origvalue2, ALfloat *origvalue3, ALfloat value1, ALfloat value2, ALfloat value3)
{
    printf("(%u, %s, %s, %s, %s) => { %f, %f, %f }\n", (uint) name, alenumString(param), ptrString(origvalue1), ptrString(origvalue2), ptrString(origvalue3), value1, value2, value3);
}

static void dump_alGetBufferi(const CallerInfo *callerinfo, ALuint name, ALenum param, ALint *origvalue, ALint value)
{
    printf("(%u, %s, %s) => { %d }\n", (uint) name, alenumString(param), ptrString(origvalue), (int) value);
}

static void dump_alGetBuffer3i(const CallerInfo *callerinfo, ALuint name, ALenum param, ALint *origvalue1, ALint *origvalue2, ALint *origvalue3, ALint value1, ALint value2, ALint value3)
{
    printf("(%u, %s, %s, %s, %s) => { %d, %d, %d }\n", (uint) name, alenumString(param), ptrString(origvalue1), ptrString(origvalue2), ptrString(origvalue3), (int) value1, (int) value2, (int) value3);
}

static void dump_alGetBufferiv(const CallerInfo *callerinfo, ALuint name, ALenum param, ALint *origvalues, uint32 numvals, ALint *values)
{
    uint32 i;
    printf("(%u, %s, %s)", (uint) name, alenumString(param), ptrString(origvalues));
    if (origvalues) {
        printf(" => {");
        for (i = 0; i < numvals; i++) {
            printf("%s %d", i > 0 ? "," : "", (int) values[i]);
        }
        printf("%s}", numvals > 0 ? " " : "");
    }
    printf("\n");
}

static void dump_alTracePushScope(const CallerInfo *callerinfo, const ALchar *str)
{
    printf("(%s)\n", litString(str));
}

static void dump_alTracePopScope(const CallerInfo *callerinfo)
{
    printf("()\n");
}

static void dump_alTraceMessage(const CallerInfo *callerinfo, const ALchar *str)
{
    printf("(%s)\n", litString(str));
}

static void dump_alTraceBufferLabel(const CallerInfo *callerinfo, ALuint name, const ALchar *str)
{
    printf("(%u, %s)\n", (uint) name, litString(str));
}

static void dump_alTraceSourceLabel(const CallerInfo *callerinfo, ALuint name, const ALchar *str)
{
    printf("(%u, %s)\n", (uint) name, litString(str));
}

static void dump_alcTraceDeviceLabel(const CallerInfo *callerinfo, ALCdevice *device, const ALchar *str)
{
    printf("(%s, %s)\n", ptrString(device), litString(str));
}

static void dump_alcTraceContextLabel(const CallerInfo *callerinfo, ALCcontext *ctx, const ALchar *str)
{
    printf("(%s, %s)\n", ptrString(ctx), litString(str));
}


// Visitors for playback on a real OpenAL implementation...

static void run_alcGetCurrentContext(const CallerInfo *callerinfo, ALCcontext *retval)
{
    REAL_alcGetCurrentContext();
}

static void run_alcGetContextsDevice(const CallerInfo *callerinfo, ALCdevice *retval, ALCcontext *ctx)
{
    REAL_alcGetContextsDevice(get_mapped_context(ctx));
}

static void run_alcIsExtensionPresent(const CallerInfo *callerinfo, ALCboolean retval, ALCdevice *device, const ALCchar *extname)
{
    REAL_alcIsExtensionPresent(get_mapped_device(device), extname);
}

static void run_alcGetProcAddress(const CallerInfo *callerinfo, void *retval, ALCdevice *device, const ALCchar *funcname)
{
    REAL_alcGetProcAddress(get_mapped_device(device), funcname);
}

static void run_alcGetEnumValue(const CallerInfo *callerinfo, ALCenum retval, ALCdevice *device, const ALCchar *enumname)
{
    REAL_alcGetEnumValue(get_mapped_device(device), enumname);
}

static void run_alcGetString(const CallerInfo *callerinfo, const ALCchar *retval, ALCdevice *device, ALCenum param)
{
    REAL_alcGetString(get_mapped_device(device), param);
}

static void run_alcCaptureOpenDevice(const CallerInfo *callerinfo, ALCdevice *retval, const ALCchar *devicename, ALCuint frequency, ALCenum format, ALCsizei buffersize)
{
    ALCdevice *dev = REAL_alcCaptureOpenDevice(devicename, frequency, format, buffersize);
    if (!dev && retval) {
        fprintf(stderr, "Uhoh, failed to open capture device when original run did!\n");
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

static void run_alcCaptureCloseDevice(const CallerInfo *callerinfo, ALCboolean retval, ALCdevice *device)
{
    REAL_alcCaptureCloseDevice(get_mapped_device(device));
}

static void run_alcOpenDevice(const CallerInfo *callerinfo, ALCdevice *retval, const ALCchar *devicename)
{
    ALCdevice *dev = REAL_alcOpenDevice(devicename);
    if (!dev && retval) {
        fprintf(stderr, "Uhoh, failed to open playback device when original run did!\n");
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

static void run_alcCloseDevice(const CallerInfo *callerinfo, ALCboolean retval, ALCdevice *device)
{
    REAL_alcCloseDevice(get_mapped_device(device));
}

static void run_alcCreateContext(const CallerInfo *callerinfo, ALCcontext *retval, ALCdevice *device, const ALCint *origattrlist, uint32 attrcount, const ALCint *attrlist)
{
    ALCcontext *ctx = REAL_alcCreateContext(get_mapped_device(device), attrlist);
    if (!ctx && retval) {
        fprintf(stderr, "Uhoh, failed to create context when original run did!\n");
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

static void run_alcMakeContextCurrent(const CallerInfo *callerinfo, ALCboolean retval, ALCcontext *ctx)
{
    REAL_alcMakeContextCurrent(get_mapped_context(ctx));
}

static void run_alcProcessContext(const CallerInfo *callerinfo, ALCcontext *ctx)
{
    REAL_alcProcessContext(get_mapped_context(ctx));
}

static void run_alcSuspendContext(const CallerInfo *callerinfo, ALCcontext *ctx)
{
    REAL_alcSuspendContext(get_mapped_context(ctx));
}

static void run_alcDestroyContext(const CallerInfo *callerinfo, ALCcontext *ctx)
{
    REAL_alcDestroyContext(get_mapped_context(ctx));
}

static void run_alcGetError(const CallerInfo *callerinfo, ALCenum retval, ALCdevice *device)
{
    REAL_alcGetError(get_mapped_device(device));
}

static void run_alcGetIntegerv(const CallerInfo *callerinfo, ALCdevice *device, ALCenum param, ALCsizei size, ALCint *origvalues, ALCint *values)
{
    REAL_alcGetIntegerv(get_mapped_device(device), param, size, values);
}

static void run_alcCaptureStart(const CallerInfo *callerinfo, ALCdevice *device)
{
    REAL_alcCaptureStart(get_mapped_device(device));
}

static void run_alcCaptureStop(const CallerInfo *callerinfo, ALCdevice *device)
{
    REAL_alcCaptureStop(get_mapped_device(device));
}

static void run_alcCaptureSamples(const CallerInfo *callerinfo, ALCdevice *device, ALCvoid *origbuffer, ALCvoid *buffer, ALCsizei samples)
{
    REAL_alcCaptureSamples(get_mapped_device(device), buffer, samples);
}

static void run_alDopplerFactor(const CallerInfo *callerinfo, ALfloat value)
{
    REAL_alDopplerFactor(value);
}

static void run_alDopplerVelocity(const CallerInfo *callerinfo, ALfloat value)
{
    REAL_alDopplerVelocity(value);
}

static void run_alSpeedOfSound(const CallerInfo *callerinfo, ALfloat value)
{
    REAL_alSpeedOfSound(value);
}

static void run_alDistanceModel(const CallerInfo *callerinfo, ALenum model)
{
    REAL_alDistanceModel(model);
}

static void run_alEnable(const CallerInfo *callerinfo, ALenum capability)
{
    REAL_alEnable(capability);
}

static void run_alDisable(const CallerInfo *callerinfo, ALenum capability)
{
    REAL_alDisable(capability);
}

static void run_alIsEnabled(const CallerInfo *callerinfo, ALboolean retval, ALenum capability)
{
    REAL_alIsEnabled(capability);
}

static void run_alGetString(const CallerInfo *callerinfo, const ALchar *retval, const ALenum param)
{
    REAL_alGetString(param);
}

static void run_alGetBooleanv(const CallerInfo *callerinfo, ALenum param, ALboolean *origvalues, uint32 numvals, ALboolean *values)
{
    REAL_alGetBooleanv(param, values);
}

static void run_alGetIntegerv(const CallerInfo *callerinfo, ALenum param, ALint *origvalues, uint32 numvals, ALboolean isenum, ALint *values)
{
    REAL_alGetIntegerv(param, values);
}

static void run_alGetFloatv(const CallerInfo *callerinfo, ALenum param, ALfloat *origvalues, uint32 numvals, ALfloat *values)
{
    REAL_alGetFloatv(param, values);
}

static void run_alGetDoublev(const CallerInfo *callerinfo, ALenum param, ALdouble *origvalues, uint32 numvals, ALdouble *values)
{
    REAL_alGetDoublev(param, values);
}

static void run_alGetBoolean(const CallerInfo *callerinfo, ALboolean retval, ALenum param)
{
    REAL_alGetBoolean(param);
}

static void run_alGetInteger(const CallerInfo *callerinfo, ALint retval, ALenum param)
{
    REAL_alGetInteger(param);
}

static void run_alGetFloat(const CallerInfo *callerinfo, ALfloat retval, ALenum param)
{
    REAL_alGetFloat(param);
}

static void run_alGetDouble(const CallerInfo *callerinfo, ALdouble retval, ALenum param)
{
    REAL_alGetDouble(param);
}

static void run_alIsExtensionPresent(const CallerInfo *callerinfo, ALboolean retval, const ALchar *extname)
{
    REAL_alIsExtensionPresent(extname);
}

static void run_alGetError(const CallerInfo *callerinfo, ALenum retval)
{
    REAL_alGetError();
}

static void run_alGetProcAddress(const CallerInfo *callerinfo, void *retval, const ALchar *funcname)
{
    REAL_alGetProcAddress(funcname);
}

static void run_alGetEnumValue(const CallerInfo *callerinfo, ALenum retval, const ALchar *enumname)
{
    REAL_alGetEnumValue(enumname);
}

static void run_alListenerfv(const CallerInfo *callerinfo, ALenum param, const ALfloat *origvalues, uint32 numvals, const ALfloat *values)
{
    REAL_alListenerfv(param, values);
}

static void run_alListenerf(const CallerInfo *callerinfo, ALenum param, ALfloat value)
{
    REAL_alListenerf(param, value);
}

static void run_alListener3f(const CallerInfo *callerinfo, ALenum param, ALfloat value1, ALfloat value2, ALfloat value3)
{
    REAL_alListener3f(param, value1, value2, value3);
}

static void run_alListeneriv(const CallerInfo *callerinfo, ALenum param, const ALint *origvalues, uint32 numvals, const ALint *values)
{
    REAL_alListeneriv(param, values);
}

static void run_alListeneri(const CallerInfo *callerinfo, ALenum param, ALint value)
{
    REAL_alListeneri(param, value);
}

static void run_alListener3i(const CallerInfo *callerinfo, ALenum param, ALint value1, ALint value2, ALint value3)
{
    REAL_alListener3i(param, value1, value2, value3);
}

static void run_alGetListenerfv(const CallerInfo *callerinfo, ALenum param, ALfloat *origvalues, uint32 numvals, ALfloat *values)
{
    REAL_alGetListenerfv(param, values);
}

static void run_alGetListenerf(const CallerInfo *callerinfo, ALenum param, ALfloat *origvalue, ALfloat value)
{
    REAL_alGetListenerf(param, &value);
}

static void run_alGetListener3f(const CallerInfo *callerinfo, ALenum param, ALfloat *origvalue1, ALfloat *origvalue2, ALfloat *origvalue3, ALfloat value1, ALfloat value2, ALfloat value3)
{
    REAL_alGetListener3f(param, &value1, &value2, &value3);
}

static void run_alGetListeneri(const CallerInfo *callerinfo, ALenum param, ALint *origvalue, ALint value)
{
    REAL_alGetListeneri(param, &value);
}

static void run_alGetListeneriv(const CallerInfo *callerinfo, ALenum param, ALint *origvalues, uint32 numvals, ALint *values)
{
    REAL_alGetListeneriv(param, values);
}

static void run_alGetListener3i(const CallerInfo *callerinfo, ALenum param, ALint *origvalue1, ALint *origvalue2, ALint *origvalue3, ALint value1, ALint value2, ALint value3)
{
    REAL_alGetListener3i(param, &value1, &value2, &value3);
}

static void run_alGenSources(const CallerInfo *callerinfo, ALsizei n, ALuint *orignames, ALuint *names)
{
    ALsizei i;
    ALuint *realnames = (ALuint *) get_ioblob(sizeof (ALuint) * n);
    memset(realnames, '\0', sizeof (ALuint) * n);
    REAL_alGenSources(n, realnames);
    for (i = 0; i < n; i++) {
        if (!realnames[i] && names[i]) {
            fprintf(stderr, "Uhoh, we didn't generate enough sources!\n");
            fprintf(stderr, "This is probably going to cause playback problems.\n");
        } else if (names[i] && realnames[i]) {
            add_source_to_map(names[i], realnames[i]);
        }
    }
}

static void run_alDeleteSources(const CallerInfo *callerinfo, ALsizei n, const ALuint *orignames, const ALuint *names)
{
    ALsizei i;
    ALuint *realnames = (ALuint *) get_ioblob(sizeof (ALuint) * n);
    for (i = 0; i < n; i++) {
        realnames[i] = get_mapped_source(names[i]);
    }
    REAL_alDeleteSources(n, realnames);
}

static void run_alIsSource(const CallerInfo *callerinfo, ALboolean retval, ALuint name)
{
    REAL_alIsSource(get_mapped_source(name));
}

static void run_alSourcefv(const CallerInfo *callerinfo, ALuint name, ALenum param, const ALfloat *origvalues, uint32 numvals, const ALfloat *values)
{
    REAL_alSourcefv(get_mapped_source(name), param, values);
}

static void run_alSourcef(const CallerInfo *callerinfo, ALuint name, ALenum param, ALfloat value)
{
    REAL_alSourcef(get_mapped_source(name), param, value);
}

static void run_alSource3f(const CallerInfo *callerinfo, ALuint name, ALenum param, ALfloat value1, ALfloat value2, ALfloat value3)
{
    REAL_alSource3f(get_mapped_source(name), param, value1, value2, value3);
}

static void run_alSourceiv(const CallerInfo *callerinfo, ALuint name, ALenum param, const ALint *origvalues, uint32 numvals, const ALint *values)
{
    REAL_alSourceiv(get_mapped_source(name), param, values);
}

static void run_alSourcei(const CallerInfo *callerinfo, ALuint name, ALenum param, ALint value)
{
    REAL_alSourcei(get_mapped_source(name), param, value);
}

static void run_alSource3i(const CallerInfo *callerinfo, ALuint name, ALenum param, ALint value1, ALint value2, ALint value3)
{
    REAL_alSource3i(get_mapped_source(name), param, value1, value2, value3);
}

static void run_alGetSourcefv(const CallerInfo *callerinfo, ALuint name, ALenum param, ALfloat *origvalues, uint32 numvals, ALfloat *values)
{
    REAL_alGetSourcefv(get_mapped_source(name), param, values);
}

static void run_alGetSourcef(const CallerInfo *callerinfo, ALuint name, ALenum param, ALfloat *origvalue, ALfloat value)
{
    REAL_alGetSourcef(get_mapped_source(name), param, &value);
}

static void run_alGetSource3f(const CallerInfo *callerinfo, ALuint name, ALenum param, ALfloat *origvalue1, ALfloat *origvalue2, ALfloat *origvalue3, ALfloat value1, ALfloat value2, ALfloat value3)
{
    REAL_alGetSource3f(get_mapped_source(name), param, &value1, &value2, &value3);
}

static void run_alGetSourceiv(const CallerInfo *callerinfo, ALuint name, ALenum param, ALboolean isenum, ALint *origvalues, uint32 numvals, ALint *values)
{
    REAL_alGetSourceiv(get_mapped_source(name), param, values);
}

static void run_alGetSourcei(const CallerInfo *callerinfo, ALuint name, ALenum param, ALboolean isenum, ALint *origvalue, ALint value)
{
    REAL_alGetSourcei(get_mapped_source(name), param, &value);
}

static void run_alGetSource3i(const CallerInfo *callerinfo, ALuint name, ALenum param, ALint *origvalue1, ALint *origvalue2, ALint *origvalue3, ALint value1, ALint value2, ALint value3)
{
    REAL_alGetSource3i(get_mapped_source(name), param, &value1, &value2, &value3);
}

static void run_alSourcePlay(const CallerInfo *callerinfo, ALuint name)
{
    REAL_alSourcePlay(get_mapped_source(name));
}

static void run_alSourcePlayv(const CallerInfo *callerinfo, ALsizei n, const ALuint *orignames, const ALuint *names)
{
    ALsizei i;
    ALuint *realnames = (ALuint *) get_ioblob(sizeof (ALuint) * n);
    for (i = 0; i < n; i++) {
        realnames[i] = get_mapped_source(names[i]);
    }
    REAL_alSourcePlayv(n, realnames);
}

static void run_alSourcePause(const CallerInfo *callerinfo, ALuint name)
{
    REAL_alSourcePause(get_mapped_source(name));
}

static void run_alSourcePausev(const CallerInfo *callerinfo, ALsizei n, const ALuint *orignames, const ALuint *names)
{
    ALsizei i;
    ALuint *realnames = (ALuint *) get_ioblob(sizeof (ALuint) * n);
    for (i = 0; i < n; i++) {
        realnames[i] = get_mapped_source(names[i]);
    }
    REAL_alSourcePausev(n, realnames);
}

static void run_alSourceRewind(const CallerInfo *callerinfo, ALuint name)
{
    REAL_alSourceRewind(get_mapped_source(name));
}

static void run_alSourceRewindv(const CallerInfo *callerinfo, ALsizei n, const ALuint *orignames, const ALuint *names)
{
    ALsizei i;
    ALuint *realnames = (ALuint *) get_ioblob(sizeof (ALuint) * n);
    for (i = 0; i < n; i++) {
        realnames[i] = get_mapped_source(names[i]);
    }
    REAL_alSourceRewindv(n, realnames);
}

static void run_alSourceStop(const CallerInfo *callerinfo, ALuint name)
{
    REAL_alSourceStop(get_mapped_source(name));
}

static void run_alSourceStopv(const CallerInfo *callerinfo, ALsizei n, const ALuint *orignames, const ALuint *names)
{
    ALsizei i;
    ALuint *realnames = (ALuint *) get_ioblob(sizeof (ALuint) * n);
    for (i = 0; i < n; i++) {
        realnames[i] = get_mapped_source(names[i]);
    }
    REAL_alSourceStopv(n, realnames);
}

static void run_alSourceQueueBuffers(const CallerInfo *callerinfo, ALuint name, ALsizei nb, const ALuint *origbufnames, const ALuint *bufnames)
{
    ALsizei i;
    ALuint *realnames = (ALuint *) get_ioblob(sizeof (ALuint) * nb);
    for (i = 0; i < nb; i++) {
        realnames[i] = get_mapped_buffer(bufnames[i]);
    }
    REAL_alSourceQueueBuffers(get_mapped_source(name), nb, realnames);
}

static void run_alSourceUnqueueBuffers(const CallerInfo *callerinfo, ALuint name, ALsizei nb, ALuint *origbufnames, ALuint *bufnames)
{
    REAL_alSourceUnqueueBuffers(get_mapped_source(name), nb, bufnames);
}

static void run_alGenBuffers(const CallerInfo *callerinfo, ALsizei n, ALuint *orignames, ALuint *names)
{
    ALsizei i;
    ALuint *realnames = (ALuint *) get_ioblob(sizeof (ALuint) * n);
    memset(realnames, '\0', sizeof (ALuint) * n);
    REAL_alGenBuffers(n, realnames);
    for (i = 0; i < n; i++) {
        if (!realnames[i] && names[i]) {
            fprintf(stderr, "Uhoh, we didn't generate enough buffers!\n");
            fprintf(stderr, "This is probably going to cause playback problems.\n");
        } else if (names[i] && realnames[i]) {
            add_buffer_to_map(names[i], realnames[i]);
        }
    }
}

static void run_alDeleteBuffers(const CallerInfo *callerinfo, ALsizei n, const ALuint *orignames, const ALuint *names)
{
    ALsizei i;
    ALuint *realnames = (ALuint *) get_ioblob(sizeof (ALuint) * n);
    for (i = 0; i < n; i++) {
        realnames[i] = get_mapped_source(names[i]);
    }
    REAL_alDeleteBuffers(n, realnames);
}

static void run_alIsBuffer(const CallerInfo *callerinfo, ALboolean retval, ALuint name)
{
    REAL_alIsBuffer(get_mapped_buffer(name));
}

static void run_alBufferData(const CallerInfo *callerinfo, ALuint name, ALenum alfmt, const ALvoid *origdata, const ALvoid *data, ALsizei size, ALsizei freq)
{
    REAL_alBufferData(get_mapped_buffer(name), alfmt, data, size, freq);
}

static void run_alBufferfv(const CallerInfo *callerinfo, ALuint name, ALenum param, const ALfloat *origvalues, uint32 numvals, const ALfloat *values)
{
    REAL_alBufferfv(get_mapped_buffer(name), param, values);
}

static void run_alBufferf(const CallerInfo *callerinfo, ALuint name, ALenum param, ALfloat value)
{
    REAL_alBufferf(get_mapped_buffer(name), param, value);
}

static void run_alBuffer3f(const CallerInfo *callerinfo, ALuint name, ALenum param, ALfloat value1, ALfloat value2, ALfloat value3)
{
    REAL_alBuffer3f(get_mapped_buffer(name), param, value1, value2, value3);
}

static void run_alBufferiv(const CallerInfo *callerinfo, ALuint name, ALenum param, const ALint *origvalues, uint32 numvals, const ALint *values)
{
    REAL_alBufferiv(get_mapped_buffer(name), param, values);
}

static void run_alBufferi(const CallerInfo *callerinfo, ALuint name, ALenum param, ALint value)
{
    REAL_alBufferi(get_mapped_buffer(name), param, value);
}

static void run_alBuffer3i(const CallerInfo *callerinfo, ALuint name, ALenum param, ALint value1, ALint value2, ALint value3)
{
    REAL_alBuffer3i(get_mapped_buffer(name), param, value1, value2, value3);
}

static void run_alGetBufferfv(const CallerInfo *callerinfo, ALuint name, ALenum param, ALfloat *origvalues, uint32 numvals, ALfloat *values)
{
    REAL_alGetBufferfv(get_mapped_buffer(name), param, values);
}

static void run_alGetBufferf(const CallerInfo *callerinfo, ALuint name, ALenum param, ALfloat *origvalue, ALfloat value)
{
    REAL_alGetBufferf(get_mapped_buffer(name), param, &value);
}

static void run_alGetBuffer3f(const CallerInfo *callerinfo, ALuint name, ALenum param, ALfloat *origvalue1, ALfloat *origvalue2, ALfloat *origvalue3, ALfloat value1, ALfloat value2, ALfloat value3)
{
    REAL_alGetBuffer3f(get_mapped_buffer(name), param, &value1, &value2, &value3);
}

static void run_alGetBufferi(const CallerInfo *callerinfo, ALuint name, ALenum param, ALint *origvalue, ALint value)
{
    REAL_alGetBufferi(get_mapped_buffer(name), param, &value);
}

static void run_alGetBuffer3i(const CallerInfo *callerinfo, ALuint name, ALenum param, ALint *origvalue1, ALint *origvalue2, ALint *origvalue3, ALint value1, ALint value2, ALint value3)
{
    REAL_alGetBuffer3i(get_mapped_buffer(name), param, &value1, &value2, &value3);
}

static void run_alGetBufferiv(const CallerInfo *callerinfo, ALuint name, ALenum param, ALint *origvalues, uint32 numvals, ALint *values)
{
    REAL_alGetBufferiv(get_mapped_buffer(name), param, values);
}

static void run_alTracePushScope(const CallerInfo *callerinfo, const ALchar *str)
{
    if (REAL_alTracePushScope) { REAL_alTracePushScope(str); }
}

static void run_alTracePopScope(const CallerInfo *callerinfo)
{
    if (REAL_alTracePopScope) { REAL_alTracePopScope(); }
}

static void run_alTraceMessage(const CallerInfo *callerinfo, const ALchar *str)
{
    if (REAL_alTraceMessage) { REAL_alTraceMessage(str); }
}

static void run_alTraceBufferLabel(const CallerInfo *callerinfo, ALuint name, const ALchar *str)
{
    if (REAL_alTraceBufferLabel) { REAL_alTraceBufferLabel(get_mapped_buffer(name), str); }
}

static void run_alTraceSourceLabel(const CallerInfo *callerinfo, ALuint name, const ALchar *str)
{
    if (REAL_alTraceSourceLabel) { REAL_alTraceSourceLabel(get_mapped_source(name), str); }
}

static void run_alcTraceDeviceLabel(const CallerInfo *callerinfo, ALCdevice *device, const ALchar *str)
{
    if (REAL_alcTraceDeviceLabel) { REAL_alcTraceDeviceLabel(get_mapped_device(device), str); }
}

static void run_alcTraceContextLabel(const CallerInfo *callerinfo, ALCcontext *ctx, const ALchar *str)
{
    if (REAL_alcTraceContextLabel) { REAL_alcTraceContextLabel(get_mapped_context(ctx), str); }
}







static void dump_callerinfo(const CallerInfo *callerinfo, const char *fn)
{
    int i;

    if (dump_callers) {
        const int frames = callerinfo->num_callstack_frames;
        int framei;
        for (i = 0; i < callerinfo->trace_scope; i++) {
            printf("    ");
        }

        printf("Call from threadid = %u, stack = {\n", (uint) callerinfo->threadid);

        for (framei = 0; framei < frames; framei++) {
            void *ptr = callerinfo->callstack[framei].frame;
            const char *str = callerinfo->callstack[framei].sym;
            for (i = 0; i < callerinfo->trace_scope; i++) {
                printf("    ");
            }
            printf("    %s\n", str ? str : ptrString(ptr));
        }

        for (i = 0; i < callerinfo->trace_scope; i++) {
            printf("    ");
        }
        printf("}\n");
    }

    if (dump_calls) {
        for (i = 0; i < callerinfo->trace_scope; i++) {
            printf("    ");
        }
        printf("%s", fn);
    }
}


#define ENTRYPOINT(ret,name,params,args,visitparams,visitargs) \
    static void visit_##name visitparams { \
        dump_callerinfo(callerinfo, #name); \
        if (dump_calls) { dump_##name visitargs; } \
        if (run_calls) { run_##name visitargs; } \
        if (dumping) { fflush(stdout); } \
    }


#include "altrace_entrypoints.h"


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
            run_calls = 1;
        } else if (strcmp(arg, "--no-run") == 0) {
            run_calls = 0;
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

    init_altrace_playback(fname, run_calls);
    process_log();
    quit_altrace_playback();
    return 0;
}

// end of altrace_cli.c ...

