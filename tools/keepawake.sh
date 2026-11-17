#!/bin/sh
# Run as root by the Homebrew Channel while a gamepad is in use (the app grabs
# gamepads, so webOS never sees their input): reset webOS's no-operation
# timers (screensaver, automatic standby) and leave the screensaver if it is
# already showing.  Read-only otherwise; nothing is changed on disk.
LS=/usr/bin/luna-send

T=$($LS -n 1 luna://com.webos.service.config/getConfigs \
      '{"configNames":["com.webos.service.nop.screenSaverTimeout"]}' |
    sed -n 's/.*screenSaverTimeout"[^0-9]*\([0-9][0-9]*\).*/\1/p')
[ -n "$T" ] && $LS -n 1 luna://com.webos.service.nop/removeNopTimer \
    "{\"timerId\":\"[TIMER]ScreenSaver:$T\",\"option\":\"reset\"}" >/dev/null

for id in $($LS -n 2 -w 1500 luna://com.webos.service.nop/getTimerInfo '{"subscribe":true}' |
            grep -o '\[TIMER\]automaticStandby[^"]*'); do
    $LS -n 1 luna://com.webos.service.nop/removeNopTimer \
        "{\"timerId\":\"$id\",\"option\":\"reset\"}" >/dev/null
done

case "$($LS -n 1 luna://com.webos.service.tvpower/power/getPowerState '{}')" in
    *"Screen Saver"*) $LS -n 1 luna://com.webos.service.tvpower/power/turnOnScreen '{}' >/dev/null ;;
esac
echo ok
