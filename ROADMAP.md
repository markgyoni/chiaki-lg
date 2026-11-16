# Controller Roadmap

Goal: every controller button, stick and trigger works correctly. A DualSense on a PS5 gets touchpad, motion, rumble, adaptive triggers and lightbar.

Work through the stages in order. Each stage can be built, installed and tested on the TV on its own, and each one ends with a "Done when" checklist. Mark items `[x]` as you go.

---

## Background: why the buttons are wrong

LG webOS kernels do not have the `hid-playstation` driver (added in Linux 5.12). So a DualSense is driven by **`hid-generic`**, which numbers the HID buttons in descriptor order:

| HID button | DualSense | evdev code (hid-generic) | Current `input.c` maps to |
|---|---|---|---|
| 1 | Square | `BTN_SOUTH` 0x130 | Cross |
| 2 | Cross | `BTN_EAST` 0x131 | Circle |
| 3 | Circle | `BTN_C` 0x132 | – |
| 4 | Triangle | `BTN_NORTH` 0x133 | Square |
| 5 | L1 | `BTN_WEST` 0x134 | Triangle |
| 6 | R1 | `BTN_Z` 0x135 | – |
| 7 | L2 (digital) | `BTN_TL` 0x136 | L1 |
| 8 | R2 (digital) | `BTN_TR` 0x137 | R1 |
| 9 | Create | `BTN_TL2` 0x138 | ignored |
| 10 | Options | `BTN_TR2` 0x139 | ignored |
| 11 | L3 | `BTN_SELECT` 0x13a | Share |
| 12 | R3 | `BTN_START` 0x13b | Options |
| 13 | PS | `BTN_MODE` 0x13c | PS |
| 14 | Touchpad click | `BTN_THUMBL` 0x13d | L3 |
| 15 | Mute | `BTN_THUMBR` 0x13e | R3 |

Axes (hid-generic DualSense/DS4): `ABS_X/Y` left stick, `ABS_Z/RZ` right stick, `ABS_RX` = L2 analog, `ABS_RY` = R2 analog, `ABS_HAT0X/Y` d-pad, range 0–255.

The current table was written for an **Xbox controller over Bluetooth**. Its descriptor has gaps in the button numbering, so A/B/X/Y happen to land on the expected codes.

hid-generic exposes **no touchpad coordinates, no motion sensors and no force feedback** for the DualSense. All of those need raw HID (`/dev/hidrawN`).

---

## Stage 0: Fork, build baseline, diagnostics

**Why:** You need a working build, and you need to see what each controller actually reports on *your* TV before changing any mappings.

- [x] Fork, set up the toolchain and build with `./build-webos.sh`. *(Built in WSL Debian with buildroot-nc4 `webos-a38c582`; streaming with the new build still to be confirmed on the TV.)*
- [x] At startup, log a **capability dump** for every `/dev/input/event*` that passes `is_gamepad()`. Use `app_log_always`, tag `[PAD]`.
  - Name (`EVIOCGNAME`), bus/vendor/product/version (`EVIOCGID`).
  - Kernel driver: readlink `/sys/class/input/eventN/device/device/driver` → basename (`hid-generic`, `sony`, `playstation`, `microsoft`, `xpad`, …).
  - Supported `EV_KEY` codes, `EV_ABS` codes with min/max, and whether `EV_FF` is supported.
- [x] Add a `"input_debug": true` config flag that logs every raw `EV_KEY`/`EV_ABS` event (`type code value`).
- [x] **Check that hidraw is accessible** (yes: the app runs as uid 5131 in group `compositor`, which owns the nodes): list `/dev/hidraw*` and try `open(O_RDWR)` on each. Log `HIDIOCGRAWINFO` (bustype, vendor, product) and the result. Stages 4–8 depend on this.
- [~] Capture logs for every controller you own (DualSense BT done: `docs/controllers/dualsense-bt-webos6.md`) (DualSense USB + BT, DS4, Xbox BT, Xbox USB if available). Save them under `docs/controllers/` as reference.

**Done when:** for each controller you have a log of its driver, IDs, codes and ranges, and you know whether hidraw opens.

Tools added in this stage: `tools/paddiag` (SSH CLI: `info`, `events`, `hidraw`), `tools/deploy-tv.sh` (copy a build onto a rooted TV without touching `config.json`).

---

## Stage 1: Per-controller mapping profiles (fixes the reported bugs)

**Why:** a single hard-coded mapping cannot be right for different drivers.

**Design:** add `src/pad_profile.c/.h`:

```c
typedef struct {
    const char *name;
    uint32_t    key_map[KEY_MAX + 1 - BTN_MISC];  // evdev code → CHIAKI_CONTROLLER_BUTTON_* (0 = unmapped)
    int8_t      abs_role[ABS_CNT];                 // evdev axis → ROLE_LX, ROLE_LY, ROLE_RX, ROLE_RY, ROLE_L2, ROLE_R2, ROLE_HATX, ROLE_HATY, ROLE_NONE
    bool        chord_touchpad;                    // Select+Start → Touchpad (pads without a touchpad)
} PadProfile;
```

