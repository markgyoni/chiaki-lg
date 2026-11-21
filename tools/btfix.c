/* btfix [--check | --undo | --scan <library>]
 *
 * In-memory patch for LG's MTK Bluedroid (libbluetooth.default.so, loaded in
 * webos-bluetooth-service) so HID OUTPUT reports to devices whose Bluetooth
 * name contains "Wireless Controller" are sent as DATA on the interrupt
 * channel instead of SET_REPORT on the control channel.  LG's stack already
 * does this for names starting with "Xbox Wireless Controller"; a DualSense
 * ignores SET_REPORT output, so rumble/triggers/lightbar need this.
 *
 * The check lives in btif_hh_setreport():
 *
 *     if (strncmp(name, XBOX_NAME, strlen(XBOX_NAME)) != 0) -> SET_REPORT
 *
 * and becomes  strstr(name, XBOX_NAME + 5) == NULL  ("Wireless Controller").
 *
 * Finding it: the library file is searched for that check as LG compiles it
 * (unoptimised Thumb code); addresses and stack offsets may differ between
 * firmware builds, the instructions may not.  It is used only if
 *  - exactly one place in the library matches,
 *  - the pointer it compares against really is "Xbox Wireless Controller",
 *  - its calls really go to strlen and strncmp, and strstr can be called,
 *  - the branch after it goes where the branch right before it goes (the
 *    SET_REPORT path),
 *  - the running service has exactly these bytes there.
 * Anything else: refuse, change nothing.
 *
 * Safety:
 *  - Nothing on disk is changed.  The patch lives in the service's private
 *    copy of the code page and disappears when the service restarts or the TV
 *    reboots.
 *  - Write order keeps every intermediate state safe: first the branch becomes
 *    an unconditional "use SET_REPORT" (the stock path for every non-Xbox
 *    device), then the compare is rewritten, then the branch becomes the new
 *    condition.  Every store is a single aligned halfword or word, so no torn
 *    instruction can be fetched.
 *  - Every write is read back; on any mismatch the original bytes are
 *    restored.
 *
 * --scan only reads a library file and reports whether it could be patched
 * (no root, no running service needed; works on any machine).
 *
 * Exit codes: 0 ok (patched / already patched / --check says patched /
 *             --scan: patchable), 1 --check: not patched, 2 environment
 *             error, 3 unknown build. */
#define _FILE_OFFSET_BITS 64
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char XBOX_NAME[] = "Xbox Wireless Controller";

/* ── the library file ─────────────────────────────────────────────────────── */

static uint8_t *lib;            /* whole file */
static size_t   lib_len;

typedef struct { uint32_t vaddr, off, filesz; } Seg;
static Seg     segs[8];
static int     n_segs;

typedef struct { uint32_t addr, off, size, link, entsize; } Sect;
static Sect    s_plt, s_relplt, s_reldyn, s_dynsym, s_dynstr;

static uint32_t u32(size_t off) { return off + 4 <= lib_len ? lib[off] | lib[off+1] << 8 | lib[off+2] << 16 | (uint32_t)lib[off+3] << 24 : 0; }
static uint16_t u16(size_t off) { return off + 2 <= lib_len ? (uint16_t)(lib[off] | lib[off+1] << 8) : 0; }

/* file offset of a virtual address, or (size_t)-1 */
static size_t v2o(uint32_t va)
{
    for (int i = 0; i < n_segs; i++)
        if (va >= segs[i].vaddr && va - segs[i].vaddr < segs[i].filesz)
            return segs[i].off + (va - segs[i].vaddr);
    return (size_t)-1;
}

static int load_lib(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 64L * 1024 * 1024 || !(lib = malloc((size_t)n)) || fread(lib, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return 0;
    }
    fclose(f);
    lib_len = (size_t)n;
    /* 32-bit little-endian ARM ELF */
    if (lib_len < 52 || memcmp(lib, "\177ELF\1\1", 6) || u16(18) != 40) return 0;
    uint32_t phoff = u32(28), shoff = u32(32);
    uint16_t phnum = u16(44), shnum = u16(48), shstrndx = u16(50);
    for (int i = 0; i < phnum && n_segs < 8; i++) {
        size_t p = phoff + (size_t)i * 32;
        if (u32(p) == 1)                                  /* PT_LOAD */
            segs[n_segs++] = (Seg){ u32(p + 8), u32(p + 4), u32(p + 16) };
    }
    if (!shnum || shstrndx >= shnum) return 0;
    size_t strtab = u32(shoff + (size_t)shstrndx * 40 + 16);
    for (int i = 0; i < shnum; i++) {
        size_t s = shoff + (size_t)i * 40;
        size_t no = strtab + u32(s);
        if (no >= lib_len) continue;
        const char *name = (const char *)lib + no;
        Sect sec = { u32(s + 12), u32(s + 16), u32(s + 20), u32(s + 24), u32(s + 36) };
        if (!strcmp(name, ".plt")) s_plt = sec;
        else if (!strcmp(name, ".rel.plt")) s_relplt = sec;
        else if (!strcmp(name, ".rel.dyn")) s_reldyn = sec;
        else if (!strcmp(name, ".dynsym")) s_dynsym = sec;
        else if (!strcmp(name, ".dynstr")) s_dynstr = sec;
    }
    return n_segs && s_plt.size && s_relplt.size && s_dynsym.size && s_dynstr.size;
}

