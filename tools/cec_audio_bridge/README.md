# cec_audio_bridge

This tool turns a Raspberry Pi connected to your TV or projector over HDMI into the **HDMI CEC
Audio System**. The TV, or a playback device such as an Apple TV, then sends its volume and
mute keys to the Pi. The Pi forwards them to a Home Assistant `media_player`. With espgensam
that means the TV remote controls the Genelec monitors.

It uses the Linux kernel's CEC interface directly; libcec is not needed.

## What it does

- It claims CEC logical address 5 (Audio System) and turns on *System Audio Mode*, so the TV
  mutes its own speakers and hands volume control to the Pi.
- **Volume Up/Down** changes the media player's volume by a fixed step (1 dB by default on
  espgensam). Holding the key keeps stepping.
- **Mute** toggles mute.
- **Absolute volume** (CEC 2.0 *Set Audio Volume Level*) sets the volume directly.
- The TV's volume display is kept in sync, including changes made from Home Assistant, the dial
  or GLM.
- Optionally, with `--follow-power`, the speakers go to standby with the TV and wake when it
  turns on.

The Pi never switches the TV's input.

## Requirements

- A Raspberry Pi running Raspberry Pi OS Trixie, connected to an HDMI input of the TV or
  projector. Any HDMI port on the CEC bus works. On a Pi 4 or 5, use the port next to the
  USB-C power connector (HDMI0, `/dev/cec0`).
- HDMI CEC enabled on the TV and on the playback devices.
- No other Audio System, such as a soundbar or AV receiver, on the same HDMI bus.
- Home Assistant with the media player you want to control, for example espgensam's
  `media_player.genelec_sam_system`.

## Installation

1. Install the dependencies and check that the CEC device exists:

   ```bash
   sudo apt install python3-websockets v4l-utils
   ls -l /dev/cec*        # expect /dev/cec0, owned by group 'video'
   ```

2. Copy this directory to the Pi:

   ```bash
   sudo cp -r tools/cec_audio_bridge /opt/cec_audio_bridge
   ```

3. In Home Assistant, open your profile, go to **Security** and create a **long-lived access
   token**. Store it on the Pi so that only root can read it:

   ```bash
   sudo install -d -m 700 /etc/cec-audio-bridge
   sudo sh -c 'cat > /etc/cec-audio-bridge/token' # paste the token, then Ctrl-D
   sudo chmod 600 /etc/cec-audio-bridge/token
   ```

4. Try it in the foreground first. Your user must be in the `video` group; root is not needed:

   ```bash
   HA_TOKEN="$(sudo cat /etc/cec-audio-bridge/token)" \
       python3 /opt/cec_audio_bridge/cec_audio_bridge.py \
       --ha-url ws://homeassistant.local:8123/api/websocket \
       --entity media_player.genelec_sam_system -v
   ```

   Press volume up on the TV remote. You should see `User Control Pressed` in the log and
   the volume change in Home Assistant.

5. Install it as a service:

   ```bash
   sudo cp /opt/cec_audio_bridge/cec-audio-bridge.default /etc/default/cec-audio-bridge
   sudo nano /etc/default/cec-audio-bridge        # set your URL and entity
   sudo cp /opt/cec_audio_bridge/cec-audio-bridge.service /etc/systemd/system/
   sudo systemctl daemon-reload
   sudo systemctl enable --now cec-audio-bridge
   journalctl -u cec-audio-bridge -f
   ```

## TV and player settings

- **Epson projectors**: enable **HDMI Link**, then set **Audio Out Device** to the AV
  system rather than the projector. Menu names vary by model.
- **Other TVs**: look for the CEC brand name (Anynet+, SimpLink, Bravia Sync, …) and an option
  to output sound through the "receiver", "audio system" or "HDMI ARC/CEC" device.
- **Apple TV**: in **Settings → Remotes and Devices**, turn on **Control TVs and Receivers**
  and set **Volume Control** to **Auto**, or to the CEC option if one is listed.

## Options

| Option | Default | |
|---|---|---|
| `--device` | `/dev/cec0` | CEC device |
| `--ha-url` | `ws://homeassistant.local:8123/api/websocket` | Home Assistant WebSocket URL; use `wss://` for HTTPS |
| `--token-file` | `HA_TOKEN` environment variable | File holding the access token |
| `--entity` | `media_player.genelec_sam_system` | Media player to control |
| `--step` | `0.02` | Volume change per key press, as a fraction of the slider. On espgensam's default -80…-30 dB range, 0.02 is 1 dB |
| `--osd-name` | `Genelec` | Name the TV shows for the device |
| `--cec-version` | `2.0` | `1.4` for older TVs that misbehave with 2.0; this disables absolute volume |
| `--follow-power` | off | Speakers follow TV standby |
| `--no-initiate-sam` | off | Wait for the TV to request System Audio Mode instead of announcing it |
| `-v` | off | Log every CEC message |

## Troubleshooting

- **Watching the bus**: `sudo cec-ctl -d0 -M` shows every message to and from the Pi while the
  bridge runs. Monitoring needs root; the bridge itself does not.
- **"logical address 5 is already taken"**: another device on the bus claims to be the Audio
  System. Turn its CEC off, or remove it from the chain.
- **"No physical address yet"**: the Pi does not see the display. Some projectors stop
  answering on HDMI in standby. The bridge waits and takes over again when the display returns.
- **The TV still uses its own speaker**: check the TV's audio-output setting, then look in
  `-v` output for `System Audio Mode Request` from address 0. Some TVs only ask once at power
  on.
- **Volume steps are too big or too small**: adjust `--step`.
