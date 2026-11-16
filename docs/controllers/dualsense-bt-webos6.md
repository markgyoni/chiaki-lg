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
