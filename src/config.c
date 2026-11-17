#include "config.h"
#include "app_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <json-c/json.h>
#include <sys/stat.h>

/* ── JSON helpers ──────────────────────────────────────────────────────────── */

static char *json_get_str(struct json_object *obj, const char *key)
{
    struct json_object *val;
    if (!json_object_object_get_ex(obj, key, &val)) return NULL;
    const char *s = json_object_get_string(val);
    return s ? strdup(s) : NULL;
}

static int json_get_int(struct json_object *obj, const char *key, int def)
{
    struct json_object *val;
    if (!json_object_object_get_ex(obj, key, &val)) return def;
    return json_object_get_int(val);
}

static bool json_get_bool(struct json_object *obj, const char *key, bool def)
{
    struct json_object *val;
    if (!json_object_object_get_ex(obj, key, &val)) return def;
    return json_object_get_boolean(val);
}

/* NULL for missing or empty strings */
static char *json_get_nonempty(struct json_object *obj, const char *key)
{
    char *s = json_get_str(obj, key);
    if (s && !s[0]) { free(s); s = NULL; }
    return s;
}

void config_set_str(char **dst, const char *src)
{
    char *n = src ? strdup(src) : NULL;
    free(*dst);
    *dst = n;
}

static const char *SLEEP_STRS[] = { "off", "on", "ask" };
static const char *CORNER_STRS[] = { "top-left", "top-right", "bottom-left", "bottom-right" };

static int parse_sleep(struct json_object *obj, const char *key, int def)
{
    char *s = json_get_str(obj, key);   // "off" | "on" | "ask" (older configs: true/false)
    int v = !s ? def : !strcmp(s, "ask") ? SLEEP_ASK
                     : (!strcmp(s, "on") || !strcmp(s, "true")) ? SLEEP_ON : SLEEP_OFF;
    free(s);
    return v;
}

/* ── consoles ──────────────────────────────────────────────────────────────── */

static void console_defaults(ConsoleConfig *c)
{
    memset(c, 0, sizeof(*c));
    c->ps5 = true;
    c->video_width = 1920;
    c->video_height = 1080;
    c->video_fps = 60;
    c->video_bitrate = 15000;
    c->video_codec = strdup("h265");
    c->wakeup = true;
    c->sleep_on_exit = SLEEP_ASK;
}

static void console_free(ConsoleConfig *c)
{
    free(c->name); free(c->host); free(c->mac);
    free(c->registered_key_b64); free(c->rp_key_b64); free(c->video_codec);
    memset(c, 0, sizeof(*c));
}

/* Reads a console object.  Also used for the pre-0.7 flat config, whose keys
 * sit at the top level (ps5_mac instead of mac, no name). */
static void console_read(ConsoleConfig *c, struct json_object *o)
{
    console_defaults(c);
    c->name = json_get_nonempty(o, "name");
    c->host = json_get_nonempty(o, "host");
    c->mac = json_get_nonempty(o, "mac");
    if (!c->mac) c->mac = json_get_nonempty(o, "ps5_mac");
    c->ps5 = json_get_bool(o, "ps5", true);
    c->registered_key_b64 = json_get_nonempty(o, "registered_key");
    c->rp_key_b64 = json_get_nonempty(o, "rp_key");
    c->rp_key_type = json_get_int(o, "rp_key_type", 0);
    c->video_width = json_get_int(o, "video_width", 1920);
    c->video_height = json_get_int(o, "video_height", 1080);
    c->video_fps = json_get_int(o, "video_fps", 60);
    c->video_bitrate = json_get_int(o, "video_bitrate", 15000);
    char *codec = json_get_nonempty(o, "video_codec");
    if (codec) { free(c->video_codec); c->video_codec = codec; }
    c->wakeup = json_get_bool(o, "wakeup", true);
    c->sleep_on_exit = parse_sleep(o, "sleep_on_exit", SLEEP_ASK);
    // PS5 Remote Play streams at up to 1080p; larger requests are refused
    // ("AvCap failed to initialize video"), so clamp.
    if (c->video_width > 1920 || c->video_height > 1080) {
        app_log_always("[CFG] %dx%d is above what Remote Play streams — using 1920x1080\n",
                       c->video_width, c->video_height);
        c->video_width = 1920;
        c->video_height = 1080;
    }
    if (!c->name) c->name = strdup(c->ps5 ? "PlayStation 5" : "PlayStation 4");
}

