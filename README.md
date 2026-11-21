<p align="center">
  <img src="docs/branding/icon-160.png" width="120" alt="Chiaki-webOS">
</p>

<h1 align="center">Chiaki-webOS</h1>

<p align="center">Play your PS5 or PS4 on your LG TV, with no PC or extra box in between.</p>

---

Chiaki-webOS is a Remote Play app for LG webOS TVs. It streams your PlayStation to the TV over your home network, and you play with your controller as if the console were plugged in.

## What you get

- **PS5 and PS4**, up to 1080p at 60 fps, with HDR
- **Your DualSense the way it should be**: touchpad, motion aiming, vibration, adaptive triggers and the lightbar
- **DualShock 4 and Xbox controllers** work too
- **Wakes your console** from rest mode, and can put it back to rest when you stop playing
- **Several consoles and several PSN accounts** on one TV
- **Easy setup on the TV**: sign in with your phone by scanning a QR code, then enter the PIN shown on the console
- Works with the **TV remote, the Magic Remote pointer or a controller**

## What you need

- An LG TV with **webOS 4 or newer**, in Developer Mode or rooted with the [Homebrew Channel](https://www.webosbrew.org/)
- A **PS5 or PS4** with Remote Play turned on, on the same home network as the TV
- A **controller** connected to the TV (Bluetooth or USB)

A wired network connection for the console (and ideally the TV) gives the smoothest picture.

## Install

Download the `.ipk` file from the [latest release](../../releases/latest) and install it with [webOS Dev Manager](https://github.com/webosbrew/dev-manager-desktop).

## Get started

1. Turn your console fully on (not in rest mode).
2. Open **Chiaki-webOS** and select **Add your console**. Your console shows up in the list.
3. Scan the QR code with your phone and sign in with the PSN account you use on the console. You end up on a page that may look empty: type the short code from its address into the TV.
4. On the console, open **Settings › System › Remote Play › Link Device** (PS4: **Settings › Remote Play Connection Settings › Add Device**) and type the PIN into the TV.
5. Select your console and play.

To wake the PS5 from rest mode, turn on **Settings › System › Power Saving › Features Available in Rest Mode › Enable Turning On PS5 from Network**.

## While playing

| | |
|---|---|
| **Back** or **Down** on the remote | Leave the game (asks first) |
| **Up** on the remote, or tap **Mute** on the DualSense | Show or hide the connection info |
| Hold **Mute** on the DualSense for 3 seconds | Back to the menu |

In the menus, **Back** on the remote (or **Circle**) always takes you one step back.

## DualSense over Bluetooth

Over USB the DualSense's vibration, adaptive triggers and lightbar work on any TV. Over Bluetooth they don't out of the box: the TV's Bluetooth system delivers those commands to Xbox controllers the way they need them, but sends them to every other controller in a form the DualSense quietly ignores.

On a **rooted** TV, Chiaki-webOS fixes this with its **Bluetooth fix** (App settings › Controllers, on by default). It tweaks that one decision in the TV's running Bluetooth service, so controllers named "Wireless Controller" (DualSense, DualShock 4, Xbox) are treated alike. Nothing on the TV's storage is changed: restarting the TV undoes it, and the app applies it again automatically.

The fix only touches the TV if it finds exactly the code it expects; on any other Bluetooth system it does nothing. If it doesn't work on your TV, [open an issue](../../issues) with your TV model and webOS version.

## Tips

- **Picture stutters?** Open the console's **Settings** in the app and lower the bitrate, or use a wired connection.
- **Vibration, triggers or lightbar not working over Bluetooth?** See [DualSense over Bluetooth](#dualsense-over-bluetooth) above.
- **Settings not kept right after installing?** On a TV without root, restart the TV once after the first install.
- **Coming from chiaki-lg?** Install Chiaki-webOS and open it once: your consoles and settings are copied over. Then remove the old app.

Something not working? [Open an issue](../../issues) with your TV model and webOS version.

## Credits

Chiaki-webOS is built on [chiaki-ng](https://github.com/streetpea/chiaki-ng) and the original [Chiaki](https://git.sr.ht/~thestr4ng3r/chiaki). It started from [chiaki-lg](https://github.com/TacticalBill/chiaki-lg) by TacticalBill and uses [SS4S](https://github.com/mariotaku/ss4s) from moonlight-tv for picture and sound. Thanks to the [webOS Homebrew](https://www.webosbrew.org/) community.

## License

[GNU AGPL v3](LICENSE), the same as chiaki-ng. To build it yourself, see [docs/BUILDING.md](docs/BUILDING.md).

Chiaki-webOS is not affiliated with Sony Interactive Entertainment or LG Electronics. PlayStation, PS4, PS5 and DualSense are trademarks of Sony Interactive Entertainment.
