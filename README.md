# Firmware-HRMS

ESP32 firmware for the fingerprint attendance devices, with OTA updates delivered as
GitHub Releases from this (private) repo.

## Layout

```
arduinoCode/
  arduinoCode.ino       -- the firmware
  secrets.h.example      -- template for the local-only GITHUB_PAT
.github/workflows/
  pr-title-version.yml   -- checks every PR into main has a [vX.Y] tag in its title
  tag-on-merge.yml       -- tags main with that version when the PR merges
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

`main` is protected -- no direct pushes, everything goes through a PR. Give the PR
a title that includes the version to ship, in `[vX.Y]` form, e.g.:

```
[v0.2] Fix rotary encoder debounce
```

`pr-title-version.yml` blocks any PR into `main` that's missing that tag. Once the
PR is merged, `tag-on-merge.yml` pulls the version back out of the title, tags
`main` with it, and pushes the tag -- which triggers
`.github/workflows/release-firmware.yml`:
1. Stamps `FIRMWARE_VERSION` in `arduinoCode.ino` to match the tag.
2. Writes `secrets.h` from the `DEVICE_OTA_PAT` secret.
3. Compiles with `arduino-cli` for FQBN `esp32:esp32:esp32` (change this in the
   workflow if your hardware uses a different ESP32 board/variant).
4. Publishes a GitHub Release tagged with that version, with the compiled binary
   attached as `firmware.bin`.

You can still cut a one-off release by tagging manually (`git tag vX.Y && git push
origin vX.Y`) -- tag pushes aren't blocked by the branch protection on `main`, only
direct commits to the branch are.

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
