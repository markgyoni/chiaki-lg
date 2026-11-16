/* paddiag — standalone controller diagnostics for webOS (run over SSH).
 *
 *   paddiag [info]                         evdev + hidraw capability dump
 *   paddiag events [--grab] [/dev/input/eventN ...]
 *                                          live EV_KEY/EV_ABS monitor (default:
 *                                          every gamepad candidate); Ctrl-C quits
 *   paddiag hidraw /dev/hidrawN [--ms N] [--feature]
 *                                          sample raw HID input reports; --feature
 *                                          reads Sony calibration report 0x05 first
 *                                          (switches BT DualSense/DS4 to full
 *                                          reports; evdev goes dead until reconnect)
 */
#include "pad_diag.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>

#include <linux/input.h>

#define BITS_TEST(arr, bit) (((arr)[(bit) / 8]) & (1u << ((bit) % 8)))
#define MAX_DEVS 8

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int s) { (void)s; g_stop = 1; }

static void out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static bool is_candidate(int fd)
{
    uint8_t keybits[(KEY_MAX + 7) / 8 + 1] = {0};
    uint8_t absbits[(ABS_MAX + 7) / 8 + 1] = {0};
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
    return BITS_TEST(keybits, 0x130) && BITS_TEST(absbits, ABS_X) && BITS_TEST(absbits, ABS_Y);
}

typedef struct {
    int  fd;
    char path[64];
    int  min[ABS_CNT], max[ABS_CNT], last[ABS_CNT];
} Dev;

static int open_dev(Dev *d, const char *path, bool grab)
{
    d->fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (d->fd < 0) {
        out("open %s: %s\n", path, strerror(errno));
        return -1;
    }
    snprintf(d->path, sizeof(d->path), "%s", path);
    for (int a = 0; a < ABS_CNT; a++) {
        struct input_absinfo ai;
        if (ioctl(d->fd, EVIOCGABS(a), &ai) == 0) {
            d->min[a] = ai.minimum; d->max[a] = ai.maximum; d->last[a] = ai.value;
        } else {
            d->min[a] = d->max[a] = d->last[a] = 0;
        }
    }
    char name[256] = {0};
    ioctl(d->fd, EVIOCGNAME(sizeof(name) - 1), name);
    if (grab) {
        int r = ioctl(d->fd, EVIOCGRAB, 1);
        out("monitoring %s \"%s\" (grab: %s)\n", path, name, r == 0 ? "OK" : strerror(errno));
    } else {
        out("monitoring %s \"%s\"\n", path, name);
    }
    return 0;
}

static int cmd_events(int argc, char **argv)
{
    bool grab = false;
    Dev devs[MAX_DEVS];
    int nd = 0;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--grab")) grab = true;
    }
    for (int i = 0; i < argc && nd < MAX_DEVS; i++) {
        if (argv[i][0] == '-') continue;
        if (open_dev(&devs[nd], argv[i], grab) == 0) nd++;
    }
    if (nd == 0) {
        DIR *dir = opendir("/dev/input");
        struct dirent *ent;
        while (dir && (ent = readdir(dir)) && nd < MAX_DEVS) {
            if (strncmp(ent->d_name, "event", 5)) continue;
            char path[64];
            snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
            int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            bool cand = is_candidate(fd);
            close(fd);
            if (cand && open_dev(&devs[nd], path, grab) == 0) nd++;
        }
        if (dir) closedir(dir);
    }
    if (nd == 0) {
        out("no gamepad candidates found\n");
        return 1;
    }
    out("press buttons / move sticks; Ctrl-C to stop\n");

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!g_stop) {
        struct pollfd p[MAX_DEVS];
        for (int i = 0; i < nd; i++) { p[i].fd = devs[i].fd; p[i].events = POLLIN; }
        if (poll(p, nd, 200) <= 0) continue;
        for (int i = 0; i < nd; i++) {
            if (!(p[i].revents & POLLIN)) continue;
            struct input_event evs[64];
            ssize_t n = read(devs[i].fd, evs, sizeof(evs));
            if (n <= 0) continue;
            for (int k = 0; k < (int)(n / sizeof(evs[0])); k++) {
                struct input_event *e = &evs[k];
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                double t = (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9;
                const char *dev = strrchr(devs[i].path, '/') + 1;
                if (e->type == EV_KEY) {
                    const char *nm = pad_diag_key_name(e->code);
                    out("%8.3f %s KEY 0x%03x %-12s %s\n", t, dev, e->code,
                        nm ? nm : "?", e->value ? (e->value == 2 ? "repeat" : "down") : "up");
                } else if (e->type == EV_ABS && e->code < ABS_CNT) {
                    if (!pad_diag_abs_significant(e->value, devs[i].min[e->code],
                                                  devs[i].max[e->code], &devs[i].last[e->code]))
                        continue;
                    const char *nm = pad_diag_abs_name(e->code);
                    out("%8.3f %s ABS 0x%02x %-12s %d  [%d..%d]\n", t, dev, e->code,
                        nm ? nm : "?", e->value, devs[i].min[e->code], devs[i].max[e->code]);
                } else if (e->type == EV_MSC && e->code == MSC_SCAN) {
                    /* HID usage of the next key event — handy for unknown pads */
                    out("%8.3f %s MSC_SCAN 0x%08x\n", t, dev, (unsigned)e->value);
                }
            }
        }
    }
    for (int i = 0; i < nd; i++) {
        if (grab) ioctl(devs[i].fd, EVIOCGRAB, 0);
        close(devs[i].fd);
    }
    return 0;
}

static int cmd_hidraw(int argc, char **argv)
{
    const char *path = NULL;
    int ms = 2000;
    bool feature = false;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--feature")) feature = true;
        else if (!strcmp(argv[i], "--ms") && i + 1 < argc) ms = atoi(argv[++i]);
        else path = argv[i];
    }
    if (!path) {
        out("usage: paddiag hidraw /dev/hidrawN [--ms N] [--feature]\n");
        return 2;
    }
    pad_diag_hidraw_sample(out, path, ms, feature);
    return 0;
}

int main(int argc, char **argv)
{
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    const char *cmd = argc > 1 ? argv[1] : "info";
    if (!strcmp(cmd, "info")) {
        pad_diag_log_process_ids(out);
        pad_diag_dump_evdev(out);
        pad_diag_dump_hidraw(out);
        return 0;
    }
    if (!strcmp(cmd, "events")) return cmd_events(argc - 2, argv + 2);
    if (!strcmp(cmd, "hidraw")) return cmd_hidraw(argc - 2, argv + 2);

    out("usage: paddiag [info | events [--grab] [dev...] | hidraw DEV [--ms N] [--feature]]\n");
    return 2;
}
