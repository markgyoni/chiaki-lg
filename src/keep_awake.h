#pragma once
/* Keep the TV awake while a gamepad is used.
 *
 * The app grabs gamepads (EVIOCGRAB), so webOS never sees their input and
 * starts the screensaver (and, after hours, automatic standby) even while
 * someone navigates the menus or plays with a controller.  keep_awake_poke()
 * runs keepawake.sh (next to the app) as root through the Homebrew Channel at
 * most every 20 s: it resets webOS's no-operation timers and leaves the
 * screensaver if it is showing.  Rooted TVs only; otherwise a no-op.
 * Non-blocking (worker thread). */
void keep_awake_poke(void);
