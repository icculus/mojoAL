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

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif


static LPALCTRACEDEVICELABEL palcTraceDeviceLabel;
static LPALCTRACECONTEXTLABEL palcTraceContextLabel;
static LPALTRACEPUSHSCOPE palTracePushScope;
static LPALTRACEPOPSCOPE palTracePopScope;
static LPALTRACEMESSAGE palTraceMessage;
static LPALTRACEBUFFERLABEL palTraceBufferLabel;
static LPALTRACESOURCELABEL palTraceSourceLabel;

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

typedef struct
{
    ALuint sid;
    int x;
    int y;
} obj;

/* !!! FIXME: eventually, add more sources and sounds. */
static obj objects[2];  /* one listener, one source. */
static int draggingobj = -1;

static int obj_under_mouse(const int x, const int y)
{
    const SDL_Point p = { x, y };
    const obj *o = objects;
    int i;
    for (i = 0; i < SDL_arraysize(objects); i++, o++) {
        const SDL_Rect r = { o->x - 25, o->y - 25, 50, 50 };
        if (SDL_PointInRect(&p, &r)) {
            return i;
        }
    }
    return -1;
}

static int mainloop(SDL_Renderer *renderer)
{
    int i;
    SDL_Event e;

    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT:
                return 0;

            case SDL_KEYDOWN:
                if (e.key.keysym.sym == SDLK_ESCAPE) {
                    return 0;
                }
                break;

            case SDL_MOUSEBUTTONUP:
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == 1) {
                    if (e.button.state == SDL_RELEASED) {
                        if (palTraceMessage) palTraceMessage("Mouse button released");
                        draggingobj = -1;
                    } else {
                        if (palTraceMessage) palTraceMessage("Mouse button pressed");
                        draggingobj = obj_under_mouse(e.button.x, e.button.y);
                    }
                }
                break;

            case SDL_MOUSEMOTION:
                if (draggingobj != -1) {
                    obj *o = &objects[draggingobj];
                    o->x = SDL_min(800, SDL_max(0, e.motion.x));
                    o->y = SDL_min(600, SDL_max(0, e.motion.y));
                    /* we are treating the 2D view as the X and Z coordinate, as if we're looking at it from above.
                       From this configuration, the Y coordinate would be depth, and we leave that at zero.
                       the listener's default "at" orientation is towards the north in this configuration, with its
                       "up" pointing at the camera. Since we are rendering the audio in relation to a listener we
                       move around in 2D space in the camera's view, it's obviously detached from the camera itself. */
                    if (o->sid == 0) {  /* it's the listener. */
                        alListener3f(AL_POSITION, ((o->x / 400.0f) - 1.0f) * 10.0f, 0.0f, ((o->y / 300.0f) - 1.0f) * 10.0f);
                    } else {
                        alSource3f(o->sid, AL_POSITION, ((o->x / 400.0f) - 1.0f) * 10.0f, 0.0f, ((o->y / 300.0f) - 1.0f) * 10.0f);
                    }
                }
                break;

        }
    }

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0xFF);
    SDL_RenderClear(renderer);

    for (i = 0; i < SDL_arraysize(objects); i++) {
        const obj *o = &objects[i];
        const SDL_Rect r = { o->x - 25, o->y - 25, 50, 50 };
        if (o->sid == 0) {
            SDL_SetRenderDrawColor(renderer, 0x00, 0xFF, 0x00, 0xFF);
        } else {
            SDL_SetRenderDrawColor(renderer, 0x00, 0x00, 0xFF, 0xFF);
        }
        SDL_RenderFillRect(renderer, &r);
    }

    SDL_RenderPresent(renderer);

    return 1;
}


#ifdef __EMSCRIPTEN__
static void emscriptenMainloop(void *arg)
{
    (void) mainloop((SDL_Renderer *) arg);
}
#endif


