#include "console_net.h"
#include "app_log.h"

#include <chiaki/base64.h>
#include <chiaki/discoveryservice.h>
#include <chiaki/regist.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── discovery ─────────────────────────────────────────────────────────────── */

#define MAX_FOUND 8

struct ConsoleDiscovery {
    ChiakiDiscoveryService service;
    pthread_mutex_t lock;
    DiscoveredConsole found[MAX_FOUND];
    int n;
};

static void copy_str(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src ? src : "");
}

static void hosts_cb(ChiakiDiscoveryHost *hosts, size_t count, void *user)
{
    ConsoleDiscovery *d = user;
    pthread_mutex_lock(&d->lock);
    d->n = 0;
    for (size_t i = 0; i < count && d->n < MAX_FOUND; i++) {
        ChiakiDiscoveryHost *h = &hosts[i];
        if (!h->host_addr) continue;
        DiscoveredConsole *c = &d->found[d->n++];
        memset(c, 0, sizeof(*c));
        copy_str(c->addr, sizeof(c->addr), h->host_addr);
        copy_str(c->name, sizeof(c->name), h->host_name);
        copy_str(c->host_id, sizeof(c->host_id), h->host_id);
        c->ps5 = chiaki_discovery_host_is_ps5(h);
        c->target = (int)chiaki_discovery_host_system_version_target(h);
        c->state = h->state == CHIAKI_DISCOVERY_HOST_STATE_READY ? CONSOLE_STATE_READY
                 : h->state == CHIAKI_DISCOVERY_HOST_STATE_STANDBY ? CONSOLE_STATE_STANDBY
                 : CONSOLE_STATE_UNKNOWN;
    }
    pthread_mutex_unlock(&d->lock);
}

ConsoleDiscovery *console_discovery_start(ChiakiLog *log)
{
    ConsoleDiscovery *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    pthread_mutex_init(&d->lock, NULL);

    struct sockaddr_in bc = { 0 };
    bc.sin_family = AF_INET;
    bc.sin_addr.s_addr = 0xffffffff;          // 255.255.255.255
    struct sockaddr_storage addr;
    memset(&addr, 0, sizeof(addr));
    memcpy(&addr, &bc, sizeof(bc));

    ChiakiDiscoveryServiceOptions opt = { 0 };
    opt.ping_ms = 500;
    opt.ping_initial_ms = 100;
    opt.hosts_max = MAX_FOUND;
    opt.host_drop_pings = 6;
    opt.send_addr = &addr;
    opt.send_addr_size = sizeof(bc);
    opt.cb = hosts_cb;
    opt.cb_user = d;
    if (chiaki_discovery_service_init(&d->service, &opt, log) != CHIAKI_ERR_SUCCESS) {
        app_log_always("[DISCOVERY] could not start\n");
        pthread_mutex_destroy(&d->lock);
        free(d);
        return NULL;
    }
    return d;
}

void console_discovery_stop(ConsoleDiscovery *d)
{
    if (!d) return;
    chiaki_discovery_service_fini(&d->service);
    pthread_mutex_destroy(&d->lock);
    free(d);
}

int console_discovery_snapshot(ConsoleDiscovery *d, DiscoveredConsole *out, int max)
{
    if (!d) return 0;
    pthread_mutex_lock(&d->lock);
    int n = d->n < max ? d->n : max;
    memcpy(out, d->found, sizeof(DiscoveredConsole) * (size_t)n);
    pthread_mutex_unlock(&d->lock);
    return n;
}

/* ── registration ──────────────────────────────────────────────────────────── */

struct ConsoleRegist {
    ChiakiRegist regist;
    char host[64];
    pthread_mutex_t lock;
    RegistState state;
    RegistResult result;
    bool started;           // chiaki_regist_start succeeded (thread to stop)
};

static void regist_cb(ChiakiRegistEvent *ev, void *user)
{
    ConsoleRegist *r = user;
    pthread_mutex_lock(&r->lock);
    if (ev->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS && ev->registered_host) {
        ChiakiRegisteredHost *h = ev->registered_host;
        RegistResult *o = &r->result;
        memset(o, 0, sizeof(*o));
        snprintf(o->nickname, sizeof(o->nickname), "%.*s", (int)sizeof(h->server_nickname), h->server_nickname);
        memcpy(o->mac, h->server_mac, sizeof(o->mac));
        chiaki_base64_encode((const uint8_t *)h->rp_regist_key, sizeof(h->rp_regist_key),
                             o->regist_key_b64, sizeof(o->regist_key_b64));
        chiaki_base64_encode(h->rp_key, sizeof(h->rp_key), o->rp_key_b64, sizeof(o->rp_key_b64));
        o->rp_key_type = (int)h->rp_key_type;
        o->ps5 = chiaki_target_is_ps5(h->target);
        r->state = REGIST_OK;
        app_log_always("[REGIST] registered with \"%s\"\n", o->nickname);
    } else {
        r->state = REGIST_FAILED;
        app_log_always("[REGIST] %s\n", ev->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_CANCELED ? "canceled" : "failed");
    }
    pthread_mutex_unlock(&r->lock);
}

ConsoleRegist *console_regist_start(ChiakiLog *log, const char *host, bool ps5, int target,
                                    const char *account_id_b64, uint32_t pin)
{
    ConsoleRegist *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    pthread_mutex_init(&r->lock, NULL);
    snprintf(r->host, sizeof(r->host), "%s", host);

    ChiakiRegistInfo info;
    memset(&info, 0, sizeof(info));
    info.host = r->host;
    info.broadcast = false;
    info.target = target > 0 ? (ChiakiTarget)target : (ps5 ? CHIAKI_TARGET_PS5_1 : CHIAKI_TARGET_PS4_10);
    if (info.target == CHIAKI_TARGET_PS5_UNKNOWN) info.target = CHIAKI_TARGET_PS5_1;
    if (info.target == CHIAKI_TARGET_PS4_UNKNOWN) info.target = CHIAKI_TARGET_PS4_10;
    info.pin = pin;
    size_t len = sizeof(info.psn_account_id);
    if (!account_id_b64 ||
        chiaki_base64_decode(account_id_b64, strlen(account_id_b64), info.psn_account_id, &len) != CHIAKI_ERR_SUCCESS ||
        len != CHIAKI_PSN_ACCOUNT_ID_SIZE) {
        app_log_always("[REGIST] invalid PSN account ID\n");
        r->state = REGIST_FAILED;
        return r;
    }
    app_log_always("[REGIST] registering with %s (target %d)\n", r->host, (int)info.target);
    if (chiaki_regist_start(&r->regist, log, &info, regist_cb, r) != CHIAKI_ERR_SUCCESS) {
        app_log_always("[REGIST] could not start\n");
        r->state = REGIST_FAILED;
        return r;
    }
    r->state = REGIST_RUNNING;
    r->started = true;
    return r;
}

RegistState console_regist_poll(ConsoleRegist *r, RegistResult *out)
{
    if (!r) return REGIST_FAILED;
    pthread_mutex_lock(&r->lock);
    RegistState s = r->state;
    if (s == REGIST_OK && out) *out = r->result;
    pthread_mutex_unlock(&r->lock);
    return s;
}

void console_regist_free(ConsoleRegist *r)
{
    if (!r) return;
    if (r->started) {
        chiaki_regist_stop(&r->regist);
        chiaki_regist_fini(&r->regist);
    }
    pthread_mutex_destroy(&r->lock);
    free(r);
}
