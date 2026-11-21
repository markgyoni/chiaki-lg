#include "stats.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <chiaki/session.h> // for CHIAKI_CODEC_* constants

#ifdef HAVE_NDL_DIRECTMEDIA
#include <dlfcn.h>
#endif

StreamStatsCounters g_stream_stats;

void stats_reset(StreamStatsCounters *c)
{
    if(!c)
        return;
    atomic_store_explicit(&c->video_bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&c->video_frames, 0, memory_order_relaxed);
    atomic_store_explicit(&c->video_feed_fail, 0, memory_order_relaxed);

    atomic_store_explicit(&c->audio_bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&c->audio_packets, 0, memory_order_relaxed);

    atomic_store_explicit(&c->video_latency_ms, -1, memory_order_relaxed);
    atomic_store_explicit(&c->rtt_ms, -1, memory_order_relaxed);
    atomic_store_explicit(&c->packet_loss_pm, -1, memory_order_relaxed);
    atomic_store_explicit(&c->frames_lost, 0, memory_order_relaxed);
    atomic_store_explicit(&c->started_ms, 0, memory_order_relaxed);

    // Don't force format to 0 if the caller already set it, but it's fine either way.
    // Keep current format as-is.
}

void stats_set_video_format(StreamStatsCounters *c, int w, int h, int fps, int codec)
{
    if(!c)
        return;
    atomic_store_explicit(&c->video_w, w, memory_order_relaxed);
    atomic_store_explicit(&c->video_h, h, memory_order_relaxed);
    atomic_store_explicit(&c->video_fps, fps, memory_order_relaxed);
    atomic_store_explicit(&c->video_codec, codec, memory_order_relaxed);
}

void stats_overlay_init(StatsOverlay *o)
{
    memset(o, 0, sizeof(*o));
    o->enabled = false;
}

void stats_overlay_toggle(StatsOverlay *o)
{
    if(!o)
        return;
    o->enabled = !o->enabled;
    // force immediate refresh
    o->last_sample_ms = 0;
}

static const char *codec_to_str(int codec)
{
    switch(codec)
    {
        case CHIAKI_CODEC_H265: return "H.265";
        case CHIAKI_CODEC_H265_HDR: return "H.265 HDR";
        case CHIAKI_CODEC_H264: return "H.264";
        default: return "Unknown";
    }
}

static int query_video_render_buf_frames(void)
{
#ifdef HAVE_NDL_DIRECTMEDIA
    /* Looked up at run time: webOS 4's NDL lacks this call, and linking it
     * directly would end the app the first time the stats are shown. */
    static int (*get_len)(int *);
    static bool looked_up;
    if(!looked_up) {
        looked_up = true;
        void *h = dlopen("libNDL_directmedia.so.1", RTLD_LAZY | RTLD_NOLOAD);
        if(!h) h = dlopen("libNDL_directmedia.so.1", RTLD_LAZY);
        if(h) get_len = (int (*)(int *))dlsym(h, "NDL_DirectVideoGetRenderBufferLength");
    }
    int len = -1;
    if(!get_len || get_len(&len) != 0 || len < 0)
        return -1;
    return len;
#else
    return -1;
#endif
}

