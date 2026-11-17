#include "ds_audio.h"
#include "app_log.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RATE            48000
#define HAPTICS_RATE    3000
#define UPSAMPLE        (RATE / HAPTICS_RATE)      /* 16 */
#define OUT_CHANNELS    4
#define MIC_CHANNELS    2
#define CHUNK_FRAMES    480                        /* 10 ms */
#define RING_FRAMES     4800                       /* 100 ms of haptics */
#define MAX_QUEUED      2880                       /* > 60 ms queued: fell behind, drop */
#define KEEP_AFTER_DROP 960                        /* keep the newest 20 ms */
#define LATENCY_US      30000

struct DsAudio {
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    volatile bool   running;
    bool            want_haptics, want_mic;

    int             card;            /* ALSA card index of the DualSense, -1 = none */
    bool            haptics_open;

    /* haptics ring: stereo frames at 48 kHz */
    int16_t         ring[RING_FRAMES][2];
    int             ring_head, ring_count;
    int16_t         prev_l, prev_r;  /* last input sample, for interpolation */
    unsigned long   dropped;

    bool            mic_on;
    DsAudioMicCb    mic_cb;
    void           *mic_user;

    pthread_t       play_thread, cap_thread;
};

static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }

/* Card index from /proc/asound/cards, e.g.
 * " 2 [Controller     ]: USB-Audio - DualSense Wireless Controller" */
static int find_dualsense_card(void)
{
    FILE *f = fopen("/proc/asound/cards", "r");
    if (!f) return -1;
    char line[256];
    int card = -1;
    while (fgets(line, sizeof(line), f)) {
        int idx;
        if (sscanf(line, " %d [", &idx) == 1 && strstr(line, "DualSense")) { card = idx; break; }
    }
    fclose(f);
    return card;
}

static snd_pcm_t *open_pcm(int card, snd_pcm_stream_t dir, unsigned channels)
{
    char dev[32];
    snprintf(dev, sizeof(dev), "hw:%d,0", card);
    snd_pcm_t *pcm = NULL;
    int r = snd_pcm_open(&pcm, dev, dir, 0);
    if (r < 0) {
        app_log_always("[DSAUDIO] open %s %s failed: %s\n", dev,
                       dir == SND_PCM_STREAM_PLAYBACK ? "playback" : "capture", snd_strerror(r));
        return NULL;
    }
    r = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                           channels, RATE, 0, LATENCY_US);
    if (r < 0) {
        app_log_always("[DSAUDIO] %s: %u ch @ %d Hz not accepted: %s\n", dev, channels, RATE, snd_strerror(r));
        snd_pcm_close(pcm);
        return NULL;
    }
    return pcm;
}

/* The card's USB volume controls come up at 0 % playback ("PCM Playback
 * Volume") on the TV, which silences the haptic actuators too.  Set playback
 * to full (the haptics level comes from the stream) and the headset mic to a
 * usable gain. */
static void setup_mixer(int card)
{
    char dev[16];
    snprintf(dev, sizeof(dev), "hw:%d", card);
    snd_mixer_t *mx = NULL;
    if (snd_mixer_open(&mx, 0) < 0) return;
    if (snd_mixer_attach(mx, dev) < 0 || snd_mixer_selem_register(mx, NULL, NULL) < 0 ||
        snd_mixer_load(mx) < 0) {
        snd_mixer_close(mx);
        return;
    }
    for (snd_mixer_elem_t *e = snd_mixer_first_elem(mx); e; e = snd_mixer_elem_next(e)) {
        const char *name = snd_mixer_selem_get_name(e);
        long mn = 0, mxv = 0;
        if (snd_mixer_selem_has_playback_volume(e) &&
            snd_mixer_selem_get_playback_volume_range(e, &mn, &mxv) == 0) {
            snd_mixer_selem_set_playback_volume_all(e, mxv);
            app_log_always("[DSAUDIO] mixer '%s' playback volume -> %ld (max)\n", name, mxv);
        }
        if (snd_mixer_selem_has_playback_switch(e))
            snd_mixer_selem_set_playback_switch_all(e, 1);
        if (snd_mixer_selem_has_capture_volume(e) &&
            snd_mixer_selem_get_capture_volume_range(e, &mn, &mxv) == 0) {
            long v = mn + (mxv - mn) * 7 / 10;
            snd_mixer_selem_set_capture_volume_all(e, v);
            app_log_always("[DSAUDIO] mixer '%s' capture volume -> %ld (of %ld)\n", name, v, mxv);
        }
        if (snd_mixer_selem_has_capture_switch(e))
            snd_mixer_selem_set_capture_switch_all(e, 1);
    }
    snd_mixer_close(mx);
}

