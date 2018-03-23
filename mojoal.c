#include <stdio.h>
#include "AL/al.h"
#include "AL/alc.h"
#include "SDL.h"

#define OPENAL_VERSION_MAJOR 1
#define OPENAL_VERSION_MINOR 1
#define OPENAL_VERSION_STRING2(major, minor) #major "." #minor

/* !!! FIXME: make some decisions about VENDOR and RENDERER strings here */
#define OPENAL_VERSION_STRING OPENAL_VERSION_STRING2(OPENAL_VERSION_MAJOR, OPENAL_VERSION_MINOR)
#define OPENAL_VENDOR_STRING "Ryan C. Gordon"
#define OPENAL_RENDERER_STRING "mojoAL"

#define DEFAULT_PLAYBACK_DEVICE "Default OpenAL playback device"
#define DEFAULT_CAPTURE_DEVICE "Default OpenAL capture device"

/* Some OpenAL apps (incorrectly) generate sources in a loop at startup until
   it fails. We set an upper limit to protect against this behavior, which
   also lets us not need to worry about locking in case another thread would
   need to realloc a growing source array. */
#ifndef OPENAL_MAX_SOURCES
#define OPENAL_MAX_SOURCES 128
#endif

/* Number of buffers to allocate at once when we need a new block during alGenBuffers(). */
#ifndef OPENAL_BUFFER_BLOCK_SIZE
#define OPENAL_BUFFER_BLOCK_SIZE 256
#endif

/* !!! FIXME: update this
  The locking strategy for this OpenAL implementation is complicated.
  Not only do we generally want you to be able to call into OpenAL from any
  thread, we'll always have to compete with the SDL audio device thread.
  However, we don't want to just throw a big mutex around the whole thing,
  because not only can we safely touch two unrelated objects at the same
  time, but also because the mixer might make your simple state change call
  on the main thread block for several milliseconds if your luck runs out,
  killing your framerate. Here's the basic plan:

- The current context is an atomic pointer, so even if there's a MakeCurrent
  while an operation is in progress, the operation will either get the new
  current or the previous current and set state on whichever. This should
  protect everything but context destruction, but if you are still calling
  into the AL while destroying the context, shame on you (even there, the
  first thing context destruction will do is make the context no-longer
  current, which means the race window is pretty small).

- Source and Buffer objects, once generated, aren't freed. If deleted, we
  atomically mark them as available for reuse, but the pointers never change
  or go away until the AL context (for sources) or device (for buffers) does.

- Since we have a maximum source count, to protect against apps that allocate
  sources in a loop until they fail, the source name array is static within
  the ALCcontext object, and thus doesn't need a lock for access. Buffer
  objects can be generated until we run out of memory, so that array needs to
  be dynamic and have a lock, though. When generating sources, we walk the
  static array and compare-and-swap the "allocated" field from 0 (available)
  to 2 (temporarily claimed), filling in the temporarily-claimed names in
  the array passed to alGenSources(). If we run out of sources, we walk back
  over this array, setting all the allocated fields back to zero for other
  threads to be able to claim, set the error state, and zero out the array.
  If we have enough sources, we walk the array and set the allocated fields
  to 1 (permanently claimed).

- Buffers are allocated in blocks of OPENAL_BUFFER_BLOCK_SIZE, each block
  linked-listed to the next, as allocated. These blocks are never deallocated
  as long as the device lives, so they don't need a lock to access (and adding
  a new block is an atomic pointer compare-and-swap). Allocating buffers uses
  the same compare-and-swap marking technique that allocating sources does,
  just in these buffer blocks instead of a static array. We don't (currently)
  keep a ALuint name index array of buffers, but instead walk the list of
  blocks, OPENAL_BUFFER_BLOCK_SIZE at a time, until we find our target block,
  and then index into that.

- Buffer data is owned by the AL, but we need a lock around it, in case an
  alBufferData() call happens while mixing. !!! FIXME: maybe not...

- Buffer queues are a hot mess. alSourceQueueBuffers will build a linked
  list of buffers, then atomically move this list into position for the
  mixer to obtain it. The mixer will process this list without the need
  to be atomic (as it owns it). As processed, it moves them atomically to
  a linked list that other threads can pick up for alSourceUnqueueBuffers.
  The problem with unqueueing is that multiple threads can compete. Unlike
  queueing, where we don't care which thread wins the race to queue,
  unqueueing _must_ return buffer names in the order they were mixed,
  according to the spec, which means we need a lock. But! we only need to
  serialize the alSourceUnqueueBuffers callers, not the mixer thread, and
  only long enough to unqueue items from the list.

- Capture just locks the SDL audio device for everything, since it's a very
  lightweight load and a much simplified API; good enough.
*/


#if 0
#define FIXME(x)
#else
#define FIXME(x) { \
    static int seen = 0; \
    if (!seen) { \
        seen = 1; \
        fprintf(stderr, "FIXME: %s (%s@%s:%d)\n", x, __FUNCTION__, __FILE__, __LINE__); \
    } \
}
#endif

/* lifted this ring buffer code from my al_osx project; I wrote it all, so it's stealable. */
typedef struct
{
    ALCubyte *buffer;
    ALCsizei size;
    ALCsizei write;
    ALCsizei read;
    ALCsizei used;
} RingBuffer;

static void ring_buffer_put(RingBuffer *ring, const void *_data, const ALCsizei size)
{
    const ALCubyte *data = (const ALCubyte *) _data;
    ALCsizei cpy;
    ALCsizei avail;

    if (!size)   /* just in case... */
        return;

    /* Putting more data than ring buffer holds in total? Replace it all. */
    if (size > ring->size) {
        ring->write = 0;
        ring->read = 0;
        ring->used = ring->size;
        SDL_memcpy(ring->buffer, data + (size - ring->size), ring->size);
        return;
    }

    /* Buffer overflow? Push read pointer to oldest sample not overwritten... */
    avail = ring->size - ring->used;
    if (size > avail) {
        ring->read += size - avail;
        if (ring->read > ring->size)
            ring->read -= ring->size;
    }

    /* Clip to end of buffer and copy first block... */
    cpy = ring->size - ring->write;
    if (size < cpy)
        cpy = size;
    if (cpy) SDL_memcpy(ring->buffer + ring->write, data, cpy);

    /* Wrap around to front of ring buffer and copy remaining data... */
    avail = size - cpy;
    if (avail) SDL_memcpy(ring->buffer, data + cpy, avail);

    /* Update write pointer... */
    ring->write += size;
    if (ring->write > ring->size)
        ring->write -= ring->size;

    ring->used += size;
    if (ring->used > ring->size)
        ring->used = ring->size;
}


static ALCsizei ring_buffer_get(RingBuffer *ring, void *_data, ALCsizei size)
{
    ALCubyte *data = (ALCubyte *) _data;
    ALCsizei cpy;
    ALCsizei avail = ring->used;

    /* Clamp amount to read to available data... */
    if (size > avail)
        size = avail;
    
    /* Clip to end of buffer and copy first block... */
    cpy = ring->size - ring->read;
    if (cpy > size) cpy = size;
    if (cpy) SDL_memcpy(data, ring->buffer + ring->read, cpy);
    
    /* Wrap around to front of ring buffer and copy remaining data... */
    avail = size - cpy;
    if (avail) SDL_memcpy(data + cpy, ring->buffer, avail);

    /* Update read pointer... */
    ring->read += size;
    if (ring->read > ring->size)
        ring->read -= ring->size;

    ring->used -= size;

    return size;  /* may have been clamped if there wasn't enough data... */
}

typedef struct ALbuffer
{
    SDL_atomic_t allocated;
    ALuint name;
    ALint channels;
    ALint bits;  /* always float32 internally, but this is what alBufferData saw */
    ALsizei frequency;
    ALsizei len;   /* length of data in bytes. */
    const float *data;  /* we only work in Float32 format. */
    SDL_atomic_t refcount;  /* if zero, can be deleted or alBufferData'd */
} ALbuffer;

typedef struct BufferBlock
{
    ALbuffer buffers[OPENAL_BUFFER_BLOCK_SIZE];   /* allocate these in blocks so we can step through faster. */
    void *next;  /* void* because we'll atomicgetptr it. */
} BufferBlock;

typedef struct BufferQueueItem
{
    const ALbuffer *buffer;
    void *next;  /* void* because we'll atomicgetptr it. */
} BufferQueueItem;

typedef struct BufferQueue
{
    void *just_queued;  /* void* because we'll atomicgetptr it. */
    BufferQueueItem *head;
    BufferQueueItem *tail;
    SDL_atomic_t num_items;  /* counts just_queued+head/tail */
} BufferQueue;

typedef struct ALsource
{
    SDL_atomic_t allocated;
    ALenum state;  /* initial, playing, paused, stopped */
    ALenum type;  /* undetermined, static, streaming */
    ALboolean source_relative;
    ALboolean looping;
    ALfloat gain;
    ALfloat min_gain;
    ALfloat max_gain;
    ALfloat position[3];
    ALfloat velocity[3];
    ALfloat direction[3];
    ALfloat reference_distance;
    ALfloat max_distance;
    ALfloat rolloff_factor;
    ALfloat pitch;
    ALfloat cone_inner_angle;
    ALfloat cone_outer_angle;
    ALfloat cone_outer_gain;
    ALbuffer *buffer;
    SDL_AudioStream *stream;  /* for resampling. */
    BufferQueue buffer_queue;
    BufferQueue buffer_queue_processed;
    SDL_SpinLock buffer_queue_lock;  /* this serializes access to the API end. The mixer does not acquire this! */
    ALsizei offset;
    ALint queue_channels;
    ALsizei queue_frequency;
} ALsource;

struct ALCdevice_struct
{
    char *name;
    ALCenum error;
    ALCboolean iscapture;
    SDL_AudioDeviceID sdldevice;

    ALint channels;
    ALint frequency;
    ALCsizei framesize;

    union {
        struct {
            ALCcontext *contexts;
            BufferBlock buffer_blocks;  /* buffers are shared between contexts on the same device. */
            void *buffer_queue_pool;  /* void* because we'll atomicgetptr it. */
        } playback;
        struct {
            RingBuffer ring;  /* only used if iscapture */
        } capture;
    };
};

struct ALCcontext_struct
{
    ALCdevice *device;
    SDL_atomic_t processing;
    ALenum error;
    ALCint *attributes;
    ALCsizei attributes_count;
    struct {
        ALfloat gain;
        ALfloat position[3];
        ALfloat velocity[3];
        ALfloat orientation[6];
    } listener;

    ALenum distance_model;
    ALfloat doppler_factor;
    ALfloat doppler_velocity;
    ALfloat speed_of_sound;
    ALsource sources[OPENAL_MAX_SOURCES];   /* this array is indexed by ALuint source name. */

    ALCcontext *prev;  /* contexts are in a double-linked list */
    ALCcontext *next;
};


/* ALC implementation... */

static void *current_context = NULL;
static ALCenum null_device_error = ALC_NO_ERROR;

static void set_alc_error(ALCdevice *device, const ALCenum error)
{
    ALCenum *perr = device ? &device->error : &null_device_error;
    /* can't set a new error when the previous hasn't been cleared yet. */
    if (*perr == ALC_NO_ERROR) {
        *perr = error;
    }
}

ALCdevice *alcOpenDevice(const ALCchar *devicename)
{
    ALCdevice *dev = NULL;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == -1) {
        return NULL;
    }

    dev = (ALCdevice *) SDL_calloc(1, sizeof (ALCdevice));
    if (!dev) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return NULL;
    }

    if (!devicename) {
        devicename = DEFAULT_PLAYBACK_DEVICE;  /* so ALC_DEVICE_SPECIFIER is meaningful */
    }

    dev->name = SDL_strdup(devicename);
    if (!dev->name) {
        SDL_free(dev);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return NULL;
    }

    /* we don't open an SDL audio device until the first context is
       created, so we can attempt to match audio formats. */

    dev->iscapture = ALC_FALSE;
    return dev;
}

