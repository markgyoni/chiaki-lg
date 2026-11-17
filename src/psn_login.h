#pragma once
/* PSN sign-in for registering consoles (same OAuth flow as chiaki-ng).
 *
 * The user opens psn_login_url() on a phone (QR code on the TV), signs in,
 * and ends up on a remoteplay.dl.playstation.net/remoteplay/redirect page
 * whose address contains "code=v3.XXXXXX".  That code is exchanged for the
 * account ID (needed to register a console) and a refresh token (used for
 * PSN cloud wakeup).  Blocking network calls: run off the UI thread. */

#include <stdbool.h>
#include <stddef.h>

const char *psn_login_url(void);

/* code_or_url: "v3.XXXXXX", just the part after "v3.", or the whole address.
 * On success *account_id_b64 / *refresh_token are malloc'd (token may be NULL). */
bool psn_login_with_code(const char *code_or_url, char **account_id_b64, char **refresh_token,
                         char *err, size_t errlen);