/* ── Playback (haptics) + card discovery ──────────────────────────────────── */

static void *play_main(void *arg)
{
    DsAudio *a = arg;
    snd_pcm_t *pcm = NULL;
    int16_t chunk[CHUNK_FRAMES * OUT_CHANNELS];

    while (a->running) {
        if (!pcm) {
            int card = find_dualsense_card();
            pthread_mutex_lock(&a->lock);
            if (card != a->card) {
                if (card >= 0) {
                    app_log_always("[DSAUDIO] DualSense sound card %d found\n", card);
                    setup_mixer(card);
                } else {
                    app_log_always("[DSAUDIO] DualSense sound card gone\n");
                }
                a->card = card;
                pthread_cond_broadcast(&a->cond);
            }
            pthread_mutex_unlock(&a->lock);

            if (card >= 0 && a->want_haptics)
                pcm = open_pcm(card, SND_PCM_STREAM_PLAYBACK, OUT_CHANNELS);
            if (!pcm) { sleep_ms(1000); continue; }

            pthread_mutex_lock(&a->lock);
            a->haptics_open = true;
            a->ring_count = 0;
            pthread_mutex_unlock(&a->lock);
            app_log_always("[DSAUDIO] HD haptics on (card %d, %d ch @ %d Hz)\n", card, OUT_CHANNELS, RATE);
        }

        /* 10 ms chunk: channels 1-2 silent, 3-4 the haptics actuators. */
        memset(chunk, 0, sizeof(chunk));
        pthread_mutex_lock(&a->lock);
        int n = a->ring_count < CHUNK_FRAMES ? a->ring_count : CHUNK_FRAMES;
        for (int i = 0; i < n; i++) {
            const int16_t *s = a->ring[(a->ring_head + i) % RING_FRAMES];
            chunk[i * OUT_CHANNELS + 2] = s[0];
            chunk[i * OUT_CHANNELS + 3] = s[1];
        }
        a->ring_head = (a->ring_head + n) % RING_FRAMES;
        a->ring_count -= n;
        pthread_mutex_unlock(&a->lock);

        snd_pcm_sframes_t w = snd_pcm_writei(pcm, chunk, CHUNK_FRAMES);   /* paces the loop */
        if (w < 0) {
            int r = snd_pcm_recover(pcm, (int)w, 1);
            if (r < 0) {
                app_log_always("[DSAUDIO] haptics output error: %s — closing\n", snd_strerror((int)w));
                snd_pcm_close(pcm);
                pcm = NULL;
                pthread_mutex_lock(&a->lock);
                a->haptics_open = false;
                pthread_mutex_unlock(&a->lock);
                sleep_ms(500);
            }
        }
    }
    if (pcm) {
        snd_pcm_drop(pcm);
        snd_pcm_close(pcm);
    }
    return NULL;
}

/* ── Capture (microphone) ─────────────────────────────────────────────────── */

static void *cap_main(void *arg)
{
    DsAudio *a = arg;
    snd_pcm_t *pcm = NULL;
    int16_t buf[DS_AUDIO_MIC_FRAMES * MIC_CHANNELS];
    unsigned long frames = 0;

    while (a->running) {
        pthread_mutex_lock(&a->lock);
        bool want = a->mic_on && a->card >= 0;
        int card = a->card;
        if (!want) {
            pthread_mutex_unlock(&a->lock);
            if (pcm) {
                snd_pcm_drop(pcm);
                snd_pcm_close(pcm);
                pcm = NULL;
                app_log_always("[DSAUDIO] microphone off (%lu frames sent)\n", frames);
            }
            pthread_mutex_lock(&a->lock);
            if (a->running && !(a->mic_on && a->card >= 0)) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 200 * 1000000L;
                if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
                pthread_cond_timedwait(&a->cond, &a->lock, &ts);
            }
            pthread_mutex_unlock(&a->lock);
            continue;
        }
        pthread_mutex_unlock(&a->lock);

        if (!pcm) {
            pcm = open_pcm(card, SND_PCM_STREAM_CAPTURE, MIC_CHANNELS);
            if (!pcm) { sleep_ms(1000); continue; }
            frames = 0;
            app_log_always("[DSAUDIO] microphone on (card %d, %d ch @ %d Hz)\n", card, MIC_CHANNELS, RATE);
        }

        snd_pcm_sframes_t r = snd_pcm_readi(pcm, buf, DS_AUDIO_MIC_FRAMES);
        if (r == DS_AUDIO_MIC_FRAMES) {
            frames++;
            pthread_mutex_lock(&a->lock);
            bool on = a->mic_on;
            pthread_mutex_unlock(&a->lock);
            if (on && a->mic_cb) a->mic_cb(buf, a->mic_user);
        } else if (r < 0) {
            if (snd_pcm_recover(pcm, (int)r, 1) < 0) {
                app_log_always("[DSAUDIO] microphone error: %s — closing\n", snd_strerror((int)r));
                snd_pcm_close(pcm);
                pcm = NULL;
                sleep_ms(500);
            }
        }
    }
    if (pcm) {
        snd_pcm_drop(pcm);
        snd_pcm_close(pcm);
    }
    return NULL;
}

