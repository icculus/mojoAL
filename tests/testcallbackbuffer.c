/**
 * MojoAL; a simple drop-in OpenAL implementation.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

/* This is just test code, you don't need to compile this with MojoAL. */

#include <stdio.h>

#include "AL/al.h"
#include "AL/alc.h"

#include <SDL3/SDL.h>

typedef ALsizei (AL_APIENTRY *ALBUFFERCALLBACKTYPESOFT)(ALvoid *userptr, ALvoid *sampledata, ALsizei numbytes);
typedef void (AL_APIENTRY *LPALBUFFERCALLBACKSOFT)(ALuint buffer, ALenum format, ALsizei freq, ALBUFFERCALLBACKTYPESOFT callback, ALvoid *userptr);
static LPALBUFFERCALLBACKSOFT palBufferCallbackSOFT = NULL;


typedef struct BufferCallbackData
{
    Uint8 *wav;
    Uint32 remaining_bytes;
} BufferCallbackData;

static ALsizei AL_APIENTRY buffer_callback(ALvoid *userptr, ALvoid *sampledata, ALsizei numbytes)
{
    BufferCallbackData *data = (BufferCallbackData *) userptr;
    const Uint32 cpy = SDL_min(((Uint32) numbytes), data->remaining_bytes);
    SDL_Log("buffer_callback! want=%d have=%d cpy=%d", (int) numbytes, (int) data->remaining_bytes, (int) cpy);
    SDL_memcpy(sampledata, data->wav, (size_t) cpy);
    data->wav += cpy;
    data->remaining_bytes -= cpy;
    return cpy;
}

static int check_openal_error(const char *where)
{
    const ALenum err = alGetError();
    if (err != AL_NONE) {
        printf("OpenAL Error at %s! %s (%u)\n", where, alGetString(err), (unsigned int) err);
        return 1;
    }
    return 0;
}

static ALenum get_openal_format(const SDL_AudioSpec *spec)
{
    if ((spec->channels == 1) && (spec->format == SDL_AUDIO_U8)) {
        return AL_FORMAT_MONO8;
    } else if ((spec->channels == 1) && (spec->format == SDL_AUDIO_S16)) {
        return AL_FORMAT_MONO16;
    } else if ((spec->channels == 2) && (spec->format == SDL_AUDIO_U8)) {
        return AL_FORMAT_STEREO8;
    } else if ((spec->channels == 2) && (spec->format == SDL_AUDIO_S16)) {
        return AL_FORMAT_STEREO16;
    } else if ((spec->channels == 1) && (spec->format == SDL_AUDIO_F32)) {
        return alIsExtensionPresent("AL_EXT_FLOAT32") ? alGetEnumValue("AL_FORMAT_MONO_FLOAT32") : AL_NONE;
    } else if ((spec->channels == 2) && (spec->format == SDL_AUDIO_F32)) {
        return alIsExtensionPresent("AL_EXT_FLOAT32") ? alGetEnumValue("AL_FORMAT_STEREO_FLOAT32") : AL_NONE;
    } else if ((spec->channels == 1) && (spec->format == SDL_AUDIO_S32)) {
        return alIsExtensionPresent("AL_EXT_32bit_formats") ? alGetEnumValue("AL_FORMAT_MONO_I32") : AL_NONE;
    } else if ((spec->channels == 2) && (spec->format == SDL_AUDIO_F32)) {
        return alIsExtensionPresent("AL_EXT_32bit_formats") ? alGetEnumValue("AL_FORMAT_STEREO_I32") : AL_NONE;
    }
    return AL_NONE;
}