ALCboolean alcCloseDevice(ALCdevice *device)
{
    if (!device || device->iscapture) {
        return ALC_FALSE;
    }

    if (device->playback.contexts) {
        return ALC_FALSE;
    }

    if (device->sdldevice) {
        SDL_CloseAudioDevice(device->sdldevice);
    }

    SDL_free(device->name);
    SDL_free(device);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);

    return ALC_TRUE;
}


static ALCboolean alcfmt_to_sdlfmt(const ALCenum alfmt, SDL_AudioFormat *sdlfmt, Uint8 *channels, ALCsizei *framesize)
{
    switch (alfmt) {
        case AL_FORMAT_MONO8:
            *sdlfmt = AUDIO_U8;
            *channels = 1;
            *framesize = 1;
            break;
        case AL_FORMAT_MONO16:
            *sdlfmt = AUDIO_S16SYS;
            *channels = 1;
            *framesize = 2;
            break;
        case AL_FORMAT_STEREO8:
            *sdlfmt = AUDIO_U8;
            *channels = 2;
            *framesize = 2;
            break;
        case AL_FORMAT_STEREO16:
            *sdlfmt = AUDIO_S16SYS;
            *channels = 2;
            *framesize = 4;
            break;
        default:
            FIXME("AL_FORMAT_FLOAT32_EXT?");
            return ALC_FALSE;
    }

    return ALC_TRUE;
}

/* the just_queued list is backwards. Add it to the queue in the correct order. */
static void queue_new_buffer_items_recursive(BufferQueue *queue, BufferQueueItem *items)
{
    if (items == NULL) {
        return;
    }

    queue_new_buffer_items_recursive(queue, items->next);
    items->next = NULL;
    if (queue->tail) {
        queue->tail->next = items;
    } else {
        queue->head = items;
    }
    queue->tail = items;
}

static void obtain_newly_queued_buffers(BufferQueue *queue)
{
    BufferQueueItem *items;
    do {
        items = (BufferQueueItem *) SDL_AtomicGetPtr(&queue->just_queued);
    } while (!SDL_AtomicCASPtr(&queue->just_queued, items, NULL));

    /* Now that we own this pointer, we can just do whatever we want with it.
       Nothing touches the head/tail fields other than the mixer thread, so we
       move it there. Not even atomically!  :)
       When setting up these fields in alSourceUnqueueBuffers, there's a lock
       used that is never held by the mixer thread (which only touches
       just_queued atomically when a buffer is completely processed). */
    SDL_assert((queue->tail != NULL) == (queue->head != NULL));

    queue_new_buffer_items_recursive(queue, items);
}

static void mix_source_data_float32(ALCcontext *ctx, ALsource *src, const int channels, const float *data, float *stream, const ALsizei mixframes)
{
    ALsizei i;

    FIXME("precalculate a bunch of stuff before this function");
    const float gain = ctx->listener.gain * src->gain;
    if (gain == 0.0f) {
        return;  /* it's silent, don't spend time mixing it. */
    }

    FIXME("make this efficient.  :)");
    FIXME("currently expects output to be stereo");

    if (channels == 1) {
        const ALsizei iterations = mixframes;
        /* we only spatialize audio for mono sources */
        FIXME("Calculate distance attentuation and other 3D math things");
        if (gain == 1.0f) {
            for (i = 0; i < iterations; i++) {
                const float samp = *(data++);
                *(stream++) += samp;
                *(stream++) += samp;
            }
        } else {
            for (i = 0; i < iterations; i++) {
                const float samp = *(data++);
                *(stream++) += samp;
                *(stream++) += samp;
            }
        }
    } else if (channels == 2) {
        const ALsizei iterations = mixframes * 2;
        if (gain == 1.0f) {
            for (i = 0; i < iterations; i++) {
                const float samp = *(data++);
                *(stream++) += samp;
            }
        } else {
            for (i = 0; i < iterations; i++) {
                const float samp = *(data++);
                *(stream++) += samp * gain;
            }
        }
    }
}

static ALboolean mix_source_buffer(ALCcontext *ctx, ALsource *src, BufferQueueItem *queue, float **stream, int *len)
{
    const ALbuffer *buffer = queue ? queue->buffer : NULL;
    ALboolean processed = AL_TRUE;

    /* you can legally queue or set a NULL buffer. */
    if (buffer && buffer->data && (buffer->len > 0)) {
        const float *data = buffer->data + (src->offset / sizeof (float));
        const int bufferframesize = (int) (buffer->channels * sizeof (float));
        const int deviceframesize = ctx->device->framesize;
        const int framesneeded = *len / deviceframesize;

        SDL_assert(src->offset < buffer->len);

        if (src->stream) {  /* resampling? */
            int mixframes, mixlen, remainingmixframes;
            while ( (((mixlen = SDL_AudioStreamAvailable(src->stream)) / bufferframesize) < framesneeded) && (src->offset < buffer->len) ) {
                const int framesput = (buffer->len - src->offset) / bufferframesize;
                const int bytesput = SDL_min(framesput, 1024) * bufferframesize;
                FIXME("dynamically adjust frames here?");  /* we hardcode 1024 samples when opening the audio device, too. */
                SDL_AudioStreamPut(src->stream, data, bytesput);
                src->offset += bytesput;
                data += bytesput / sizeof (float);
            }

            mixframes = SDL_min(mixlen / bufferframesize, framesneeded);
            remainingmixframes = mixframes;
            while (remainingmixframes > 0) {
                const int mixbuflen = 1024;
                float mixbuf[mixbuflen / sizeof (float)];
                const int mixbufframes = mixbuflen / bufferframesize;
                const int getframes = SDL_min(remainingmixframes, mixbufframes);
                SDL_AudioStreamGet(src->stream, mixbuf, getframes * bufferframesize);
                mix_source_data_float32(ctx, src, buffer->channels, mixbuf, *stream, getframes);
                *len -= getframes * deviceframesize;
                *stream += getframes * ctx->device->channels;
                remainingmixframes -= getframes;
            }
        } else {
            const int framesavail = (buffer->len - src->offset) / bufferframesize;
            const int mixframes = SDL_min(framesneeded, framesavail);
            mix_source_data_float32(ctx, src, buffer->channels, data, *stream, mixframes);
            src->offset += mixframes * bufferframesize;
            *len -= mixframes * deviceframesize;
            *stream += mixframes * ctx->device->channels;
        }

        SDL_assert(src->offset <= buffer->len);

        processed = src->offset >= buffer->len;
        if (processed) {
            FIXME("does the offset have to represent the whole queue or just the current buffer?");
            src->offset = 0;
        }
    }

    return processed;
}

static inline void mix_source_buffer_queue(ALCcontext *ctx, ALsource *src, BufferQueueItem *queue, float *stream, int len)
{
    while ((len > 0) && (mix_source_buffer(ctx, src, queue, &stream, &len))) {
        /* Finished this buffer! */
        BufferQueueItem *item = queue;
        BufferQueueItem *next = queue ? queue->next : NULL;
        void *ptr;

        if (queue) {
            queue->next = NULL;
            queue = next;
        }

        SDL_assert((src->type == AL_STATIC) || (src->type == AL_STREAMING));
        if (src->type == AL_STREAMING) {  /* mark buffer processed. */
            SDL_assert(item == src->buffer_queue.head);
            src->buffer_queue.head = next;
            if (!next) {
                src->buffer_queue.tail = NULL;
            }
            SDL_AtomicAdd(&src->buffer_queue.num_items, -1);

            /* Move it to the processed queue for alSourceUnqueueBuffers() to pick up. */
            do {
                ptr = SDL_AtomicGetPtr(&src->buffer_queue_processed.just_queued);
                SDL_AtomicSetPtr(&item->next, ptr);
            } while (!SDL_AtomicCASPtr(&src->buffer_queue_processed.just_queued, ptr, item));

            SDL_AtomicAdd(&src->buffer_queue_processed.num_items, 1);
        }

        if (queue == NULL) {  /* nothing else to play? */
            if (src->looping) {
                FIXME("looping is supposed to move to AL_INITIAL then immediately to AL_PLAYING, but I'm not sure what side effect this is meant to trigger");
                if (src->type == AL_STREAMING) {
                    FIXME("what does looping do with the AL_STREAMING state?");
                }
            } else {
                src->state = AL_STOPPED;
            }
            break;  /* nothing else to mix here, so stop. */
        }
    }
}

static void mix_source(ALCcontext *ctx, ALsource *src, float *stream, int len)
{
    if (SDL_AtomicGet(&src->allocated) != 1) {
        return;  /* not in use */
    } else if (src->state != AL_PLAYING) {
        return;  /* not playing, don't process. */
    }

    if (src->type == AL_STATIC) {
        BufferQueueItem fakequeue = { src->buffer, NULL };
        mix_source_buffer_queue(ctx, src, &fakequeue, stream, len);
    } else if (src->type == AL_STREAMING) {
        obtain_newly_queued_buffers(&src->buffer_queue);
        mix_source_buffer_queue(ctx, src, src->buffer_queue.head, stream, len);
    }
}

static void mix_context(ALCcontext *ctx, float *stream, int len)
{
    ALsizei i;
    FIXME("keep a small array of playing sources so we don't have to walk the whole array");
    for (i = 0; i < OPENAL_MAX_SOURCES; i++) {
        mix_source(ctx, &ctx->sources[i], stream, len);
    }
}


/* We process all unsuspended ALC contexts during this call, mixing their
   output to (stream). SDL then plays this mixed audio to the hardware. */
static void SDLCALL playback_device_callback(void *userdata, Uint8 *stream, int len)
{
    ALCdevice *device = (ALCdevice *) userdata;
    ALCcontext *ctx;

    SDL_memset(stream, '\0', len);

    for (ctx = device->playback.contexts; ctx != NULL; ctx = ctx->next) {
        if (SDL_AtomicGet(&ctx->processing)) {
            mix_context(ctx, (float *) stream, len);
        }
    }
}

ALCcontext* alcCreateContext(ALCdevice *device, const ALCint* attrlist)
{
    ALCcontext *retval = NULL;
    ALCsizei attrcount = 0;
    ALCint freq = 48000;
    ALCboolean sync = ALC_FALSE;
    ALCint refresh = 100;
    /* we don't care about ALC_MONO_SOURCES or ALC_STEREO_SOURCES as we have no hardware limitation. */

    if (!device) {
        set_alc_error(NULL, ALC_INVALID_DEVICE);
        return NULL;
    }

    if (attrlist != NULL) {
        ALCint attr;
        while ((attr = attrlist[attrcount++]) != 0) {
            switch (attr) {
                case ALC_FREQUENCY: freq = attrlist[attrcount++]; break;
                case ALC_REFRESH: refresh = attrlist[attrcount++]; break;
                case ALC_SYNC: sync = (attrlist[attrcount++] ? ALC_TRUE : ALC_FALSE); break;
                default: FIXME("fail for unknown attributes?"); break;
            }
        }
    }

    retval = (ALCcontext *) SDL_calloc(1, sizeof (ALCcontext));
    if (!retval) {
        set_alc_error(device, ALC_OUT_OF_MEMORY);
        return NULL;
    }

    retval->attributes = (ALCint *) SDL_malloc(attrcount * sizeof (ALCint));
    if (!retval->attributes) {
        set_alc_error(device, ALC_OUT_OF_MEMORY);
        SDL_free(retval);
        return NULL;
    }
    SDL_memcpy(retval->attributes, attrlist, attrcount * sizeof (ALCint));
    retval->attributes_count = attrcount;

    if (!device->sdldevice) {
        SDL_AudioSpec desired;
        const char *devicename = device->name;

        if (SDL_strcmp(devicename, DEFAULT_PLAYBACK_DEVICE) == 0) {
            devicename = NULL;  /* tell SDL we want the best default */
        }

        /* we always want to work in float32, to keep our work simple and
           let us use SIMD, and we'll let SDL convert when feeding the device. */
        SDL_zero(desired);
        desired.freq = freq;
        desired.format = AUDIO_F32SYS;
        desired.channels = 2;  FIXME("don't force channels?");
        desired.samples = 1024;  FIXME("base this on refresh");
        desired.callback = playback_device_callback;
        desired.userdata = device;
        device->sdldevice = SDL_OpenAudioDevice(devicename, 0, &desired, NULL, 0);
        if (!device->sdldevice) {
            SDL_free(retval->attributes);
            SDL_free(retval);
            FIXME("What error do you set for this?");
            return NULL;
        }
        device->channels = 2;
        device->frequency = freq;
        device->framesize = sizeof (float) * device->channels;
        SDL_PauseAudioDevice(device->sdldevice, 0);
    }

    retval->distance_model = AL_INVERSE_DISTANCE_CLAMPED;
    retval->doppler_factor = 1.0f;
    retval->doppler_velocity = 1.0f;
    retval->speed_of_sound = 343.3f;
    retval->listener.gain = 1.0f;
    retval->device = device;
    SDL_AtomicSet(&retval->processing, 1);  /* contexts default to processing */

    SDL_LockAudioDevice(device->sdldevice);
    if (device->playback.contexts != NULL) {
        SDL_assert(device->playback.contexts->prev == NULL);
        device->playback.contexts->prev = retval;
    }
    retval->next = device->playback.contexts;
    device->playback.contexts = retval;
    SDL_UnlockAudioDevice(device->sdldevice);

    return retval;
}