void stats_overlay_update(StatsOverlay *o, const StreamStatsCounters *c, uint32_t now_ms)
{
    if(!o || !c || !o->enabled)
        return;

    const uint32_t sample_period_ms = 500;
    if(o->last_sample_ms != 0 && (uint32_t)(now_ms - o->last_sample_ms) < sample_period_ms)
        return;

    uint64_t vbytes = atomic_load_explicit(&c->video_bytes, memory_order_relaxed);
    uint64_t abytes = atomic_load_explicit(&c->audio_bytes, memory_order_relaxed);
    uint64_t vframes = atomic_load_explicit(&c->video_frames, memory_order_relaxed);
    uint64_t vfail = atomic_load_explicit(&c->video_feed_fail, memory_order_relaxed);

    int vw = atomic_load_explicit(&c->video_w, memory_order_relaxed);
    int vh = atomic_load_explicit(&c->video_h, memory_order_relaxed);
    int vfps_cfg = atomic_load_explicit(&c->video_fps, memory_order_relaxed);
    int vcodec = atomic_load_explicit(&c->video_codec, memory_order_relaxed);
    int vlat_ms_reported = atomic_load_explicit(&c->video_latency_ms, memory_order_relaxed);

    float dt = 0.0f;
    if(o->last_sample_ms != 0)
        dt = (float)((uint32_t)(now_ms - o->last_sample_ms)) / 1000.0f;

    if(dt > 0.0f)
    {
        double dv = (double)(vbytes - o->last_video_bytes);
        double da = (double)(abytes - o->last_audio_bytes);
        double df = (double)(vframes - o->last_video_frames);

        o->mbps_video = (float)((dv * 8.0) / (dt * 1000.0 * 1000.0));
        o->mbps_audio = (float)((da * 8.0) / (dt * 1000.0 * 1000.0));
        o->mbps_total = o->mbps_video + o->mbps_audio;
        o->fps = (float)(df / dt);
    }

    o->feed_fail_total = vfail;

    // Estimated pipeline buffer latency (video render queue). This is only a proxy for
    // "how much video is buffered", not a full end-to-end network latency.
    o->video_buf_frames = query_video_render_buf_frames();
    o->latency_ms = -1;
    if(o->video_buf_frames >= 0)
    {
        float fps_for_lat = (o->fps > 1.0f) ? o->fps : (float)vfps_cfg;
        if(fps_for_lat > 1.0f)
        {
            float buf_frames = (o->video_buf_frames > 0) ? (float)o->video_buf_frames : 0.5f;
            o->latency_ms = (int)((buf_frames * 1000.0f / fps_for_lat) + 0.5f);
        }
    }

    o->last_sample_ms = now_ms;
    o->last_video_bytes = vbytes;
    o->last_audio_bytes = abytes;
    o->last_video_frames = vframes;
    o->last_video_fail = vfail;

    // Build display string (kept compact for TV readability)
    int rtt = atomic_load_explicit(&c->rtt_ms, memory_order_relaxed);
    int loss_pm = atomic_load_explicit(&c->packet_loss_pm, memory_order_relaxed);
    int lost = atomic_load_explicit(&c->frames_lost, memory_order_relaxed);
    unsigned started = atomic_load_explicit(&c->started_ms, memory_order_relaxed);

    char latency[48], net[64], loss[48], up[32];
    if(vlat_ms_reported >= 0)
        snprintf(latency, sizeof(latency), "%d ms", vlat_ms_reported);
    else if(o->latency_ms >= 0)
        snprintf(latency, sizeof(latency), "~%d ms (%d frames)", o->latency_ms, o->video_buf_frames);
    else
        snprintf(latency, sizeof(latency), "n/a");
    if(rtt >= 0) snprintf(net, sizeof(net), "%d ms", rtt);
    else         snprintf(net, sizeof(net), "n/a");
    if(loss_pm >= 0) snprintf(loss, sizeof(loss), "%.1f %%   Frames lost: %d", loss_pm / 10.0, lost);
    else             snprintf(loss, sizeof(loss), "n/a   Frames lost: %d", lost);
    if(started) {
        unsigned s = (unsigned)(now_ms - started) / 1000u;
        snprintf(up, sizeof(up), "%u:%02u:%02u", s / 3600u, (s / 60u) % 60u, s % 60u);
    } else {
        snprintf(up, sizeof(up), "-");
    }

    snprintf(o->text, sizeof(o->text),
        "STREAM STATS\n"
        "Video        %dx%d  %s\n"
        "Frame rate   %.1f fps (target %d)\n"
        "Bitrate      %.1f Mbps  (audio %.2f)\n"
        "Network RTT  %s\n"
        "Packet loss  %s\n"
        "Decoder      %s   errors %llu\n"
        "Session      %s",
        vw, vh, codec_to_str(vcodec),
        o->fps, vfps_cfg,
        o->mbps_total, o->mbps_audio,
        net,
        loss,
        latency, (unsigned long long)o->feed_fail_total,
        up);
}