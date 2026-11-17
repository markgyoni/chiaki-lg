# Xbox Wireless Controller (Series X|S) over Bluetooth — LG OLED55G13LA, webOS 6.5.3

Captured 2026-11-16 from the app's startup dump (`/tmp/chiaki.log`).

```
"Xbox Wireless Controller" bus=0x05 vid=045e pid=0b20 ver=0101 driver=hid-generic
keys: 0x130..0x13e (BTN_SOUTH .. BTN_THUMBR, 15 buttons incl. unused gap codes BTN_C/BTN_Z/BTN_TL2/BTN_TR2)
abs 0x00 ABS_X      0..65535 flat=4095 fuzz=255
abs 0x01 ABS_Y      0..65535
abs 0x02 ABS_Z      0..65535   right stick X
abs 0x05 ABS_RZ     0..65535   right stick Y
abs 0x09 ABS_GAS    0..1023    RT
abs 0x0a ABS_BRAKE  0..1023    LT
abs 0x10 ABS_HAT0X  -1..1
abs 0x11 ABS_HAT0Y  -1..1
ff: not supported
hidraw: /dev/hidraw2 bus=BT vid=045e pid=0b20 rdesc=283, opens O_RDWR from the app
```

Pressed in the session: A=`BTN_SOUTH`, B=`BTN_EAST`, X=`BTN_NORTH`, Y=`BTN_WEST`, LB/RB=`BTN_TL/TR`,
View=`BTN_SELECT`, Menu=`BTN_START`, Xbox=`BTN_MODE`, LS/RS=`BTN_THUMBL/R`.
Profile `xbox_bt` was selected automatically and the user confirmed all buttons correct.

No kernel force feedback (hid-generic), so rumble needs a raw HID output report
(Xbox BT rumble report `0x03`) — Stage 3.