ALCboolean alcMakeContextCurrent(ALCcontext *context)
{
    SDL_AtomicSetPtr(&current_context, context);
    FIXME("any reason this might return ALC_FALSE?");
    return ALC_TRUE;
}

void alcProcessContext(ALCcontext *context)
{
    if (!context) {
        set_alc_error(NULL, ALC_INVALID_CONTEXT);
        return;
    }

    SDL_assert(!context->device->iscapture);
    SDL_AtomicSet(&context->processing, 1);
}

void alcSuspendContext(ALCcontext *context)
{
    if (!context) {
        set_alc_error(NULL, ALC_INVALID_CONTEXT);
        return;
    }

    SDL_assert(!context->device->iscapture);
    SDL_AtomicSet(&context->processing, 0);
}

void alcDestroyContext(ALCcontext *context)
{
    if (!context) return;

    /* do this first in case the mixer is running _right now_. */
    SDL_AtomicSet(&context->processing, 0);

    SDL_LockAudioDevice(context->device->sdldevice);
    if (context->prev) {
        context->prev->next = context->next;
    } else {
        SDL_assert(context == context->device->playback.contexts);
        context->device->playback.contexts = context->next;
    }
    if (context->next) {
        context->next->prev = context->prev;
    }
    SDL_UnlockAudioDevice(context->device->sdldevice);
}

static inline ALCcontext *get_current_context(void)
{
    return (ALCcontext *) SDL_AtomicGetPtr(&current_context);
}

ALCcontext *alcGetCurrentContext(void)
{
    return get_current_context();
}

ALCdevice *alcGetContextsDevice(ALCcontext *context)
{
    return context ? context->device : NULL;
}

ALCenum alcGetError(ALCdevice *device)
{
    ALCenum *perr = device ? &device->error : &null_device_error;
    const ALCenum retval = *perr;
    *perr = ALC_NO_ERROR;
    return retval;
}

ALCboolean alcIsExtensionPresent(ALCdevice *device, const ALCchar *extname)
{
    FIXME("write me");
    if (SDL_strcasecmp(extname, "ALC_EXT_capture") == 0) return ALC_TRUE;
    return ALC_FALSE;
}

void *alcGetProcAddress(ALCdevice *device, const ALCchar *funcname)
{
    if (!funcname) {
        set_alc_error(device, ALC_INVALID_VALUE);
        return NULL;
    }

    #define FN_TEST(fn) if (SDL_strcmp(funcname, #fn) == 0) return fn
    FN_TEST(alcCreateContext);
    FN_TEST(alcMakeContextCurrent);
    FN_TEST(alcProcessContext);
    FN_TEST(alcSuspendContext);
    FN_TEST(alcDestroyContext);
    FN_TEST(alcGetCurrentContext);
    FN_TEST(alcGetContextsDevice);
    FN_TEST(alcOpenDevice);
    FN_TEST(alcCloseDevice);
    FN_TEST(alcGetError);
    FN_TEST(alcIsExtensionPresent);
    FN_TEST(alcGetProcAddress);
    FN_TEST(alcGetEnumValue);
    FN_TEST(alcGetString);
    FN_TEST(alcGetIntegerv);
    FN_TEST(alcCaptureOpenDevice);
    FN_TEST(alcCaptureCloseDevice);
    FN_TEST(alcCaptureStart);
    FN_TEST(alcCaptureStop);
    FN_TEST(alcCaptureSamples);
    #undef FN_TEST

    FIXME("Extension entry points?");
    set_alc_error(device, ALC_INVALID_VALUE);
    return NULL;
}

ALCenum alcGetEnumValue(ALCdevice *device, const ALCchar *enumname)
{
    if (!enumname) {
        set_alc_error(device, ALC_INVALID_VALUE);
        return (ALCenum) AL_NONE;
    }

    #define ENUM_TEST(en) if (SDL_strcmp(enumname, #en) == 0) return en
    ENUM_TEST(ALC_FALSE);
    ENUM_TEST(ALC_TRUE);
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
    #undef ENUM_TEST

    FIXME("Extension enums?");
    set_alc_error(device, ALC_INVALID_VALUE);
    return (ALCenum) AL_NONE;
}

static const ALCchar *calculate_sdl_device_list(const int iscapture)
{
    /* alcGetString() has to return a const string that is not freed and might
       continue to live even if we update this list in a later query, so we
       just make a big static buffer and hope it's large enough and that other
       race conditions don't bite us. The enumeration extension shouldn't have
       reused entry points, or done this silly null-delimited string list.
       Oh well. */
    const size_t DEVICE_LIST_BUFFER_SIZE = 512;
    static ALCchar playback_list[DEVICE_LIST_BUFFER_SIZE];
    static ALCchar capture_list[DEVICE_LIST_BUFFER_SIZE];
    ALCchar list[DEVICE_LIST_BUFFER_SIZE];
    const int numdevs = SDL_GetNumAudioDevices(iscapture);
    ALCchar *final_list = iscapture ? capture_list : playback_list;
    ALCchar *ptr = list;
    size_t avail = DEVICE_LIST_BUFFER_SIZE;
    size_t cpy;
    int i;

    /* default device is always available. */
    cpy = SDL_strlcpy(ptr, iscapture ? DEFAULT_CAPTURE_DEVICE : DEFAULT_PLAYBACK_DEVICE, avail);
    SDL_assert(cpy < avail);
    ptr += cpy + 1;  /* skip past null char. */
    avail -= cpy + 1;

    for (i = 0; i < numdevs; i++) {
        const char *devname = SDL_GetAudioDeviceName(i, iscapture);
        const size_t devnamelen = SDL_strlen(devname);
        /* if we're out of space, we just have to drop devices we can't cram in the buffer. */
        if (avail > (devnamelen + 2)) {
            cpy = SDL_strlcpy(ptr, devname, avail);
            SDL_assert(cpy == (devnamelen + 1));
            SDL_assert(cpy < avail);
            ptr += cpy + 1;  /* skip past null char. */
            avail -= cpy + 1;
        }
    }

    SDL_assert(avail >= 1);
    *ptr = '\0';
    avail--;

    SDL_memcpy(final_list, list, DEVICE_LIST_BUFFER_SIZE - avail);
    return final_list;
}

const ALCchar *alcGetString(ALCdevice *device, ALCenum param)
{
    switch (param) {
        case ALC_EXTENSIONS:
            if (!device) {
                set_alc_error(NULL, ALC_INVALID_DEVICE);
            } else {
                FIXME("add some extensions.  :)");
                return "ALC_EXT_CAPTURE";
            }
            break;

        /* You open the default SDL device with a NULL device name, but that is how OpenAL
           reports an error here, so we give it a magic identifier here instead. */
        case ALC_DEFAULT_DEVICE_SPECIFIER:
            return DEFAULT_PLAYBACK_DEVICE;

        case ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER:
            return DEFAULT_CAPTURE_DEVICE;

        case ALC_DEVICE_SPECIFIER:
            return device ? device->name : calculate_sdl_device_list(0);

        case ALC_CAPTURE_DEVICE_SPECIFIER:
            return device ? device->name : calculate_sdl_device_list(1);

        case ALC_NO_ERROR: return "ALC_NO_ERROR";
        case ALC_INVALID_DEVICE: return "ALC_INVALID_DEVICE";
        case ALC_INVALID_CONTEXT:return "ALC_INVALID_CONTEXT";
        case ALC_INVALID_ENUM: return "ALC_INVALID_ENUM";
        case ALC_INVALID_VALUE: return "ALC_INVALID_VALUE";
        case ALC_OUT_OF_MEMORY: return "ALC_OUT_OF_MEMORY";

        default: break;
    }

    FIXME("other enums that should report as strings?");
    set_alc_error(device, ALC_INVALID_ENUM);
    return NULL;
}

void alcGetIntegerv(ALCdevice *device, ALCenum param, ALCsizei size, ALCint *values)
{
    ALCcontext *ctx = NULL;

    if (!size || !values) {
        return;  /* "A NULL destination or a zero size parameter will cause ALC to ignore the query." */
    }

    switch (param) {
        case ALC_CAPTURE_SAMPLES:
            if (!device || !device->iscapture) {
                set_alc_error(device, ALC_INVALID_DEVICE);
                return;
            }

            SDL_LockAudioDevice(device->sdldevice);
            *values = (ALCint) (device->capture.ring.used / device->framesize);
            SDL_UnlockAudioDevice(device->sdldevice);
            return;

        case ALC_ATTRIBUTES_SIZE:
        case ALC_ALL_ATTRIBUTES:
            if (!device || device->iscapture) {
                *values = 0;
                set_alc_error(device, ALC_INVALID_DEVICE);
                return;
            }

            ctx = get_current_context();

            FIXME("wants 'current context of specified device', but there isn't a current context per-device...");
            if ((!ctx) || (ctx->device != device)) {
                *values = 0;
                set_alc_error(device, ALC_INVALID_CONTEXT);
                return;
            }

            if (param == ALC_ALL_ATTRIBUTES) {
                if (size < ctx->attributes_count) {
                    *values = 0;
                    set_alc_error(device, ALC_INVALID_VALUE);
                    return;
                }
                SDL_memcpy(values, ctx->attributes, ctx->attributes_count * sizeof (ALCint));
            } else {
                *values = (ALCint) ctx->attributes_count;
            }
            return;

        case ALC_MAJOR_VERSION:
            *values = OPENAL_VERSION_MAJOR;
            return;

        case ALC_MINOR_VERSION:
            *values = OPENAL_VERSION_MINOR;
            return;

        default: break;
    }

    FIXME("Extension enums?");
    set_alc_error(device, ALC_INVALID_ENUM);
    *values = 0;
}


/* audio callback for capture devices just needs to move data into our
   ringbuffer for later recovery by the app in alcCaptureSamples(). SDL
   should have handled resampling and conversion for us to the expected
   audio format. */
static void SDLCALL capture_device_callback(void *userdata, Uint8 *stream, int len)
{
    ALCdevice *device = (ALCdevice *) userdata;
    SDL_assert(device->iscapture);
    ring_buffer_put(&device->capture.ring, stream, (ALCsizei) len);
}