static struct json_object *console_to_json(const ConsoleConfig *c)
{
    struct json_object *o = json_object_new_object();
#define S(k, v) json_object_object_add(o, k, json_object_new_string((v) ? (v) : ""))
    S("name", c->name);
    S("host", c->host);
    S("mac", c->mac);
    json_object_object_add(o, "ps5", json_object_new_boolean(c->ps5));
    S("registered_key", c->registered_key_b64);
    S("rp_key", c->rp_key_b64);
    json_object_object_add(o, "rp_key_type", json_object_new_int(c->rp_key_type));
    json_object_object_add(o, "video_width", json_object_new_int(c->video_width));
    json_object_object_add(o, "video_height", json_object_new_int(c->video_height));
    json_object_object_add(o, "video_fps", json_object_new_int(c->video_fps));
    json_object_object_add(o, "video_bitrate", json_object_new_int(c->video_bitrate));
    S("video_codec", c->video_codec);
    json_object_object_add(o, "wakeup", json_object_new_boolean(c->wakeup));
    S("sleep_on_exit", SLEEP_STRS[c->sleep_on_exit % 3]);
#undef S
    return o;
}

int config_add_console(AppConfig *cfg, const char *name, const char *host, bool ps5)
{
    if (cfg->n_consoles >= MAX_CONSOLES) return -1;
    ConsoleConfig *c = &cfg->consoles[cfg->n_consoles];
    console_defaults(c);
    c->ps5 = ps5;
    c->name = strdup(name && name[0] ? name : (ps5 ? "PlayStation 5" : "PlayStation 4"));
    c->host = host ? strdup(host) : NULL;
    return cfg->n_consoles++;
}

void config_remove_console(AppConfig *cfg, int idx)
{
    if (idx < 0 || idx >= cfg->n_consoles) return;
    console_free(&cfg->consoles[idx]);
    memmove(&cfg->consoles[idx], &cfg->consoles[idx + 1],
            sizeof(ConsoleConfig) * (size_t)(cfg->n_consoles - idx - 1));
    cfg->n_consoles--;
    memset(&cfg->consoles[cfg->n_consoles], 0, sizeof(ConsoleConfig));
    int cur = cfg->current;
    if (cur >= cfg->n_consoles) cur = cfg->n_consoles - 1;
    config_select_console(cfg, cur);
}

void config_select_console(AppConfig *cfg, int idx)
{
    if (idx < 0 || idx >= cfg->n_consoles) {
        cfg->current = -1;
        config_set_str(&cfg->host, NULL);
        config_set_str(&cfg->registered_key_b64, NULL);
        config_set_str(&cfg->rp_key_b64, NULL);
        config_set_str(&cfg->ps5_mac, NULL);
        cfg->wakeup = false;
        return;
    }
    const ConsoleConfig *c = &cfg->consoles[idx];
    cfg->current = idx;
    config_set_str(&cfg->host, c->host);
    config_set_str(&cfg->registered_key_b64, c->registered_key_b64);
    config_set_str(&cfg->rp_key_b64, c->rp_key_b64);
    config_set_str(&cfg->ps5_mac, c->mac);
    config_set_str(&cfg->video_codec, c->video_codec);
    cfg->rp_key_type = c->rp_key_type;
    cfg->video_width = c->video_width;
    cfg->video_height = c->video_height;
    cfg->video_fps = c->video_fps;
    cfg->video_bitrate = c->video_bitrate;
    cfg->ps5 = c->ps5;
    cfg->wakeup = c->wakeup && c->host && c->host[0];
    cfg->sleep_on_exit = c->sleep_on_exit;
}

