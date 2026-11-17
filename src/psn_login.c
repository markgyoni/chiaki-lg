#include "psn_login.h"
#include "app_log.h"

#include <chiaki/base64.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Same public OAuth client as chiaki-ng's PSN sign-in. */
#define CLIENT_ID     "ba495a24-818c-472b-b12d-ff231c1b5745"
#define CLIENT_SECRET "mvaiZkRsAsI1IBkY"
#define TOKEN_URL     "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token"
#define REDIRECT_URI  "https://remoteplay.dl.playstation.net/remoteplay/redirect"
#define SCOPE         "psn:clientapp referenceDataService:countryConfig.read " \
                      "pushNotification:webSocket.desktop.connect " \
                      "sessionManager:remotePlaySession.system.update"

const char *psn_login_url(void)
{
    return "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/authorize"
           "?service_entity=urn:service-entity:psn&response_type=code"
           "&client_id=" CLIENT_ID
           "&redirect_uri=" REDIRECT_URI
           "&scope=psn:clientapp%20referenceDataService:countryConfig.read"
           "%20pushNotification:webSocket.desktop.connect"
           "%20sessionManager:remotePlaySession.system.update"
           "&request_locale=en_US&ui=pr&service_logo=ps&layout_type=popup"
           "&smcid=remoteplay&prompt=always&PlatformPrivacyWs1=minimal";
}

typedef struct { char *buf; size_t len; } Buf;

static size_t write_cb(void *p, size_t sz, size_t n, void *u)
{
    Buf *b = u;
    size_t t = sz * n;
    char *nb = realloc(b->buf, b->len + t + 1);
    if (!nb) return 0;
    b->buf = nb;
    memcpy(b->buf + b->len, p, t);
    b->len += t;
    b->buf[b->len] = '\0';
    return t;
}

/* GET (post == NULL) or POST to url with client basic auth; returns the JSON body. */
static json_object *request(const char *url, const char *post, long *http, char *err, size_t errlen)
{
    CURL *c = curl_easy_init();
    if (!c) { snprintf(err, errlen, "curl init failed"); return NULL; }
    Buf b = { 0 };
    struct curl_slist *h = NULL;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERNAME, CLIENT_ID);
    curl_easy_setopt(c, CURLOPT_PASSWORD, CLIENT_SECRET);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    const char *ca = getenv("CURL_CA_BUNDLE");    // set by setup_ssl_ca_bundle() in main.c
    if (ca) curl_easy_setopt(c, CURLOPT_CAINFO, ca);
    if (post) {
        h = curl_slist_append(h, "Content-Type: application/x-www-form-urlencoded");
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, post);
    }
    CURLcode rc = curl_easy_perform(c);
    *http = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, http);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        snprintf(err, errlen, "network error: %s", curl_easy_strerror(rc));
        free(b.buf);
        return NULL;
    }
    json_object *j = b.buf ? json_tokener_parse(b.buf) : NULL;
    if (*http != 200)
        app_log_always("[PSN] %s HTTP %ld: %.300s\n", post ? "token" : "account", *http, b.buf ? b.buf : "");
    free(b.buf);
    return j;
}

/* "v3.AbCdEf", "AbCdEf" or a whole redirect URL containing code=... */
static void extract_code(const char *in, char *out, size_t outlen)
{
    const char *p = strstr(in, "code=");
    p = p ? p + 5 : in;
    while (*p == ' ') p++;
    size_t n = strcspn(p, "& \r\n");
    char tmp[128];
    if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
    memcpy(tmp, p, n); tmp[n] = '\0';
    if (strncmp(tmp, "v3.", 3) == 0 || strchr(tmp, '.'))
        snprintf(out, outlen, "%s", tmp);
    else
        snprintf(out, outlen, "v3.%s", tmp);
}

bool psn_login_with_code(const char *code_or_url, char **account_id_b64, char **refresh_token,
                         char *err, size_t errlen)
{
    *account_id_b64 = NULL;
    *refresh_token = NULL;
    char code[128];
    extract_code(code_or_url, code, sizeof(code));

    CURL *esc = curl_easy_init();
    char *ecode = curl_easy_escape(esc, code, 0);
    char *escope = curl_easy_escape(esc, SCOPE, 0);
    char *eredir = curl_easy_escape(esc, REDIRECT_URI, 0);
    char body[1024];
    snprintf(body, sizeof(body), "grant_type=authorization_code&code=%s&scope=%s&redirect_uri=%s",
             ecode, escope, eredir);
    curl_free(ecode); curl_free(escope); curl_free(eredir);
    curl_easy_cleanup(esc);

    long http = 0;
    json_object *tok = request(TOKEN_URL, body, &http, err, errlen);
    if (!tok || http != 200) {
        if (tok) json_object_put(tok);
        if (http) snprintf(err, errlen, "sign-in code not accepted (HTTP %ld) - codes expire after a few minutes", http);
        return false;
    }
    json_object *v;
    const char *access = json_object_object_get_ex(tok, "access_token", &v) ? json_object_get_string(v) : NULL;
    const char *refresh = json_object_object_get_ex(tok, "refresh_token", &v) ? json_object_get_string(v) : NULL;
    if (!access) {
        snprintf(err, errlen, "no access token in the reply");
        json_object_put(tok);
        return false;
    }

    char url[2048];
    snprintf(url, sizeof(url), "%s/%s", TOKEN_URL, access);
    json_object *info = request(url, NULL, &http, err, errlen);
    const char *user_id = (info && http == 200 && json_object_object_get_ex(info, "user_id", &v))
                          ? json_object_get_string(v) : NULL;
    if (!user_id) {
        if (http) snprintf(err, errlen, "could not read the account (HTTP %ld)", http);
        if (info) json_object_put(info);
        json_object_put(tok);
        return false;
    }

    /* account ID = user_id as 8 bytes little endian, base64 */
    unsigned long long id = strtoull(user_id, NULL, 10);
    uint8_t raw[8];
    for (int i = 0; i < 8; i++) raw[i] = (uint8_t)(id >> (8 * i));
    char b64[32];
    if (chiaki_base64_encode(raw, sizeof(raw), b64, sizeof(b64)) != CHIAKI_ERR_SUCCESS) {
        snprintf(err, errlen, "could not encode the account ID");
        json_object_put(info);
        json_object_put(tok);
        return false;
    }
    *account_id_b64 = strdup(b64);
    if (refresh) *refresh_token = strdup(refresh);
    app_log_always("[PSN] signed in: account ID %s, refresh token %s\n", b64, refresh ? "received" : "missing");
    json_object_put(info);
    json_object_put(tok);
    return true;
}