Choose the profile by (vendor, product, driver). If nothing matches, fall back to capability heuristics.

| Profile | Match | Buttons | Axes |
|---|---|---|---|
| `sony_generic` | VID 054c (DualSense 0ce6, Edge 0df2, DS4 05c4/09cc) **and** driver `hid-generic` (or `BTN_C` present) | table above | X/Y left, Z/RZ right, RX=L2, RY=R2 |
| `sony_kernel` | VID 054c, driver `sony`/`playstation` | SOUTH=Cross, EAST=Circle, **NORTH=Triangle, WEST=Square**, TL2/TR2 digital L2/R2, SELECT=Share, START=Options, MODE=PS | X/Y left, **RX/RY right**, **Z=L2, RZ=R2** |
| `xbox_bt_generic` | VID 045e, bus BT, has `ABS_GAS`/`ABS_BRAKE` | current mapping | X/Y left, Z/RZ right, BRAKE=L2, GAS=R2 |
| `xbox_xpad` | driver `xpad` (USB) | SOUTH=Cross, EAST=Circle, **0x133 (Xbox X)=Square, 0x134 (Xbox Y)=Triangle** | X/Y left, RX/RY right, **Z=L2, RZ=R2** |
| `generic` | fallback | `xbox_xpad` buttons | heuristics: GAS/BRAKE present → triggers there, else Z/RZ |

- [x] Implement the profile table and the selection logic. Log the chosen profile as `[PAD] profile=…`.
- [x] Rewrite `apply_key_event`/`apply_abs_event` to use the profile instead of `switch` statements. Keep the Select+Start chord only for profiles with `chord_touchpad`. On Sony pads, the real touchpad click button maps to `TOUCHPAD`.
- [x] Store the profile on each pad: `GamepadDev.profile`.
- [x] Triggers: if a profile has analog L2/R2, ignore the digital `BTN_TL2/TR2` press events. Otherwise map digital-only triggers to 0/255.
- [x] Hat/d-pad: also handle the DualSense hat on `ABS_HAT0X/Y` (hid-generic converts the hat switch to two axes; verify this in the Stage 0 logs).
- [x] Add a `"controller_profile": "auto"` config override (config + README; settings-screen entry deferred to Stage 10).
- [x] Fix the misleading comment at the top of `apply_abs_event` (GAS=R2, BRAKE=L2).
- [x] Tighten `is_gamepad()`: require a bound kernel driver. Stage 0 found that LG's virtual `"Smart Remote RCU Input"` and `"LGE Network Input"` nodes pass the current name filter and get grabbed.

**Done when** each controller passes this test matrix in the PS5 *Settings → Accessibility → Controllers → Custom button assignments* preview, or on a gamepad-test screen:
every face button, L1/R1, analog L2/R2 (full 0–255 travel), L3/R3, Create/Share, Options, PS, d-pad including diagonals, both sticks with no drift after trigger use, and touchpad click.
**Status (2026-11-16):** passed on the TV with a DualSense (BT, `sony_generic`) and an Xbox Series controller (BT, `xbox_bt`). `sony_kernel`, `xpad` and `generic` are untested (no such hardware/driver yet).

---

## Stage 2: Input robustness

**Why:** after Stage 1 the mappings are correct, but buttons can still get stuck, a controller that reconnects is never picked up again, and updates are sent too often.

- [x] **Per-frame updates**: collect changes and call `chiaki_session_set_controller_state` only on `EV_SYN/SYN_REPORT`, not on every event. Handle `SYN_DROPPED` by re-reading the full state (`EVIOCGKEY` + `EVIOCGABS`).
- [x] **Read in bulk**: `read()` an array of `input_event` per `select` wake-up instead of a single event.
- [x] **Per-pad state**: keep a `ChiakiControllerState` per pad and combine them with `chiaki_controller_state_or()` before sending. Clear a pad's state when it disconnects. Today a disconnect mid-press leaves the button held on the PS5.
- [x] **Hotplug**: watch `/dev/input` with `inotify` (`IN_CREATE`/`IN_ATTRIB`, rescan 500 ms after a change; periodic 3 s rescan if inotify is unavailable). Open, classify, grab and add new pads at runtime. Reuse slots freed by disconnects.
- [x] ~~Release the grab between reconnect attempts~~ **Decided: keep the grab for the whole app lifetime.** Releasing it would hand the pad back to webOS during reconnect waits (B → Back would quit the app). The state is re-sent in full when a new session starts (`input_set_session`).
- [x] Multi-controller policy: **merge** all pads into one player (unchanged behaviour); each pad keeps its own state, so releasing a button on one pad no longer cancels the other. Configurable selection is left for Stage 10.