static void queuewav(ALCdevice *device, const char *fname)
{
    ALenum alc_connected = 0;
    BufferCallbackData cbdata;
    ALenum alfmt = AL_NONE;
    SDL_AudioSpec spec;
    Uint8 *buf = NULL;
    Uint32 buflen = 0;
    ALuint sid = 0;
    ALuint bid = 0;

    if (!SDL_LoadWAV(fname, &spec, &buf, &buflen)) {
        printf("Loading '%s' failed! %s\n", fname, SDL_GetError());
        return;
    } else if ((alfmt = get_openal_format(&spec)) == AL_NONE) {
        printf("Can't queue '%s', format not supported by the AL.\n", fname);
        SDL_free(buf);
        return;
    }

    check_openal_error("startup");

    if (alcIsExtensionPresent(device, "ALC_EXT_DISCONNECT")) {
        alc_connected = alcGetEnumValue(device, "ALC_CONNECTED");
    }

    printf("Now queueing '%s'...\n", fname);

    alGenSources(1, &sid);
    if (check_openal_error("alGenSources")) {
        SDL_free(buf);
        return;
    }

    alGenBuffers(1, &bid);
    if (check_openal_error("alGenBuffers")) {
        alDeleteSources(1, &sid);
        check_openal_error("alDeleteSources");
        SDL_free(buf);
        return;
    }

    cbdata.wav = buf;
    cbdata.remaining_bytes = buflen;
    palBufferCallbackSOFT(bid, alfmt, spec.freq, buffer_callback, &cbdata);
    if (check_openal_error("alBufferCallbackSOFT")) {
        alDeleteSources(1, &sid);
        check_openal_error("alDeleteSources");
        alDeleteBuffers(1, &bid);
        check_openal_error("alDeleteBuffers");
        SDL_free(buf);
        return;
    }

    alSourcei(sid, AL_BUFFER, bid);
    if (check_openal_error("alSourcei")) {
        alDeleteSources(1, &sid);
        check_openal_error("alDeleteSources");
        alDeleteBuffers(1, &bid);
        check_openal_error("alDeleteBuffers");
        SDL_free(buf);
        return;
    }

    alSourcePlay(sid);
    if (check_openal_error("alSourcePlay")) {
        alDeleteSources(1, &sid);
        check_openal_error("alDeleteSources");
        alDeleteBuffers(1, &bid);
        check_openal_error("alDeleteBuffers");
        SDL_free(buf);
        return;
    }

    while (AL_TRUE) {
        ALint processed = 0;
        int failed = 0;

        if (alc_connected != 0) {
            ALCint connected = 0;
            alcGetIntegerv(device, alc_connected, 1, &connected);
            if (!connected) {
                printf("Device is apparently disconnected!\n");
                failed = 1;
            }
        }

        if (!failed) {
            ALint state = 0;
            alGetSourceiv(sid, AL_SOURCE_STATE, &state);
            failed |= check_openal_error("alGetSourceiv");
            if (!failed) {
                if (state != AL_PLAYING) {
                    printf("Source stopped because we played everything! Yay!\n");
                    break;
                }
            }
        }

        if (failed) {
            alSourceStop(sid);
            check_openal_error("alSourceStop");
            break;
        }

        SDL_Delay(10);
    }


    //alSourcei(sid, AL_BUFFER, 0);  /* force unqueueing */
    alDeleteSources(1, &sid);
    check_openal_error("alDeleteSources");
    alDeleteBuffers(1, &bid);
    check_openal_error("alDeleteBuffers");
    SDL_free(buf);
}



int main(int argc, char **argv)
{
    ALCdevice *device;
    ALCcontext *context;
    int i;

    if (argc == 1) {
        fprintf(stderr, "USAGE: %s [wavfile1] [...] [wavfileN]\n", argv[0]);
        return 1;
    }

    device = alcOpenDevice(NULL);
    if (!device)
    {
        printf("Couldn't open OpenAL default device.\n");
        return 2;
    }

    context = alcCreateContext(device, NULL);
    if (!context) {
        printf("Couldn't create OpenAL context.\n");
        alcCloseDevice(device);
        return 3;
    }
        
    alcMakeContextCurrent(context);

    if (!alIsExtensionPresent("AL_SOFT_callback_buffer") || ((palBufferCallbackSOFT = (LPALBUFFERCALLBACKSOFT) alGetProcAddress("alBufferCallbackSOFT")) == NULL)) {
        printf("AL_SOFT_callback_buffer not supported.\n");
        alcMakeContextCurrent(NULL);
        alcDestroyContext(context);
        alcCloseDevice(device);
        return 4;
    }

    for (i = 1; i < argc; i++) {
        queuewav(device, argv[i]);
    }

    alcMakeContextCurrent(NULL);
    alcDestroyContext(context);
    alcCloseDevice(device);

    printf("Done!\n");
    return 0;
}

/* end of testcallbackbuffer.c ... */

