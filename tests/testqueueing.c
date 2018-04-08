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
#include "SDL.h"

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
    if ((spec->channels == 1) && (spec->format == AUDIO_U8)) {
        return AL_FORMAT_MONO8;
    } else if ((spec->channels == 1) && (spec->format == AUDIO_S16SYS)) {
        return AL_FORMAT_MONO16;
    } else if ((spec->channels == 2) && (spec->format == AUDIO_U8)) {
        return AL_FORMAT_STEREO8;
    } else if ((spec->channels == 2) && (spec->format == AUDIO_S16SYS)) {
        return AL_FORMAT_STEREO16;
    } else if ((spec->channels == 1) && (spec->format == AUDIO_F32SYS)) {
        return alIsExtensionPresent("AL_EXT_FLOAT32") ? alGetEnumValue("AL_FORMAT_MONO_FLOAT32") : AL_NONE;
    } else if ((spec->channels == 2) && (spec->format == AUDIO_F32SYS)) {
        return alIsExtensionPresent("AL_EXT_FLOAT32") ? alGetEnumValue("AL_FORMAT_STEREO_FLOAT32") : AL_NONE;
    }
    return AL_NONE;
}

static int queueBuffer(const ALuint sid, const ALuint bid, const ALenum alfmt, const ALsizei freq, const Uint8 **ptr, Uint32 *remain)
{
    const Uint32 cpy = SDL_min(512, *remain);
    if (!cpy) {
        return 0;
    }

    alBufferData(bid, alfmt, *ptr, cpy, freq);
    if (check_openal_error("alBufferData")) {
        return 0;
    }

    alSourceQueueBuffers(sid, 1, &bid);
    if (check_openal_error("alSourceQueueBuffers")) {
        return 0;
    }

    *remain -= cpy;
    *ptr += cpy;
    return 1;
}

static void queuewav(ALCdevice *device, const char *fname)
{
    SDL_AudioSpec spec;
    ALenum alfmt = AL_NONE;
    Uint8 *buf = NULL;
    Uint32 buflen = 0;
    Uint32 remain = 0;
    const Uint8 *ptr = NULL;
    ALuint sid = 0;
    ALuint buffers[32];
    ALsizei i;
    ALenum alc_connected = 0;

    if (!SDL_LoadWAV(fname, &spec, &buf, &buflen)) {
        printf("Loading '%s' failed! %s\n", fname, SDL_GetError());
        return;
    } else if ((alfmt = get_openal_format(&spec)) == AL_NONE) {
        printf("Can't queue '%s', format not supported by the AL.\n", fname);
        SDL_FreeWAV(buf);
        return;
    }

    check_openal_error("startup");

    if (alcIsExtensionPresent(device, "ALC_EXT_DISCONNECT")) {
        alc_connected = alcGetEnumValue(device, "ALC_CONNECTED");
    }

    printf("Now queueing '%s'...\n", fname);

    alGenSources(1, &sid);
    if (check_openal_error("alGenSources")) {
        SDL_FreeWAV(buf);
        return;
    }

    alGenBuffers(SDL_arraysize(buffers), buffers);
    if (check_openal_error("alGenBuffers")) {
        alDeleteSources(1, &sid);
        check_openal_error("alDeleteSources");
        SDL_FreeWAV(buf);
        return;
    }

    remain = buflen;
    ptr = buf;

    for (i = 0; (remain > 0) && (i < SDL_arraysize(buffers)); i++) {
        if (!queueBuffer(sid, buffers[i], alfmt, spec.freq, &ptr, &remain)) {
            alDeleteSources(1, &sid);
            check_openal_error("alDeleteSources");
            alDeleteBuffers(SDL_arraysize(buffers), buffers);
            check_openal_error("alDeleteBuffers");
            SDL_FreeWAV(buf);
            return;
        }
    }

    alSourcePlay(sid);
    if (check_openal_error("alSourcePlay")) {
        alDeleteSources(1, &sid);
        check_openal_error("alDeleteSources");
        alDeleteBuffers(SDL_arraysize(buffers), buffers);
        check_openal_error("alDeleteBuffers");
        SDL_FreeWAV(buf);
        return;
    }

    while (AL_TRUE) {
        ALint processed = 0;
        int failed = 0;

        alGetSourceiv(sid, AL_BUFFERS_PROCESSED, &processed);
        failed |= check_openal_error("alGetSourceiv");

        while (!failed && (processed > 0)) {
            ALuint bid = 0;
            alSourceUnqueueBuffers(sid, 1, &bid);
            failed |= check_openal_error("alSourceUnqueueBuffers");
            if (!failed && (bid != 0) && (remain > 0)) {
                if (!queueBuffer(sid, bid, alfmt, spec.freq, &ptr, &remain)) {
                    failed = 1;
                } else {
                    printf("Requeued buffer %u (%d to go).\n", (unsigned int) bid, (int) processed);
                }
            }
            processed--;
        }

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
                    if (remain > 0) {
                        printf("Buffer queue starvation! Restarting source.\n");
                        alSourcePlay(sid);
                        failed |= check_openal_error("alSourcePlay");
                    } else {
                        printf("Source stopped because we played everything! Yay!\n");
                        break;
                    }
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
    alDeleteBuffers(SDL_arraysize(buffers), buffers);
    check_openal_error("alDeleteBuffers");
    SDL_FreeWAV(buf);
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

    for (i = 1; i < argc; i++) {
        queuewav(device, argv[i]);
    }

    alcMakeContextCurrent(NULL);
    alcDestroyContext(context);
    alcCloseDevice(device);

    printf("Done!\n");
    return 0;
}

/* end of testqueueing.c ... */