ALCdevice* alcCaptureOpenDevice(const ALCchar *devicename, ALCuint frequency, ALCenum format, ALCsizei buffersize)
{
    ALCdevice *device = NULL;
    SDL_AudioSpec desired;
    ALCsizei framesize = 0;

    SDL_zero(desired);
    if (!alcfmt_to_sdlfmt(format, &desired.format, &desired.channels, &framesize)) {
        return NULL;
    }

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == -1) {
        return NULL;
    }

    device = (ALCdevice *) SDL_calloc(1, sizeof (ALCdevice));
    if (!device) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return NULL;
    }

    desired.freq = frequency;
    desired.samples = 1024;  FIXME("is this a reasonable value?");
    desired.callback = capture_device_callback;
    desired.userdata = device;

    device->iscapture = ALC_TRUE;

    if (!devicename) {
        devicename = DEFAULT_CAPTURE_DEVICE;  /* so ALC_CAPTURE_DEVICE_SPECIFIER is meaningful */
    }

    device->name = SDL_strdup(devicename);
    if (!device->name) {
        SDL_free(device);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return NULL;
    }

    if (SDL_strcmp(devicename, DEFAULT_CAPTURE_DEVICE) == 0) {
        devicename = NULL;  /* tell SDL we want the best default */
    }

    device->frequency = frequency;
    device->framesize = framesize;
    device->capture.ring.size = framesize * buffersize;

    if (device->capture.ring.size < buffersize) {
        device->capture.ring.buffer = NULL;  /* uhoh, integer overflow! */
    } else {
        device->capture.ring.buffer = (ALCubyte *) SDL_malloc(device->capture.ring.size);
    }

    if (!device->capture.ring.buffer) {
        SDL_free(device->name);
        SDL_free(device);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }

    device->sdldevice = SDL_OpenAudioDevice(devicename, 1, &desired, NULL, 0);
    if (!device->sdldevice) {
        SDL_free(device->capture.ring.buffer);
        SDL_free(device->name);
        SDL_free(device);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return NULL;
    }

    return device;
}

ALCboolean alcCaptureCloseDevice(ALCdevice *device)
{
    if (!device || !device->iscapture) {
        return ALC_FALSE;
    }

    if (device->sdldevice) {
        SDL_CloseAudioDevice(device->sdldevice);
    }

    SDL_free(device->capture.ring.buffer);
    SDL_free(device->name);
    SDL_free(device);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);

    return ALC_TRUE;
}

void alcCaptureStart(ALCdevice *device)
{
    if (device && device->iscapture) {
        /* alcCaptureStart() drops any previously-buffered data. */
        FIXME("does this clear the ring buffer if the device is already started?");
        device->capture.ring.read = 0;
        device->capture.ring.write = 0;
        device->capture.ring.used = 0;
        SDL_PauseAudioDevice(device->sdldevice, 0);
    }
}

void alcCaptureStop(ALCdevice *device)
{
    if (device && device->iscapture) {
        SDL_PauseAudioDevice(device->sdldevice, 1);
    }
}

void alcCaptureSamples(ALCdevice *device, ALCvoid *buffer, ALCsizei samples)
{
    ALCsizei requested_bytes;
    if (!device || !device->iscapture) {
        return;
    }

    requested_bytes = samples * device->framesize;

    SDL_LockAudioDevice(device->sdldevice);
    if (requested_bytes > device->capture.ring.used) {
        SDL_UnlockAudioDevice(device->sdldevice);
        FIXME("set error state?");
        return;  /* this is an error state, according to the spec. */
    }

    ring_buffer_get(&device->capture.ring, buffer, requested_bytes);
    SDL_UnlockAudioDevice(device->sdldevice);
}


/* AL implementation... */

static ALenum null_context_error = AL_NO_ERROR;

static void set_al_error(ALCcontext *ctx, const ALenum error)
{
    ALenum *perr = ctx ? &ctx->error : &null_context_error;
    /* can't set a new error when the previous hasn't been cleared yet. */
    if (*perr == AL_NO_ERROR) {
        *perr = error;
    }
}

static inline ALboolean is_source_valid(ALCcontext *ctx, const ALuint name)
{
    return (ctx && name && (name < OPENAL_MAX_SOURCES) && (SDL_AtomicGet(&ctx->sources[name-1].allocated) == 1)) ? AL_TRUE : AL_FALSE;
}

static ALsource *get_source(ALCcontext *ctx, const ALuint name)
{
    if (!ctx) {
        set_al_error(ctx, AL_INVALID_OPERATION);
        return NULL;
    }

    if (!is_source_valid(ctx, name)) {
        set_al_error(ctx, AL_INVALID_NAME);
        return NULL;
    }

    /* if this object is deleted, the pointer remains valid, so we'll let it slide through on a race condition. */
    return &ctx->sources[name - 1];
}

static ALbuffer *get_buffer(ALCcontext *ctx, const ALuint name)
{
    BufferBlock *block;
    ALbuffer *buffer = NULL;
    ALuint block_offset = 0;

    if (!ctx) {
        set_al_error(ctx, AL_INVALID_OPERATION);
        return NULL;
    } else if (name == 0) {
        set_al_error(ctx, AL_INVALID_NAME);
        return NULL;
    }

    block = &ctx->device->playback.buffer_blocks;
    while (block != NULL) {
        const ALuint next_offset = block_offset + SDL_arraysize(block->buffers);
        if ((block_offset < name) && (next_offset >= name)) {
            buffer = &block->buffers[(name - block_offset) - 1];
            if (SDL_AtomicGet(&buffer->allocated) == 1) {
                return buffer;
            }
            break;
        }

        block = (BufferBlock *) SDL_AtomicGetPtr(&block->next);
        block_offset += SDL_arraysize(block->buffers);
    }

    set_al_error(ctx, AL_INVALID_NAME);
    return NULL;
}

void alDopplerFactor(ALfloat value)
{
    ALCcontext *ctx = get_current_context();
    if (!ctx) {
        set_al_error(ctx, AL_INVALID_OPERATION);
    } else if (value < 0.0f) {
        set_al_error(ctx, AL_INVALID_VALUE);
    } else {
        ctx->doppler_factor = value;
    }
}

void alDopplerVelocity(ALfloat value)
{
    ALCcontext *ctx = get_current_context();
    if (!ctx) {
        set_al_error(ctx, AL_INVALID_OPERATION);
    } else if (value < 0.0f) {
        set_al_error(ctx, AL_INVALID_VALUE);
    } else {
        ctx->doppler_velocity = value;
    }
}

void alSpeedOfSound(ALfloat value)
{
    ALCcontext *ctx = get_current_context();
    if (!ctx) {
        set_al_error(ctx, AL_INVALID_OPERATION);
    } else if (value < 0.0f) {
        set_al_error(ctx, AL_INVALID_VALUE);
    } else {
        ctx->speed_of_sound = value;
    }
}

void alDistanceModel(ALenum model)
{
    ALCcontext *ctx = get_current_context();
    if (!ctx) {
        set_al_error(ctx, AL_INVALID_OPERATION);
        return;
    }

    switch (model) {
        case AL_NONE:
        case AL_INVERSE_DISTANCE:
        case AL_INVERSE_DISTANCE_CLAMPED:
        case AL_LINEAR_DISTANCE:
        case AL_LINEAR_DISTANCE_CLAMPED:
        case AL_EXPONENT_DISTANCE:
        case AL_EXPONENT_DISTANCE_CLAMPED:
            ctx->distance_model = model;
            return;
        default: break;
    }
    set_al_error(ctx, AL_INVALID_ENUM);
}

void alEnable(ALenum capability)
{
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);  /* nothing in AL 1.1 uses this. */
}

void alDisable(ALenum capability)
{
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);  /* nothing in AL 1.1 uses this. */
}

ALboolean alIsEnabled(ALenum capability)
{
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);  /* nothing in AL 1.1 uses this. */
    return AL_FALSE;
}

const ALchar* alGetString(ALenum param)
{
    switch (param) {
        case AL_EXTENSIONS: FIXME("add some extensions.  :)"); return "";
        case AL_VERSION: return OPENAL_VERSION_STRING;
        case AL_RENDERER: return OPENAL_RENDERER_STRING;
        case AL_VENDOR: return OPENAL_VENDOR_STRING;
        case AL_NO_ERROR: return "AL_NO_ERROR";
        case AL_INVALID_NAME: return "AL_INVALID_NAME";
        case AL_INVALID_ENUM: return "AL_INVALID_ENUM";
        case AL_INVALID_VALUE: return "AL_INVALID_VALUE";
        case AL_INVALID_OPERATION: return "AL_INVALID_OPERATION";
        case AL_OUT_OF_MEMORY: return "AL_OUT_OF_MEMORY";

        default: break;
    }

    FIXME("other enums that should report as strings?");
    set_al_error(get_current_context(), ALC_INVALID_ENUM);

    return NULL;
}

void alGetBooleanv(ALenum param, ALboolean *values)
{
    ALCcontext *ctx = get_current_context();
    if (!ctx) {
        set_al_error(ctx, AL_INVALID_OPERATION);
        return;
    }

    if (!values) return;  /* legal no-op */

    /* nothing in core OpenAL 1.1 uses this */
    FIXME("extensions?");
    set_al_error(ctx, AL_INVALID_ENUM);
}

void alGetIntegerv(ALenum param, ALint *values)
{
    ALCcontext *ctx = get_current_context();
    if (!ctx) {
        set_al_error(ctx, AL_INVALID_OPERATION);
        return;
    }

    if (!values) return;  /* legal no-op */

    switch (param) {
        case AL_DISTANCE_MODEL: *values = (ALint) ctx->distance_model; return;
        default: break;
    }

    FIXME("What happens if you pass something that needs != 1 float, like AL_ORIENTATION?");
    FIXME("extensions?");
    set_al_error(ctx, AL_INVALID_ENUM);
}

void alGetFloatv(ALenum param, ALfloat *values)
{
    ALCcontext *ctx = get_current_context();
    if (!ctx) {
        set_al_error(ctx, AL_INVALID_OPERATION);
        return;
    }

    if (!values) return;  /* legal no-op */

    switch (param) {
        case AL_DOPPLER_FACTOR: *values = ctx->doppler_factor; return;
        case AL_DOPPLER_VELOCITY: *values = ctx->doppler_velocity; return;
        case AL_SPEED_OF_SOUND: *values = ctx->speed_of_sound; return;
        default: break;
    }

    FIXME("What happens if you pass something that needs != 1 float, like AL_ORIENTATION?");
    FIXME("extensions?");
    set_al_error(ctx, AL_INVALID_ENUM);
}

void alGetDoublev(ALenum param, ALdouble *values)
{
    ALCcontext *ctx = get_current_context();
    if (!ctx) {
        set_al_error(ctx, AL_INVALID_OPERATION);
        return;
    }

    if (!values) return;  /* legal no-op */

    /* nothing in core OpenAL 1.1 uses this */
    FIXME("Should GetFloatv params work here??");
    FIXME("extensions?");
    set_al_error(ctx, AL_INVALID_ENUM);
}

ALboolean alGetBoolean(ALenum param)
{
    ALboolean retval = AL_FALSE;
    alGetBooleanv(param, &retval);
    return retval;
}

ALint alGetInteger(ALenum param)
{
    ALint retval = 0;
    alGetIntegerv(param, &retval);
    return retval;
}

ALfloat alGetFloat(ALenum param)
{
    ALfloat retval = 0.0f;
    alGetFloatv(param, &retval);
    return retval;
}

ALdouble alGetDouble(ALenum param)
{
    ALdouble retval = 0.0f;
    alGetDoublev(param, &retval);
    return retval;
}

ALenum alGetError(void)
{
    ALCcontext *ctx = get_current_context();
    ALenum *perr = ctx ? &ctx->error : &null_context_error;
    const ALenum retval = *perr;
    *perr = AL_NO_ERROR;
    return retval;
}

ALboolean alIsExtensionPresent(const ALchar *extname)
{
    FIXME("This entry point is currently unimplemented.");
    return AL_FALSE;
}

