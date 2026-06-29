# mitsubishi-heatpump

ESP-IDF firmware for an **M5Stack Stamp-S3Bat** (ESP32-S3) that controls a
**Mitsubishi Electric** indoor unit over its **CN105** serial port and bridges
it to **Home Assistant over MQTT**.

It is the ground-up ESP-IDF successor to
[`mitsubishi2MQTT`](https://github.com/gysmo38/mitsubishi2MQTT) (Arduino/ESP8266)
and preserves that project's MQTT topic contract, so existing Home Assistant
entities keep working.

> **Status: scaffold.** The architecture, threading, WiFi (incl. captive-portal
> provisioning + mDNS), the on-device web UI / REST API, MQTT client wiring, and
> the PMIC I²C driver are in place and build. The CN105 packet engine and the
> MQTT JSON payloads are marked `TODO(port)` stubs — fill them in once hardware
> is on the bench. CI builds green today.

## Hardware

| Part | Role |
|------|------|
| ESP32-S3-PICO-1 (8MB flash) | application MCU |
| PY32 "M5PM1" PMIC (I²C `0x6E`) | rails, Li-ion charge gate (`CHG_EN`), VBAT/VIN telemetry |
| LGS4056HDA | Li-ion charger (200 mA float / 650 mA preset, set in HW) |
| 400 mAh LiPo (Adafruit 3898) | buffer for CN105 power blips + WiFi TX spikes |

The CN105 5V rail is current-limited, so the firmware runs a **closed-loop charge
governor** (`m5pm1::PMIC::governor_tick`) that gates `CHG_EN` on the VIN reading
to keep input draw within budget. See `components/m5pm1/`.

### Default pin map (override in `menuconfig`)

| Function | GPIO |
|----------|------|
| PMIC I²C SDA / SCL | 48 / 47 (internal bus, per S015 schematic) |
| CN105 UART TX / RX | 1 / 2 (**confirm against your harness**) |

## Architecture

```
              app_main
                 │
   ┌─────────────┼───────────────┬────────────────┐
   ▼             ▼               ▼                ▼
 m5pm1        wifi_manager    hvac_mqtt          cn105
 (PMIC,       (STA + SoftAP   (esp-mqtt, HA      (Mitsubishi
  charge       provisioning)   discovery, LWT)    CN105 protocol)
  governor)
   │                             ▲   │              │
   │ pmic_task (1 Hz)            │   │ commands     │ cn105_task (10 Hz)
   └────────────────────────────┘   └──────────────┘
```

## MQTT contract (from mitsubishi2MQTT)

Base: `<base_topic>/<friendly_name>`

| Direction | Topic suffix |
|-----------|--------------|
| subscribe | `/mode/set` `/temp/set` `/remote_temp/set` `/fan/set` `/vane/set` `/wideVane/set` `/system/set` `/ota/set` |
| publish | `/state` (retained) `/settings` `/availability` (LWT) `/debug/packets` `/debug/logs` |
| discovery | `homeassistant/climate/<friendly_name>/config` (retained) |

## Build / flash

ESP-IDF **v5.4.3** (matches CI).

```bash
# one-time, or after cleaning
idf.py set-target esp32s3

# bench config: copy and edit local creds (gitignored)
cp sdkconfig.defaults.local.example sdkconfig.defaults.local

idf.py build
idf.py -p <PORT> flash monitor
```

WiFi/MQTT can be set at build time (`sdkconfig.defaults.local` /
`menuconfig`) or at runtime (NVS, via the captive-portal SoftAP). On first boot
with no credentials the device starts a `mitsubishi-heatpump-XXXX` SoftAP
(captive portal at `http://192.168.4.1/`); pick a network and enter the
passphrase, and it saves to NVS and reboots into STA mode.

## Web UI / REST API

Once connected to WiFi the device serves a small diagnostics/control dashboard
at `http://<ip>/` (or `http://mitsubishi-heatpump.local/` via mDNS). It is for
provisioning and diagnostics — MQTT/Home Assistant remains the primary control
path. The same JSON API backs it:

| Method | Path | Purpose |
|--------|------|---------|
| `GET`  | `/` | gzip'd dashboard |
| `GET`  | `/api/status` | version, ip, uptime, free heap, unit/MQTT link, PMIC power telemetry |
| `GET`  | `/api/settings` | current heat-pump settings + status |
| `POST` | `/api/settings` | apply any subset of `{power,mode,temperature,fan,vane,wideVane,remoteTemp}` |
| `POST` | `/api/system/restart` | reboot |
| `POST` | `/api/system/factory_reset` | erase WiFi creds + reboot into setup |

Web commands reuse `hvac_mqtt::Command`, so the web and MQTT control paths
funnel through identical apply logic in `main.cpp`.

## OTA updates

Dual-app partition table (`ota_0` / `ota_1`) with rollback protection
(`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`). Three ways to update, one apply
pipeline (`main/ota.cpp`):

- **Local upload** — drag a `.bin` onto the dashboard's *Firmware update* card,
  or `POST /api/ota` with the raw image as the body.
- **HTTPS pull** — give it a URL (e.g. a GitHub release asset) via the dashboard
  or `POST /api/ota/url` `{"url":"…"}`; it downloads + flashes in the background.
  `GET /api/ota/status` reports `{state,progress,message}`.
- **MQTT** — publish the firmware URL to `<base>/<friendly_name>/ota/set`.

Rollback safety: a freshly-booted OTA image starts in `PENDING_VERIFY`; the
firmware calls `esp_ota_mark_app_valid_cancel_rollback()` once WiFi reconnects,
so a broken image that can't get online is automatically rolled back on reset.

## Releasing

Bump `set(PROJECT_VER "x.y.z")` in `CMakeLists.txt`, then run the **Create
Release** workflow. It tags `vx.y.z`, builds, generates a changelog from
conventional commits, and attaches the flashable binaries.

## Layout

| Path | Purpose |
|------|---------|
| `main/` | entry point, WiFi manager, Kconfig |
| `components/cn105/` | Mitsubishi CN105 serial protocol (port of SwiCago/HeatPump) |
| `components/m5pm1/` | PY32 PMIC I²C driver + charge governor |
| `components/hvac_mqtt/` | MQTT bridge + Home Assistant discovery |
| `.github/workflows/` | `build.yml`, `create-release.yml` |