static const char *sym_name(uint32_t idx)
{
    size_t s = s_dynsym.off + (size_t)idx * 16;
    if (idx * 16 >= s_dynsym.size) return "";
    size_t n = s_dynstr.off + u32(s);
    return n < lib_len ? (const char *)lib + n : "";
}

/* GOT slot an ARM PLT stub at va jumps through, or 0 */
static uint32_t plt_got(uint32_t va)
{
    size_t o = v2o(va);
    if (o == (size_t)-1 || va < s_plt.addr || va + 12 > s_plt.addr + s_plt.size) return 0;
    uint32_t w0 = u32(o), w1 = u32(o + 4), w2 = u32(o + 8);
    if ((w0 & 0xfffff000u) != 0xe28fc000u || (w1 & 0xfffff000u) != 0xe28cc000u ||
        (w2 & 0xfffff000u) != 0xe5bcf000u) return 0;     /* add ip,pc / add ip,ip / ldr pc,[ip]! */
    uint32_t r0 = ((w0 >> 8) & 0xf) * 2, r1 = ((w1 >> 8) & 0xf) * 2;
    uint32_t i0 = w0 & 0xff, i1 = w1 & 0xff;
    i0 = r0 ? (i0 >> r0) | (i0 << (32 - r0)) : i0;
    i1 = r1 ? (i1 >> r1) | (i1 << (32 - r1)) : i1;
    return va + 8 + i0 + i1 + (w2 & 0xfff);
}

/* name of the function an ARM PLT stub at va calls */
static const char *plt_name(uint32_t va)
{
    uint32_t got = plt_got(va);
    if (!got) return "";
    for (uint32_t i = 0; i + 8 <= s_relplt.size; i += 8)
        if (u32(s_relplt.off + i) == got && (u32(s_relplt.off + i + 4) & 0xff) == 22)   /* JUMP_SLOT */
            return sym_name(u32(s_relplt.off + i + 4) >> 8);
    return "";
}

/* address of the PLT stub for a function name, or 0 */
static uint32_t plt_find(const char *name)
{
    for (uint32_t va = s_plt.addr; va + 12 <= s_plt.addr + s_plt.size; va += 4)
        if (plt_got(va) && !strcmp(plt_name(va), name)) return va;
    return 0;
}

/* value stored at pointer-sized data va once relocated (RELATIVE or GLOB_DAT) */
static int load_ptr(uint32_t va, uint32_t *out)
{
    size_t o = v2o(va);
    if (o == (size_t)-1) return 0;
    for (uint32_t i = 0; s_reldyn.size && i + 8 <= s_reldyn.size; i += 8) {
        if (u32(s_reldyn.off + i) != va) continue;
        uint32_t info = u32(s_reldyn.off + i + 4), type = info & 0xff;
        if (type == 23) { *out = u32(o); return 1; }                       /* RELATIVE: addend in place */
        if (type == 21 || type == 2) {                                     /* GLOB_DAT / ABS32 */
            size_t s = s_dynsym.off + (size_t)(info >> 8) * 16;
            if (!u32(s + 4)) return 0;                                     /* undefined symbol */
            *out = u32(s + 4) + (type == 2 ? u32(o) : 0);
            return 1;
        }
        return 0;
    }
    return 0;
}

static int is_xbox_name(uint32_t va)
{
    size_t o = v2o(va);
    return o != (size_t)-1 && o + sizeof XBOX_NAME <= lib_len && !memcmp(lib + o, XBOX_NAME, sizeof XBOX_NAME);
}

/* ── Thumb encodings ──────────────────────────────────────────────────────── */