void *alGetProcAddress(const ALchar *funcname)
{
    ALCcontext *ctx = get_current_context();
    FIXME("fail if ctx == NULL?");
    if (!funcname) {
        set_al_error(ctx, AL_INVALID_VALUE);
        return NULL;
    }

    #define FN_TEST(fn) if (SDL_strcmp(funcname, #fn) == 0) return fn
    FN_TEST(alDopplerFactor);
    FN_TEST(alDopplerVelocity);
    FN_TEST(alSpeedOfSound);
    FN_TEST(alDistanceModel);
    FN_TEST(alEnable);
    FN_TEST(alDisable);
    FN_TEST(alIsEnabled);
    FN_TEST(alGetString);
    FN_TEST(alGetBooleanv);
    FN_TEST(alGetIntegerv);
    FN_TEST(alGetFloatv);
    FN_TEST(alGetDoublev);
    FN_TEST(alGetBoolean);
    FN_TEST(alGetInteger);
    FN_TEST(alGetFloat);
    FN_TEST(alGetDouble);
    FN_TEST(alGetError);
    FN_TEST(alIsExtensionPresent);
    FN_TEST(alGetProcAddress);
    FN_TEST(alGetEnumValue);
    FN_TEST(alListenerf);
    FN_TEST(alListener3f);
    FN_TEST(alListenerfv);
    FN_TEST(alListeneri);
    FN_TEST(alListener3i);
    FN_TEST(alListeneriv);
    FN_TEST(alGetListenerf);
    FN_TEST(alGetListener3f);
    FN_TEST(alGetListenerfv);
    FN_TEST(alGetListeneri);
    FN_TEST(alGetListener3i);
    FN_TEST(alGetListeneriv);
    FN_TEST(alGenSources);
    FN_TEST(alDeleteSources);
    FN_TEST(alIsSource);
    FN_TEST(alSourcef);
    FN_TEST(alSource3f);
    FN_TEST(alSourcefv);
    FN_TEST(alSourcei);
    FN_TEST(alSource3i);
    FN_TEST(alSourceiv);
    FN_TEST(alGetSourcef);
    FN_TEST(alGetSource3f);
    FN_TEST(alGetSourcefv);
    FN_TEST(alGetSourcei);
    FN_TEST(alGetSource3i);
    FN_TEST(alGetSourceiv);
    FN_TEST(alSourcePlayv);
    FN_TEST(alSourceStopv);
    FN_TEST(alSourceRewindv);
    FN_TEST(alSourcePausev);
    FN_TEST(alSourcePlay);
    FN_TEST(alSourceStop);
    FN_TEST(alSourceRewind);
    FN_TEST(alSourcePause);
    FN_TEST(alSourceQueueBuffers);
    FN_TEST(alSourceUnqueueBuffers);
    FN_TEST(alGenBuffers);
    FN_TEST(alDeleteBuffers);
    FN_TEST(alIsBuffer);
    FN_TEST(alBufferData);
    FN_TEST(alBufferf);
    FN_TEST(alBuffer3f);
    FN_TEST(alBufferfv);
    FN_TEST(alBufferi);
    FN_TEST(alBuffer3i);
    FN_TEST(alBufferiv);
    FN_TEST(alGetBufferf);
    FN_TEST(alGetBuffer3f);
    FN_TEST(alGetBufferfv);
    FN_TEST(alGetBufferi);
    FN_TEST(alGetBuffer3i);
    FN_TEST(alGetBufferiv);
    #undef FN_TEST

    FIXME("Extension entry points?");
    set_al_error(ctx, ALC_INVALID_VALUE);
    return NULL;
}

ALenum alGetEnumValue(const ALchar *enumname)
{
    ALCcontext *ctx = get_current_context();
    FIXME("fail if ctx == NULL?");
    if (!enumname) {
        set_al_error(ctx, AL_INVALID_VALUE);
        return AL_NONE;
    }

    #define ENUM_TEST(en) if (SDL_strcmp(enumname, #en) == 0) return en
    ENUM_TEST(AL_NONE);
    ENUM_TEST(AL_FALSE);
    ENUM_TEST(AL_TRUE);
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
    ENUM_TEST(AL_NO_ERROR);
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
    #undef ENUM_TEST

    FIXME("Extension enums?");
    set_al_error(ctx, AL_INVALID_VALUE);
    return AL_NONE;
}

void alListenerf(ALenum param, ALfloat value)
{
    switch (param) {
        case AL_GAIN:
            alListenerfv(param, &value);
            return;
        default: break;
    }

    FIXME("What happens if you pass something that needs more than 1 float, like AL_POSITION?");
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);
}

void alListener3f(ALenum param, ALfloat value1, ALfloat value2, ALfloat value3)
{
    switch (param) {
        case AL_POSITION:
        case AL_VELOCITY: {
            const ALfloat values[3] = { value1, value2, value3 };
            alListenerfv(param, values);
            return;
        }
        default: break;
    }

    FIXME("What happens if you pass something that needs != 3 floats, like AL_GAIN?");
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);
}

void alListenerfv(ALenum param, const ALfloat *values)
{
    ALfloat dummy[6];
    ALCcontext *ctx = get_current_context();
    if (!ctx) {
        set_al_error(ctx, AL_INVALID_OPERATION);
        return;
    }

    if (!values) values = dummy;

    switch (param) {
        case AL_GAIN:
            ctx->listener.gain = *values;
            return;

        case AL_POSITION:
            SDL_memcpy(ctx->listener.position, values, sizeof (*values) * 3);
            return;

        case AL_VELOCITY:
            SDL_memcpy(ctx->listener.velocity, values, sizeof (*values) * 3);
            return;

        case AL_ORIENTATION:
            SDL_memcpy(ctx->listener.orientation, values, sizeof (*values) * 6);
            return;

        default: break;
    }

    FIXME("What happens if you pass something that needs != 3 floats, like AL_GAIN?");
    FIXME("extensions?");
    set_al_error(ctx, AL_INVALID_ENUM);
}

void alListeneri(ALenum param, ALint value)
{
    FIXME("What happens if you pass something that needs != 1 int, like AL_GAIN?");
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);
}

void alListener3i(ALenum param, ALint value1, ALint value2, ALint value3)
{
    switch (param) {
        case AL_POSITION:
        case AL_VELOCITY: {
            const ALint values[3] = { value1, value2, value3 };
            alListeneriv(param, values);
            return;
        }
        default: break;
    }

    FIXME("What happens if you pass something that needs != 3 ints, like AL_GAIN?");
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);
}

void alListeneriv(ALenum param, const ALint *values)
{
    ALint dummy[6];
    ALCcontext *ctx = get_current_context();
    if (!ctx) {
        set_al_error(ctx, AL_INVALID_OPERATION);
        return;
    }

    if (!values) values = dummy;

    switch (param) {
        case AL_POSITION:
            ctx->listener.position[0] = (ALfloat) values[0];
            ctx->listener.position[1] = (ALfloat) values[1];
            ctx->listener.position[2] = (ALfloat) values[2];
            return;

        case AL_VELOCITY:
            ctx->listener.velocity[0] = (ALfloat) values[0];
            ctx->listener.velocity[1] = (ALfloat) values[1];
            ctx->listener.velocity[2] = (ALfloat) values[2];
            return;

        case AL_ORIENTATION:
            ctx->listener.orientation[0] = (ALfloat) values[0];
            ctx->listener.orientation[1] = (ALfloat) values[1];
            ctx->listener.orientation[2] = (ALfloat) values[2];
            ctx->listener.orientation[3] = (ALfloat) values[3];
            ctx->listener.orientation[4] = (ALfloat) values[4];
            ctx->listener.orientation[5] = (ALfloat) values[5];
            return;

        default: break;
    }

    FIXME("What happens if you pass something that needs != 3 ints, like AL_GAIN?");
    FIXME("extensions?");
    set_al_error(ctx, AL_INVALID_ENUM);
}

void alGetListenerf(ALenum param, ALfloat *value)
{
    switch (param) {
        case AL_GAIN:
            alGetListenerfv(param, value);
            return;
        default: break;
    }

    FIXME("What happens if you pass something that needs != 1 float, like AL_ORIENTATION?");
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);
}

void alGetListener3f(ALenum param, ALfloat *value1, ALfloat *value2, ALfloat *value3)
{
    ALfloat values[3];
    switch (param) {
        case AL_POSITION:
        case AL_VELOCITY:
            alGetListenerfv(param, values);
            if (value1) *value1 = values[0];
            if (value2) *value2 = values[1];
            if (value3) *value3 = values[2];
            return;

        default: break;
    }

    FIXME("What happens if you pass something that needs != 3 float, like AL_GAIN?");
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);
}

void alGetListenerfv(ALenum param, ALfloat *values)
{
    ALfloat dummy[6];
    ALCcontext *ctx = get_current_context();
    if (!ctx) {
        set_al_error(ctx, AL_INVALID_OPERATION);
        return;
    }

    if (!values) values = dummy;

    switch (param) {
        case AL_GAIN:
            *values = ctx->listener.gain;
            return;

        case AL_POSITION:
            SDL_memcpy(values, ctx->listener.position, sizeof (ALfloat) * 3);
            return;

        case AL_VELOCITY:
            SDL_memcpy(values, ctx->listener.velocity, sizeof (ALfloat) * 3);
            return;

        case AL_ORIENTATION:
            SDL_memcpy(values, ctx->listener.orientation, sizeof (ALfloat) * 6);
            return;

        default: break;
    }

    FIXME("What happens if you pass something that needs != 3 float, like AL_GAIN?");
    FIXME("extensions?");
    set_al_error(ctx, AL_INVALID_ENUM);
}

void alGetListeneri(ALenum param, ALint *value)
{
    FIXME("What happens if you pass something that needs != 1 int, like AL_POSITION?");
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);
}

void alGetListener3i(ALenum param, ALint *value1, ALint *value2, ALint *value3)
{
    ALint values[3];
    switch (param) {
        case AL_POSITION:
        case AL_VELOCITY:
            alGetListeneriv(param, values);
            if (value1) *value1 = values[0];
            if (value2) *value2 = values[1];
            if (value3) *value3 = values[2];
            return;

        default: break;
    }

    FIXME("What happens if you pass something that needs != 3 int, like AL_GAIN?");
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);
}

void alGetListeneriv(ALenum param, ALint *values)
{
    ALint dummy[6];
    ALCcontext *ctx = get_current_context();
    if (!ctx) {
        set_al_error(ctx, AL_INVALID_OPERATION);
        return;
    }

    if (!values) values = dummy;

    switch (param) {
        case AL_POSITION:
            values[0] = (ALint) ctx->listener.position[0];
            values[1] = (ALint) ctx->listener.position[1];
            values[2] = (ALint) ctx->listener.position[2];
            return;

        case AL_VELOCITY:
            values[0] = (ALint) ctx->listener.velocity[0];
            values[1] = (ALint) ctx->listener.velocity[1];
            values[2] = (ALint) ctx->listener.velocity[2];
            return;

        case AL_ORIENTATION:
            values[0] = (ALint) ctx->listener.orientation[0];
            values[1] = (ALint) ctx->listener.orientation[1];
            values[2] = (ALint) ctx->listener.orientation[2];
            values[3] = (ALint) ctx->listener.orientation[3];
            values[4] = (ALint) ctx->listener.orientation[4];
            values[5] = (ALint) ctx->listener.orientation[5];
            return;

        default: break;
    }

    FIXME("What happens if you pass something that needs != ints, like AL_GAIN?");
    FIXME("extensions?");
    set_al_error(ctx, AL_INVALID_ENUM);
}

void alGenSources(ALsizei n, ALuint *names)
{
    ALCcontext *ctx = get_current_context();
    ALsizei found = 0;
    ALuint i;

    if (!ctx) {
        set_al_error(ctx, AL_INVALID_OPERATION);
        return;
    }

    for (i = 0; i < OPENAL_MAX_SOURCES; i++) {
        if (SDL_AtomicCAS(&ctx->sources[i].allocated, 0, 2)) {  /* 0==unused, 1==in use, 2==trying to acquire. */
            names[found++] = i + 1; /* plus 1 because 0 is the null value */
            if (found == n) {
                break;
            }
        }
    }

    SDL_assert(found <= n);

    if (found < n) { /* not enough sources! */
        for (i = 0; i < (ALuint) found; i++) {
            SDL_AtomicSet(&ctx->sources[names[i]-1].allocated, 0);  /* give this one back. */
        }
        SDL_memset(names, '\0', sizeof (*names) * n);
        set_al_error(ctx, AL_OUT_OF_MEMORY);
        return;
    }

    for (i = 0; i < (ALuint) n; i++) {
        ALsource *src = &ctx->sources[names[i]-1];
        /* don't SDL_zerop() this source, because we need src->allocated to stay at 2 until initialized. */
        src->state = AL_INITIAL;
        src->type = AL_UNDETERMINED;
        src->source_relative = AL_FALSE;
        src->looping = AL_FALSE;
        src->gain = 1.0f;
        src->min_gain = 0.0f;
        src->max_gain = 1.0f;
        SDL_zero(src->position);
        SDL_zero(src->velocity);
        SDL_zero(src->direction);
        src->reference_distance = 1.0f;
        src->max_distance = FLT_MAX;
        src->rolloff_factor = 1.0f;
        src->pitch = 1.0f;
        src->cone_inner_angle = 360.0f;
        src->cone_outer_angle = 360.0f;
        src->cone_outer_gain = 0.0f;
        src->buffer = NULL;
        src->stream = NULL;
        SDL_zero(src->buffer_queue);
        SDL_zero(src->buffer_queue_processed);
        src->queue_channels = 0;
        src->queue_frequency = 0;
        SDL_AtomicSet(&src->allocated, 1);   /* we officially own it. */
    }
}

