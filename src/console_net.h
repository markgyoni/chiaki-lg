#pragma once
/* Console discovery (who is on the network, awake or in rest mode) and
 * registration (pairing with the PIN shown on the console), on top of
 * libchiaki.  Both run on libchiaki threads; the UI polls them. */

#include <chiaki/log.h>
#include <stdbool.h>
#include <stdint.h>

/* ── discovery ─────────────────────────────────────────────────────────────── */

typedef enum { CONSOLE_STATE_UNKNOWN = 0, CONSOLE_STATE_READY, CONSOLE_STATE_STANDBY } ConsoleState;

typedef struct {
    char addr[48];
    char name[64];
    char host_id[32];       // console MAC as reported (hex, no separators)
    bool ps5;
    int  target;            // ChiakiTarget for registration
    ConsoleState state;
} DiscoveredConsole;

typedef struct ConsoleDiscovery ConsoleDiscovery;

ConsoleDiscovery *console_discovery_start(ChiakiLog *log);
void              console_discovery_stop(ConsoleDiscovery *d);
/* Copy up to max consoles seen recently; returns the count. */
int               console_discovery_snapshot(ConsoleDiscovery *d, DiscoveredConsole *out, int max);

/* ── registration ──────────────────────────────────────────────────────────── */

typedef enum { REGIST_RUNNING = 0, REGIST_OK, REGIST_FAILED } RegistState;

typedef struct {
    char    nickname[0x20];
    uint8_t mac[6];
    char    regist_key_b64[64];
    char    rp_key_b64[64];
    int     rp_key_type;
    bool    ps5;
} RegistResult;

typedef struct ConsoleRegist ConsoleRegist;

/* host: console IPv4.  target: from discovery, or 0 to pick PS5/PS4 latest from ps5.
 * account_id_b64: the PSN account ID.  pin: the 8 digits shown on the console. */
ConsoleRegist *console_regist_start(ChiakiLog *log, const char *host, bool ps5, int target,
                                    const char *account_id_b64, uint32_t pin);
RegistState    console_regist_poll(ConsoleRegist *r, RegistResult *out);
void           console_regist_free(ConsoleRegist *r);