/* BLX <imm> (Thumb → ARM): target, or 0 if not a BLX */
static uint32_t blx_target(uint32_t va, uint16_t h1, uint16_t h2)
{
    if ((h1 & 0xf800) != 0xf000 || (h2 & 0xd001) != 0xc000) return 0;
    uint32_t s = (h1 >> 10) & 1, j1 = (h2 >> 13) & 1, j2 = (h2 >> 11) & 1;
    uint32_t i1 = !(j1 ^ s), i2 = !(j2 ^ s);
    int32_t imm = (int32_t)((s << 24) | (i1 << 23) | (i2 << 22) | ((h1 & 0x3ffu) << 12) | ((h2 & 0x7feu) << 1));
    imm = (imm << 7) >> 7;                                                 /* sign-extend 25 bits */
    return ((va + 4) & ~3u) + (uint32_t)imm;
}

static int blx_encode(uint32_t va, uint32_t target, uint16_t *h1, uint16_t *h2)
{
    int32_t imm = (int32_t)(target - ((va + 4) & ~3u));
    if (imm & 3 || imm < -(1 << 24) || imm >= (1 << 24)) return 0;
    uint32_t u = (uint32_t)imm, s = (u >> 24) & 1, i1 = (u >> 23) & 1, i2 = (u >> 22) & 1;
    *h1 = (uint16_t)(0xf000 | (s << 10) | ((u >> 12) & 0x3ff));
    *h2 = (uint16_t)(0xc000 | (!(i1 ^ s) << 13) | (!(i2 ^ s) << 11) | ((u >> 1) & 0x7fe));
    return blx_target(va, *h1, *h2) == target;
}

/* target of a conditional branch B<c> <imm8> */
static uint32_t bcond_target(uint32_t va, uint16_t h) { return va + 4 + (uint32_t)((int32_t)(int8_t)(h & 0xff) * 2); }

/* ── finding the check ────────────────────────────────────────────────────── */

/* The compiled check, as halfwords; 0 = checked separately below.
 *   ldr r3,[pc,#L1]; add r3,pc; ldr r5,[r3]      r5 = XBOX_NAME
 *   ldr r3,[pc,#L2]; add r3,pc; ldr r3,[r3]
 *   mov r0,r3; blx strlen; mov r3,r0; mov r2,r3
 *   mov r1,r5; ldr.w r0,[r7,#name]; blx strncmp
 *   mov r3,r0; cmp r3,#0; bne SET_REPORT
 * preceded by "cmp r3,#0; beq SET_REPORT". */
static const uint16_t PAT[] = {
    0, 0x447b, 0x681d, 0, 0x447b, 0x681b, 0x4618, 0, 0, 0x4603, 0x461a,
    0x4629, 0xf8d7, 0, 0, 0, 0x4603, 0x2b00, 0,
};
#define PAT_N   (sizeof PAT / sizeof PAT[0])
enum { P_L1 = 0, P_L2 = 3, P_STRLEN = 7, P_MOV = 11, P_LDRW2 = 13, P_STRNCMP = 14, P_BNE = 18 };

typedef struct {
    uint32_t mov, blx, br;            /* addresses of the three patch sites */
    uint8_t  mov_o[2], mov_f[2];
    uint8_t  blx_o[4], blx_f[4];
    uint8_t  br_o[2], br_t[2], br_f[2];
    uint32_t win, win_len;            /* bytes that must match in memory */
} Patch;
static Patch pt;
static char why[160] = "no matching code";

static uint32_t lit_ptr(uint32_t va, uint16_t ldr)
{
    /* ldr rX,[pc,#imm]; add rX,pc  ->  rX = literal + (va + 2) + 4 */
    size_t lo = v2o(((va + 4) & ~3u) + (ldr & 0xffu) * 4);
    return lo == (size_t)-1 ? 0 : u32(lo) + va + 6;
}

