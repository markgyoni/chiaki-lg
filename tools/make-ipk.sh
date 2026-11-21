#!/usr/bin/env bash
# Build an .ipk from a staged app tree without ares-cli, in the layout
# ares-package produces (debian-binary, control.tar.gz, data.tar.gz with
# usr/palm/applications/<id>/ and usr/palm/packages/<id>/packageinfo.json).
# File modes are kept, so build it on Linux/WSL, not from a Windows checkout.
#
# Usage: tools/make-ipk.sh <staging-dir> [out-dir]
set -euo pipefail

STAGING="$(cd "${1:?usage: $0 <staging-dir> [out-dir]}" && pwd)"
OUT="$(mkdir -p "${2:-.}" && cd "${2:-.}" && pwd)"
field() { sed -n "s/.*\"$1\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" "$STAGING/appinfo.json" | head -1; }
ID="$(field id)"; VER="$(field version)"; TITLE="$(field title)"; VENDOR="$(field vendor)"
[[ -n "$ID" && -n "$VER" ]] || { echo "no id/version in $STAGING/appinfo.json" >&2; exit 1; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
APP="$TMP/data/usr/palm/applications/$ID"
PKG="$TMP/data/usr/palm/packages/$ID"
mkdir -p "$APP" "$PKG" "$TMP/control"
cp -a "$STAGING"/. "$APP/"
rm -f "$APP/config.json"                       # never ship a user's settings
# The app runs as its own unprivileged user and keeps config.json in its
# directory, so it must be writable.  The installer keeps the modes from the
# package; ares-package produces world-writable trees, so match it.
chmod -R a+rwX "$TMP/data"
cat > "$PKG/packageinfo.json" <<EOF
{
  "app": "$ID",
  "id": "$ID",
  "loc_name": "$TITLE",
  "package_format_version": 2,
  "vendor": "$VENDOR",
  "version": "$VER"
}
EOF
cat > "$TMP/control/control" <<EOF
Package: $ID
Version: $VER
Section: misc
Priority: optional
Architecture: arm
Installed-Size: $(du -sb "$TMP/data" | cut -f1)
Maintainer: N/A <nobody@example.com>
Description: This is a webOS application.
webOS-Package-Format-Version: 2
webOS-Packager-Version: x.y.x
EOF
echo "2.0" > "$TMP/debian-binary"
(cd "$TMP/control" && tar --owner=0 --group=0 --numeric-owner -czf ../control.tar.gz control)
(cd "$TMP/data" && tar --owner=0 --group=0 --numeric-owner -czf ../data.tar.gz usr)
IPK="$OUT/${ID}_${VER}_arm.ipk"
rm -f "$IPK"
# U: real timestamps (Debian ar zeroes them by default)
(cd "$TMP" && ar rcU "$IPK" debian-binary control.tar.gz data.tar.gz)
echo "$IPK"
