#pragma once
#include <stdbool.h>

enum { SLEEP_OFF = 0, SLEEP_ON = 1, SLEEP_ASK = 2 };

#define MAX_CONSOLES 8

/* One registered console (PS5 or PS4) and its own stream settings. */
typedef struct {
    char    *name;              // shown on the home screen (console nickname by default)
    char    *host;              // IPv4 address
    char    *mac;               // "aa:bb:cc:dd:ee:ff" for Wake-on-LAN, may be empty
    bool     ps5;
    char    *registered_key_b64;
    char    *rp_key_b64;
    int      rp_key_type;
    int      video_width;
    int      video_height;
    int      video_fps;
    int      video_bitrate;     // kbps
    char    *video_codec;       // "h265", "h265_hdr" or "h264"
    bool     wakeup;            // wake the console when connecting
    int      sleep_on_exit;     // SLEEP_OFF / SLEEP_ON / SLEEP_ASK
} ConsoleConfig;

typedef struct {
    /* consoles */
    ConsoleConfig consoles[MAX_CONSOLES];
    int      n_consoles;
    int      current;           // console selected for the next stream (-1 = none)

    /* PSN account (shared by all consoles) */
    char    *psn_account_id_b64; // 8-byte account ID, base64 (from PSN sign-in)
    char    *psn_refresh_token;  // OAuth refresh token for cloud wakeup (optional)

    /* Active console, copied from consoles[current] by config_select_console().
     * The stream code reads these. */
    char    *host;
    char    *registered_key_b64;
    char    *rp_key_b64;
    int      rp_key_type;
    int      video_width;
    int      video_height;
    int      video_fps;
    int      video_bitrate;
    bool     ps5;
    char    *video_codec;
    bool     wakeup;
    char    *ps5_mac;
    int      sleep_on_exit;

    /* app-wide */
    bool     hw_decode;
    int      audio_volume;
    int      wakeup_delay_ms;
    int      stats_corner;      // stats overlay corner: 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
    char    *ss4s_module;
    int      log_level;         // chiaki bitmask: DEBUG=1 VERBOSE=2 INFO=4 WARNING=8 ERROR=16
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

/* Write everything back (keeps unknown keys already in the file). */
bool config_save(const AppConfig *cfg, const char *path);

/* Make console idx the active one (copies its settings into the flat fields). */
void config_select_console(AppConfig *cfg, int idx);

/* A new console with default stream settings; returns its index or -1 if full. */
int  config_add_console(AppConfig *cfg, const char *name, const char *host, bool ps5);
void config_remove_console(AppConfig *cfg, int idx);

/* Replace *dst with a copy of src (NULL → NULL). */
void config_set_str(char **dst, const char *src);
