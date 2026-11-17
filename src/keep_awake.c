#include "keep_awake.h"
#include "app_log.h"

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define POKE_INTERVAL_S 20

static atomic_bool g_running;
static atomic_bool g_unavailable;     // no Homebrew Channel / script: stop trying
static time_t      g_last_poke;

static bool script_path(char *out, size_t n)
{
    char exe[PATH_MAX];
    ssize_t l = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (l <= 0) return false;
    exe[l] = '\0';
    char *slash = strrchr(exe, '/');
    if (!slash) return false;
    *slash = '\0';
    snprintf(out, n, "%s/keepawake.sh", exe);
    return access(out, R_OK) == 0;
}

static void *poke_thread(void *arg)
{
    (void)arg;
    char path[PATH_MAX], cmd[PATH_MAX + 256], out[512] = "";
    if (!script_path(path, sizeof(path))) {
        app_log_always("[AWAKE] keepawake.sh missing next to the app - TV may dim while using a gamepad\n");
        g_unavailable = true;
    } else {
        const char *ls = access("/usr/bin/luna-send-pub", X_OK) == 0 ? "/usr/bin/luna-send-pub" : "/usr/bin/luna-send";
        snprintf(cmd, sizeof(cmd),
                 "%s -n 1 -w 8000 luna://org.webosbrew.hbchannel.service/exec "
                 "'{\"command\":\"sh %s\"}' </dev/null 2>/dev/null", ls, path);
        FILE *f = popen(cmd, "r");
        size_t len = 0;
        if (f) {
            size_t r;
            while (len < sizeof(out) - 1 && (r = fread(out + len, 1, sizeof(out) - 1 - len, f)) > 0) len += r;
            out[len] = '\0';
            pclose(f);
        }
        if (!strstr(out, "ok")) {
            app_log_always("[AWAKE] Homebrew Channel exec unavailable (%.200s) - TV may dim while using a gamepad\n",
                           out[0] ? out : "no reply");
            g_unavailable = true;
        }
    }
    g_running = false;
    return NULL;
}

void keep_awake_poke(void)
{
    if (g_unavailable || g_running) return;
    time_t now = time(NULL);
    if (g_last_poke && now - g_last_poke < POKE_INTERVAL_S) return;
    g_last_poke = now;
    g_running = true;
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, poke_thread, NULL) != 0) g_running = false;
    pthread_attr_destroy(&a);
}