static int check_site(uint32_t va)
{
    size_t o = v2o(va);
    uint16_t h[PAT_N];
    for (size_t i = 0; i < PAT_N; i++) {
        h[i] = u16(o + i * 2);
        if (PAT[i] && h[i] != PAT[i]) return 0;
    }
    if ((h[P_L1] & 0xff00) != 0x4b00 || (h[P_L2] & 0xff00) != 0x4b00) return 0;    /* ldr r3,[pc,#] */
    if ((h[P_LDRW2] & 0xf000) != 0x0000) return 0;                                 /* ldr.w r0,[r7,#] */
    if ((h[P_BNE] & 0xff00) != 0xd100) return 0;                                   /* bne */
    uint16_t pcmp = u16(o - 4), pbeq = u16(o - 2);
    if (pcmp != 0x2b00 || (pbeq & 0xff00) != 0xd000) return 0;                     /* cmp r3,#0; beq */

    /* from here on it looks like the check: explain any refusal */
    uint32_t br = va + P_BNE * 2, target = bcond_target(br, h[P_BNE]);
    if (bcond_target(va - 2, pbeq) != target) { snprintf(why, sizeof why, "branches at 0x%x disagree", va); return -1; }
    uint32_t p1, p2;
    if (!load_ptr(lit_ptr(va, h[P_L1]), &p1) || !load_ptr(lit_ptr(va + 6, h[P_L2]), &p2) ||
        !is_xbox_name(p1) || p1 != p2) {
        snprintf(why, sizeof why, "check at 0x%x does not compare with \"%s\"", va, XBOX_NAME);
        return -1;
    }
    uint32_t bl1 = va + P_STRLEN * 2, bl2 = va + P_STRNCMP * 2;
    if (strcmp(plt_name(blx_target(bl1, h[P_STRLEN], h[P_STRLEN + 1])), "strlen") ||
        strcmp(plt_name(blx_target(bl2, h[P_STRNCMP], h[P_STRNCMP + 1])), "strncmp")) {
        snprintf(why, sizeof why, "check at 0x%x does not call strlen/strncmp", va);
        return -1;
    }
    uint32_t strstr_va = plt_find("strstr");
    uint16_t n1, n2;
    if (!strstr_va || !blx_encode(bl2, strstr_va, &n1, &n2)) {
        snprintf(why, sizeof why, "strstr is not callable from 0x%x", bl2);
        return -1;
    }
    if (n1 != h[P_STRNCMP] && (bl2 & 3)) {
        snprintf(why, sizeof why, "call at 0x%x cannot be rewritten in one store", bl2);
        return -1;
    }
    int32_t boff = (int32_t)(target - (br + 4));
    if (boff < -2048 || boff > 2046) return -1;

    pt.mov = va + P_MOV * 2;
    pt.mov_o[0] = 0x29; pt.mov_o[1] = 0x46;                       /* mov r1, r5 */
    pt.mov_f[0] = 0x69; pt.mov_f[1] = 0x1d;                       /* adds r1, r5, #5 */
    pt.blx = bl2;
    memcpy(pt.blx_o, lib + v2o(bl2), 4);
    pt.blx_f[0] = (uint8_t)n1; pt.blx_f[1] = (uint8_t)(n1 >> 8);
    pt.blx_f[2] = (uint8_t)n2; pt.blx_f[3] = (uint8_t)(n2 >> 8);
    pt.br = br;
    memcpy(pt.br_o, lib + v2o(br), 2);
    uint16_t b = (uint16_t)(0xe000 | (((uint32_t)boff >> 1) & 0x7ff));   /* b SET_REPORT */
    pt.br_t[0] = (uint8_t)b; pt.br_t[1] = (uint8_t)(b >> 8);
    pt.br_f[0] = pt.br_o[0]; pt.br_f[1] = 0xd0;                   /* beq SET_REPORT */
    pt.win = va - 4;
    pt.win_len = 4 + PAT_N * 2;
    return 1;
}

static int find_check(void)
{
    int found = 0;
    for (int s = 0; s < n_segs; s++) {
        if (!segs[s].filesz) continue;
        for (uint32_t va = segs[s].vaddr + 4; va + PAT_N * 2 <= segs[s].vaddr + segs[s].filesz; va += 2) {
            /* a look-alike that fails the checks is skipped (why says what) */
            if (check_site(va) > 0 && ++found > 1) { snprintf(why, sizeof why, "the check matches more than once"); return 0; }
        }
    }
    return found == 1;
}

/* ── the running service ──────────────────────────────────────────────────── */

static int memfd;
static unsigned long base;

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

/* load address and file of libbluetooth.default.so in pid */
static unsigned long find_base(int pid, char *path, size_t n)
{
    char p[64], line[512]; unsigned long b = 0;
    snprintf(p, sizeof p, "/proc/%d/maps", pid);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi, off; char perm[8]; int pos = 0;
        char *lib_at = strstr(line, "/libbluetooth.default.so");
        if (!lib_at) continue;
        if (sscanf(line, "%lx-%lx %7s %lx %*s %*s %n", &lo, &hi, perm, &off, &pos) >= 4 && off == 0 && perm[2] == 'x') {
            b = lo;
            snprintf(path, n, "%s", line + pos);
            path[strcspn(path, "\n")] = '\0';
            break;
        }
    }
    fclose(f);
    return b;
}

