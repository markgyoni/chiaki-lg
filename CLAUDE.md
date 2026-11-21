# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Chiaki-webOS (formerly chiaki-lg, renamed in 0.9.0) is a native C port of [chiaki-ng](https://github.com/streetpea/chiaki-ng) (PS4/PS5 Remote Play) for LG webOS TVs. This repo contains only the app layer (`src/`, ~5k lines of C11); the Remote Play protocol comes from `libchiaki`, built from a separate chiaki-ng checkout. App ID is `org.homebrew.chiaki`. License AGPL-3.0-only with the OpenSSL exception (LICENSE, same as chiaki-ng).

## Build

There are no tests or linters. The only supported build is cross-compiling via the script, on Linux/WSL2 (not native Windows):

```bash
export TOOLCHAIN_DIR=~/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot   # webosbrew native-toolchain, after relocate-sdk.sh
./build-webos.sh ../chiaki-ng 2>&1 | tee build.log                        # arg defaults to ../chiaki-ng
```

Output: `build-webos/*.ipk`. Install with `ares-install --device myTV build-webos/*.ipk`. Prereqs: `pip3 install nanopb protobuf --break-system-packages`, `ares-cli` on PATH.

- **Never run `cmake --build` directly** on a fresh tree — `build-webos.sh` does essential setup that CMakeLists.txt alone can't: builds deps into `/tmp/webos-staging` (cached; delete a lib's output there to force a rebuild), clones `ss4s` into `third-party/ss4s`, writes `cmake/FindNanopb.cmake` and a `CMAKE_PROJECT_INCLUDE` hints file, wraps pkg-config to strip the sysroot from staging paths, pre-generates `takion.pb.c/h` with host Python and neuters the generated `chiaki-pb` make rule (WSL PATH with `(x86)` breaks `/bin/sh`), and wipes the CMake cache each run. `third-party/`, `cmake/`, `build-webos/` are gitignored build artifacts.
- The script **patches the chiaki-ng checkout in place** (`lib/src/thread.c`: `pthread_clockjoin_np` → `pthread_timedjoin_np`; `lib/src/common.c`: `aligned_alloc` → `posix_memalign`) because webOS glibc is old.
- New `.c` files must be added to `add_executable(chiaki-webos ...)` in `CMakeLists.txt`.
- On WSL keep sources on the Linux filesystem (`~/`), not `/mnt/c/`.

Host gotchas seen on Debian 13 WSL with buildroot-nc4 `webos-a38c582`: the SDK's bundled `cmake` needs `libssl.so.1.1` (replace `bin/cmake` with a pip-installed `cmake<4`; CMake 4 breaks old deps), chiaki-ng's `lib/protobuf` needs a host `protoc` at configure time, and `relocate-sdk.sh` needs the `file` command. Without `ares-package` on PATH the script stops after staging `build-webos/ipk-staging`.

## Versioning

The version lives only in `packaging/appinfo.json` (`version`). CMake embeds it plus the git revision (`$CHIAKI_WEBOS_GIT`, or the older `$CHIAKI_LG_GIT`, from the WSL build wrapper, else `git describe --always --dirty`) as `APP_VERSION`/`APP_GIT`; the launcher header shows `vX.Y.Z (rev)` and the log starts with `[APP] Chiaki-webOS vX.Y.Z (rev)`. Scheme: `0.MINOR.PATCH` — bump MINOR when a ROADMAP stage is completed, PATCH for fixes in between; tag releases `vX.Y.Z`.

## Dev loop on a rooted TV

- `tools/deploy-tv.sh <ssh-host> [staging-dir]` copies a build into the app dir over SSH (root), preserving `config.json` and credentials. Reinstalling an IPK replaces the app dir and loses them.
- `paddiag` (built and shipped next to the binary) is the controller diagnostics CLI: `paddiag info`, `paddiag events [--grab]`, `paddiag hidraw /dev/hidrawN [--feature]`. Shared code lives in `src/pad_diag.c`; the app logs the same dump at startup (`[PAD]`/`[HID]`), and `"input_debug": true` adds raw `[PADEV]` event lines.
- Controller findings per device go in `docs/controllers/`.

## Debugging on the TV

Logs go to `/tmp/chiaki.log` on the TV (truncated each launch): `ares-shell --device myTV -- "tail -f /tmp/chiaki.log"`. Runtime config lives at `/media/developer/apps/usr/palm/applications/org.homebrew.chiaki/config.json`.

