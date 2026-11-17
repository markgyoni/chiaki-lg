#pragma once
/* DualSense USB audio (ROADMAP Stage 11).
 *
 * Over USB the DualSense is also a USB sound card: a 4-channel 48 kHz output
 * (1-2 speaker/headset, 3-4 the two haptic actuators) and a 2-channel 48 kHz
 * microphone input.  This module finds that card with ALSA and
 *   - plays the PS5's haptics stream (3 kHz stereo PCM from the haptics sink)
 *     on channels 3-4, upsampled to 48 kHz ("HD haptics", instead of turning
 *     it into plain rumble);
 *   - captures the microphone in 10 ms frames (480 stereo samples) for the
 *     Opus encoder.
 *
 * A background thread keeps looking for the card (USB hot-plug) and reopens
 * it after errors.  All functions are thread-safe. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct DsAudio DsAudio;

#define DS_AUDIO_MIC_FRAMES 480   /* 10 ms at 48 kHz */

/* Called on the capture thread with DS_AUDIO_MIC_FRAMES stereo S16 frames. */
typedef void (*DsAudioMicCb)(int16_t *pcm, void *user);

DsAudio *ds_audio_new(bool haptics, bool mic, DsAudioMicCb mic_cb, void *mic_user);
void     ds_audio_free(DsAudio *a);

/* True while a DualSense sound card is open for haptics output. */
bool ds_audio_haptics_active(DsAudio *a);
/* True while a DualSense sound card is present (microphone usable). */
bool ds_audio_card_present(DsAudio *a);

/* Haptics frame from chiaki's haptics sink: S16 stereo at 3000 Hz. */
void ds_audio_push_haptics(DsAudio *a, const uint8_t *buf, size_t size);

/* Start/stop microphone capture (the callback only runs while enabled). */
void ds_audio_mic_enable(DsAudio *a, bool on);