void alDeleteSources(ALsizei n, const ALuint *names)
{
    ALCcontext *ctx = get_current_context();
    ALsizei i;

    if (!ctx) {
        set_al_error(ctx, AL_INVALID_OPERATION);
        return;
    }

    for (i = 0; i < n; i++) {
        const ALuint name = names[i];
        if (name == 0) {
            /* ignore it. */ FIXME("Spec says alDeleteBuffers() can have a zero name as a legal no-op, but this text isn't included in alDeleteSources...");
        } else if (!is_source_valid(ctx, name)) {
            /* "If one or more of the specified names is not valid, an AL_INVALID_NAME error will be recorded, and no objects will be deleted." */
            set_al_error(ctx, AL_INVALID_NAME);
            return;
        }
    }

    for (i = 0; i < n; i++) {
        const ALuint name = names[i];
        if (name != 0) {
            ALsource *src = &ctx->sources[name - 1];
            FIXME("race conditions for state, buffer queue, etc");
            FIXME("clear buffer queue");
            if (src->stream) {
                SDL_FreeAudioStream(src->stream);
                src->stream = NULL;
            }
            SDL_AtomicSet(&src->allocated, 0);
        }
    }
}

ALboolean alIsSource(ALuint name)
{
    return is_source_valid(get_current_context(), name);
}

void alSourcef(ALuint name, ALenum param, ALfloat value)
{
    switch (param) {
        case AL_GAIN:
        case AL_MIN_GAIN:
        case AL_MAX_GAIN:
        case AL_REFERENCE_DISTANCE:
        case AL_ROLLOFF_FACTOR:
        case AL_MAX_DISTANCE:
        case AL_PITCH:
        case AL_CONE_INNER_ANGLE:
        case AL_CONE_OUTER_ANGLE:
        case AL_CONE_OUTER_GAIN:
        case AL_SEC_OFFSET:
        case AL_SAMPLE_OFFSET:
        case AL_BYTE_OFFSET:
            alSourcefv(name, param, &value);
            return;
        default: break;
    }

    FIXME("What happens if you pass something that needs != ints, like AL_GAIN?");
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);
}

void alSource3f(ALuint name, ALenum param, ALfloat value1, ALfloat value2, ALfloat value3)
{
    switch (param) {
        case AL_POSITION:
        case AL_VELOCITY:
        case AL_DIRECTION: {
            const ALfloat values[3] = { value1, value2, value3 };
            alSourcefv(name, param, values);
            return;
        }
        default: break;
    }

    FIXME("What happens if you pass something that needs != ints, like AL_GAIN?");
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);
}

void alSourcefv(ALuint name, ALenum param, const ALfloat *values)
{
    ALCcontext *ctx = get_current_context();
    ALsource *src = get_source(ctx, name);
    if (!src) return;

    FIXME("this needs a lock");  // ...or atomic operations.

    switch (param) {
        case AL_GAIN: src->gain = *values; return;
        case AL_POSITION: SDL_memcpy(src->position, values, sizeof (ALfloat) * 3); return;
        case AL_VELOCITY: SDL_memcpy(src->velocity, values, sizeof (ALfloat) * 3); return;
        case AL_DIRECTION: SDL_memcpy(src->direction, values, sizeof (ALfloat) * 3); return;
        case AL_MIN_GAIN: src->min_gain = *values; return;
        case AL_MAX_GAIN: src->max_gain = *values; return;
        case AL_REFERENCE_DISTANCE: src->reference_distance = *values; return;
        case AL_ROLLOFF_FACTOR: src->rolloff_factor = *values; return;
        case AL_MAX_DISTANCE: src->max_distance = *values; return;
        case AL_PITCH: src->pitch = *values; return;
        case AL_CONE_INNER_ANGLE: src->cone_inner_angle = *values; return;
        case AL_CONE_OUTER_ANGLE: src->cone_outer_angle = *values; return;
        case AL_CONE_OUTER_GAIN: src->cone_outer_gain = *values; return;

        default: break;
    }

    FIXME("What happens if you pass something that needs != float, like AL_LOOPING?");
    FIXME("extensions?");
    set_al_error(ctx, AL_INVALID_ENUM);
}

void alSourcei(ALuint name, ALenum param, ALint value)
{
    switch (param) {
        case AL_SOURCE_RELATIVE:
        case AL_LOOPING:
        case AL_BUFFER:
        case AL_REFERENCE_DISTANCE:
        case AL_ROLLOFF_FACTOR:
        case AL_MAX_DISTANCE:
        case AL_CONE_INNER_ANGLE:
        case AL_CONE_OUTER_ANGLE:
        case AL_SEC_OFFSET:
        case AL_SAMPLE_OFFSET:
        case AL_BYTE_OFFSET:
            alSourceiv(name, param, &value);
            return;
        default: break;
    }

    FIXME("What happens if you pass something that needs != ints, like AL_GAIN?");
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);
}

void alSource3i(ALuint name, ALenum param, ALint value1, ALint value2, ALint value3)
{
    switch (param) {
        case AL_DIRECTION: {
            const ALint values[3] = { (ALint) value1, (ALint) value2, (ALint) value3 };
            alSourceiv(name, param, values);
            return;
        }
        default: break;
    }

    FIXME("What happens if you pass something that needs != ints, like AL_GAIN?");
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);
}

void alSourceiv(ALuint name, ALenum param, const ALint *values)
{
    ALCcontext *ctx = get_current_context();
    ALsource *src = get_source(ctx, name);
    if (!src) return;

    FIXME("this needs a lock");  // ...or atomic operations.

    switch (param) {
        case AL_BUFFER:
            FIXME("split this into a separate function");
            if ((src->state == AL_PLAYING) || (src->state == AL_PAUSED)) {
                set_al_error(ctx, AL_INVALID_OPERATION);  /* can't change buffer on playing/paused sources */
            } else {
                const ALuint bufname = (ALuint) *values;
                ALbuffer *buffer = NULL;
                if (bufname && ((buffer = get_buffer(ctx, bufname)) == NULL)) {
                    set_al_error(ctx, AL_INVALID_VALUE);
                } else {
                    SDL_AudioStream *stream = NULL;
                    /* We only use the stream for resampling, not for channel conversion. */
                    FIXME("keep the existing stream if formats match?");
                    if (ctx->device->frequency != buffer->frequency) {
                        stream = SDL_NewAudioStream(AUDIO_F32SYS, buffer->channels, buffer->frequency, AUDIO_F32SYS, buffer->channels, ctx->device->frequency);
                        if (!stream) {
                            set_al_error(ctx, AL_OUT_OF_MEMORY);
                            return;
                        }
                        FIXME("need a way to prealloc space in the stream, so the mixer doesn't have to malloc");
                    }

                    if (src->buffer != buffer) {
                        if (src->buffer) {
                            SDL_AtomicDecRef(&src->buffer->refcount);
                        }
                        if (buffer) {
                            SDL_AtomicIncRef(&buffer->refcount);
                        }
                    }
                    src->buffer = buffer;
                    src->type = buffer ? AL_STATIC : AL_UNDETERMINED;
                    src->queue_channels = 0;
                    src->queue_frequency = 0;
                    FIXME("must dump buffer queue");
                    /* if this was AL_PLAYING and the mixer is mixing this source RIGHT NOW you could alSourceStop and call this while it's still mixing. */
                    FIXME("race conditions...");
                    if (src->stream != stream) {
                        if (src->stream) {
                            SDL_FreeAudioStream(src->stream);
                        }
                        src->stream = stream;
                    }
                }
            }
            return;

        case AL_SOURCE_RELATIVE: src->source_relative = *values ? AL_TRUE : AL_FALSE; return;
        case AL_LOOPING: src->looping = *values ? AL_TRUE : AL_FALSE; return;
        case AL_REFERENCE_DISTANCE: src->reference_distance = (ALfloat) *values; return;
        case AL_ROLLOFF_FACTOR: src->rolloff_factor = (ALfloat) *values; return;
        case AL_MAX_DISTANCE: src->max_distance = (ALfloat) *values; return;
        case AL_CONE_INNER_ANGLE: src->cone_inner_angle = (ALfloat) *values; return;
        case AL_CONE_OUTER_ANGLE: src->cone_outer_angle = (ALfloat) *values; return;

        case AL_DIRECTION:
            src->direction[0] = (ALfloat) values[0];
            src->direction[1] = (ALfloat) values[1];
            src->direction[2] = (ALfloat) values[2];
            return;

        case AL_SEC_OFFSET:
        case AL_SAMPLE_OFFSET:
        case AL_BYTE_OFFSET:
            FIXME("offsets");
            /*return*/break;

        default: break;
    }

    FIXME("What happens if you pass something that needs != float, like AL_LOOPING?");
    FIXME("extensions?");
    set_al_error(ctx, AL_INVALID_ENUM);
}

void alGetSourcef(ALuint name, ALenum param, ALfloat *value)
{
    switch (param) {
        case AL_GAIN:
        case AL_MIN_GAIN:
        case AL_MAX_GAIN:
        case AL_REFERENCE_DISTANCE:
        case AL_ROLLOFF_FACTOR:
        case AL_MAX_DISTANCE:
        case AL_PITCH:
        case AL_CONE_INNER_ANGLE:
        case AL_CONE_OUTER_ANGLE:
        case AL_CONE_OUTER_GAIN:
        case AL_SEC_OFFSET:
        case AL_SAMPLE_OFFSET:
        case AL_BYTE_OFFSET:
            alGetSourcefv(name, param, value);
            return;
        default: break;
    }

    FIXME("What happens if you pass something that needs != ints, like AL_GAIN?");
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);
}

void alGetSource3f(ALuint name, ALenum param, ALfloat *value1, ALfloat *value2, ALfloat *value3)
{
    switch (param) {
        case AL_POSITION:
        case AL_VELOCITY:
        case AL_DIRECTION: {
            ALfloat values[3];
            alGetSourcefv(name, param, values);
            if (value1) *value1 = values[0];
            if (value2) *value2 = values[1];
            if (value3) *value3 = values[2];
            return;
        }
        default: break;
    }

    FIXME("What happens if you pass something that needs != ints, like AL_GAIN?");
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);
}

void alGetSourcefv(ALuint name, ALenum param, ALfloat *values)
{
    ALCcontext *ctx = get_current_context();
    ALsource *src = get_source(ctx, name);
    if (!src) return;

    FIXME("this needs a lock");  // ...or atomic operations.

    switch (param) {
        case AL_GAIN: *values = src->gain; return;
        case AL_POSITION: SDL_memcpy(values, src->position, sizeof (ALfloat) * 3); return;
        case AL_VELOCITY: SDL_memcpy(values, src->velocity, sizeof (ALfloat) * 3); return;
        case AL_DIRECTION: SDL_memcpy(values, src->direction, sizeof (ALfloat) * 3); return;
        case AL_MIN_GAIN: *values = src->min_gain; return;
        case AL_MAX_GAIN: *values = src->max_gain; return;
        case AL_REFERENCE_DISTANCE: *values = src->reference_distance; return;
        case AL_ROLLOFF_FACTOR: *values = src->rolloff_factor; return;
        case AL_MAX_DISTANCE: *values = src->max_distance; return;
        case AL_PITCH: *values = src->pitch; return;
        case AL_CONE_INNER_ANGLE: *values = src->cone_inner_angle; return;
        case AL_CONE_OUTER_ANGLE: *values = src->cone_outer_angle; return;
        case AL_CONE_OUTER_GAIN:  *values = src->cone_outer_gain; return;
        default: break;
    }

    FIXME("What happens if you pass something that needs != float, like AL_LOOPING?");
    FIXME("extensions?");
    set_al_error(ctx, AL_INVALID_ENUM);
}

