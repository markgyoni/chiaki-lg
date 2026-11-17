#!/usr/bin/env bash
# Copy a built app tree onto a rooted TV over SSH, keeping config.json and
# imported credentials.  Faster than packaging + ares-install during development.
#
# Usage: tools/deploy-tv.sh <ssh-host> [staging-dir]
#   ssh-host     an ssh alias/host that logs in as root on the TV
#   staging-dir  defaults to build-webos/ipk-staging
set -euo pipefail

HOST="${1:?usage: $0 <ssh-host> [staging-dir]}"
STAGING="${2:-$(dirname "$0")/../build-webos/ipk-staging}"
APP_DIR=/media/developer/apps/usr/palm/applications/org.homebrew.chiaki

[[ -f "$STAGING/chiaki-webos" ]] || { echo "no chiaki-webos in $STAGING" >&2; exit 1; }

# Stop a running instance so the binary is not busy.
ssh "$HOST" "luna-send -n 1 luna://com.webos.applicationManager/closeByAppId '{\"id\":\"org.homebrew.chiaki\"}' >/dev/null 2>&1 || true; mkdir -p $APP_DIR/lib"

# Ship the app files only (never '.', whose owner/mode would replace the app
# dir's, and never config.json / credentials, which live only on the TV).
FILES=$(cd "$STAGING" && ls -d chiaki-webos paddiag btfix appinfo.json icon.png largeIcon.png lib 2>/dev/null | tr '\n' ' ')
tar -C "$STAGING" -cf - $FILES | ssh "$HOST" "tar -C $APP_DIR -xof - && cd $APP_DIR && chown -R root:root $FILES && chmod 755 chiaki-webos lib && chmod 755 paddiag btfix 2>/dev/null; ls -la $APP_DIR"
echo "Deployed to $HOST:$APP_DIR"
