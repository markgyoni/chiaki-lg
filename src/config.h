#pragma once
#include <stdbool.h>

enum { SLEEP_OFF = 0, SLEEP_ON = 1, SLEEP_ASK = 2 };

typedef struct {
    char    *host;
    char    *psn_account_id_b64;
    char    *registered_key_b64;
    char    *rp_key_b64;
    int      rp_key_type;
    int      video_width;
    int      video_height;
    int      video_fps;
    int      video_bitrate;
    bool     ps5;
    bool     hw_decode;
    char    *video_codec;       // "h265" (default), "h265_hdr" (PS5 HDR/HEVC), or "h264"
    int      audio_volume;
    bool     wakeup;
    char    *ps5_mac;
    int      wakeup_delay_ms;
    int      sleep_on_exit;     // rest mode when leaving: SLEEP_OFF / SLEEP_ON / SLEEP_ASK
    int      stats_corner;      // stats overlay corner: 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
    char    *ss4s_module;
    int      log_level;         // chiaki bitmask: DEBUG=1 VERBOSE=2 INFO=4 WARNING=8 ERROR=16
    // PSN cloud wakeup — needed when PS5 ignores local UDP wakeup (port 987).
    // Copy from: %APPDATA%\Roaming\Chiaki\Chiaki.conf  →  psn_refresh_token = v3.xxxxx
    char    *psn_refresh_token; // long-lived OAuth2 refresh token (optional)
    bool     input_debug;       // log every raw gamepad event + sample hidraw reports
    bool     rumble;            // forward PS5 rumble to controllers
    bool     dualsense_hid;     // read DualSense via raw HID (touchpad, motion)
    bool     motion;            // forward DualSense gyro/accel to the PS5
    bool     trigger_effects;   // DualSense adaptive triggers
    bool     dualsense_bt_patch; // run tools/btfix (root, via Homebrew Channel) so DualSense
                                 // outputs work over Bluetooth
    char    *controller_profile; // "auto" or a pad_profile id (sony_generic, xbox_bt, ...)
} AppConfig;

int  config_load(AppConfig *cfg, const char *path);
void config_free(AppConfig *cfg);