Logging (`src/app_log.h`, implemented in `main.c`): `app_log()` is gated by `log_level`; `app_log_always()` is unconditional — use it for session lifecycle, wakeup, and errors so they're visible at the default `warning` level. Log lines are tagged (`[APP]`, `[AUTO]`, etc.).

## Architecture

**Control flow (`main.c`)**: load config → create SDL window/renderer → `input_init()` → loop { `ui_run_home()` start screen (console list; returns CONNECT/QUIT with the chosen console made active) → `SS4S_Init` once with the auto-detected NDL module, player open per stream → optional wakeup on a worker thread (skipped if port 9295 already answers; PSN cloud first if `psn_refresh_token` set, then UDP/WoL) → **session/reconnect loop**: `chiaki_session_init/start`, inner SDL event loop until `g_session_ended`, then `join/fini`; `should_reconnect(quit_reason)` decides retry vs leave } . Leaving with Down twice / holding DualSense Mute / Back while starting returns to the start screen; rest mode per console (`sleep_on_exit` Off/On/Ask, Ask shows `ui_ask_rest_mode`).

**Rendering model**: Video is *not* drawn by the app. NAL units from chiaki's video callback go straight to `SS4S_PlayerVideoFeed()` → webOS NDL hardware decoder, which renders on a hardware plane *below* the app surface. The EGL surface has an 8-bit alpha channel and is cleared to transparent every frame; SDL/GL is used only for UI (launcher, loading screen, stats overlay, hints). Audio likewise feeds raw Opus packets to `SS4S_PlayerAudioFeed()` (NDL hardware Opus; no software decode).

**SS4S module selection** (`detect_webos_ss4s_module` in `main.c`): `ss4s_module: "auto"` reads `/var/run/nyx/os_info.json` → `ndl-webos5` (5+) or `ndl-webos4`; falls back to probing for `libndl-directmedia2.so`. SS4S modules are separate `.so`s dlopen'd from `lib/` next to the binary (rpath `$ORIGIN/lib`), not linked in.

**Threads**: chiaki invokes video/audio callbacks on its own threads; they update `g_stream_stats` (atomics in `stats.h`). `StatsOverlay` is main-thread only and samples those counters. Gamepads are read in a dedicated evdev thread in `input.c` with `EVIOCGRAB` (bypasses SDL joystick and stops webOS stealing B/A/Guide) and push `ChiakiControllerState` directly to the session; `input_set_session(ctx, NULL)` must be called before `chiaki_session_join/fini`. SDL events are only the TV remote (Up toggles stats, Down twice → menu, Back/Home exits); DualSense Mute is read from raw HID (`input_take_mute_event`: tap = stats, hold 3 s = menu).

**Config**: `config.c` loads `config.json` via json-c into `AppConfig` and writes it with `config_save()` (keeps unknown keys). Consoles live in `consoles[]` (`ConsoleConfig`: name, host, mac, ps5, Remote Play keys, per-console video/wake/rest settings); `config_select_console()` copies the chosen one into the flat "active console" fields the stream code reads. Pre-0.7 single-console configs (top-level `registered_key`/`host`) are migrated on load. `log_level` is a chiaki bitmask internally, a string in JSON. Consoles are added in the app (`ui_home.c`): discovery (`console_net.c`, libchiaki discovery service) → PSN sign-in (`psn_login.c`, QR code of the OAuth URL, the user types the redirect code) → PIN → `chiaki_regist`. When adding a config field, update `AppConfig`/`ConsoleConfig`, `config.c` load/save, the settings UI (`ui.c` app settings or `ui_home.c` console settings), and the README config table.

**PSN cloud wakeup** (`main.c`, libcurl + OpenSSL): refresh-token → access token → list devices → create/delete a push session. `setup_ssl_ca_bundle()` locates a CA bundle on the TV for curl.

**Controller work** follows `ROADMAP.md` (staged plan: per-driver mapping profiles → robustness → rumble → DualSense hidraw backend for touch/motion/triggers/haptics). Key fact: webOS kernels lack `hid-playstation`, so a DualSense is driven by `hid-generic` and its evdev button codes differ from the Xbox-oriented table in `input.c`.

`NDL_directmedia.h` declares the webOS NDL API (stub lib in SDK sysroot, real one on device); `HAVE_NDL_DIRECTMEDIA` enables NDL queries used for the overlay's buffer-latency estimate.
