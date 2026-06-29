# Copilot Instructions — mitsubishi-heatpump

## Conversation Preferences

- **Do not use popup selection dialogs.** Ask questions directly in the chat and
  wait for a response. Inline conversation preferred.
- Be direct and concise. Skip preamble like "I'll now…" — just do the work.
- When multiple approaches exist, pick the best one and proceed. Only ask if the
  choice has significant irreversible consequences.
- **Always work on a feature branch** — never commit directly to `main`. The user
  merges via PR.
- Use **conventional-commit** messages (`feat:`, `fix:`, `docs:`, `ci:`,
  `refactor:`, `test:`, `chore:`); the release workflow builds changelogs from
  them. Scoped variants are fine: `feat(cn105):`, `fix(mqtt):`.
- When asked for a PR description, output it in **Markdown**.

## Project Overview

ESP-IDF firmware for an **M5Stack Stamp-S3Bat (ESP32-S3)** that controls a
**Mitsubishi Electric** indoor unit over its **CN105** serial port and bridges it
to **Home Assistant over MQTT**. Successor to the Arduino/ESP8266
`gysmo38/mitsubishi2MQTT`; preserves that project's MQTT topic contract.

- **Framework**: ESP-IDF v5.4.3 (CI), `>=5.3` accepted
- **Target**: ESP32-S3 (`idf.py set-target esp32s3`)
- **Language**: C++ (`app_main` is `extern "C"`)
- **Transport**: esp-mqtt; small local web UI is for provisioning/diagnostics only

## Repository Structure

| Path | Purpose |
|------|---------|
| `main/` | entry point (`main.cpp`), `wifi_manager`, `Kconfig.projbuild` |
| `components/cn105/` | Mitsubishi CN105 serial protocol (port of SwiCago/HeatPump) |
| `components/m5pm1/` | PY32 "M5PM1" PMIC I²C driver (0x6E) + charge governor |
| `components/hvac_mqtt/` | MQTT bridge + Home Assistant discovery |
| `.github/workflows/` | `build.yml`, `create-release.yml` |

## Hardware Cheat Sheet

- **PMIC = PY32 "M5PM1"** at I²C `0x6E`. Gates the LGS4056 charger via `CHG_EN`
  (PWR_CFG reg `0x06` bit0). Charge **current** is a hardware preset (200/650 mA),
  **not** I²C-settable — the firmware lever is on/off only. `CHG_EN` auto-clears
  on PMIC reset, so re-assert after boot (done in `init_pmic()`).
- Internal I²C: **SDA=GPIO48, SCL=GPIO47** (per S015 schematic).
- The battery is a **buffer** for CN105 power blips / WiFi TX spikes, not a
  runtime source. Keep the charge governor gating on VIN.
- CN105 is **2400 baud, 8-E-1**, `0xFC`-framed.

## Code Conventions

- ESP-IDF patterns: `ESP_LOGI/W/E`, `esp_err_t` returns, check error codes.
- **Printf**: don't use `%lld`/`%llu` on the Xtensa toolchain — cast to
  `(long)`/`(unsigned long)` and use `%ld`/`%lu`.
- Namespaces: `cn105::`, `m5pm1::`, `hvac_mqtt::`, `wifi::`.
- Keep `sdkconfig.defaults` the source of truth for board config; never commit
  `sdkconfig`, `build/`, or `managed_components/`.
- Secrets/creds live in `sdkconfig.defaults.local` (gitignored) or NVS — never
  hardcode them in committed source.

## Porting Backlog (the stubs)

- `components/cn105/cn105.cpp` — packet engine (`TODO(port)`): connect handshake,
  frame assembly + checksum, decode get/status packets, transmit set packets,
  remote-temperature. Reference: SwiCago/HeatPump.
- `components/hvac_mqtt/hvac_mqtt.cpp` — JSON for `publish_state`,
  `publish_settings`, `publish_discovery` (`TODO(port)`), matching mitsubishi2MQTT.
- `main/wifi_manager.cpp` — captive-portal page for provisioning (currently a bare
  SoftAP). Port arctic-sniffer's `dns_server` + portal HTML.
- `components/m5pm1/m5pm1.cpp` — `TODO(verify)` the ADC scaling + BATT_LVP encoding
  against hardware.

## After Changes — Checklist

- [ ] `idf.py build` passes (CI runs the same in `espressif/idf:v5.4.3`).
- [ ] Update `README.md` if behavior/architecture/MQTT contract changed.
- [ ] Bump `PROJECT_VER` in `CMakeLists.txt` for a release.
- [ ] Conventional commit messages; one logical change per commit.
- [ ] Don't break the MQTT topic contract without intent — HA entities depend on it.