**Done when:** you can power a controller off and on mid-stream and it resumes, no button stays stuck after a disconnect, and two pads don't fight each other.  **Status (2026-11-16):** passed on the TV — DualSense and Xbox reconnect mid-stream, no stuck input on disconnect, two pads together.

---

## Stage 3: Standard rumble (Xbox, DS4 on the kernel driver)

**Why:** the PS5 sends `CHIAKI_EVENT_RUMBLE`, and today `main.c` ignores it.
**Stage 1 finding:** the Xbox Series pad on webOS 6 is driven by hid-generic too and has **no** `EV_FF`, so on this TV Xbox rumble also needs a raw HID output report (Xbox BT report `0x03`: enable mask, left/right trigger motors, strong/weak motors, duration, delay, repeat). Evdev FF stays as the path for pads whose driver supports it.

- [ ] Open pads with `O_RDWR`; currently `O_RDONLY`, which blocks force-feedback writes.
- [ ] For pads with `EV_FF` + `FF_RUMBLE`: upload one `ff_effect` with `EVIOCSFF`, then update and play it by writing `EV_FF` events. Scale `left` → strong magnitude and `right` → weak magnitude (`u8 → u16`).
- [ ] Route `CHIAKI_EVENT_RUMBLE` from `session_event_cb`, which runs on a chiaki thread, to a new `input_set_rumble(ctx, left, right)`. Store the values and let the input thread apply them, with no blocking I/O in the callback.
- [ ] Stop rumble on session end and app exit.
- [ ] Add a `"rumble": true` config toggle.

**Done when:** an Xbox pad (BT/USB) and a DS4 (if it uses the `sony` driver) vibrate in a game, and the vibration stops on disconnect.

---

## Stage 4: DualSense raw-HID backend (input side)

**Why:** touchpad, motion and every DualSense output need the raw HID reports. hid-generic does not expose them.

**Design:** add `src/dualsense.c/.h`. For a DualSense, **hidraw becomes the source of truth for the whole controller state**. Keep the evdev node open and grabbed only so webOS doesn't react to the buttons, and ignore its events.

