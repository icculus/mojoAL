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

static void get_alc_string(ALCdevice *device, const ALenum token, const char *tokenstr, const ALCboolean iserr)
{
    const char *str = alcGetString(device, token);
    check_openal_alc_error(device, "alcGetString");
    if (str && !device && ((token == ALC_DEVICE_SPECIFIER) || (token == ALC_CAPTURE_DEVICE_SPECIFIER))) {
        printf(" * %s:\n", tokenstr);
        while (*str) {
            printf("  - ");
            while (*str) {
                putchar(*str);
                str++;
            }
            printf("\n");
            str++;
        }
    } else if (iserr) {
        if (SDL_strcmp(str, tokenstr) != 0) {
            printf(" * %s reported INCORRECT STRING ('%s')!\n", tokenstr, str);
        }
    } else {
        printf(" * %s: %s\n", tokenstr, str);
    }
}

static void get_alc_strings(ALCdevice *device, const char *devname)
{
    printf("Strings for device %s ...\n", devname);
    #define GET_ALC_STRING(e) get_alc_string(device, e, #e, ALC_FALSE)
    #define GET_ALC_STRING_ERROR(e) get_alc_string(device, e, #e, ALC_TRUE)
    GET_ALC_STRING(ALC_EXTENSIONS);
    GET_ALC_STRING(ALC_DEFAULT_DEVICE_SPECIFIER);
    GET_ALC_STRING(ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER);
    GET_ALC_STRING(ALC_DEVICE_SPECIFIER);
    GET_ALC_STRING(ALC_CAPTURE_DEVICE_SPECIFIER);
    GET_ALC_STRING_ERROR(ALC_NO_ERROR);
    GET_ALC_STRING_ERROR(ALC_INVALID_DEVICE);
    GET_ALC_STRING_ERROR(ALC_INVALID_CONTEXT);
    GET_ALC_STRING_ERROR(ALC_INVALID_ENUM);
    GET_ALC_STRING_ERROR(ALC_INVALID_VALUE);
    GET_ALC_STRING_ERROR(ALC_OUT_OF_MEMORY);
    #undef GET_ALC_STRING
    #undef GET_ALC_STRING_ERROR
}


static void get_al_string(const ALenum token, const char *tokenstr, const ALboolean iserr)
{
    const char *str = alGetString(token);
    check_openal_error("alGetString");
    if (iserr) {
        if (SDL_strcmp(str, tokenstr) != 0) {
            printf(" * %s reported INCORRECT STRING ('%s')!\n", tokenstr, str);
        }
    } else {
        printf(" * %s: %s\n", tokenstr, str);
    }
}

static void get_al_strings(void)
{
    printf("Strings for the AL ...\n");
    #define GET_AL_STRING(e) get_al_string(e, #e, AL_FALSE)
    #define GET_AL_STRING_ERROR(e) get_al_string(e, #e, AL_TRUE)
    GET_AL_STRING(AL_EXTENSIONS);
    GET_AL_STRING(AL_VERSION);
    GET_AL_STRING(AL_RENDERER);
    GET_AL_STRING(AL_VENDOR);
    GET_AL_STRING_ERROR(AL_NO_ERROR);
    GET_AL_STRING_ERROR(AL_INVALID_NAME);
    GET_AL_STRING_ERROR(AL_INVALID_ENUM);
    GET_AL_STRING_ERROR(AL_INVALID_VALUE);
    GET_AL_STRING_ERROR(AL_INVALID_OPERATION);
    GET_AL_STRING_ERROR(AL_OUT_OF_MEMORY);
    #undef GET_AL_STRING
    #undef GET_AL_STRING_ERROR
}


int main(int argc, char **argv)
{
    const char *devname = argv[1];  /* may be NULL, that's okay. */
    ALCdevice *device;
    ALCcontext *context;

    check_openal_alc_error(NULL, "startup");

    get_alc_strings(NULL, "[null device]");

    device = alcOpenDevice(devname);
    if (!device)
    {
        printf("Couldn't open OpenAL device.\n");
        return 2;
    }
    check_openal_alc_error(NULL, "alcOpenDevice");

    get_alc_strings(device, devname ? devname : "[default device]");

    context = alcCreateContext(device, NULL);
    if (!context) {
        printf("Couldn't create OpenAL context.\n");
        alcCloseDevice(device);
        return 3;
    }
    check_openal_alc_error(device, "alcCreateContext");
        
    alcMakeContextCurrent(context);
    check_openal_alc_error(device, "alcMakeContextCurrent");

    get_al_strings();

    alcMakeContextCurrent(NULL);
    check_openal_alc_error(device, "alcMakeContextCurrent(NULL)");
    alcDestroyContext(context);
    check_openal_alc_error(device, "alcDestroyContext");
    alcCloseDevice(device);
    check_openal_alc_error(NULL, "alcCloseDevice");

    printf("Done!\n");
    return 0;
}

/* end of testopenalinfo.c ... */

