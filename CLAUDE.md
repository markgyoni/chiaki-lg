# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Chiaki-lg is a native C port of [chiaki-ng](https://github.com/streetpea/chiaki-ng) (PS4/PS5 Remote Play) for LG webOS TVs. This repo contains only the app layer (`src/`, ~5k lines of C11); the Remote Play protocol comes from `libchiaki`, built from a separate chiaki-ng checkout. App ID is `org.homebrew.chiaki`. License AGPL-3.0.

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

## Debugging on the TV

Logs go to `/tmp/chiaki.log` on the TV (truncated each launch): `ares-shell --device myTV -- "tail -f /tmp/chiaki.log"`. Runtime config lives at `/media/developer/apps/usr/palm/applications/org.homebrew.chiaki/config.json`.

Logging (`src/app_log.h`, implemented in `main.c`): `app_log()` is gated by `log_level`; `app_log_always()` is unconditional — use it for session lifecycle, wakeup, and errors so they're visible at the default `warning` level. Log lines are tagged (`[APP]`, `[AUTO]`, etc.).

## Architecture

**Control flow (`main.c`)**: load config → create SDL window/renderer → `ui_run_registration()` launcher (IP entry, INI import, settings; returns CONNECT/QUIT) → `SS4S_Init` with auto-detected NDL module → optional wakeup (PSN cloud first if `psn_refresh_token` set, then UDP/WoL) → `input_init()` → **session/reconnect loop**: `chiaki_session_init/start`, inner SDL event loop until `g_session_ended`, then `join/fini`; `should_reconnect(quit_reason)` decides whether to retry (transient network errors) or exit (deliberate disconnect / rest mode). `sleep_on_exit` calls `chiaki_session_goto_bed`. Cleanup uses `goto cleanup_*` labels.

**Rendering model**: Video is *not* drawn by the app. NAL units from chiaki's video callback go straight to `SS4S_PlayerVideoFeed()` → webOS NDL hardware decoder, which renders on a hardware plane *below* the app surface. The EGL surface has an 8-bit alpha channel and is cleared to transparent every frame; SDL/GL is used only for UI (launcher, loading screen, stats overlay, hints). Audio likewise feeds raw Opus packets to `SS4S_PlayerAudioFeed()` (NDL hardware Opus; no software decode).

**SS4S module selection** (`detect_webos_ss4s_module` in `main.c`): `ss4s_module: "auto"` reads `/var/run/nyx/os_info.json` → `ndl-webos5` (5+) or `ndl-webos4`; falls back to probing for `libndl-directmedia2.so`. SS4S modules are separate `.so`s dlopen'd from `lib/` next to the binary (rpath `$ORIGIN/lib`), not linked in.

**Threads**: chiaki invokes video/audio callbacks on its own threads; they update `g_stream_stats` (atomics in `stats.h`). `StatsOverlay` is main-thread only and samples those counters. Gamepads are read in a dedicated evdev thread in `input.c` with `EVIOCGRAB` (bypasses SDL joystick and stops webOS stealing B/A/Guide) and push `ChiakiControllerState` directly to the session; `input_set_session(ctx, NULL)` must be called before `chiaki_session_join/fini`. SDL events are only the TV remote (Up-hold toggles stats, Back/Home exits).

**Config**: `config.c` loads/creates `config.json` via json-c into `AppConfig`; there is no save function in `config.c` — writes happen in `ui.c` (`json_object_to_file_ext`) when settings/IP/import are saved. `log_level` is stored as a chiaki bitmask internally but as a string in JSON. `config_import.c` parses a chiaki-ng `chiaki-ng-Default.ini` export (registration keys, base64) from the app dir and renames it to `.imported` on success; the PS5 IP is not in the INI and must be entered in the UI. When adding a config field, update `AppConfig`, `config.c` load/defaults, the settings UI and JSON writers in `ui.c`, `packaging/config.json*`, and the README config table.

**PSN cloud wakeup** (`main.c`, libcurl + OpenSSL): refresh-token → access token → list devices → create/delete a push session. `setup_ssl_ca_bundle()` locates a CA bundle on the TV for curl.

**Controller work** follows `ROADMAP.md` (staged plan: per-driver mapping profiles → robustness → rumble → DualSense hidraw backend for touch/motion/triggers/haptics). Key fact: webOS kernels lack `hid-playstation`, so a DualSense is driven by `hid-generic` and its evdev button codes differ from the Xbox-oriented table in `input.c`.

`NDL_directmedia.h` declares the webOS NDL API (stub lib in SDK sysroot, real one on device); `HAVE_NDL_DIRECTMEDIA` enables NDL queries used for the overlay's buffer-latency estimate.
