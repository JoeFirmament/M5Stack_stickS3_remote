# StickS3 Remote — To-do

## Next

- [ ] Add a dedicated `SONY A7M4` profile without replacing `CAMERA` (iPhone).
  - Emulate the Sony RMT-P1BT Bluetooth remote protocol; this is not BLE HID.
  - Switch the ESP32-S3 Bluetooth stack to central/client mode when this profile starts.
  - Scan, pair, bond, reconnect, and show `SEARCHING / PAIRING / CONNECTED` on screen.
  - First milestone: AF-ON, still-image shutter, and movie REC start/stop.
  - Second milestone: C1, manual focus +/- and power-zoom +/-.
  - Reboot cleanly when entering/leaving this profile so Mac/iPhone HID connections do not interfere.

## Backlog

- [ ] Give Mac, iPhone, and other HID host profiles separate Bluetooth identities to prevent the wrong host from auto-reconnecting.
- [ ] Keep the Reader profile available; CrossMux currently lacks stable BLE HID host support.
- [ ] Implement the existing IR profile with StickS3's built-in IR transmitter and receiver.
  - Learn and store named commands for TV, air conditioner, amplifier, and other appliances.
  - Disable the speaker amplifier while receiving, use the ESP32 RMT peripheral, and guide the user to keep the remotes at least 30 cm apart during learning.

## Completed

- [x] ESP-IDF BLE keyboard and consumer-control reports work with macOS and iPhone.
- [x] iPhone camera shutter via volume key.
- [x] Joystick2 at I2C `0x63` on GPIO9/GPIO10, including four directions, click, and hold.
