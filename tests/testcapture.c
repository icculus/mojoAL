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

static int check_openal_alc_error(ALCdevice *device, const char *where)
{
    const ALCenum err = alcGetError(device);
    if (err != AL_NONE) {
        printf("ALC Error at %s! %s (%u)\n", where, alcGetString(device, err), (unsigned int) err);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    #define alfmt AL_FORMAT_MONO16
    #define cfmt Sint16
    #define freq 44100
    #define total_samples freq * 5
    static cfmt buf[total_samples];
    ALenum alc_connected = 0;
    ALCint connected = ALC_TRUE;
    ALCdevice *device;
    ALCdevice *capture;
    ALCcontext *context;
    ALint samples, state;
    ALuint sid, bid;

    device = alcOpenDevice(NULL);
    check_openal_alc_error(device, "alcOpenDevice");
    if (!device) {
        printf("Couldn't open OpenAL default device.\n");
        return 2;
    }

    context = alcCreateContext(device, NULL);
    check_openal_alc_error(device, "alcCreateContext");
    if (!context) {
        printf("Couldn't create OpenAL context.\n");
        alcCloseDevice(device);
        return 3;
    }
        
    alcMakeContextCurrent(context);
    check_openal_alc_error(device, "alcMakeContextCurrent");

    capture = alcCaptureOpenDevice(NULL, freq, alfmt, total_samples);
    check_openal_alc_error(capture, "alcCaptureOpenDevice");
    if (!capture) {
        printf("Couldn't open OpenAL default capture device.\n");
        return 4;
    }

    if (alcIsExtensionPresent(capture, "ALC_EXT_DISCONNECT")) {
        alc_connected = alcGetEnumValue(capture, "ALC_CONNECTED");
    }

    printf("recording...\n");
    alcCaptureStart(capture);
    check_openal_alc_error(capture, "alcCaptureStart");

    do {
        SDL_Delay(100);
        alcGetIntegerv(capture, ALC_CAPTURE_SAMPLES, 1, &samples);
        check_openal_alc_error(capture, "alcGetIntegerv");
        if (alc_connected != 0) {
            alcGetIntegerv(capture, alc_connected, 1, &connected);
            check_openal_alc_error(capture, "alcGetIntegerv");
        }
    } while (connected && (samples < total_samples));

    if (!connected) {
        printf("(Uhoh, recording device was disconnected! Carrying on...)\n");
    }

    alcCaptureSamples(capture, buf, samples);
    check_openal_alc_error(capture, "alcCaptureSamples");
    alcCaptureStop(capture);
    check_openal_alc_error(capture, "alcCaptureStop");
    alcCaptureCloseDevice(capture);
    check_openal_alc_error(NULL, "alcCaptureCloseDevice");

    alGenSources(1, &sid);
    check_openal_error("alGenSources");
    alGenBuffers(1, &bid);
    check_openal_error("alGenBuffers");

    printf("Playing...\n");

    alBufferData(bid, alfmt, buf, sizeof (buf), freq);
    check_openal_error("alBufferData");
    alSourcei(sid, AL_BUFFER, bid);
    check_openal_error("alSourcei");
    alSourcePlay(sid);
    check_openal_error("alSourcePlay");

    do {
        SDL_Delay(100);
        alGetSourceiv(sid, AL_SOURCE_STATE, &state);
        check_openal_error("alGetSourceiv");
    } while (state == AL_PLAYING);

    if (alcIsExtensionPresent(device, "ALC_EXT_DISCONNECT")) {
        alc_connected = alcGetEnumValue(device, "ALC_CONNECTED");
        check_openal_alc_error(device, "alcGetEnumValue");
        alcGetIntegerv(device, alc_connected, 1, &connected);
        check_openal_alc_error(device, "alcGetIntegerv");
        if (!connected) {
            printf("(Uhoh, playback device was disconnected!)\n");
        }
    }

    printf("Cleaning up...\n");

    alDeleteSources(1, &sid);
    check_openal_error("alDeleteSources");
    alDeleteBuffers(1, &bid);
    check_openal_error("alDeleteBuffers");

    alcMakeContextCurrent(NULL);
    check_openal_alc_error(device, "alcMakeContextCurrent");
    alcDestroyContext(context);
    check_openal_alc_error(device, "alcDestroyContext");
    alcCloseDevice(device);
    check_openal_alc_error(NULL, "alcCloseDevice");

    printf("Done!\n");
    return 0;
}

/* end of testcapture.c ... */