/* ── API ──────────────────────────────────────────────────────────────────── */

DsAudio *ds_audio_new(bool haptics, bool mic, DsAudioMicCb mic_cb, void *mic_user)
{
    if (!haptics && !mic) return NULL;
    DsAudio *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    pthread_mutex_init(&a->lock, NULL);
    pthread_cond_init(&a->cond, NULL);
    a->running = true;
    a->want_haptics = haptics;
    a->want_mic = mic;
    a->card = -1;
    a->mic_cb = mic_cb;
    a->mic_user = mic_user;
    if (pthread_create(&a->play_thread, NULL, play_main, a) != 0) {
        free(a);
        return NULL;
    }
    if (mic && pthread_create(&a->cap_thread, NULL, cap_main, a) != 0)
        a->want_mic = false;
    return a;
}

void ds_audio_free(DsAudio *a)
{
    if (!a) return;
    pthread_mutex_lock(&a->lock);
    a->running = false;
    a->mic_on = false;
    pthread_cond_broadcast(&a->cond);
    pthread_mutex_unlock(&a->lock);
    pthread_join(a->play_thread, NULL);
    if (a->want_mic) pthread_join(a->cap_thread, NULL);
    if (a->dropped)
        app_log_always("[DSAUDIO] haptics: dropped %lu late frames\n", a->dropped);
    pthread_cond_destroy(&a->cond);
    pthread_mutex_destroy(&a->lock);
    free(a);
}

bool ds_audio_haptics_active(DsAudio *a)
{
    if (!a) return false;
    pthread_mutex_lock(&a->lock);
    bool on = a->haptics_open;
    pthread_mutex_unlock(&a->lock);
    return on;
}

bool ds_audio_card_present(DsAudio *a)
{
    if (!a) return false;
    pthread_mutex_lock(&a->lock);
    bool on = a->card >= 0;
    pthread_mutex_unlock(&a->lock);
    return on;
}

void ds_audio_push_haptics(DsAudio *a, const uint8_t *buf, size_t size)
{
    if (!a || size < 4) return;
    size_t n = size / 4;
    pthread_mutex_lock(&a->lock);
    if (!a->haptics_open) { pthread_mutex_unlock(&a->lock); return; }
    if (a->ring_count > MAX_QUEUED) {               /* fell behind (network stall) */
        int drop = a->ring_count - KEEP_AFTER_DROP;
        a->ring_head = (a->ring_head + drop) % RING_FRAMES;
        a->ring_count -= drop;
        a->dropped += (unsigned long)drop;
    }
    for (size_t i = 0; i < n; i++) {
        int16_t l, r;
        memcpy(&l, buf + i * 4, 2);
        memcpy(&r, buf + i * 4 + 2, 2);
        /* 3 kHz → 48 kHz: linear interpolation from the previous sample. */
        for (int k = 1; k <= UPSAMPLE; k++) {
            if (a->ring_count == RING_FRAMES) break;
            int16_t *d = a->ring[(a->ring_head + a->ring_count) % RING_FRAMES];
            d[0] = (int16_t)(a->prev_l + ((int)l - a->prev_l) * k / UPSAMPLE);
            d[1] = (int16_t)(a->prev_r + ((int)r - a->prev_r) * k / UPSAMPLE);
            a->ring_count++;
        }
        a->prev_l = l;
        a->prev_r = r;
    }
    pthread_mutex_unlock(&a->lock);
}

void ds_audio_mic_enable(DsAudio *a, bool on)
{
    if (!a || !a->want_mic) return;
    pthread_mutex_lock(&a->lock);
    a->mic_on = on;
    pthread_cond_broadcast(&a->cond);
    pthread_mutex_unlock(&a->lock);
}