/* ── load / save ───────────────────────────────────────────────────────────── */

int config_load(AppConfig *cfg, const char *path)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->current = -1;

    struct json_object *root = json_object_from_file(path);
    if (!root) {
        fprintf(stderr, "[CONFIG] Config not found at %s — starting empty\n", path);
        root = json_object_new_object();   // written on the first save
    }

    /* consoles (or a pre-0.7 single-console config at the top level) */
    struct json_object *arr;
    if (json_object_object_get_ex(root, "consoles", &arr) && json_object_is_type(arr, json_type_array)) {
        size_t n = json_object_array_length(arr);
        for (size_t i = 0; i < n && cfg->n_consoles < MAX_CONSOLES; i++)
            console_read(&cfg->consoles[cfg->n_consoles++], json_object_array_get_idx(arr, i));
    } else {
        char *legacy_key = json_get_nonempty(root, "registered_key");
        if (legacy_key) {
            console_read(&cfg->consoles[cfg->n_consoles++], root);
            app_log_always("[CFG] migrated the single-console config to the console list\n");
        }
        free(legacy_key);
    }

    cfg->psn_account_id_b64 = json_get_nonempty(root, "psn_account_id");
    cfg->psn_refresh_token  = json_get_nonempty(root, "psn_refresh_token");

    cfg->audio_volume     = json_get_int(root,  "audio_volume",     100);
    cfg->hw_decode        = json_get_bool(root, "hw_decode",        false);
    cfg->wakeup_delay_ms  = json_get_int(root,  "wakeup_delay_ms",  60000);
    {
        char *s = json_get_str(root, "stats_corner");
        cfg->stats_corner = !s ? 0 : !strcmp(s, "top-right") ? 1 : !strcmp(s, "bottom-left") ? 2
                          : !strcmp(s, "bottom-right") ? 3 : 0;
        free(s);
    }
    cfg->input_debug      = json_get_bool(root, "input_debug",      false);
    cfg->rumble           = json_get_bool(root, "rumble",           true);
    cfg->dualsense_hid    = json_get_bool(root, "dualsense_hid",    true);
    cfg->motion           = json_get_bool(root, "motion",           true);
    cfg->trigger_effects  = json_get_bool(root, "trigger_effects",  true);
    cfg->dualsense_bt_patch = json_get_bool(root, "dualsense_bt_patch", true);
    cfg->controller_profile = json_get_nonempty(root, "controller_profile");
    if (!cfg->controller_profile) cfg->controller_profile = strdup("auto");

    // log_level: string or legacy bool
    {
        struct json_object *lv;
        if (json_object_object_get_ex(root, "log_level", &lv)) {
            if (json_object_get_type(lv) == json_type_boolean) {
                cfg->log_level = json_object_get_boolean(lv) ? (8|16) : 0;
            } else {
                const char *s = json_object_get_string(lv);
                if      (!s || !strcmp(s,"off"))     cfg->log_level = 0;
                else if (!strcmp(s,"error"))         cfg->log_level = 16;
                else if (!strcmp(s,"warning"))       cfg->log_level = 16|8;
                else if (!strcmp(s,"info"))          cfg->log_level = 16|8|4;
                else if (!strcmp(s,"verbose"))       cfg->log_level = 16|8|4|2;
                else if (!strcmp(s,"debug"))         cfg->log_level = 16|8|4|2|1;
                else                                 cfg->log_level = 16|8;
            }
        } else if (json_object_object_get_ex(root, "logging", &lv)) {
            cfg->log_level = json_object_get_boolean(lv) ? (8|16) : 0;
        } else {
            cfg->log_level = 16|8;
        }
    }

    // "auto" → runtime webOS version detection in main.c
    cfg->ss4s_module = json_get_nonempty(root, "ss4s_module");
    if (!cfg->ss4s_module) cfg->ss4s_module = strdup("auto");

    int last = json_get_int(root, "last_console", 0);
    json_object_put(root);

    /* defaults for the active-console fields, then the last used console */
    cfg->video_width = 1920; cfg->video_height = 1080; cfg->video_fps = 60;
    cfg->video_bitrate = 15000; cfg->ps5 = true;
    cfg->video_codec = strdup("h265");
    config_select_console(cfg, cfg->n_consoles ? (last >= 0 && last < cfg->n_consoles ? last : 0) : -1);

    fprintf(stderr, "[CONFIG] Loaded: %d console(s), current=%d psn_account=%s psn_wakeup=%s log_level=0x%x\n",
            cfg->n_consoles, cfg->current, cfg->psn_account_id_b64 ? "yes" : "no",
            cfg->psn_refresh_token ? "yes" : "no", cfg->log_level);
    return 0;
}

