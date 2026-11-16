# DualSense over Bluetooth — LG OLED55G13LA, webOS 6.5.3

Captured 2026-11-16 with `paddiag` as root over SSH.

## System

- webOS 6.5.3 (`kisscurl-koli`), firmware 03.53.45, SoC o20n
- Kernel `4.4.84-229.kcl4tv.6` (aarch64 kernel, 32-bit ARM userland)
- HID drivers available: `hid-generic`, `sony`, `microsoft`, `logitech`, … (no `hid-playstation`)
- Bluetooth HID goes through BlueZ → `uhid` (`/sys/devices/virtual/misc/uhid/0005:054C:0CE6.0001`)

## evdev (`/dev/input/event12`)

```
"DualSense Wireless Controller" bus=0x05 vid=054c pid=0ce6 ver=0100 driver=hid-generic
keys: 0x130(BTN_SOUTH) 0x131(BTN_EAST) 0x132(BTN_C) 0x133(BTN_NORTH) 0x134(BTN_WEST) 0x135(BTN_Z)
      0x136(BTN_TL) 0x137(BTN_TR) 0x138(BTN_TL2) 0x139(BTN_TR2) 0x13a(BTN_SELECT) 0x13b(BTN_START)
      0x13c(BTN_MODE) 0x13d(BTN_THUMBL)
abs 0x00 ABS_X      0..255 flat=15
abs 0x01 ABS_Y      0..255 flat=15
abs 0x02 ABS_Z      0..255 flat=15
abs 0x03 ABS_RX     0..255 flat=15   (rest value 0 -> trigger)
abs 0x04 ABS_RY     0..255 flat=15   (rest value 0 -> trigger)
abs 0x05 ABS_RZ     0..255 flat=15
abs 0x10 ABS_HAT0X  -1..1
abs 0x11 ABS_HAT0Y  -1..1
ff: not supported
```

14 buttons (HID buttons 1–14; mute is not exposed). This matches the hid-generic
mapping table in `ROADMAP.md`.

## hidraw (`/dev/hidraw0`)

- `bus=BT vid=054c pid=0ce6`, report descriptor 320 bytes, opens `O_RDWR` as root.
- Default: simple report `0x01`, 10 bytes, ~133 Hz:
  `01 82 7d 82 7d 08 00 08 00 00` = id, LX, LY, RX, RY, buttons0 (hat=8 neutral), buttons1, counter|buttons2, L2, R2
- `HIDIOCGFEATURE(0x05)` (calibration) **returns EIO** through uhid, but the request still
  reaches the pad: it switches to full report `0x31`, 78 bytes, ~195 Hz:
  `31 91 82 7d 82 7d 00 00 01 08 00 00 00 77 db f7 e8 02 00 19 00 fa ff 0a 00 8d 1f 80 05 ...`
  (same payload layout as hid-playstation's `dualsense_input_report`, shifted by one byte).
- After the switch, hid-generic no longer produces evdev button events (expected) until
  the controller reconnects.

## Other gamepad-looking nodes

LG virtual devices `"Smart Remote RCU Input"` (event8) and `"LGE Network Input"` (event9)
have no driver, advertise every key/axis with bogus ranges (min=32768 max=32767) and pass
the current `is_gamepad()` filter (it only rejects names starting with `LGE`/`LG `), so the
app grabs them too. To fix in Stage 1/2: require a bound HID/USB driver.

## In-app session (input_debug), 2026-11-16

- The app runs as `uid=5131 gid=5000 groups=15,19,29,44,505,509,511,777`. Group 505 is
  `compositor`, which owns `/dev/input/event*` and `/dev/hidraw*`, so **the app can open
  hidraw read/write** (`/dev/hidraw0 ... open(O_RDWR)=OK`).
- A second hidraw node appears for the LG Magic Remote (`vid=000f pid=3412 "LGE MR21N"`);
  filter by Sony VID/PID.
- Raw key events matched the hid-generic table: Square→`BTN_SOUTH`, Cross→`BTN_EAST`,
  Circle→`BTN_C`, Triangle→`BTN_NORTH`, L1→`BTN_WEST`, R1→`BTN_Z`, L2→`BTN_TL` (+`ABS_RX`),
  R2→`BTN_TR` (+`ABS_RY`), Create→`BTN_TL2`, Options→`BTN_TR2`, L3→`BTN_SELECT`,
  R3→`BTN_START`, touchpad click→`BTN_THUMBL`, d-pad→`ABS_HAT0X/Y`.
- No `BTN_MODE` (PS) event was logged — to be rechecked.
- The app also opened and grabbed `"Smart Remote RCU Input"` (event8); it produced no events.

## Output reports over Bluetooth (Stage 7, 2026-11-16)

webOS 6 does **not** use BlueZ for Bluetooth HID. `webos-bluetooth-service` embeds MTK's
Bluedroid (`/usr/lib/libbluetooth.default.so`) and owns `/dev/uhid`. A `write()` to the
DualSense hidraw node becomes `UHID_OUTPUT` → `btif_hh_setreport(OUTPUT)`:

- Default: **SET_REPORT on the control channel** (`52 31 …`). The DualSense answers the
  handshake with SUCCESS and then ignores the report, whatever the CRC seed (`A2`, `52`, none).
  Unknown report IDs (`0x02`) get `ERR_INVALID_REPORT_ID`, so the pad does parse them.
- LG special case: if the device's stored name starts with **"Xbox Wireless Controller"**,
  the report goes out as **DATA on the interrupt channel** (`A2 31 …`, `BTA_HhSendData`).
  That is the form the DualSense acts on (and how Linux/Windows send it).
- Luna `com.webos.service.bluetooth2/hid/internal/sendData` (root) also uses the interrupt
  channel, but with header `0xA3` (DATA/feature); the pad ignores that too.
- Renaming the pad in `/mnt/lg/cmn_data/btsvc/bluedroid-mtk/rec/bluedroid/bt_config.conf`
  does not stick: the name is refreshed from the device on every connection.
- Two SET_REPORTs in flight at once (the second sent before the first's handshake) **stop
  the pad's input reports until it is power-cycled**. Single writes survived 25/s.

Fix: `tools/btfix` (run as root; the app uses the Homebrew Channel's `exec`) patches
`btif_hh_setreport` in the running process via `/proc/<pid>/mem`:
`strncmp(name, "Xbox Wireless Controller", 24)` → `strstr(name, "Wireless Controller")`
(three halfwords at lib offsets `0x7fe96`, `0x7fe9e`, `0x7fea4`). It verifies a CRC32 of the
whole 356-byte function first (build: webOS 6.5.3-47, firmware 03.53.45), writes in an order
whose intermediate states all fall back to SET_REPORT, and reads back / rolls back. Nothing on
disk changes; restarting the Bluetooth service or rebooting restores the original.
`btfix --check` / `--undo` are available over SSH.

Useful TV logs for this: `/var/log/bt_snoop_hci.cfa_MTK` (btsnoop, UTC timestamps, rotates at
~2 MB) and `/var/log/bt_stack.log_MTK`. The hidraw `HID_UNIQ` address is byte-reversed.

The pad's microphone and speaker are not usable over BT (proprietary audio-in-HID reports);
see ROADMAP Stage 11 for the USB audio plan.
