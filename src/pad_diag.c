#include "pad_diag.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <linux/hidraw.h>
#include <linux/input.h>

/* Old webOS sysroot kernel headers may lack these. */
#ifndef BTN_DPAD_UP
#define BTN_DPAD_UP    0x220
#define BTN_DPAD_DOWN  0x221
#define BTN_DPAD_LEFT  0x222
#define BTN_DPAD_RIGHT 0x223
#endif
#ifndef HIDIOCGFEATURE
#define HIDIOCGFEATURE(len) _IOC(_IOC_WRITE | _IOC_READ, 'H', 0x07, len)
#endif
#ifndef BUS_BLUETOOTH
#define BUS_BLUETOOTH 0x05
#endif

#define BITS_TEST(arr, bit) (((arr)[(bit) / 8]) & (1u << ((bit) % 8)))

const char *pad_diag_key_name(unsigned code)
{
    switch (code) {
    case 0x110: return "BTN_LEFT";
    case 0x111: return "BTN_RIGHT";
    case 0x112: return "BTN_MIDDLE";
    case 0x120: return "BTN_TRIGGER";
    case 0x121: return "BTN_THUMB";
    case 0x122: return "BTN_THUMB2";
    case 0x123: return "BTN_TOP";
    case 0x124: return "BTN_TOP2";
    case 0x125: return "BTN_PINKIE";
    case 0x126: return "BTN_BASE";
    case 0x127: return "BTN_BASE2";
    case 0x128: return "BTN_BASE3";
    case 0x129: return "BTN_BASE4";
    case 0x12a: return "BTN_BASE5";
    case 0x12b: return "BTN_BASE6";
    case 0x12f: return "BTN_DEAD";
    case 0x130: return "BTN_SOUTH";
    case 0x131: return "BTN_EAST";
    case 0x132: return "BTN_C";
    case 0x133: return "BTN_NORTH";
    case 0x134: return "BTN_WEST";
    case 0x135: return "BTN_Z";
    case 0x136: return "BTN_TL";
    case 0x137: return "BTN_TR";
    case 0x138: return "BTN_TL2";
    case 0x139: return "BTN_TR2";
    case 0x13a: return "BTN_SELECT";
    case 0x13b: return "BTN_START";
    case 0x13c: return "BTN_MODE";
    case 0x13d: return "BTN_THUMBL";
    case 0x13e: return "BTN_THUMBR";
    case 0x14a: return "BTN_TOUCH";
    case 0x145: return "BTN_TOOL_FINGER";
    case 0x14d: return "BTN_TOOL_DOUBLETAP";
    case 0x167: return "KEY_RECORD";
    case BTN_DPAD_UP:    return "BTN_DPAD_UP";
    case BTN_DPAD_DOWN:  return "BTN_DPAD_DOWN";
    case BTN_DPAD_LEFT:  return "BTN_DPAD_LEFT";
    case BTN_DPAD_RIGHT: return "BTN_DPAD_RIGHT";
    default: return NULL;
    }
}

const char *pad_diag_abs_name(unsigned code)
{
    static const char *names[] = {
        [0x00] = "ABS_X",      [0x01] = "ABS_Y",     [0x02] = "ABS_Z",
        [0x03] = "ABS_RX",     [0x04] = "ABS_RY",    [0x05] = "ABS_RZ",
        [0x06] = "ABS_THROTTLE", [0x07] = "ABS_RUDDER", [0x08] = "ABS_WHEEL",
        [0x09] = "ABS_GAS",    [0x0a] = "ABS_BRAKE",
        [0x10] = "ABS_HAT0X",  [0x11] = "ABS_HAT0Y", [0x12] = "ABS_HAT1X",
        [0x13] = "ABS_HAT1Y",  [0x14] = "ABS_HAT2X", [0x15] = "ABS_HAT2Y",
        [0x16] = "ABS_HAT3X",  [0x17] = "ABS_HAT3Y",
        [0x2f] = "ABS_MT_SLOT", [0x35] = "ABS_MT_POSITION_X",
        [0x36] = "ABS_MT_POSITION_Y", [0x39] = "ABS_MT_TRACKING_ID",
    };
    if (code < sizeof(names) / sizeof(names[0]))
        return names[code];
    return NULL;
}