static int rd(uint32_t va, uint8_t *buf, size_t n) { return pread(memfd, buf, n, (off_t)(base + va)) == (ssize_t)n; }
static int wr(uint32_t va, const uint8_t *v, size_t n)
{
    uint8_t back[4];
    return pwrite(memfd, v, n, (off_t)(base + va)) == (ssize_t)n && rd(va, back, n) && !memcmp(back, v, n);
}

/* 0 = original, 1 = patched, 2 = mid-way, -1 = not the code we expect */
static int state(void)
{
    uint8_t m[64];
    if (pt.win_len > sizeof m || !rd(pt.win, m, pt.win_len)) return -1;
    uint8_t *mov = m + (pt.mov - pt.win), *blx = m + (pt.blx - pt.win), *br = m + (pt.br - pt.win);
    int mo = !memcmp(mov, pt.mov_o, 2), mf = !memcmp(mov, pt.mov_f, 2);
    int lo = !memcmp(blx, pt.blx_o, 4), lf = !memcmp(blx, pt.blx_f, 4);
    int bo = !memcmp(br, pt.br_o, 2), bf = !memcmp(br, pt.br_f, 2), bt = !memcmp(br, pt.br_t, 2);
    if (!(mo || mf) || !(lo || lf) || !(bo || bf || bt)) return -1;
    memcpy(mov, pt.mov_o, 2); memcpy(blx, pt.blx_o, 4); memcpy(br, pt.br_o, 2);
    if (memcmp(m, lib + v2o(pt.win), pt.win_len)) return -1;          /* not the same build */
    if (mo && lo && bo) return 0;
    if (mf && lf && bf) return 1;
    return 2;
}

/* the call: 4 bytes in one aligned store, or just the second halfword */
static int wr_blx(const uint8_t *v)
{
    if (!memcmp(v, pt.blx_o, 2) && !memcmp(v, pt.blx_f, 2)) return wr(pt.blx + 2, v + 2, 2);
    return wr(pt.blx, v, 4);
}

/* Apply (want=1) or remove (want=0) with safe ordering. */
static int apply(int want)
{
    if (!wr(pt.br, pt.br_t, 2)) return 0;                                  /* everyone -> SET_REPORT */
    if (!wr(pt.mov, want ? pt.mov_f : pt.mov_o, 2)) return 0;
    if (!wr_blx(want ? pt.blx_f : pt.blx_o)) return 0;
    return wr(pt.br, want ? pt.br_f : pt.br_o, 2);
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "--scan")) {
        if (!load_lib(argv[2])) { printf("btfix: %s: not a readable ARM library\n", argv[2]); return 2; }
        if (!find_check()) { printf("btfix: unknown Bluetooth library build (%s), nothing changed\n", why); return 3; }
        printf("btfix: %s: patchable (check at 0x%x)\n", argv[2], pt.mov);
        return 0;
    }
    int check = argc > 1 && !strcmp(argv[1], "--check");
    int undo  = argc > 1 && !strcmp(argv[1], "--undo");
    int pid = find_pid();
    if (pid < 0) { printf("btfix: webos-bluetooth-service not running\n"); return 2; }
    char path[256] = "";
    if (!(base = find_base(pid, path, sizeof path))) { printf("btfix: libbluetooth.default.so not mapped in pid %d\n", pid); return 2; }
    if (!load_lib(path)) { printf("btfix: cannot read %s\n", path); return 2; }
    if (!find_check()) { printf("btfix: unknown Bluetooth library build (%s), nothing changed\n", why); return 3; }
    char p[64]; snprintf(p, sizeof p, "/proc/%d/mem", pid);
    if ((memfd = open(p, check ? O_RDONLY : O_RDWR)) < 0) { perror("btfix: open (root needed)"); return 2; }

    static const char *names[] = { "original", "patched", "partial" };
    int s = state();
    if (s < 0) { printf("btfix: unknown Bluetooth library build (running code differs), nothing changed\n"); return 3; }
    if (check) { printf("btfix: pid %d: %s\n", pid, names[s]); return s == 1 ? 0 : 1; }

    int want = !undo;
    if (s == want) { printf("btfix: pid %d: already %s\n", pid, names[s]); return 0; }
    if (!apply(want)) {
        int ok = apply(0);                                                 /* roll back to stock */
        printf("btfix: write failed, %s\n", ok ? "restored original" : "ROLLBACK FAILED (restart Bluetooth or reboot)");
        return 2;
    }
    s = state();
    printf("btfix: pid %d: %s\n", pid, s >= 0 ? names[s] : "unknown?");
    return s == want ? 0 : 2;
}
