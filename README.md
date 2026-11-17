# Chiaki-lg

A native port of [chiaki-ng](https://github.com/streetpea/chiaki-ng) for LG webOS smart TVs.
Streams PS4/PS5 Remote Play directly to your LG webOS TV.

> **Tested on:** PS5 + webOS 5+. PS4 and webOS 4.x are supported in the code and should work, but have not been directly tested. Please open an issue if you encounter problems on those platforms.

> **AI disclosure:** This project was developed with assistance from [Claude.ai](https://claude.ai), [ChatGPT](https://chat.openai.com), and [Google Gemini](https://gemini.google.com). All generated code was reviewed, tested, and integrated by the project author.

---

## Features

- **PS4 and PS5 Remote Play** over local network
- **1080p60 streaming** with H.265, H.265 HDR, or H.264 video codecs
- **Hardware video decode** via webOS NDL (direct media pipeline) — video is decoded on a dedicated hardware plane below the app surface, not software-rendered
- **Native Opus audio passthrough** — raw Opus packets fed directly to webOS NDL hardware decoder (no software decode step)
- **Console manager** — finds PS5/PS4 consoles on the network, links them with the console's PIN (PSN sign-in via a QR code on your phone, once), and keeps several consoles with their own stream settings
- **Full gamepad support** — DualSense (touchpad, motion, vibration, adaptive triggers, lightbar), Xbox Wireless Controller (with rumble), DualShock 4 and other Bluetooth/USB gamepads via direct evdev with exclusive grab (`EVIOCGRAB`); see [Gamepad support](#gamepad-support)
- **Controller test screen** — see live what the PS5 receives and test vibration, triggers and lightbar
- **Wake-on-LAN** — wakes PS5 from rest mode before connecting (UDP broadcast + unicast)
- **PSN cloud wakeup** — optional Sony push-notification wakeup via PSN session API when local UDP wakeup is unreliable (requires PSN refresh token)
- **Rest mode on exit** — Off, On, or Ask (a prompt when you leave the stream)
- **Auto-reconnect** — retries on transient network errors; exits cleanly on deliberate disconnects
- **Stats overlay** — resolution/codec, frame rate, bitrate, network round trip, packet loss, lost frames, decoder buffer and session time; toggled with Up on the remote or Mute on the DualSense, in any screen corner
- **webOS version auto-detection** — automatically selects the correct NDL backend (`ndl-webos5` or `ndl-webos4`) at runtime
- **Two-tier logging** — critical diagnostics always written; verbose chatter gated by `log_level`

---

## Quick start

### Step 1 — Download and install

Download the latest `.ipk` from the [Releases](../../releases/latest) page, then install it on your TV:

```bash
ares-setup-device   # one-time TV setup — TV must be on and in Developer Mode
ares-install --device myTV org.homebrew.chiaki_*.ipk
```

Or use [WebOS Dev Manager](https://github.com/webosbrew/dev-manager-desktop) if you prefer a GUI — it can install IPKs directly via drag and drop.

**TV requirements:** LG webOS 4.x or 5+ TV with [Homebrew Channel](https://github.com/webosbrew/webos-homebrew-channel) installed and Developer Mode enabled.

### Step 2 — Add your console

1. Turn on the PS5 or PS4 (fully on, not rest mode) on the same network as the TV, and launch **Chiaki-lg**.
2. Select **+ Add your console**. Consoles on the network are listed; pick yours (or enter its address manually).
3. The first time, sign in to PlayStation Network: scan the QR code with your phone and sign in. You end up on a page whose address contains `code=v3.XXXXXX`; select **Enter code** on the TV and type the characters after `v3.`. This is needed once for all consoles.
4. On the console open **Settings → System → Remote Play → Link Device** (PS4: **Settings → Remote Play Connection Settings → Add Device**) and type the 8-digit number into the TV.

The console appears on the start screen. Add more consoles the same way.

### Step 3 — Play

Select a console and press OK to connect. Each console has **Settings** (name, address, resolution, frame rate, bitrate, codec, wake on connect, rest mode on exit, link again, remove) and **Remove**. **App settings** holds the controller, stream overlay and logging options; **Controller test** checks every button, the touchpad, motion and the controller outputs.

The menus work with the TV remote or a gamepad (D-pad/left stick to move, Cross to select, Circle to go back). Configs from older versions (one console, imported from chiaki-ng) are moved into the console list automatically.

---

## Configuration

Config file location on the TV:
```
/media/developer/apps/usr/palm/applications/org.homebrew.chiaki/config.json
```

Everything is managed in the app; you can also edit the file over SSH or WebOS Dev Manager.

### Example config

```json
{
    "consoles": [
        {
            "name": "Living room PS5",
            "host": "192.168.1.100",
            "mac": "aa:bb:cc:dd:ee:ff",
            "ps5": true,
            "registered_key": "…",
            "rp_key": "…",
            "rp_key_type": 2,
            "video_width": 1920,
            "video_height": 1080,
            "video_fps": 60,
            "video_bitrate": 15000,
            "video_codec": "h265",
            "wakeup": true,
            "sleep_on_exit": "ask"
        }
    ],
    "last_console": 0,
    "psn_account_id": "…",
    "psn_refresh_token": "…",
    "stats_corner": "top-left",
    "log_level": "warning"
}
```

### Config reference

Per console (`consoles[]`, up to 8):

| Field | Type | Default | Description |
|---|---|---|---|
| `name` | string | console nickname | Shown on the start screen |
| `host` | string | | Console IPv4 address |
| `mac` | string | from linking | Console MAC (Wake-on-LAN) |
| `ps5` | bool | `true` | `true` PS5, `false` PS4 |
| `registered_key`, `rp_key`, `rp_key_type` | | from linking | Remote Play credentials |
| `video_width` / `video_height` | int | `1920` / `1080` | 720p or 1080p (PS5 Remote Play streams at most 1080p) |
| `video_fps` | int | `60` | `30` or `60` |
| `video_bitrate` | int | `15000` | kbps |
| `video_codec` | string | `"h265"` | `"h265"`, `"h265_hdr"` or `"h264"` (PS4 always uses H.264) |
| `wakeup` | bool | `true` | Wake the console when connecting |
| `sleep_on_exit` | string | `"ask"` | Rest mode when you leave a stream: `"off"`, `"on"` or `"ask"` (a prompt when leaving with Back, Down twice or holding Mute) |

App-wide:

| Field | Type | Default | Description |
|---|---|---|---|
| `last_console` | int | `0` | Console selected on the start screen |
| `psn_account_id` | string | from sign-in | PSN account ID (base64), needed to link consoles |
| `psn_refresh_token` | string | from sign-in | PSN token for cloud wakeup (see below) |
| `audio_volume` | int | `100` | Audio volume (0–100) |
| `wakeup_delay_ms` | int | `60000` | Max time (ms) to wait for a console to wake |
| `stats_corner` | string | `"top-left"` | Stats overlay corner: `"top-left"`, `"top-right"`, `"bottom-left"`, `"bottom-right"` |
| `ss4s_module` | string | `"auto"` | SS4S backend. `"auto"` detects webOS version at startup and selects `ndl-webos5` (webOS 5+) or `ndl-webos4` (webOS 4.x) automatically. Override only if auto-detection fails. |
| `log_level` | string | `"warning"` | `"off"`, `"error"`, `"warning"`, `"info"`, `"verbose"`, `"debug"` |
| `controller_profile` | string | `"auto"` | Gamepad button/axis mapping. `"auto"` picks per controller from vendor ID and kernel driver. Overrides: `"sony_generic"` (DualSense/DS4 on the generic HID driver, the usual case on LG TVs), `"sony_kernel"`, `"xbox_bt"`, `"xpad"`, `"generic"`. The choice is logged as `profile=` in `/tmp/chiaki.log`. |
| `rumble` | bool | `true` | Forward PS5 vibration (rumble and the haptics stream, converted to rumble) to the controller: DualSense (USB, or Bluetooth with `dualsense_bt_patch`), Xbox pads over Bluetooth, and pads whose kernel driver supports force feedback. |
| `dualsense_hid` | bool | `true` | Read a DualSense through its raw HID reports instead of the TV's generic driver. Needed for the touchpad, motion, vibration, adaptive triggers and the lightbar. Set `false` to fall back to plain buttons/sticks. |
| `motion` | bool | `true` | Send DualSense gyro/accelerometer to the PS5 (motion aiming). Needs `dualsense_hid`. |
| `trigger_effects` | bool | `true` | DualSense adaptive triggers. Needs `dualsense_hid`. |
| `dualsense_bt_patch` | bool | `true` | Make DualSense rumble, adaptive triggers and lightbar work over **Bluetooth**. LG's Bluetooth stack sends controller output in a form the DualSense ignores (it only does it right for Xbox pads). On a **rooted** TV the app runs the bundled `btfix` helper through the Homebrew Channel, which fixes this in the running Bluetooth service's memory: nothing on disk changes, and a reboot undoes it. It only patches a known firmware build (webOS 6.5.3 / 03.53.45 so far) and refuses anything else. Without root, or with `false`, DualSense outputs work over USB only. The log shows `[BTFIX]`. |
| `input_debug` | bool | `false` | Log every raw gamepad event (`[PADEV]`) and sample raw HID reports at startup. Controller capabilities (`[PAD]`/`[HID]`) are always logged. |

### PSN cloud wakeup (optional)

When the console ignores local wakeup packets, the app wakes it through PSN. The sign-in done when you add a console stores the token for this, so there is nothing extra to set up. Also enable **PS5 Settings → System → Power Saving → Features Available in Rest Mode → Enable Turning On PS5 from Network**.

---

## Gamepad support

Connect a gamepad via Bluetooth or USB, before or after launching the app (hot-plug works, also mid-stream). Up to four pads can be connected; their inputs are merged.

The app reads gamepads directly from the kernel (evdev) and grabs them exclusively (`EVIOCGRAB`), so webOS can't steal buttons (B→Back, A→OK, Guide→Home). LG kernels have no `hid-playstation` driver, so a DualSense is additionally read through its raw HID reports, which is what makes the touchpad, motion and the DualSense outputs work.

### Supported controllers

| Controller | Connection | Layout (`profile=`) | Status |
|---|---|---|---|
| DualSense / DualSense Edge | Bluetooth | `sony_generic` | Tested |
| DualSense / DualSense Edge | USB | `sony_generic` | Tested |
| Xbox Series / One (BT firmware) | Bluetooth | `xbox_bt` | Tested |
| Xbox / XInput pads | USB (`xpad`) | `xpad` | Mapping only, untested |
| DualShock 4 | Bluetooth / USB | `sony_generic` or `sony_kernel` | Buttons and sticks, untested |
| Other HID gamepads | Bluetooth / USB | `generic` (axes guessed) | Untested |

### Features per controller

| | DualSense (USB) | DualSense (Bluetooth) | Xbox (Bluetooth) | Others |
|---|---|---|---|---|
| Buttons, sticks, analog triggers | ✅ | ✅ | ✅ | ✅ |
| Touchpad (touch, two fingers, click) | ✅ | ✅ | Click via **View + Menu** together | Click via **Select + Start** |
| Motion (gyro aiming) | ✅ | ✅ | — | — |
| Vibration (incl. PS5 haptics → rumble) | ✅ | ✅ with the Bluetooth fix¹ | ✅ | If the kernel driver supports force feedback |
| Adaptive triggers | ✅ | ✅ with the Bluetooth fix¹ | — | — |
| Lightbar / player LEDs | ✅ | ✅ with the Bluetooth fix¹ | — | — |
| HD haptics, microphone, speaker/headset jack | Not possible on LG TVs (see ROADMAP Stage 11) | — | — | — |

¹ DualSense outputs over Bluetooth need the in-memory Bluetooth fix (`dualsense_bt_patch`, see the config reference): a rooted TV with the Homebrew Channel and a known firmware build (webOS 6.5.3 / 03.53.45 so far). Without it, use USB for vibration, triggers and the lightbar. The launcher and the controller test show which outputs are active for each pad.

PS5 games mostly send vibration as a haptics audio stream; the app turns it into normal rumble for every pad that can vibrate.

Use **Controller test** in the launcher to check a pad: it shows exactly what the PS5 will receive (buttons, sticks, triggers, touches, motion) and can test vibration, adaptive triggers, the lightbar and player LEDs. Hold **PS** for 2 s to leave it with the controller.

The TV remote is not forwarded to the PS5 as controller input. During streaming it serves only:

| TV Remote Button | Action |
|---|---|
| **Up** | Toggle stats overlay |
| **Down** twice (within 0.7 s) | Leave the stream and go back to the menu |
| **Back** | Exit the app (while the stream is still connecting: cancel and go back to the menu) |
| **Home** | Exit the app |

On a DualSense (raw HID backend), the **Mute** button is not sent to the console:

| DualSense | Action |
|---|---|
| **Mute** (tap) | Toggle stats overlay |
| **Mute** (hold 3 s) | Leave the stream and go back to the menu |

With `Rest mode on exit = Ask`, leaving the stream (Back, Down twice, holding Mute) asks whether to put the console in rest mode.

---

## Logs

Logs are written to `/tmp/chiaki.log` on the TV (cleared on each launch).

```bash
# Stream logs live
ares-shell --device myTV -- "tail -f /tmp/chiaki.log"

# Download the log
ares-shell --device myTV -- "cat /tmp/chiaki.log" > chiaki.log
```

### Controller diagnostics

Every launch logs each input device (`[PAD]`: name, bus, vendor/product, kernel driver, buttons, axis ranges, rumble support) and each raw HID node (`[HID]`). Set `"input_debug": true` to also log every button/axis event as `[PADEV]` lines.

The IPK also ships a standalone `paddiag` tool for use over SSH (root):

```bash
cd /media/developer/apps/usr/palm/applications/org.homebrew.chiaki
./paddiag info                      # capability dump
./paddiag events                    # live button/axis monitor (Ctrl-C to stop)
./paddiag hidraw /dev/hidraw0       # sample raw HID input reports
```

Set `"log_level": "info"` or `"debug"` in config.json for more detail. Critical events (session lifecycle, wakeup, errors) are always logged regardless of level.

---

## Troubleshooting

**Stream doesn't start / connection timeout**
Ensure Remote Play is enabled on your PS5 (Settings → System → Remote Play → Enable Remote Play). Verify the IP in `config.json` and that the TV and PS5 are on the same subnet.

**"Connection failed: the console could not start a ... stream"**
The PS5 refused the video settings (`AvCap failed to initialize video` in the log). PS5 Remote Play streams at up to **1080p**; 1440p and 2160p are refused, so the app no longer offers them and treats larger values in `config.json` as 1080p. If it still happens at 1080p, try H.264.

**Black screen with audio**
Likely a codec or NDL issue. Check logs. Try `"video_codec": "h264"` as a fallback — H.264 has broader compatibility across NDL versions.

**Black screen on webOS 4**
Check `/tmp/chiaki.log` for the `[AUTO]` line confirming which SS4S module was selected. If auto-detection chose `ndl-webos5` incorrectly, set `"ss4s_module": "ndl-webos4"` in `config.json` manually.

**PS5 won't wake from rest mode**
Add your `psn_refresh_token` to `config.json` to enable PSN cloud wakeup (see above).

**Gamepad buttons intercepted by webOS**
Check logs for `EVIOCGRAB FAILED`. Disconnect and reconnect the controller after launching the app.

**Controller not detected, or buttons mixed up**
Every launch logs one `[PAD]` line per input device, e.g.

```
[PAD] /dev/input/event13 "DualSense Wireless Controller" bus=0x05 vid=054c pid=0ce6 driver=hid-generic
[INPUT] Opened gamepad: "DualSense Wireless Controller" ... profile=sony_generic (PlayStation pad on hid-generic)
```

`bus=0x05` is Bluetooth, `0x03` USB. If your pad has a `[PAD]` line but no `Opened gamepad`, it lacks a south button plus left stick, or has no kernel driver (`driver=(none)`); please open an issue with the log. If buttons are swapped, try another **Button layout** in Settings (`controller_profile`) and report which one works. Turn on **Controller debug log** (`input_debug`) to log every raw button/axis event as `[PADEV]`.

**DualSense: no vibration / triggers / lightbar over Bluetooth**
Look for the `[BTFIX]` line. `active` means the fix is applied; `unavailable: ...` gives the reason (not rooted, unknown firmware, Homebrew Channel missing). The `[INPUT] ... DualSense outputs` line says what is enabled per pad. Over USB the outputs always work.

**DualSense: no touchpad or motion**
Needs `"dualsense_hid": true`. The log should show `DualSense raw HID backend on` and `DualSense full reports active`. If `[HID]` lines say `open ... failed`, the app can't read the controller's raw HID node.

**App crashes on launch**
Usually a malformed `config.json`. Check `/tmp/chiaki.log`. Delete the config file to start fresh, then add your console again.

---

## Architecture

### Video pipeline

H.264/H.265 NAL units from the chiaki-ng callback are fed directly to `SS4S_PlayerVideoFeed()`, which routes them to webOS NDL's hardware video decoder. NDL renders on a hardware plane *below* the app's OpenGL surface. The EGL surface is configured with an 8-bit alpha channel and cleared to transparent each frame so the video plane shows through. The SDL/GL layer is only used for UI overlays.

### Audio pipeline

Raw Opus packets from the chiaki-ng audio callback are fed directly to `SS4S_PlayerAudioFeed()` with codec `SS4S_AUDIO_OPUS`. webOS NDL has native Opus hardware decoding — there is no software decode step.

### Input pipeline

Gamepad input uses direct Linux evdev reads with `EVIOCGRAB` for exclusive device access, bypassing SDL's joystick subsystem. The evdev reader runs in a dedicated thread and pushes `ChiakiControllerState` updates directly to the chiaki session.

### webOS version auto-detection

At startup, the app reads `/var/run/nyx/os_info.json` (present on all webOS devices) to determine the major webOS version, then selects `ndl-webos5` (webOS 5+) or `ndl-webos4` (webOS 4.x). If that file is unreadable it falls back to probing for `libndl-directmedia2.so` on disk. The result is logged as `[AUTO]` in `/tmp/chiaki.log`.

---

## Building from source

Most users should download the IPK from the [Releases](../../releases/latest) page. Build from source only if you want to make code changes or the pre-built IPK doesn't work for your TV.

### Requirements

- Linux or WSL2 build machine
- GCC, CMake ≥ 3.16, make, pkg-config, wget, git, Python 3 + pip
- [webOS native SDK toolchain](https://github.com/webosbrew/native-toolchain) — extract and run `relocate-sdk.sh`
- [ares-cli](https://github.com/webosbrew/ares-cli-rs) in PATH

```bash
pip3 install nanopb protobuf --break-system-packages
```

> **WSL users:** keep source files on a Linux filesystem (`~/`) rather than `/mnt/c/`. Always build via `./build-webos.sh` — not `cmake --build` directly.

### Build

```bash
git clone https://github.com/streetpea/chiaki-ng.git
git clone <this-repo-url> chiaki-lg
cd chiaki-lg

export TOOLCHAIN_DIR=~/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot
./build-webos.sh ../chiaki-ng 2>&1 | tee build.log
```

The script cross-compiles all dependencies (OpenSSL, Opus, FFmpeg, json-c, miniupnpc, cURL, GF-Complete, Jerasure, SS4S), patches chiaki-ng sources for webOS glibc compatibility, pre-generates nanopb protobuf sources, builds the binary, and packages an `.ipk` via `ares-package`. Dependencies are cached in `/tmp/webos-staging` and skipped on subsequent runs.

The IPK is output to `build-webos/*.ipk`.

### Common build errors

**"cannot find -lchiaki"** or **"Syntax error: ( unexpected"** — You ran `cmake --build` directly. Always use `./build-webos.sh`.

**"nanopb_generator.py not found"** — Run `pip3 install nanopb --break-system-packages`.

---

## Source files

| File | Purpose |
|---|---|
| `main.c` | Entry point, session lifecycle, wakeup (UDP + PSN cloud), webOS version detection, SDL event loop |
| `config.c` / `config.h` | JSON config loader, default config generation |
| `ui_home.c` | Start screen: console list, add console (discovery, PSN sign-in with QR code, PIN), console settings |
| `console_net.c` / `console_net.h` | Console discovery and registration (libchiaki) |
| `psn_login.c` / `psn_login.h` | PSN sign-in: code → account ID and refresh token |
| `third_party/qrcodegen.*` | QR code generator (Project Nayuki, MIT) |
| `video.c` / `video.h` | Video callback → SS4S/NDL feed, codec negotiation, stats counters |
| `audio.c` / `audio.h` | Audio callback → SS4S/NDL Opus feed |
| `input.c` / `input.h` | Evdev gamepad reader thread with `EVIOCGRAB` |
| `ui.c` / `ui.h` | Launcher UI, settings screen, loading screen, stats overlay renderer |
| `stats.c` / `stats.h` | Thread-safe stream statistics and overlay state |
| `app_log.h` | Shared logging macros |
| `NDL_directmedia.h` | webOS NDL API declarations |
| `CMakeLists.txt` | Build system |
| `build-webos.sh` | One-shot cross-compile + IPK packaging script |
| `appinfo.json` | webOS app metadata |
| `config.json` | Default config (deployed to TV, populated on first run) |
| `config_json.example` | Annotated config template |

---

## Dependencies

| Library | Version | Link | Purpose |
|---|---|---|---|
| chiaki-ng | main | static | PS Remote Play protocol |
| SS4S | — | dynamic | webOS NDL video/audio abstraction (ndl-webos4 + ndl-webos5) |
| OpenSSL | 3.2.1 | static | TLS for PSN API, crypto for chiaki |
| Opus | 1.4 | static | Audio codec (chiaki internal use) |
| FFmpeg | 6.1.1 | static | H.264/H.265 codec support |
| json-c | 0.17 | static | Config file parsing |
| cURL | 8.7.1 | static | PSN API HTTP calls |
| miniupnpc | 2.2.7 | static | UPnP (chiaki dependency) |
| GF-Complete | master | static | Erasure coding (chiaki dependency) |
| Jerasure | 2.0 | static | FEC (chiaki dependency) |
| SDL2 | 2.30.x | dynamic | Window/GL surface, TV remote input |
| nanopb | 0.4.x | static | Protobuf (chiaki submodule) |

---

## License

AGPL-3.0 — same as [chiaki-ng](https://github.com/streetpea/chiaki-ng), on which this project is based.
