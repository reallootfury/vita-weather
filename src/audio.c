#include "audio.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/audioout.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <tremor/ivorbisfile.h>

#include "net.h"

#define AUDIO_FRAMES 1024

enum {
    AUDIO_AMBIENCE_NONE = -1,
    AUDIO_AMBIENCE_RAIN = 0,
    AUDIO_AMBIENCE_STORM,
    AUDIO_AMBIENCE_COUNT
};

static const char *const g_ambience_paths[AUDIO_AMBIENCE_COUNT] = {
    "app0:weather-rain.ogg",
    "app0:weather-storm.ogg"
};

static volatile int g_audio_running;
static volatile int g_audio_target = AUDIO_AMBIENCE_NONE;
static SceUID g_audio_thread = -1;
static int g_audio_unavailable;

static int ambience_for_weather(const WeatherData *weather)
{
    if (!weather) return AUDIO_AMBIENCE_NONE;
    int group = weather_condition_group(weather->weather_code);
    if (group == 2) return AUDIO_AMBIENCE_RAIN;
    if (group == 4) return AUDIO_AMBIENCE_STORM;
    return AUDIO_AMBIENCE_NONE;
}

static int open_ambience(int target, OggVorbis_File *stream)
{
    if (target < 0 || target >= AUDIO_AMBIENCE_COUNT) return -1;
    FILE *file = fopen(g_ambience_paths[target], "rb");
    if (!file) return -1;
    memset(stream, 0, sizeof(*stream));
    if (ov_open(file, stream, NULL, 0) < 0) {
        fclose(file);
        return -1;
    }
    vorbis_info *info = ov_info(stream, -1);
    if (!info || info->rate != 48000 || info->channels != 2) {
        ov_clear(stream);
        return -1;
    }
    return 0;
}

static int audio_thread_main(unsigned int args, void *argp)
{
    (void)args;
    (void)argp;
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AUDIO_FRAMES,
                                   48000, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) {
        app_log("weather ambience audio port failed: 0x%08x", port);
        return port;
    }
    app_log("weather ambience BGM port opened: 48000 Hz stereo");
    int volume[2] = {6200, 6200};
    sceAudioOutSetVolume(port,
                         SCE_AUDIO_VOLUME_FLAG_L_CH |
                         SCE_AUDIO_VOLUME_FLAG_R_CH,
                         volume);

    int16_t pcm[AUDIO_FRAMES * 2] __attribute__((aligned(64)));
    OggVorbis_File stream;
    int stream_open = 0;
    int opened_target = AUDIO_AMBIENCE_NONE;

    while (g_audio_running) {
        int target = g_audio_target;
        if (target != opened_target) {
            if (stream_open) {
                ov_clear(&stream);
                stream_open = 0;
            }
            opened_target = target;
            if (target >= 0 && target < AUDIO_AMBIENCE_COUNT) {
                stream_open = open_ambience(target, &stream) == 0;
                app_log("weather ambience loop: target=%d path='%s' status=%s",
                        target, g_ambience_paths[target],
                        stream_open ? "playing" : "unavailable");
            }
        }
        if (!stream_open) {
            sceKernelDelayThread(50000);
            continue;
        }

        int filled = 0;
        int failures = 0;
        while (filled < (int)sizeof(pcm) && failures < 4) {
            int section = 0;
            long got = ov_read(&stream, (unsigned char *)pcm + filled,
                               (int)sizeof(pcm) - filled, &section);
            if (got > 0) {
                filled += (int)got;
                failures = 0;
            } else if (got == 0) {
                if (ov_pcm_seek(&stream, 0) < 0) ++failures;
            } else {
                ++failures;
            }
        }
        if (filled < (int)sizeof(pcm))
            memset((unsigned char *)pcm + filled, 0, sizeof(pcm) - filled);
        sceAudioOutOutput(port, pcm);
    }

    if (stream_open) ov_clear(&stream);
    sceAudioOutOutput(port, NULL);
    sceAudioOutReleasePort(port);
    return 0;
}

int weather_audio_init(void)
{
    if (g_audio_thread >= 0) return 0;
    if (g_audio_unavailable) return -1;
    int model = sceKernelGetModel();
    if (model != SCE_KERNEL_MODEL_VITA &&
        model != SCE_KERNEL_MODEL_VITATV) {
        /* Vita3K can expose a zero model while its SDL audio backend is
         * unavailable; calling sceAudioOutOpenPort then terminates this
         * emulator build instead of returning an error. Real hardware reports
         * one of the public model constants, so fail closed only on an invalid
         * platform identity. */
        g_audio_unavailable = 1;
        app_log("weather ambience unavailable: invalid platform model=0x%x",
                model);
        return -1;
    }
    g_audio_running = 1;
    g_audio_target = AUDIO_AMBIENCE_NONE;
    g_audio_thread = sceKernelCreateThread("weather_audio", audio_thread_main,
                                           0x10000120, 96 * 1024,
                                           0, 0, NULL);
    if (g_audio_thread < 0) return g_audio_thread;
    int result = sceKernelStartThread(g_audio_thread, 0, NULL);
    if (result < 0) {
        sceKernelDeleteThread(g_audio_thread);
        g_audio_thread = -1;
        g_audio_running = 0;
        return result;
    }
    app_log("weather ambience ready: public-domain Ogg loops, default off");
    return 0;
}

void weather_audio_update(const WeatherData *weather, int enabled)
{
    int target = enabled ? ambience_for_weather(weather) : AUDIO_AMBIENCE_NONE;
    if (target != AUDIO_AMBIENCE_NONE && g_audio_thread < 0) {
        int result = weather_audio_init();
        if (result < 0) {
            if (!g_audio_unavailable)
                app_log("weather ambience unavailable: 0x%08x", result);
            return;
        }
    }
    if (target == g_audio_target) return;
    g_audio_target = target;
    app_log("weather ambience changed: enabled=%d target=%d", enabled, target);
}

void weather_audio_shutdown(void)
{
    g_audio_running = 0;
    if (g_audio_thread >= 0) {
        sceKernelWaitThreadEnd(g_audio_thread, NULL, NULL);
        sceKernelDeleteThread(g_audio_thread);
        g_audio_thread = -1;
    }
    g_audio_target = AUDIO_AMBIENCE_NONE;
}