void alGetSourcei(ALuint name, ALenum param, ALint *value)
{
    switch (param) {
        case AL_SOURCE_STATE:
        case AL_SOURCE_RELATIVE:
        case AL_LOOPING:
        case AL_BUFFER:
        case AL_BUFFERS_QUEUED:
        case AL_BUFFERS_PROCESSED:
        case AL_SOURCE_TYPE:
        case AL_REFERENCE_DISTANCE:
        case AL_ROLLOFF_FACTOR:
        case AL_MAX_DISTANCE:
        case AL_CONE_INNER_ANGLE:
        case AL_CONE_OUTER_ANGLE:
        case AL_SEC_OFFSET:
        case AL_SAMPLE_OFFSET:
        case AL_BYTE_OFFSET:
            alGetSourceiv(name, param, value);
            return;
        default: break;
    }

    FIXME("What happens if you pass something that needs != ints, like AL_GAIN?");
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);
}

void alGetSource3i(ALuint name, ALenum param, ALint *value1, ALint *value2, ALint *value3)
{
    switch (param) {
        case AL_DIRECTION: {
            ALint values[3];
            alGetSourceiv(name, param, values);
            if (value1) *value1 = values[0];
            if (value2) *value2 = values[1];
            if (value3) *value3 = values[2];
            return;
        }
        default: break;
    }

    FIXME("What happens if you pass something that needs != ints, like AL_GAIN?");
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);
}

void alGetSourceiv(ALuint name, ALenum param, ALint *values)
{
    ALCcontext *ctx = get_current_context();
    ALsource *src = get_source(ctx, name);
    if (!src) return;

    FIXME("this needs a lock");  // ...or atomic operations.

    switch (param) {
        case AL_SOURCE_STATE: *values = (ALint) src->state; return;
        case AL_SOURCE_TYPE: *values = (ALint) src->type; return;
        case AL_BUFFER: *values = (ALint) src->buffer ? src->buffer->name : 0; return;
        case AL_BUFFERS_QUEUED: *values = (ALint) SDL_AtomicGet(&src->buffer_queue.num_items); return;
        case AL_BUFFERS_PROCESSED: *values = (ALint) SDL_AtomicGet(&src->buffer_queue_processed.num_items); return;
        case AL_SOURCE_RELATIVE: *values = (ALint) src->source_relative; return;
        case AL_LOOPING: *values = (ALint) src->looping; return;
        case AL_REFERENCE_DISTANCE: *values = (ALint) src->reference_distance; return;
        case AL_ROLLOFF_FACTOR: *values = (ALint) src->rolloff_factor; return;
        case AL_MAX_DISTANCE: *values = (ALint) src->max_distance; return;
        case AL_CONE_INNER_ANGLE: *values = (ALint) src->cone_inner_angle; return;
        case AL_CONE_OUTER_ANGLE: *values = (ALint) src->cone_outer_angle; return;
        case AL_DIRECTION:
            values[0] = (ALint) src->direction[0];
            values[1] = (ALint) src->direction[1];
            values[2] = (ALint) src->direction[2];
            return;

        case AL_SEC_OFFSET:
        case AL_SAMPLE_OFFSET:
        case AL_BYTE_OFFSET:
            FIXME("offsets");
            break; /*return;*/

        default: break;
    }

    FIXME("What happens if you pass something that needs != float, like AL_LOOPING?");
    FIXME("extensions?");
    set_al_error(ctx, AL_INVALID_ENUM);
}

static void source_play(ALCcontext *ctx, const ALuint name)
{
    ALsource *src = get_source(ctx, name);
    if (src) {
        FIXME("this needs a lock");
        if (src->state != AL_PAUSED) {
            src->offset = 0;
        }
        src->state = AL_PLAYING;
    }
}

static void source_stop(ALCcontext *ctx, const ALuint name)
{
    ALsource *src = get_source(ctx, name);
    if (src) {
        FIXME("this needs a lock");
        if (src->state != AL_INITIAL) {
            src->state = AL_STOPPED;
        }
    }
}

static void source_rewind(ALCcontext *ctx, const ALuint name)
{
    ALsource *src = get_source(ctx, name);
    if (src) {
        FIXME("this needs a lock");
        src->state = AL_INITIAL;
        src->offset = 0;
    }
}

static void source_pause(ALCcontext *ctx, const ALuint name)
{
    ALsource *src = get_source(ctx, name);
    if (src) {
        FIXME("this needs a lock");
        if (src->state == AL_PLAYING) {
            src->state = AL_PAUSED;
        }
    }
}

/* deal with alSourcePlay and alSourcePlayv (etc) boiler plate... */
#define SOURCE_STATE_TRANSITION_OP(alfn, fn) \
    void alSource##alfn(ALuint name) { source_##fn(get_current_context(), name); } \
    void alSource##alfn##v(ALsizei n, const ALuint *sources) { \
        ALCcontext *ctx = get_current_context(); \
        if (!ctx) { \
            set_al_error(ctx, AL_INVALID_OPERATION); \
        } else { \
            ALsizei i; \
            SDL_LockAudioDevice(ctx->device->sdldevice);  /* lock the SDL device so these all start mixing in the same callback. */ \
            for (i = 0; i < n; i++) { \
                source_##fn(ctx, sources[i]); \
            } \
            SDL_UnlockAudioDevice(ctx->device->sdldevice); \
        } \
    }

SOURCE_STATE_TRANSITION_OP(Play, play)
SOURCE_STATE_TRANSITION_OP(Stop, stop)
SOURCE_STATE_TRANSITION_OP(Rewind, rewind)
SOURCE_STATE_TRANSITION_OP(Pause, pause)


void alSourceQueueBuffers(ALuint name, ALsizei nb, const ALuint *bufnames)
{
    BufferQueueItem *queue = NULL;
    BufferQueueItem *queueend = NULL;
    void *ptr;
    ALsizei i;
    ALCcontext *ctx = get_current_context();
    ALsource *src = get_source(ctx, name);
    ALint queue_channels = 0;
    ALsizei queue_frequency = 0;
    ALboolean failed = AL_FALSE;
    SDL_AudioStream *stream = NULL;

    if (!src) {
        return;
    }

    if (src->type == AL_STATIC) {
        set_al_error(ctx, AL_INVALID_OPERATION);
        return;
    }

    if (nb == 0) {
        return;  /* nothing to do. */
    }

    for (i = nb; i > 0; i--) {  /* build list in reverse */
        BufferQueueItem *item = NULL;
        const ALuint bufname = bufnames[i-1];
        ALbuffer *buffer = bufname ? get_buffer(ctx, bufname) : NULL;
        if (!buffer && bufname) {  /* uhoh, bad buffer name! */
            set_al_error(ctx, AL_INVALID_VALUE);
            failed = AL_TRUE;
            break;
        }

        if (buffer) {
            if (queue_channels == 0) {
                SDL_assert(queue_frequency == 0);
                queue_channels = buffer->channels;
                queue_frequency = buffer->frequency;
            } else if ((queue_channels != buffer->channels) || (queue_frequency != buffer->frequency)) {
                /* the whole queue must be the same format. */
                set_al_error(ctx, AL_INVALID_VALUE);
                failed = AL_TRUE;
                break;
            }
        }

        do {
            ptr = SDL_AtomicGetPtr(&ctx->device->playback.buffer_queue_pool);
            item = (BufferQueueItem *) ptr;
            if (!item) break;
            ptr = item->next;
        } while (!SDL_AtomicCASPtr(&ctx->device->playback.buffer_queue_pool, item, ptr));

        if (!item) {  /* allocate a new item */
            item = (BufferQueueItem *) SDL_calloc(1, sizeof (BufferQueueItem));
            if (!item) {
                set_al_error(ctx, AL_OUT_OF_MEMORY);
                failed = AL_TRUE;
                break;
            }
        }

        item->buffer = buffer;

        SDL_assert((queue != NULL) == (queueend != NULL));
        if (queueend) {
            queueend->next = item;
        } else {
            queue = item;
        }
        queueend = item;
    }

    if (!failed) {
        if (src->queue_frequency && queue_frequency) {  /* could be zero if we only queued AL name 0. */
            SDL_assert(src->queue_channels);
            SDL_assert(queue_channels);
            if ((src->queue_channels != queue_channels) || (src->queue_frequency != queue_frequency)) {
                set_al_error(ctx, AL_INVALID_VALUE);
                failed = AL_TRUE;
            }
        }
    }

    if (!src->queue_frequency) {
        SDL_assert(!src->queue_channels);
        SDL_assert(!src->stream);
        /* We only use the stream for resampling, not for channel conversion. */
        if (ctx->device->frequency != queue_frequency) {
            stream = SDL_NewAudioStream(AUDIO_F32SYS, queue_channels, queue_frequency, AUDIO_F32SYS, queue_channels, ctx->device->frequency);
            if (!stream) {
                set_al_error(ctx, AL_OUT_OF_MEMORY);
                failed = AL_TRUE;
            }
            FIXME("need a way to prealloc space in the stream, so the mixer doesn't have to malloc");
        }
    }

    if (failed) {
        if (queue) {
            /* put the whole new queue back in the pool for reuse later. */
            do {
                ptr = SDL_AtomicGetPtr(&ctx->device->playback.buffer_queue_pool);
                SDL_AtomicSetPtr(&queueend->next, ptr);
            } while (!SDL_AtomicCASPtr(&ctx->device->playback.buffer_queue_pool, ptr, queue));
        }
        if (stream) {
            SDL_FreeAudioStream(stream);
        }
        return;
    }

    src->type = AL_STREAMING;

    if (!src->queue_channels) {
        src->queue_channels = queue_channels;
        src->queue_frequency = queue_frequency;
        src->stream = stream;
    }

    /* so we're going to put these on a linked list called just_queued,
        where things build up in reverse order, to keep this on a single
        pointer. The theory is we'll atomicgetptr the pointer, set that
        pointer as the "next" for our list, and then atomiccasptr our new
        list against the original pointer. If the CAS succeeds, we have
        a complete list, atomically set. If it fails, try again with
        the new pointer we found, updating our next pointer again. It'll
        either be NULL (the mixer got it) or some other pointer (another
        thread queued something while we were working).

        The mixer does an atomiccasptr to grab the current list, swapping
        in a NULL. Once it has the list, it's safe to do what it likes
        with it, as nothing else owns the pointers in that list. */

    do {
        ptr = SDL_AtomicGetPtr(&src->buffer_queue.just_queued);
        SDL_AtomicSetPtr(&queueend->next, ptr);
    } while (!SDL_AtomicCASPtr(&src->buffer_queue.just_queued, ptr, queue));

    SDL_AtomicAdd(&src->buffer_queue.num_items, (int) nb);
}

