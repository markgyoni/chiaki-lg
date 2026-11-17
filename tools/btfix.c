/* btfix [--check | --undo]
 *
 * In-memory patch for LG's MTK Bluedroid (libbluetooth.default.so, loaded in
 * webos-bluetooth-service) so HID OUTPUT reports to devices whose Bluetooth
 * name contains "Wireless Controller" are sent as DATA on the interrupt
 * channel instead of SET_REPORT on the control channel.  LG's stack already
 * does this for names starting with "Xbox Wireless Controller"; a DualSense
 * ignores SET_REPORT output, so rumble/triggers/lightbar need this.
 *
 * Safety:
 *  - Nothing on disk is changed.  The patch lives in the service's private
 *    copy of the code page and disappears when the service restarts or the TV
 *    reboots.
 *  - Only one known build is touched: the whole patched function (356 bytes)
 *    must match a CRC32 of the original, with patch sites normalised.  Any
 *    other firmware -> refuse, change nothing.
 *  - Write order keeps every intermediate state safe: first the branch becomes
 *    an unconditional "use SET_REPORT" (the stock path for every non-Xbox
 *    device), then the compare is rewritten, then the branch becomes the new
 *    condition.  Each store is a single halfword (the blx's first halfword is
 *    unchanged), so no torn instruction can be fetched.
 *  - Every write is read back; on any mismatch the original bytes are
 *    restored.
 *
 * Exit codes: 0 ok (patched / already patched / --check says patched),
 *             1 --check: not patched, 2 environment error, 3 unknown build. */
#define _FILE_OFFSET_BITS 64
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FUNC_OFF  0x7fdd8u          /* btif_hh_setreport */
#define FUNC_LEN  356u
#define FUNC_CRC  0xa10ab9f8u       /* original bytes, webOS 6.5.3 / 03.53.45 */

typedef struct { uint32_t off; uint8_t orig[2], tmp[2], fin[2]; } Site;

/* All patch sites are halfwords. */
enum { S_MOV, S_BLX2, S_BR, N_SITES };
static const Site sites[N_SITES] = {
    [S_MOV]  = { 0x7fe96, {0x29,0x46}, {0x29,0x46}, {0x69,0x1d} }, /* mov r1,r5 -> adds r1,r5,#5 ("Wireless Controller") */
    [S_BLX2] = { 0x7fe9e, {0x2e,0xed}, {0x2e,0xed}, {0xd2,0xee} }, /* blx strncmp -> blx strstr (2nd halfword) */
    [S_BR]   = { 0x7fea4, {0x37,0xd1}, {0x37,0xe0}, {0x37,0xd0} }, /* bne 7ff16 -> (b 7ff16) -> beq 7ff16 */
};
static const uint8_t blx1[2] = {0x86,0xf7};          /* blx first halfword, same before/after */

static int memfd;
static unsigned long base;

static uint32_t crc32(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) { crc ^= *p++; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u))); }
    return ~crc;
}

static int find_pid(void)
{
    DIR *d = opendir("/proc"); struct dirent *e; int pid = -1;
    while (d && (e = readdir(d)) && pid < 0) {
        int n = atoi(e->d_name); if (n <= 0) continue;
        char p[64], comm[64] = "";
        snprintf(p, sizeof p, "/proc/%d/comm", n);
        FILE *f = fopen(p, "r"); if (!f) continue;
        if (fgets(comm, sizeof comm, f) && !strncmp(comm, "webos-bluetooth", 15)) pid = n;
        fclose(f);
    }
    if (d) closedir(d);
    return pid;
}

static unsigned long find_base(int pid)
{
    char p[64], line[512]; unsigned long b = 0;
    snprintf(p, sizeof p, "/proc/%d/maps", pid);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi, off; char perm[8];
        if (!strstr(line, "/libbluetooth.default.so")) continue;
        if (sscanf(line, "%lx-%lx %7s %lx", &lo, &hi, perm, &off) == 4 && off == 0 && perm[2] == 'x' &&
            hi - lo >= FUNC_OFF + FUNC_LEN) { b = lo; break; }
    }
    fclose(f);
    return b;
}

static int rd(uint32_t off, uint8_t *buf, size_t n) { return pread(memfd, buf, n, (off_t)(base + off)) == (ssize_t)n; }
static int wr(uint32_t off, const uint8_t *v)
{
    uint8_t back[2];
    return pwrite(memfd, v, 2, (off_t)(base + off)) == 2 && rd(off, back, 2) && !memcmp(back, v, 2);
}

/* 0 = original, 1 = patched, 2 = mid-way (tmp branch), -1 = unknown build */
static int state(void)
{
    uint8_t f[FUNC_LEN];
    if (!rd(FUNC_OFF, f, sizeof f)) return -1;
    if (memcmp(f + (0x7fe9c - FUNC_OFF), blx1, 2)) return -1;
    int orig = 0, fin = 0, tmp = 0;
    for (int i = 0; i < N_SITES; i++) {
        uint8_t *p = f + (sites[i].off - FUNC_OFF);
        if (!memcmp(p, sites[i].orig, 2)) orig++;
        else if (!memcmp(p, sites[i].fin, 2)) fin++;
        else if (!memcmp(p, sites[i].tmp, 2)) tmp++;
        else return -1;
        memcpy(p, sites[i].orig, 2);                 /* normalise for the CRC */
    }
    if (crc32(0, f, sizeof f) != FUNC_CRC) return -1;
    if (orig == N_SITES) return 0;
    if (fin == N_SITES) return 1;
    return 2;
}

/* Apply (want=1) or remove (want=0) with safe ordering. */
static int apply(int want)
{
    const uint8_t *br_tmp = sites[S_BR].tmp;
    if (!wr(sites[S_BR].off, br_tmp)) return 0;                  /* everyone -> SET_REPORT */
    if (!wr(sites[S_MOV].off,  want ? sites[S_MOV].fin  : sites[S_MOV].orig))  return 0;
    if (!wr(sites[S_BLX2].off, want ? sites[S_BLX2].fin : sites[S_BLX2].orig)) return 0;
    return wr(sites[S_BR].off, want ? sites[S_BR].fin : sites[S_BR].orig);
}

int main(int argc, char **argv)
{
    int check = argc > 1 && !strcmp(argv[1], "--check");
    int undo  = argc > 1 && !strcmp(argv[1], "--undo");
    int pid = find_pid();
    if (pid < 0) { printf("btfix: webos-bluetooth-service not running\n"); return 2; }
    if (!(base = find_base(pid))) { printf("btfix: libbluetooth.default.so not mapped in pid %d\n", pid); return 2; }
    char p[64]; snprintf(p, sizeof p, "/proc/%d/mem", pid);
    if ((memfd = open(p, check ? O_RDONLY : O_RDWR)) < 0) { perror("btfix: open (root needed)"); return 2; }

    static const char *names[] = { "original", "patched", "partial" };
    int s = state();
    if (s < 0) { printf("btfix: unknown Bluetooth library build, nothing changed\n"); return 3; }
    if (check) { printf("btfix: pid %d: %s\n", pid, names[s]); return s == 1 ? 0 : 1; }

    int want = !undo;
    if (s == want) { printf("btfix: pid %d: already %s\n", pid, names[s]); return 0; }
    if (!apply(want)) {
        int ok = apply(0);                                         /* roll back to stock */
        printf("btfix: write failed, %s\n", ok ? "restored original" : "ROLLBACK FAILED (restart Bluetooth or reboot)");
        return 2;
    }
    s = state();
    printf("btfix: pid %d: %s\n", pid, s >= 0 ? names[s] : "unknown?");
    return s == want ? 0 : 2;
}
