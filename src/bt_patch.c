#include "bt_patch.h"
#include "app_log.h"

#include <json-c/json.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static atomic_int g_state = BT_PATCH_IDLE;
static atomic_bool g_permanent_failure;      /* set by the worker, read by requesters */
static char g_reason[256] = "not requested";
static pthread_mutex_t g_reason_mutex = PTHREAD_MUTEX_INITIALIZER;

static void set_reason(const char *r)
{
    pthread_mutex_lock(&g_reason_mutex);
    snprintf(g_reason, sizeof(g_reason), "%s", r);
    pthread_mutex_unlock(&g_reason_mutex);
}

const char *bt_patch_reason(void)
{
    /* Only read after the state has settled; a torn read is harmless (log text). */
    return g_reason;
}

BtPatchState bt_patch_state(void) { return (BtPatchState)atomic_load(&g_state); }

/* Directory of our own executable; btfix is installed next to it. */
static bool btfix_path(char *out, size_t n)
{
    char exe[PATH_MAX];
    ssize_t l = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (l <= 0) return false;
    exe[l] = '\0';
    char *slash = strrchr(exe, '/');
    if (!slash) return false;
    *slash = '\0';
    snprintf(out, n, "%s/btfix", exe);
    return access(out, X_OK) == 0;
}

/* Run a command, capture up to n-1 bytes of its stdout+stderr. */
static int run_capture(const char *cmd, char *out, size_t n)
{
    out[0] = '\0';
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    size_t len = 0, r;
    while (len < n - 1 && (r = fread(out + len, 1, n - 1 - len, f)) > 0) len += r;
    out[len] = '\0';
    return pclose(f);
}

static void trim(char *s)
{
    size_t l = strlen(s);
    while (l && (s[l - 1] == '\n' || s[l - 1] == '\r' || s[l - 1] == ' ')) s[--l] = '\0';
}

/* Interpret btfix's one-line output.  Returns the new state. */
static BtPatchState classify(const char *text)
{
    if (strstr(text, ": patched") || strstr(text, "already patched"))
        return BT_PATCH_ACTIVE;
    if (strstr(text, "unknown Bluetooth library build") || strstr(text, "not mapped"))
        g_permanent_failure = true;               /* other firmware: never touch it */
    return BT_PATCH_UNAVAILABLE;
}

static void *worker(void *arg)
{
    (void)arg;
    char path[PATH_MAX], cmd[PATH_MAX + 256], out[4096], msg[256];
    BtPatchState st = BT_PATCH_UNAVAILABLE;

    if (!btfix_path(path, sizeof(path))) {
        set_reason("btfix helper missing next to the app binary");
        g_permanent_failure = true;
    } else if (geteuid() == 0) {
        /* Running as root (developer setup): call it directly. */
        snprintf(cmd, sizeof(cmd), "'%s' 2>&1", path);
        run_capture(cmd, out, sizeof(out));
        trim(out);
        st = classify(out);
        set_reason(out[0] ? out : "btfix gave no output");
    } else {
        /* Normal app: ask the Homebrew Channel (rooted TVs only) to run it. */
        const char *ls = access("/usr/bin/luna-send-pub", X_OK) == 0 ? "/usr/bin/luna-send-pub"
                                                                      : "/usr/bin/luna-send";
        snprintf(cmd, sizeof(cmd),
                 "%s -n 1 -w 8000 luna://org.webosbrew.hbchannel.service/exec "
                 "'{\"command\":\"%s\"}' </dev/null 2>/dev/null", ls, path);
        run_capture(cmd, out, sizeof(out));
        /* The reply can be preceded by loader noise (e.g. an "ld.so: object
         * '$LD_PRELOAD' ... ignored" line from the app's environment): parse
         * from the first '{'. */
        const char *json = strchr(out, '{');
        json_object *root = json ? json_tokener_parse(json) : NULL;
        json_object *v;
        const char *stdout_s = NULL, *err_s = NULL;
        if (root) {
            if (json_object_object_get_ex(root, "stdoutString", &v)) stdout_s = json_object_get_string(v);
            if (json_object_object_get_ex(root, "errorText", &v))    err_s    = json_object_get_string(v);
        }
        if (stdout_s && stdout_s[0]) {
            snprintf(msg, sizeof(msg), "%s", stdout_s);
            trim(msg);
            st = classify(msg);
            set_reason(msg);
        } else {
            /* No Homebrew Channel (unrooted) or the call was refused. */
            snprintf(msg, sizeof(msg), "Homebrew Channel root exec unavailable (%s)",
                     err_s ? err_s : (out[0] ? out : "no reply"));
            trim(msg);
            set_reason(msg);
            g_permanent_failure = true;
        }
        if (root) json_object_put(root);
    }

    app_log_always("[BTFIX] %s: %s\n", st == BT_PATCH_ACTIVE ? "active" : "unavailable",
                   bt_patch_reason());
    atomic_store(&g_state, st);
    return NULL;
}

void bt_patch_request(void)
{
    int cur = atomic_load(&g_state);
    if (cur == BT_PATCH_RUNNING) return;
    if (cur == BT_PATCH_UNAVAILABLE && g_permanent_failure) return;
    /* Re-run even when active: the Bluetooth service may have restarted
     * (which drops the patch and reconnects the pad).  btfix is idempotent. */
    if (!atomic_compare_exchange_strong(&g_state, &cur, BT_PATCH_RUNNING)) return;
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, worker, NULL) != 0) {
        set_reason("could not start worker thread");
        atomic_store(&g_state, BT_PATCH_UNAVAILABLE);
    }
    pthread_attr_destroy(&a);
}
