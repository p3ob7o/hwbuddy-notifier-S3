# hwbuddy-notifier-S3

A hardware buddy for [Claude Desktop](https://claude.ai/download) that runs on
the [M5Stack M5StickS3](https://shop.m5stack.com/products/m5sticks3-esp32s3-mini-iot-dev-kit).
Pairs with Claude Desktop's Hardware Buddy panel over Bluetooth LE and surfaces
permission prompts on the device — approve or deny with the buttons, shake to
dismiss, with an animated character and a soft chime.

Written from scratch against Anthropic's BLE protocol spec
([REFERENCE.md](https://github.com/anthropics/claude-desktop-buddy/blob/main/REFERENCE.md))
— Nordic UART Service plus newline-delimited JSON. **Not** a port of Anthropic's
[reference firmware](https://github.com/anthropics/claude-desktop-buddy), which
targets the older M5StickC Plus.

## Features

- **Sleeps when idle.** Screen off, BLE link active. Tap a button to peek at
  status (connection, message, token counts) for a few seconds.
- **Animated character on prompt.** A GIF plays alongside the tool name and a
  one-line hint when Claude Desktop asks for permission.
- **Pill selector UI.** Side button (B) cycles between Approve / Deny; front
  button (A) confirms.
- **Audible chime.** A short melody plays on prompt arrival — silenced if the
  device is face-up (you're already looking at it).
- **Shake to dismiss.** A vigorous shake while a prompt is active = Deny.
- **Encrypted BLE link.** LE Secure Connections with bonding. A 6-digit
  passkey appears on the device; the desktop prompts the user to type it.
  Transcript snippets that flow over the link are AES-CCM-encrypted.
- **Status reporting.** Battery, uptime, free heap, and approval stats are
  reported back via the `status` ack and shown in the Hardware Buddy panel.

https://github.com/user-attachments/assets/a288a85c-3d04-4cc3-89a9-1e13994b7970

## Hardware

- [M5Stack M5StickS3](https://shop.m5stack.com/products/m5sticks3-esp32s3-mini-iot-dev-kit)
  — ESP32-S3, 1.14" 135×240 IPS (ST7789), BMI270 IMU, AXP2101 PMIC, speaker.
- USB-C data cable.

## Build & flash

[PlatformIO Core](https://platformio.org/install/cli) required.

```bash
brew install platformio        # macOS; or: pip install platformio
pio run -t upload              # build + flash via USB
```

**First flash quirk:** the ESP32-S3's native USB doesn't always auto-enter
download mode. If you get `Could not configure port: Device not configured`,
hold the front (M5) button and briefly press the side power button — the
screen goes dark, that's download mode. Re-run upload. Subsequent flashes
auto-reset cleanly.

## Pairing

1. **Claude Desktop** → **Help → Troubleshooting → Enable Developer Mode**.
<img width="345" height="432" alt="Screenshot 2026-05-12 at 00 08 37" src="https://github.com/user-attachments/assets/66c9dd4b-9581-4e76-9a2e-fdf49c6e5e3b" />

2. **Developer → Open Hardware Buddy…**
3. Click **Connect**, pick `Claude-XXXX` from the list.
4. The M5 shows a 6-digit passkey. macOS asks you to type it on the desktop.
5. Done. The bond is persisted; reconnects are silent.

https://github.com/user-attachments/assets/d5e79edf-de0d-4a65-99d1-d1dde6848541

If you reflash with a different security setup, also Forget the device in
macOS System Settings → Bluetooth so the stale LTK is cleared.

## Adding your own character + melody

This repo ships **without** a character GIF or a recognisable melody — you
provide both.

### Character

```bash
pip install Pillow
python tools/encode_gif.py assets/your-char.gif > src/character_gif.h
pio run -t upload
```

`tools/encode_gif.py` resizes to 135×135, alpha-composites onto black (so
transparent-bg art renders cleanly), and quantizes all frames against a single
shared global palette (per-frame local palettes cause visible hue shifts with
the embedded GIF decoder). Best results from a transparent-background GIF.

### Melody

`src/melody.h` is a flat list of `{frequency_Hz, duration_ms}` entries. Rests
are `frequency = 0`. Edit by hand. The shipped default is a three-note arpeggio.

## Protocol

The wire format is fully documented at
<https://github.com/anthropics/claude-desktop-buddy/blob/main/REFERENCE.md>.

Implemented in this firmware:

- Heartbeat snapshot (parsed for `msg`, counts, `tokens`, `prompt`)
- `{"cmd":"status"}` → ack with `bat`, `sys`, `stats`, `sec: true`
- `{"cmd":"name"}` / `{"cmd":"owner"}` — persist + ack
- `{"cmd":"unpair"}` — erase BLE bonds + ack
- `{"cmd":"permission","id":...,"decision":"once|deny"}` — sent on confirm
- `{"time": [epoch, tz_offset]}` — sets system clock

Folder push is **not** implemented.

## License

GPL-3.0-or-later — see [LICENSE](LICENSE). GPL-3.0 (not GPL-2.0) because two
of the runtime dependencies (NimBLE-Arduino and AnimatedGIF) are Apache 2.0,
which the FSF considers incompatible with GPL-2.0.

## Credits

- Protocol and reference firmware from
  [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy).
- Libraries: [M5Unified](https://github.com/m5stack/M5Unified) (MIT),
  [M5GFX](https://github.com/m5stack/M5GFX) (MIT),
  [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) (Apache-2.0),
  [ArduinoJson](https://github.com/bblanchon/ArduinoJson) (MIT),
  [AnimatedGIF](https://github.com/bitbank2/AnimatedGIF) (Apache-2.0).
