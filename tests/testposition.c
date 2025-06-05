/**
 * MojoAL; a simple drop-in OpenAL implementation.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

// This is just test code, you don't need to compile this with MojoAL.

#include "AL/al.h"
#include "AL/alc.h"

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

static LPALCTRACEDEVICELABEL palcTraceDeviceLabel;
static LPALCTRACECONTEXTLABEL palcTraceContextLabel;
static LPALTRACEPUSHSCOPE palTracePushScope;
static LPALTRACEPOPSCOPE palTracePopScope;
static LPALTRACEMESSAGE palTraceMessage;
static LPALTRACEBUFFERLABEL palTraceBufferLabel;
static LPALTRACESOURCELABEL palTraceSourceLabel;

static ALCdevice *device = NULL;
static ALCcontext *context = NULL;
static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;

static bool check_openal_error(const char *where)
{
    const ALenum err = alGetError();
    if (err != AL_NONE) {
        SDL_Log("OpenAL Error at %s! %s (%u)", where, alGetString(err), (unsigned int) err);
        return true;
    }
    return false;
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

typedef struct
{
    ALuint sid;
    ALuint bid;
    float x;
    float y;
} obj;

// !!! FIXME: eventually, add more sources and sounds.
static obj objects[2];  // one listener, one source.
static int draggingobj = -1;

static int obj_under_mouse(const float x, const float y)
{
    const SDL_FPoint p = { x, y };
    const obj *o = objects;
    int i;
    for (i = 0; i < SDL_arraysize(objects); i++, o++) {
        const SDL_FRect r = { o->x - 25.0f, o->y - 25.0f, 50.0f, 50.0f };
        if (SDL_PointInRectFloat(&p, &r)) {
            return i;
        }
    }
    return -1;
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    SDL_SetAppMetadata("MojoAL testposition", "1.0", "org.icculus.mojoal-testposition");

    if (argc != 2) {
        SDL_Log("USAGE: %s [wavfile]", argv[0]);
        return SDL_APP_FAILURE;
    }

    const char *fname = argv[1];

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init(SDL_INIT_VIDEO) failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    if (!SDL_CreateWindowAndRenderer("MojoAL testposition", 800, 600, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        SDL_Log("Couldn't create window/renderer: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    SDL_SetRenderLogicalPresentation(renderer, 800, 600, SDL_LOGICAL_PRESENTATION_LETTERBOX);

    device = alcOpenDevice(NULL);
    if (!device) {
        SDL_Log("Couldn't open OpenAL default device.");
        return SDL_APP_FAILURE;
    }

    if (alcIsExtensionPresent(device, "ALC_EXT_trace_info")) {
        palcTraceDeviceLabel = (LPALCTRACEDEVICELABEL) alcGetProcAddress(device, "alcTraceDeviceLabel");
        palcTraceContextLabel = (LPALCTRACECONTEXTLABEL) alcGetProcAddress(device, "alcTraceContextLabel");
    }

    context = alcCreateContext(device, NULL);
    if (!context) {
        SDL_Log("Couldn't create OpenAL context.");
        return SDL_APP_FAILURE;
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

    obj *o = objects;
    SDL_AudioSpec spec;
    ALenum alfmt = AL_NONE;
    Uint8 *buf = NULL;
    Uint32 buflen = 0;
    ALuint sid = 0;
    ALuint bid = 0;

    if (!SDL_LoadWAV(fname, &spec, &buf, &buflen)) {
        SDL_Log("Loading '%s' failed! %s", fname, SDL_GetError());
        return SDL_APP_FAILURE;
    } else if ((alfmt = get_openal_format(&spec)) == AL_NONE) {
        SDL_Log("Can't queue '%s', format not supported by the AL.", fname);
        SDL_free(buf);
        return SDL_APP_FAILURE;
    }

    check_openal_error("startup");

    SDL_Log("Now queueing '%s'...", fname);

    if (palTracePushScope) palTracePushScope("Initial setup");

    alGenSources(1, &sid);
    if (check_openal_error("alGenSources")) {
        SDL_free(buf);
        return SDL_APP_FAILURE;
    }

    if (palTraceSourceLabel) palTraceSourceLabel(sid, "Moving source");

    alGenBuffers(1, &bid);
    if (check_openal_error("alGenBuffers")) {
        alDeleteSources(1, &sid);
        check_openal_error("alDeleteSources");
        SDL_free(buf);
        return SDL_APP_FAILURE;
    }

    if (palTraceBufferLabel) palTraceBufferLabel(bid, "Sound effect");

    alBufferData(bid, alfmt, buf, buflen, spec.freq);
    SDL_free(buf);
    check_openal_error("alBufferData");
    alSourcei(sid, AL_BUFFER, bid);
    check_openal_error("alSourcei");
    alSourcei(sid, AL_LOOPING, AL_TRUE);
    check_openal_error("alSourcei");

    // the listener.
    o->sid = 0;
    o->bid = 0;
    o->x = 400.0f;
    o->y = 300.0f;
    alListener3f(AL_POSITION, ((o->x / 400.0f) - 1.0f) * 10.0f, 0.0f, ((o->y / 300.0f) - 1.0f) * 10.0f);
    o++;

    o->sid = sid;
    o->bid = bid;
    o->x = 400.0f;
    o->y = 50.0f;
    alSource3f(o->sid, AL_POSITION, ((o->x / 400.0f) - 1.0f) * 10.0f, 0.0f, ((o->y / 300.0f) - 1.0f) * 10.0f);
    o++;

    alSourcePlay(sid);
    check_openal_error("alSourcePlay");

    if (palTracePopScope) palTracePopScope();

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    switch (event->type) {
        case SDL_EVENT_QUIT:
            return SDL_APP_SUCCESS;

        case SDL_EVENT_KEY_DOWN:
            if (event->key.key == SDLK_ESCAPE) {
                return SDL_APP_SUCCESS;
            }
            break;

        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (event->button.button == 1) {
                if (!event->button.down) {
                    if (palTraceMessage) palTraceMessage("Mouse button released");
                    draggingobj = -1;
                } else {
                    if (palTraceMessage) palTraceMessage("Mouse button pressed");
                    draggingobj = obj_under_mouse(event->button.x, event->button.y);
                }
            }
            break;

        case SDL_EVENT_MOUSE_MOTION:
            if (draggingobj != -1) {
                obj *o = &objects[draggingobj];
                o->x = SDL_min(800.0f, SDL_max(0.0f, event->motion.x));
                o->y = SDL_min(600.0f, SDL_max(0.0f, event->motion.y));
                /* we are treating the 2D view as the X and Z coordinate, as if we're looking at it from above.
                   From this configuration, the Y coordinate would be depth, and we leave that at zero.
                   the listener's default "at" orientation is towards the north in this configuration, with its
                   "up" pointing at the camera. Since we are rendering the audio in relation to a listener we
                   move around in 2D space in the camera's view, it's obviously detached from the camera itself. */
                if (o->sid == 0) {  // it's the listener.
                    alListener3f(AL_POSITION, ((o->x / 400.0f) - 1.0f) * 10.0f, 0.0f, ((o->y / 300.0f) - 1.0f) * 10.0f);
                    check_openal_error("alListener3f");
                } else {
                    alSource3f(o->sid, AL_POSITION, ((o->x / 400.0f) - 1.0f) * 10.0f, 0.0f, ((o->y / 300.0f) - 1.0f) * 10.0f);
                    check_openal_error("alSource3f");
                }
            }
            break;
    }

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0xFF);
    SDL_RenderClear(renderer);

    for (int i = 0; i < SDL_arraysize(objects); i++) {
        const obj *o = &objects[i];
        const SDL_FRect r = { o->x - 25, o->y - 25, 50, 50 };
        if (o->sid == 0) {
            SDL_SetRenderDrawColor(renderer, 0x00, 0xFF, 0x00, 0xFF);
        } else {
            SDL_SetRenderDrawColor(renderer, 0x00, 0x00, 0xFF, 0xFF);
        }
        SDL_RenderFillRect(renderer, &r);
    }

    SDL_RenderPresent(renderer);
    return SDL_APP_CONTINUE;
}

// This function runs once at shutdown.
void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    for (int i = 0; i < SDL_arraysize(objects); i++) {
        obj *o = &objects[i];
        if (o->sid) {
            alSourceStop(o->sid);
            check_openal_error("alSourceStop");
            alSourcei(o->sid, AL_BUFFER, 0);
            check_openal_error("alSourcei");
            alDeleteSources(1, &o->sid);
            check_openal_error("alDeleteSources");
        }
        if (o->bid) {
            alDeleteBuffers(1, &o->bid);
            check_openal_error("alDeleteBuffers");
        }
    }

    if (context) {
        alcMakeContextCurrent(NULL);
        alcDestroyContext(context);
    }

    if (device) {
        alcCloseDevice(device);
    }

    if (result == SDL_APP_SUCCESS) {
        SDL_Log("Done!");
    }
}

// end of testposition.c ...