void alSourceUnqueueBuffers(ALuint name, ALsizei nb, ALuint *bufnames)
{
    BufferQueueItem *queueend = NULL;
    BufferQueueItem *queue;
    BufferQueueItem *item;
    void *ptr;
    ALsizei i;
    ALCcontext *ctx = get_current_context();
    ALsource *src = get_source(ctx, name);
    if (!src) {
        return;
    }

    if (src->type == AL_STATIC) {
        set_al_error(ctx, AL_INVALID_OPERATION);
        return;
    }

    if (nb == 0) {
        return;  /* nothing to do. */
    }

    /* this could be kinda a long lock, but only if you have two threads
       trying to unqueue from the same source right after the mixer moved
       an obscenely large number of buffers to the processed queue. That is
       to say: it's a pathological (and probably not ever real) scenario. */
    SDL_AtomicLock(&src->buffer_queue_lock);
    if (((ALsizei) SDL_AtomicGet(&src->buffer_queue.num_items)) < nb) {
        SDL_AtomicUnlock(&src->buffer_queue_lock);
        set_al_error(ctx, AL_INVALID_VALUE);
        return;
    }

    SDL_AtomicAdd(&src->buffer_queue_processed.num_items, -((int) nb));

    obtain_newly_queued_buffers(&src->buffer_queue_processed);

    item = queue = src->buffer_queue_processed.head;
    for (i = 0; i < nb; i++) {
        /* buffer_queue_processed.num_items said list was long enough. */
        SDL_assert(item != NULL);
        item = item->next;
    }
    src->buffer_queue_processed.head = item;
    if (!item) {
        src->buffer_queue_processed.tail = NULL;
    }

    SDL_AtomicUnlock(&src->buffer_queue_lock);

    item = queue;
    for (i = 0; i < nb; i++) {
        bufnames[i] = item->buffer ? item->buffer->name : 0;
        queueend = item;
        item = item->next;
    }

    /* put the whole new queue back in the pool for reuse later. */
    SDL_assert(queueend != NULL);
    do {
        ptr = SDL_AtomicGetPtr(&ctx->device->playback.buffer_queue_pool);
        SDL_AtomicSetPtr(&queueend->next, ptr);
    } while (!SDL_AtomicCASPtr(&ctx->device->playback.buffer_queue_pool, ptr, queue));
}

void alGenBuffers(ALsizei n, ALuint *names)
{
    ALCcontext *ctx = get_current_context();
    BufferBlock *endblock;
    BufferBlock *block;
    ALbuffer **objects = NULL;
    ALsizei found = 0;
    ALuint block_offset = 0;
    ALuint i;

    if (!ctx) {
        set_al_error(ctx, AL_INVALID_OPERATION);
        return;
    }

    objects = SDL_calloc(n, sizeof (ALbuffer *));
    if (!objects) {
        set_al_error(ctx, AL_OUT_OF_MEMORY);
        return;
    }

    FIXME("add an indexing array instead of walking the buffer blocks for lookup?");  // thread safety, blah blah blah

    block = endblock = &ctx->device->playback.buffer_blocks;  /* the first one is a static piece of the context */
    while (found < n) {
        if (!block) {  /* out of blocks? Add a new one. */
            block = (BufferBlock *) SDL_calloc(1, sizeof (BufferBlock));
            if (!block) {
                for (i = 0; i < (ALuint) found; i++) {
                    SDL_AtomicSet(&objects[i]->allocated, 0);  /* return any temp-acquired buffers. */
                }
                SDL_free(objects);
                SDL_memset(names, '\0', sizeof (*names) * n);
                set_al_error(ctx, AL_OUT_OF_MEMORY);
                return;
            }

            if (!SDL_AtomicCASPtr(&endblock->next, NULL, block)) {
                /* another thread beat us to adding a new block; free our new block, try again with theirs. */
                SDL_free(block);
                endblock = SDL_AtomicGetPtr(&endblock->next);
                block = endblock;
            }
        }

        for (i = 0; i < SDL_arraysize(block->buffers); i++) {
            if (SDL_AtomicCAS(&block->buffers[i].allocated, 0, 2)) {  /* 0==unused, 1==in use, 2==trying to acquire. */
                objects[found] = &block->buffers[i];
                names[found++] = (i + block_offset) + 1;  /* +1 so it isn't zero. */
                if (found == n) {
                    break;
                }
            }
        }

        if (found == n) {
            break;
        }

        endblock = block;
        block = (BufferBlock *) SDL_AtomicGetPtr(&block->next);
        block_offset += SDL_arraysize(block->buffers);
    }

    SDL_assert(found == n);  /* we should have either gotten space or bailed on alloc failure */

    for (i = 0; i < (ALuint) n; i++) {
        ALbuffer *buffer = objects[i];
        /* don't SDL_zerop() this buffer, because we need buffer->allocated to stay at 2 until initialized. */
        buffer->name = names[i];
        buffer->channels = 1;
        buffer->bits = 16;
        buffer->frequency = 0;
        buffer->len = 0;
        buffer->data = NULL;
        SDL_AtomicSet(&buffer->refcount, 0);
        SDL_AtomicSet(&buffer->allocated, 1);  /* we officially own it. */
    }

    SDL_free(objects);
}

void alDeleteBuffers(ALsizei n, const ALuint *names)
{
    ALCcontext *ctx = get_current_context();
    ALsizei i;

    if (!ctx) {
        set_al_error(ctx, AL_INVALID_OPERATION);
        return;
    }

    for (i = 0; i < n; i++) {
        const ALuint name = names[i];
        if (name == 0) {
            /* ignore it. */
        } else {
            ALbuffer *buffer = get_buffer(ctx, name);
            if (!buffer) {
                /* "If one or more of the specified names is not valid, an AL_INVALID_NAME error will be recorded, and no objects will be deleted." */
                set_al_error(ctx, AL_INVALID_NAME);
                return;
            } else if (SDL_AtomicGet(&buffer->refcount) != 0) {
                set_al_error(ctx, AL_INVALID_OPERATION);  /* still in use */
                return;
            }
        }
    }

    for (i = 0; i < n; i++) {
        const ALuint name = names[i];
        if (name != 0) {
            ALbuffer *buffer = get_buffer(ctx, name);
            SDL_assert(buffer != NULL);
            void *data = (void *) buffer->data;
            if (!SDL_AtomicCAS(&buffer->allocated, 1, 0)) {
                /* uh-oh!! */
            } else {
                buffer->data = NULL;
                SDL_free(data);
            }
        }
    }
}

ALboolean alIsBuffer(ALuint name)
{
    ALCcontext *ctx = get_current_context();
    return (ctx && (get_buffer(ctx, name) != NULL)) ? AL_TRUE : AL_FALSE;
}

void alBufferData(ALuint name, ALenum alfmt, const ALvoid *data, ALsizei size, ALsizei freq)
{
    ALCcontext *ctx = get_current_context();
    ALbuffer *buffer = get_buffer(ctx, name);
    SDL_AudioCVT sdlcvt;
    Uint8 channels;
    SDL_AudioFormat sdlfmt;
    ALCsizei framesize;
    int rc;
    int prevrefcount;

    if (!buffer) return;

    if (!alcfmt_to_sdlfmt(alfmt, &sdlfmt, &channels, &framesize)) {
        set_al_error(ctx, AL_INVALID_VALUE);
        return;
    }

    /* increment refcount so this can't be deleted or alBufferData'd from another thread */
    prevrefcount = SDL_AtomicIncRef(&buffer->refcount);
    SDL_assert(prevrefcount >= 0);
    if (prevrefcount != 0) {
        /* this buffer is being used by some source. Unqueue it first. */
        SDL_AtomicDecRef(&buffer->refcount);
        set_al_error(ctx, AL_INVALID_OPERATION);
        return;
    }

    if (SDL_AtomicGet(&buffer->allocated) != 1) {
        /* uhoh, something deleted us before we could IncRef! */
        /* don't decref; until reallocated, it's meaningless. When reallocated, it's forced to zero. */
        set_al_error(ctx, AL_INVALID_NAME);
        return;
    }

    /* right now we take a moment to convert the data to float32, since that's
       the format we want to work in, but we don't resample or change the channels */
    SDL_zero(sdlcvt);
    rc = SDL_BuildAudioCVT(&sdlcvt, sdlfmt, channels, (int) freq, AUDIO_F32SYS, channels, (int) freq);
    if (rc == -1) {
        SDL_AtomicDecRef(&buffer->refcount);
        set_al_error(ctx, AL_OUT_OF_MEMORY);  /* not really, but oh well. */
        return;
    }

    sdlcvt.len = sdlcvt.len_cvt = size;
    sdlcvt.buf = (Uint8 *) SDL_malloc(size * sdlcvt.len_mult);
    if (!sdlcvt.buf) {
        SDL_AtomicDecRef(&buffer->refcount);
        set_al_error(ctx, AL_OUT_OF_MEMORY);
        return;
    }
    SDL_memcpy(sdlcvt.buf, data, size);

    if (rc == 1) {  /* conversion necessary */
        rc = SDL_ConvertAudio(&sdlcvt);
        SDL_assert(rc == 0);  /* this shouldn't fail. */
        if (sdlcvt.len_cvt < (size * sdlcvt.len_mult)) {  /* maybe shrink buffer */
            void *ptr = SDL_realloc(sdlcvt.buf, sdlcvt.len_cvt);
            if (ptr) {
                sdlcvt.buf = (Uint8 *) ptr;
            }
        }
    }

    SDL_free((void *) buffer->data);  /* nuke any previous data. */
    buffer->data = (const float *) sdlcvt.buf;
    buffer->channels = (ALint) channels;
    buffer->bits = (ALint) SDL_AUDIO_BITSIZE(sdlfmt);  /* we're in float32, though. */
    buffer->frequency = freq;
    buffer->len = (ALsizei) sdlcvt.len_cvt;

    SDL_AtomicDecRef(&buffer->refcount);  /* ready to go! */
}

void alBufferf(ALuint name, ALenum param, ALfloat value)
{
    /* nothing in core OpenAL 1.1 uses this */
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);
}

void alBuffer3f(ALuint name, ALenum param, ALfloat value1, ALfloat value2, ALfloat value3)
{
    /* nothing in core OpenAL 1.1 uses this */
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);
}

void alBufferfv(ALuint name, ALenum param, const ALfloat *values)
{
    /* nothing in core OpenAL 1.1 uses this */
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);
}

void alBufferi(ALuint name, ALenum param, ALint value)
{
    /* nothing in core OpenAL 1.1 uses this */
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);
}

void alBuffer3i(ALuint name, ALenum param, ALint value1, ALint value2, ALint value3)
{
    /* nothing in core OpenAL 1.1 uses this */
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);
}

void alBufferiv(ALuint name, ALenum param, const ALint *values)
{
    /* nothing in core OpenAL 1.1 uses this */
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);
}

void alGetBufferf(ALuint name, ALenum param, ALfloat *value)
{
    /* nothing in core OpenAL 1.1 uses this */
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);
}

void alGetBuffer3f(ALuint name, ALenum param, ALfloat *value1, ALfloat *value2, ALfloat *value3)
{
    /* nothing in core OpenAL 1.1 uses this */
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);
}

void alGetBufferfv(ALuint name, ALenum param, ALfloat *values)
{
    /* nothing in core OpenAL 1.1 uses this */
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);
}

void alGetBufferi(ALuint name, ALenum param, ALint *value)
{
    switch (param) {
        case AL_FREQUENCY:
        case AL_SIZE:
        case AL_BITS:
        case AL_CHANNELS:
            alGetBufferiv(name, param, value);
            return;
        default: break;
    }

    FIXME("What happens if you pass something that needs != ints?");
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);
}

void alGetBuffer3i(ALuint name, ALenum param, ALint *value1, ALint *value2, ALint *value3)
{
    /* nothing in core OpenAL 1.1 uses this */
    FIXME("extensions?");
    set_al_error(get_current_context(), AL_INVALID_ENUM);
}

void alGetBufferiv(ALuint name, ALenum param, ALint *values)
{
    ALCcontext *ctx = get_current_context();
    ALbuffer *buffer = get_buffer(ctx, name);
    if (!buffer) return;

    FIXME("this needs a lock");  // ...or atomic operations.
    switch (param) {
        case AL_FREQUENCY: *values = (ALint) buffer->frequency; return;
        case AL_SIZE: *values = (ALint) buffer->len; return;
        case AL_BITS: *values = (ALint) buffer->bits; return;
        case AL_CHANNELS: *values = (ALint) buffer->channels; return;
        default: break;
    }

    FIXME("extensions?");
    set_al_error(ctx, AL_INVALID_ENUM);
}

/* end of mojoal.c ... */