void pad_diag_evdev_driver(const char *event_path, char *out, size_t out_sz)
{
    out[0] = '\0';
    const char *base = strrchr(event_path, '/');
    base = base ? base + 1 : event_path;

    char link[256], target[256];
    snprintf(link, sizeof(link), "/sys/class/input/%s/device/device/driver", base);
    ssize_t n = readlink(link, target, sizeof(target) - 1);
    if (n <= 0) return;
    target[n] = '\0';
    const char *drv = strrchr(target, '/');
    snprintf(out, out_sz, "%s", drv ? drv + 1 : target);
}

void pad_diag_log_process_ids(pad_diag_log_fn out)
{
    gid_t groups[64];
    int ng = getgroups(64, groups);
    char buf[512];
    int off = 0;
    for (int i = 0; i < ng && off < (int)sizeof(buf) - 16; i++)
        off += snprintf(buf + off, sizeof(buf) - off, "%s%u", i ? "," : "", (unsigned)groups[i]);
    if (ng <= 0) snprintf(buf, sizeof(buf), "(none)");
    out("[PAD] process uid=%u euid=%u gid=%u egid=%u groups=%s\n",
        (unsigned)getuid(), (unsigned)geteuid(), (unsigned)getgid(), (unsigned)getegid(), buf);
}

/* Sorted list of /dev entries starting with `prefix`; caller frees. */
static int list_dev_nodes(const char *dir_path, const char *prefix, char ***out)
{
    *out = NULL;
    DIR *dir = opendir(dir_path);
    if (!dir) return 0;
    int n = 0, cap = 0;
    char **list = NULL;
    struct dirent *ent;
    size_t plen = strlen(prefix);
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, prefix, plen) != 0) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            char **nl = realloc(list, cap * sizeof(*list));
            if (!nl) break;
            list = nl;
        }
        size_t len = strlen(dir_path) + strlen(ent->d_name) + 2;
        list[n] = malloc(len);
        if (!list[n]) break;
        snprintf(list[n], len, "%s/%s", dir_path, ent->d_name);
        n++;
    }
    closedir(dir);
    /* natural-ish sort: shorter names first so event2 < event10 */
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            size_t li = strlen(list[i]), lj = strlen(list[j]);
            if (lj < li || (lj == li && strcmp(list[j], list[i]) < 0)) {
                char *t = list[i]; list[i] = list[j]; list[j] = t;
            }
        }
    *out = list;
    return n;
}

static void free_list(char **list, int n)
{
    for (int i = 0; i < n; i++) free(list[i]);
    free(list);
}