static const char *log_level_str(int mask)
{
    if (mask == 0) return "off";
    if (mask == 16) return "error";
    if ((mask & 31) == 31) return "debug";
    if ((mask & 30) == 30) return "verbose";
    if ((mask & 28) == 28) return "info";
    return "warning";
}

bool config_save(const AppConfig *cfg, const char *path)
{
    struct json_object *root = json_object_from_file(path);
    if (!root) root = json_object_new_object();

    /* pre-0.7 per-console keys now live in "consoles" */
    static const char *LEGACY[] = { "host", "ps5", "ps5_mac", "registered_key", "rp_key", "rp_key_type",
                                    "video_width", "video_height", "video_fps", "video_bitrate",
                                    "video_codec", "wakeup", "sleep_on_exit" };
    for (size_t i = 0; i < sizeof(LEGACY) / sizeof(LEGACY[0]); i++)
        json_object_object_del(root, LEGACY[i]);

    struct json_object *arr = json_object_new_array();
    for (int i = 0; i < cfg->n_consoles; i++)
        json_object_array_add(arr, console_to_json(&cfg->consoles[i]));
    json_object_object_del(root, "consoles");
    json_object_object_add(root, "consoles", arr);

#define SETV(k, v) do { json_object_object_del(root, k); json_object_object_add(root, k, v); } while (0)
    SETV("last_console", json_object_new_int(cfg->current < 0 ? 0 : cfg->current));
    SETV("psn_account_id", json_object_new_string(cfg->psn_account_id_b64 ? cfg->psn_account_id_b64 : ""));
    SETV("psn_refresh_token", json_object_new_string(cfg->psn_refresh_token ? cfg->psn_refresh_token : ""));
    SETV("stats_corner", json_object_new_string(CORNER_STRS[cfg->stats_corner & 3]));
    SETV("log_level", json_object_new_string(log_level_str(cfg->log_level)));
    SETV("input_debug", json_object_new_boolean(cfg->input_debug));
    SETV("rumble", json_object_new_boolean(cfg->rumble));
    SETV("dualsense_hid", json_object_new_boolean(cfg->dualsense_hid));
    SETV("motion", json_object_new_boolean(cfg->motion));
    SETV("trigger_effects", json_object_new_boolean(cfg->trigger_effects));
    SETV("dualsense_bt_patch", json_object_new_boolean(cfg->dualsense_bt_patch));
    SETV("controller_profile", json_object_new_string(cfg->controller_profile ? cfg->controller_profile : "auto"));
#undef SETV

    char dir[256];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdir(dir, 0755); }
    int r = json_object_to_file_ext(path, root, JSON_C_TO_STRING_PRETTY);
    json_object_put(root);
    if (r != 0) app_log_always("[CFG] could not write %s\n", path);
    return r == 0;
}

void config_free(AppConfig *cfg)
{
    for (int i = 0; i < cfg->n_consoles; i++) console_free(&cfg->consoles[i]);
    free(cfg->host);
    free(cfg->psn_account_id_b64);
    free(cfg->registered_key_b64);
    free(cfg->rp_key_b64);
    free(cfg->ps5_mac);
    free(cfg->ss4s_module);
    free(cfg->video_codec);
    free(cfg->psn_refresh_token);
    free(cfg->controller_profile);
    memset(cfg, 0, sizeof(*cfg));
}
