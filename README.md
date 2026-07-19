# Firmware-HRMS

ESP32 firmware for the fingerprint attendance devices, with OTA updates delivered as
GitHub Releases from this (private) repo.

## Layout

```
arduinoCode/
  arduinoCode.ino       -- the firmware
  secrets.h.example      -- template for the local-only GITHUB_PAT
.github/workflows/
  release-firmware.yml   -- builds + publishes a release on every version tag
```

## One-time setup

1. **Create the device OTA token.** GitHub -> Settings -> Developer settings ->
   Fine-grained personal access tokens -> generate one scoped to **only this repo**,
   with repository permission **Contents: Read-only**. Nothing else. This token gets
   baked into every device's firmware binary (see the security note below), so keep
   its scope as narrow as possible.

2. **Add it as a repo secret.** In Firmware-HRMS -> Settings -> Secrets and variables
   -> Actions, add a secret named `DEVICE_OTA_PAT` with that token's value. CI writes
   it into `secrets.h` at build time so it ends up in every compiled release --
   that's the only way OTA can keep working across updates.

3. **Local builds** (for testing before you tag a release): copy
   `arduinoCode/secrets.h.example` to `arduinoCode/secrets.h` and put a real token in
   it. `secrets.h` is gitignored -- never commit it.

## Cutting a release

```
git tag v1.0.1
git push origin v1.0.1
```

Pushing a tag matching `v*.*.*` triggers `.github/workflows/release-firmware.yml`,
which:
1. Stamps `FIRMWARE_VERSION` in `arduinoCode.ino` to match the tag (so you don't have
   to hand-edit it before tagging).
2. Writes `secrets.h` from the `DEVICE_OTA_PAT` secret.
3. Compiles with `arduino-cli` for FQBN `esp32:esp32:esp32` (change this in the
   workflow if your hardware uses a different ESP32 board/variant).
4. Publishes a GitHub Release tagged with that version, with the compiled binary
   attached as `firmware.bin`.

## How devices update

Devices do **not** poll automatically -- selecting **"Check Update"** on the device
menu (rotary encoder push) triggers `checkForOTAUpdate()`, which:
1. GETs `/repos/Sri-kanth-J/Firmware-HRMS/releases/latest` with the baked-in PAT.
2. Compares the release tag to the device's compiled-in `FIRMWARE_VERSION`.
3. If different, downloads `firmware.bin` and flashes it via the ESP32 `Update`
   library, showing progress on the TFT, then reboots.

## Security note

This repo is private, so every device carries the same long-lived `GITHUB_PAT` to
read it -- a real tradeoff, accepted here for simplicity. That token is:
- Scoped to **read-only Contents on this repo only** (see setup step 1) -- it can't
  read any other repo or do anything but download release assets from this one.
- Embedded in every shipped binary, so a physically dumped/decompiled device exposes
  it. **Rotate `DEVICE_OTA_PAT` (and re-release) if a device is ever lost, stolen, or
  opened up**, since there's no per-device revocation -- it's one shared token.

## Hardware reference

- TFT: ST7789 SPI, CS=5 DC=2 RST=4 (hardware VSPI SCK=18 MOSI=23 MISO=19)
- R503 fingerprint sensor: UART2, RX=16 TX=17
- Rotary encoder: CLK=25 DT=26 SW=27 (menu nav + select)
- Buzzer: GPIO32

## Required Arduino libraries (for local IDE builds)

Install via Library Manager or as ZIP: `WiFiManager` (tzapu), `FPM` (brianrho),
`DIYables TFT SPI` (pulls in `Adafruit GFX Library`), `ArduinoJson` (v6.x --
this code targets 6.21.6, not v7's API).