static void dump_one_evdev(pad_diag_log_fn out, const char *path)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (errno != ENXIO && errno != ENODEV)  /* stale node, nothing behind it */
            out("[PAD] %s: open failed: %s\n", path, strerror(errno));
        return;
    }

    char name[256] = {0};
    ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
    struct input_id id;
    memset(&id, 0, sizeof(id));
    ioctl(fd, EVIOCGID, &id);
    char drv[64];
    pad_diag_evdev_driver(path, drv, sizeof(drv));

    uint8_t evbits[(EV_MAX + 7) / 8 + 1];
    uint8_t keybits[(KEY_MAX + 7) / 8 + 1];
    uint8_t absbits[(ABS_MAX + 7) / 8 + 1];
    uint8_t ffbits[(FF_MAX + 7) / 8 + 1];
    memset(evbits, 0, sizeof(evbits));
    memset(keybits, 0, sizeof(keybits));
    memset(absbits, 0, sizeof(absbits));
    memset(ffbits, 0, sizeof(ffbits));
    ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits);
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);

    bool has_south = BITS_TEST(keybits, 0x130);
    bool has_xy    = BITS_TEST(absbits, ABS_X) && BITS_TEST(absbits, ABS_Y);
    bool has_ff    = BITS_TEST(evbits, EV_FF);

    out("[PAD] %s \"%s\" bus=0x%02x vid=%04x pid=%04x ver=%04x driver=%s%s\n",
        path, name, id.bustype, id.vendor, id.product, id.version,
        drv[0] ? drv : "(none)",
        (has_south && has_xy) ? "  <-- has BTN_SOUTH + ABS_X/Y" : "");

    /* Only detail devices backed by a real HID/USB driver.  LG's virtual
     * remotes/IME devices (no driver) advertise every key and axis with
     * bogus ranges and would flood the log. */
    if (!drv[0] || !(has_south || has_xy || BITS_TEST(keybits, 0x120))) {
        close(fd);
        return;
    }

    char buf[1024];
    int off = 0;
    for (unsigned c = 0x100; c < 0x2ff && off < (int)sizeof(buf) - 40; c++) {
        if (!BITS_TEST(keybits, c)) continue;
        const char *nm = pad_diag_key_name(c);
        off += snprintf(buf + off, sizeof(buf) - off, " 0x%03x%s%s%s", c,
                        nm ? "(" : "", nm ? nm : "", nm ? ")" : "");
    }
    out("[PAD]   keys:%s\n", off ? buf : " (none in BTN range)");

    for (unsigned a = 0; a < ABS_CNT; a++) {
        if (!BITS_TEST(absbits, a)) continue;
        struct input_absinfo ai;
        if (ioctl(fd, EVIOCGABS(a), &ai) != 0) continue;
        const char *nm = pad_diag_abs_name(a);
        out("[PAD]   abs 0x%02x %-10s min=%d max=%d flat=%d fuzz=%d res=%d value=%d\n",
            a, nm ? nm : "?", ai.minimum, ai.maximum, ai.flat, ai.fuzz,
            ai.resolution, ai.value);
    }

    if (has_ff) {
        ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ffbits)), ffbits);
        int n_eff = 0;
        ioctl(fd, EVIOCGEFFECTS, &n_eff);
        out("[PAD]   ff: effects=%d%s%s%s%s%s\n", n_eff,
            BITS_TEST(ffbits, FF_RUMBLE)   ? " RUMBLE"   : "",
            BITS_TEST(ffbits, FF_PERIODIC) ? " PERIODIC" : "",
            BITS_TEST(ffbits, FF_CONSTANT) ? " CONSTANT" : "",
            BITS_TEST(ffbits, FF_GAIN)     ? " GAIN"     : "",
            BITS_TEST(ffbits, FF_AUTOCENTER) ? " AUTOCENTER" : "");
    } else {
        out("[PAD]   ff: not supported\n");
    }

    int wfd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    out("[PAD]   open(O_RDWR): %s\n", wfd >= 0 ? "OK" : strerror(errno));
    if (wfd >= 0) close(wfd);

    close(fd);
}

void pad_diag_dump_evdev(pad_diag_log_fn out)
{
    char **nodes;
    int n = list_dev_nodes("/dev/input", "event", &nodes);
    out("[PAD] ---- evdev scan: %d nodes ----\n", n);
    for (int i = 0; i < n; i++)
        dump_one_evdev(out, nodes[i]);
    free_list(nodes, n);
}

static const char *bus_name(unsigned bus)
{
    switch (bus) {
    case 0x03: return "USB";
    case BUS_BLUETOOTH: return "BT";
    case 0x06: return "VIRTUAL";
    default: return "?";
    }
}