static void spatialize(SDL_Renderer *renderer, const char *fname)
{
    obj *o = objects;
    SDL_AudioSpec spec;
    ALenum alfmt = AL_NONE;
    Uint8 *buf = NULL;
    Uint32 buflen = 0;
    ALuint sid = 0;
    ALuint bid = 0;

    if (!SDL_LoadWAV(fname, &spec, &buf, &buflen)) {
        printf("Loading '%s' failed! %s\n", fname, SDL_GetError());
        return;
    } else if ((alfmt = get_openal_format(&spec)) == AL_NONE) {
        printf("Can't queue '%s', format not supported by the AL.\n", fname);
        SDL_FreeWAV(buf);
        return;
    }

    check_openal_error("startup");

    printf("Now queueing '%s'...\n", fname);

    if (palTracePushScope) palTracePushScope("Initial setup");

    alGenSources(1, &sid);
    if (check_openal_error("alGenSources")) {
        SDL_FreeWAV(buf);
        return;
    }

    if (palTraceSourceLabel) palTraceSourceLabel(sid, "Moving source");

    alGenBuffers(1, &bid);
    if (check_openal_error("alGenBuffers")) {
        alDeleteSources(1, &sid);
        check_openal_error("alDeleteSources");
        SDL_FreeWAV(buf);
        return;
    }

    if (palTraceBufferLabel) palTraceBufferLabel(bid, "Sound effect");

    alBufferData(bid, alfmt, buf, buflen, spec.freq);
    SDL_FreeWAV(buf);
    check_openal_error("alBufferData");
    alSourcei(sid, AL_BUFFER, bid);
    check_openal_error("alSourcei");
    alSourcei(sid, AL_LOOPING, AL_TRUE);
    check_openal_error("alSourcei");
    alSourcePlay(sid);
    check_openal_error("alSourcePlay");

    /* the listener. */
    o->sid = 0;
    o->x = 400;
    o->y = 300;
    alListener3f(AL_POSITION, ((o->x / 400.0f) - 1.0f) * 10.0f, 0.0f, ((o->y / 300.0f) - 1.0f) * 10.0f);
    o++;

    o->sid = sid;
    o->x = 400;
    o->y = 50;
    alSource3f(o->sid, AL_POSITION, ((o->x / 400.0f) - 1.0f) * 10.0f, 0.0f, ((o->y / 300.0f) - 1.0f) * 10.0f);
    o++;

    if (palTracePopScope) palTracePopScope();

    #ifdef __EMSCRIPTEN__
    emscripten_set_main_loop_arg(emscriptenMainloop, renderer, 0, 1);
    #else
    while (mainloop(renderer)) { /* go again */ }
    #endif


    //alSourcei(sid, AL_BUFFER, 0);  /* force unqueueing */
    alDeleteSources(1, &sid);
    check_openal_error("alDeleteSources");
    alDeleteBuffers(1, &bid);
    check_openal_error("alDeleteBuffers");
}


int main(int argc, char **argv)
{
    ALCdevice *device;
    ALCcontext *context;
    SDL_Window *window;
    SDL_Renderer *renderer;

    if (argc != 2) {
        fprintf(stderr, "USAGE: %s [wavfile]\n", argv[0]);
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO) == -1) {
        fprintf(stderr, "SDL_Init(SDL_INIT_VIDEO) failed: %s\n", SDL_GetError());
        return 2;
    }

    window = SDL_CreateWindow(argv[0], SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED, 800, 600,
                              SDL_WINDOW_RESIZABLE);

    if (!window) {
        fprintf(stderr, "SDL_CreateWindow() failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 3;
    }

    renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer() failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 4;
    }
    SDL_RenderSetLogicalSize(renderer, 800, 600);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0xFF);
    SDL_RenderClear(renderer);
    SDL_RenderPresent(renderer);

    device = alcOpenDevice(NULL);
    if (!device)
    {
        printf("Couldn't open OpenAL default device.\n");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 5;
    }

    if (alcIsExtensionPresent(device, "ALC_EXT_trace_info")) {
        palcTraceDeviceLabel = (LPALCTRACEDEVICELABEL) alcGetProcAddress(device, "alcTraceDeviceLabel");
        palcTraceContextLabel = (LPALCTRACECONTEXTLABEL) alcGetProcAddress(device, "alcTraceContextLabel");
    }

    context = alcCreateContext(device, NULL);
    if (!context) {
        printf("Couldn't create OpenAL context.\n");
        alcCloseDevice(device);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 6;
    }

    if (palcTraceDeviceLabel) palcTraceDeviceLabel(device, "The playback device");
    if (palcTraceContextLabel) palcTraceContextLabel(context, "Main context");

    alcMakeContextCurrent(context);

    if (alIsExtensionPresent("AL_EXT_trace_info")) {
        palTracePushScope = (LPALTRACEPUSHSCOPE) alGetProcAddress("alTracePushScope");
        palTracePopScope = (LPALTRACEPOPSCOPE) alGetProcAddress("alTracePopScope");
        palTraceMessage = (LPALTRACEMESSAGE) alGetProcAddress("alTraceMessage");
        palTraceBufferLabel = (LPALTRACEBUFFERLABEL) alGetProcAddress("alTraceBufferLabel");
        palTraceSourceLabel = (LPALTRACESOURCELABEL) alGetProcAddress("alTraceSourceLabel");
    }

    spatialize(renderer, argv[1]);

    alcMakeContextCurrent(NULL);
    alcDestroyContext(context);
    alcCloseDevice(device);


    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    printf("Done!\n");
    return 0;
}

/* end of testposition.c ... */