Reference implementations (read these, don't guess offsets):
- Linux `drivers/hid/hid-playstation.c`: `struct dualsense_input_report`, the calibration parsing, `struct dualsense_output_report_*`, the BT CRC.
- SDL `src/joystick/hidapi/SDL_hidapi_ps5.c`: BT enhanced-mode handling and the effects packet layout.

Tasks:
- [ ] Find the matching hidraw node: `/sys/class/hidraw/hidrawN/device` and `/sys/class/input/eventM/device/device` resolve to the same HID device, or compare `HIDIOCGRAWINFO` vendor/product.
- [ ] Detect USB or BT from `HIDIOCGRAWINFO.bustype`.
- [ ] Read the calibration feature report `0x05` (`HIDIOCGFEATURE`). On BT this also switches the pad from the short `0x01` report to the full `0x31` report. **Stage 0 finding:** on webOS 6 (BlueZ → uhid) the ioctl returns `EIO` but the switch to `0x31` still happens, so the calibration data must be fetched another way (retry, or fall back to hid-playstation's default calibration).
- [ ] Parse the input report: USB `0x01`, 64 bytes; BT `0x31`, 78 bytes, same payload shifted by 1. Fields: sticks, triggers, buttons + hat, gyro, accel, sensor timestamp, two touch points (active flag, id, 12-bit x/y).
- [ ] Fill `ChiakiControllerState` buttons, sticks and triggers from the report, with the same semantics as Stage 1.
- [ ] Fall back to the Stage 1 `sony_generic` evdev profile if hidraw can't be opened.
- [ ] Do the same for the DS4: USB `0x01`, BT `0x11`. Similar fields, different offsets, and the touchpad is 1920×942. This step is optional and can be its own sub-stage.

**Done when:** with `input_debug` on, the log shows correct buttons, sticks, triggers, raw gyro/accel and touch coordinates from hidraw over both USB and BT.

---

## Stage 5: Touchpad

- [ ] Map finger contacts to `chiaki_controller_state_start_touch`, `_set_touch_pos` and `_stop_touch`, tracking per-finger ids (two fingers max).
- [ ] Scale coordinates to the coordinate space chiaki expects. Check how chiaki-ng's GUI controller code scales DualSense touch (PS5 vs PS4 host) and copy that.
- [ ] Touchpad click → `CHIAKI_CONTROLLER_BUTTON_TOUCHPAD`. For non-touch pads, keep the chord/Share-button fallback.

**Done when:** swiping works in a game or menu that uses the touchpad (e.g. the PS5 home screen's touchpad scrolling, or Astro's Playroom), including two-finger touches and the click.

---

## Stage 6: Motion (gyro / accelerometer)

- [ ] Apply calibration from feature report `0x05` (bias and sensitivity, see `dualsense_get_calibration_data` in hid-playstation).
- [ ] Convert to the units chiaki-ng uses: gyro in rad/s, accel in g. Check chiaki-ng's SDL sensor path for the exact convention and axis order.
- [ ] Fill `gyro_*`, `accel_*` and `orient_*`. chiaki-ng has an orientation tracker in `lib/include/chiaki/orientation.h`; feed it gyro/accel + timestamp and apply it to the controller state. Confirm the API in your checkout.
- [ ] Handle `CHIAKI_EVENT_MOTION_RESET` by resetting the tracker.

**Done when:** gyro aiming works in a game with motion controls, without drift or inverted axes.

---

## Stage 7: DualSense mode + outputs (rumble, adaptive triggers, lightbar)

- [ ] When a DualSense is the active pad and `cfg.ps5`: set `info.enable_dualsense = true` (`main.c`, connect info). Make it a config toggle.
- [ ] Build the output report: USB `0x02` (48 bytes); BT `0x31` (78 bytes, with a sequence tag and a **CRC32 over seed byte `0xA2` + report**). Send it with `write()` on hidraw from the input thread, only when something changes, and rate-limited.
- [ ] **Rumble**: `CHIAKI_EVENT_RUMBLE` → motor bytes, using compatible-vibration flags.
- [ ] **Adaptive triggers**: `CHIAKI_EVENT_TRIGGER_EFFECTS` → `type_left` + `left[10]` / `type_right` + `right[10]` copied into the right/left trigger effect blocks, with the trigger-effect enable flags set.
- [ ] **Lightbar**: `CHIAKI_EVENT_LED_COLOR` → lightbar RGB. **Player LED**: `CHIAKI_EVENT_PLAYER_INDEX`.
- [ ] **Intensity**: `CHIAKI_EVENT_HAPTIC_INTENSITY` / `CHIAKI_EVENT_TRIGGER_INTENSITY` → reduction fields.
- [ ] **Reset all outputs** (triggers off, motors off) on session end, disconnect and app exit. Otherwise the triggers stay stiff.
- [ ] Config toggles: `"trigger_effects": true`, `"rumble": true` (shared with Stage 3).

**Done when:** in Astro's Playroom (free on PS5) the triggers resist, the pad rumbles, the lightbar follows the game, and everything resets after quitting. Works over USB and BT.

---

## Stage 8: Haptics → rumble emulation

**Why:** with `enable_dualsense`, PS5 games often send their vibration as a *haptics audio stream* instead of rumble events.

- [ ] Register a haptics sink with `chiaki_session_set_haptics_sink`. The callbacks get low-rate PCM.
- [ ] Turn the stream into motor values, as chiaki-ng does for controllers without HD haptics: an amplitude envelope per channel → left/right motor. Find and reuse the logic in the chiaki-ng GUI.
- [ ] Feed the result into the same output path as Stage 7 (DualSense) and Stage 3 (other pads).

**Done when:** games that only use haptics still produce vibration.

---

## Stage 9 (stretch): True HD haptics

Hard and uncertain on a TV. Attempt it only after Stages 0–8 are solid.

- **USB:** the DualSense is also a USB audio device. Haptics go to channels 3–4 of its 4-channel output. Requires `snd-usb-audio` on the TV kernel and ALSA access from the app. Check `/proc/asound/cards` with the pad plugged in.
- **BT:** haptic audio is sent inside HID output reports. See chiaki-ng's implementation and SDL's PS5 driver.

---

## Stage 10: Settings UI, docs, release

- [ ] Add a "Controller" section to the settings screen (`ui.c`): profile override, rumble, trigger effects, DualSense mode, input debug.
- [ ] Optional: a gamepad-test screen in the launcher that shows live buttons, axes, touches and motion. It removes the need for a PS5 to verify mappings.
- [ ] Update the README: supported-controller table, feature matrix (buttons / touch / motion / rumble / triggers / haptics per controller and connection type), troubleshooting with `[PAD]` log lines.
- [ ] Bump the version in `packaging/appinfo.json` and tag a release.

---

## Assumptions to verify early

| Assumption | Verified in | If false |
|---|---|---|
| DualSense uses `hid-generic` on your TV | Stage 0 log | Use the matching profile instead; mapping still table-driven |
| App can open `/dev/hidraw*` read/write | Stage 0 | Stages 4–8 for DualSense are blocked; look at running with elevated rights via Homebrew Channel, or settle for evdev-only |
| BT DualSense switches to full reports after feature report 0x05 | Stage 4 | Follow SDL's enhanced-mode sequence |
| Xbox BT pad advertises `EV_FF` on webOS | Stage 3 | Xbox rumble needs raw HID too |