void pad_diag_dump_hidraw(pad_diag_log_fn out)
{
    char **nodes;
    int n = list_dev_nodes("/dev", "hidraw", &nodes);
    out("[HID] ---- hidraw scan: %d nodes ----\n", n);
    for (int i = 0; i < n; i++) {
        int fd = open(nodes[i], O_RDWR | O_NONBLOCK | O_CLOEXEC);
        int open_err = (fd < 0) ? errno : 0;
        if (fd < 0)
            fd = open(nodes[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            out("[HID] %s: open(O_RDWR) failed: %s; open(O_RDONLY) failed: %s\n",
                nodes[i], strerror(open_err), strerror(errno));
            continue;
        }
        struct hidraw_devinfo info;
        memset(&info, 0, sizeof(info));
        ioctl(fd, HIDIOCGRAWINFO, &info);
        char name[256] = {0};
        ioctl(fd, HIDIOCGRAWNAME(sizeof(name) - 1), name);
        int desc_size = -1;
        ioctl(fd, HIDIOCGRDESCSIZE, &desc_size);
        out("[HID] %s bus=%s(0x%02x) vid=%04x pid=%04x name=\"%s\" rdesc=%d open(O_RDWR)=%s\n",
            nodes[i], bus_name(info.bustype), info.bustype,
            (unsigned)(uint16_t)info.vendor, (unsigned)(uint16_t)info.product,
            name, desc_size, open_err ? strerror(open_err) : "OK");
        close(fd);
    }
    free_list(nodes, n);
}

static void hex_line(char *out, size_t out_sz, const uint8_t *data, int len)
{
    size_t off = 0;
    for (int i = 0; i < len && off + 4 < out_sz; i++)
        off += snprintf(out + off, out_sz - off, "%02x ", data[i]);
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

void pad_diag_hidraw_sample(pad_diag_log_fn out, const char *hidraw_path,
                            int ms, bool request_feature)
{
    int fd = open(hidraw_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        out("[HID] sample %s: open failed: %s\n", hidraw_path, strerror(errno));
        return;
    }

    char hex[3 * 128 + 1];
    if (request_feature) {
        uint8_t feat[64];
        memset(feat, 0, sizeof(feat));
        feat[0] = 0x05;  /* Sony calibration report */
        int r = ioctl(fd, HIDIOCGFEATURE(sizeof(feat)), feat);
        if (r < 0) {
            out("[HID] feature 0x05: failed: %s\n", strerror(errno));
        } else {
            hex_line(hex, sizeof(hex), feat, r);
            out("[HID] feature 0x05: %d bytes: %s\n", r, hex);
        }
    }

    struct { int id, len, count; } kinds[16];
    int nk = 0, total = 0;
    long end = now_ms() + ms;
    for (;;) {
        long left = end - now_ms();
        if (left <= 0) break;
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, (int)left) <= 0) continue;
        uint8_t rep[128];
        ssize_t n = read(fd, rep, sizeof(rep));
        if (n <= 0) continue;
        total++;
        int k;
        for (k = 0; k < nk; k++)
            if (kinds[k].id == rep[0] && kinds[k].len == n) break;
        if (k == nk && nk < 16) {
            kinds[nk].id = rep[0];
            kinds[nk].len = (int)n;
            kinds[nk].count = 0;
            nk++;
            hex_line(hex, sizeof(hex), rep, (int)n);
            out("[HID] first report id=0x%02x len=%zd: %s\n", rep[0], n, hex);
        }
        if (k < 16) kinds[k].count++;
    }
    out("[HID] sample %s: %d reports in %d ms\n", hidraw_path, total, ms);
    for (int k = 0; k < nk; k++)
        out("[HID]   id=0x%02x len=%d count=%d\n", kinds[k].id, kinds[k].len, kinds[k].count);
    close(fd);
}

bool pad_diag_abs_significant(int val, int min, int max, int *last)
{
    int range = max - min;
    int step = range >= 16 ? range / 16 : 1;
    int d = val - *last;
    if (d < 0) d = -d;
    if (d >= step || ((val == min || val == max) && val != *last)) {
        *last = val;
        return true;
    }
    return false;
}
