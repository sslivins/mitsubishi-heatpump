# mitsubishi-heatpump

ESP-IDF firmware for an **M5Stack Stamp-S3Bat** (ESP32-S3) that controls a
**Mitsubishi Electric** indoor unit over its **CN105** serial port and bridges
it to **Home Assistant over MQTT**.

It is the ground-up ESP-IDF successor to
[`mitsubishi2MQTT`](https://github.com/gysmo38/mitsubishi2MQTT) (Arduino/ESP8266)
and preserves that project's MQTT topic contract, so existing Home Assistant
entities keep working.

> **Status: working.** Architecture, threading, WiFi (incl. captive-portal
> provisioning + mDNS), the on-device web UI / REST API, the MQTT client +
> Home Assistant discovery (climate + firmware `update` entities), the PMIC I²C
> driver, and the **CN105 packet engine** are all in place; deployed units drive
> real indoor units and report live telemetry. A multi-head **zone coordination**
> layer keeps heads that share one outdoor compressor from fighting over
> HEAT/COOL. CI builds green.

## Hardware

The controller is an off-the-shelf **[M5Stack Stamp-S3Bat][stamp]** module — an
ESP32-S3 (`ESP32-S3-PICO-1-N8R8`, 8 MB flash / 8 MB PSRAM) with an on-board
battery/PMIC subsystem, so a single small LiPo buffers the whole thing off the
heat pump's own 5 V rail. No custom PCB is required; you solder five wires from
its castellated pads to a CN105 cable.

| Part | Role |
|------|------|
| [M5Stack Stamp-S3Bat][stamp] (`ESP32-S3-PICO-1-N8R8`) | application MCU + WiFi |
| M5MP1 PMIC (I²C `0x6E`, on-module) | rails, Li-ion charge gate (`CHG_EN`), VBAT/VIN telemetry |
| LGS4056HDA-4.35 (on-module) | Li-ion charger |
| 400 mAh LiPo ([Adafruit 3898][lipo], SH1.0-2P) | buffer for CN105 power blips + WiFi TX spikes |

[stamp]: https://shop.m5stack.com/products/m5stamps3-bat-module-with-battery-connector
[lipo]: https://www.adafruit.com/product/3898

The CN105 5 V rail is current-limited, so the firmware runs a **closed-loop
charge governor** (`m5pm1::PMIC::governor_tick`) that gates `CHG_EN` on the VIN
reading to keep input draw within budget — that's what lets the module run
directly off CN105 5 V with the LiPo absorbing transients. See
`components/m5pm1/`. Board pinout and schematic: the
[M5Stack Stamp-S3Bat docs][stampdocs].

[stampdocs]: https://docs.m5stack.com/en/core/Stamp-S3Bat

### Default pin map (override in `menuconfig` → *Component config → mitsubishi-heatpump*)

| Function | GPIO | Notes |
|----------|------|-------|
| CN105 UART TX (→ heat pump RX, CN105 pin 5) | **GPIO 1** | UART port 1, 2400 baud 8E1 |
| CN105 UART RX (← heat pump TX, CN105 pin 4) | **GPIO 2** | |
| PMIC I²C SDA / SCL | **48 / 47** | internal bus (per S015 schematic); don't remap |

Defaults live in [`main/Kconfig.projbuild`](main/Kconfig.projbuild)
(`CN105_UART_TX_PIN`, `CN105_UART_RX_PIN`, `CN105_UART_PORT`, `PMIC_I2C_*`).

### CN105 connector & cable

Mitsubishi indoor units expose a 5-pin **CN105** service header. The mating
connector is a **JST PA series, 2.0 mm pitch, 5-position** housing:
[JST **PAP-05V-S**][pap] (the DigiKey part) with **SPH-002T-P0.5S** crimp
terminals. Crimping JST PA by hand is fiddly, so **a pre-made CN105 pigtail is
the easy path** — search "Mitsubishi CN105 cable/pigtail"; e.g. the pre-wired
cables from [Serin Labs][serin] are a clean, known-good option.

[pap]: https://www.digikey.com/en/products/detail/jst-sales-america-inc/pap-05v-s/759977
[serin]: https://serin-labs.com/wiring.html

**CN105 pinout** (looking at the header on the indoor-unit PCB) and how it maps
to the Stamp-S3Bat:

| CN105 pin | Signal | Wire to Stamp-S3Bat |
|-----------|--------|---------------------|
| 1 | **12 V** | **do not connect** |
| 2 | GND | `GND` |
| 3 | 5 V | `5V`/`VIN` (powers the module + charges the LiPo) |
| 4 | TX (data **from** the heat pump) | **RX** → GPIO 2 |
| 5 | RX (data **to** the heat pump) | **TX** → GPIO 1 |

The data lines are **crossed** — heat-pump TX → ESP RX, heat-pump RX → ESP TX.
The CN105 bus is **5 V TTL** while the ESP32-S3 GPIOs are 3.3 V (not 5 V
tolerant); many builds direct-connect and work, but a small level shifter on the
ESP **RX** line is the electrically-correct choice. Always plug/unplug the
connector with the unit powered **off**, and never wire pin 1 (12 V) to the ESP.

Good background/wiring references (they target ESPHome/Arduino but the wiring and
protocol are identical):

- [SwiCago/HeatPump][swicago] — the original CN105 protocol library this port is based on
- [Serin Labs wiring guide][serin] — CN105 pinout + diagrams
- [ESPHome `mitsubishi_cn105`][esphome] and [echavet/MitsubishiCN105ESPHome][echavet]

[swicago]: https://github.com/SwiCago/HeatPump
[esphome]: https://esphome.io/components/climate/mitsubishi_cn105/
[echavet]: https://github.com/echavet/MitsubishiCN105ESPHome

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
| subscribe | `/mode/set` `/temp/set` `/remote_temp/set` `/fan/set` `/vane/set` `/wideVane/set` `/system/set` `/ota/set` `/update/install` |
| publish | `/state` (retained) `/settings` `/availability` (LWT) `/update/state` (retained) `/debug/packets` `/debug/logs` |
| discovery | `homeassistant/climate/<friendly_name>/config` (retained) `homeassistant/update/<friendly_name>/config` (retained) |

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

WiFi can be set at build time (`sdkconfig.defaults.local` / `menuconfig`) or at
runtime (NVS, via the captive-portal SoftAP). On first boot with no credentials
the device starts a `mitsubishi-heatpump-XXXX` SoftAP (captive portal at
`http://192.168.4.1/`); pick a network and enter the passphrase, and it saves to
NVS and reboots into STA mode. The **MQTT broker** is likewise configurable at
build time *or* at runtime from the web UI (**System → MQTT / Home Assistant**),
persisted to NVS; saving reboots the device. Each unit derives a short
hardware-unique id from its factory MAC (e.g. `E608`) that is reused everywhere
so multiple units never collide: the SoftAP name (`mitsubishi-heatpump-<id>`),
the **mDNS hostname** (`mitsubishi-heatpump-<id>.local`), and the default MQTT
node (leaving `friendly_name` blank yields `heatpump-<id>`). The same id is the
HA `unique_id`/device identity. Units are also **self-discoverable** via DNS-SD:
each advertises an `_http._tcp` service with TXT records (`id`, `fw`, `model`,
`path`), so `avahi-browse -rt _http._tcp` / Bonjour / a controller can list every
unit without knowing the hostname.

## Web UI / REST API

Once connected to WiFi the device serves a small diagnostics/control dashboard
at `http://<ip>/` (or `http://mitsubishi-heatpump-<id>.local/` via mDNS, where
`<id>` is the unit's MAC suffix shown on the System tab). It is for
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
| `GET`  | `/api/mqtt` | current broker settings `{host,port,username,base_topic,friendly_name,password_set,connected}` (password never returned) |
| `POST` | `/api/mqtt` | save broker settings to NVS + reboot (`{host,port,username,password?,base_topic,friendly_name}`; omit `password` to keep the stored one) |
| `GET`  | `/api/wifi` | current network `{ssid,mode,connected,ip,ap_name,password_set}` (password never returned) |
| `POST` | `/api/wifi` | save credentials to NVS + reboot (`{ssid,password?}`; omit `password` to keep the stored one) |
| `GET`  | `/api/update` | cached GitHub release check `{current,latest,update_available,checking,checked,release_url,error}` |
| `POST` | `/api/update/check` | trigger an immediate GitHub `/releases/latest` poll (background) |
| `POST` | `/api/update/install` | download + flash the latest release (if newer) |

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

### GitHub release auto-update

A background poller (`ota::start_update_checker`) hits
`https://api.github.com/repos/sslivins/mitsubishi-heatpump/releases/latest`
every 6 h (and on demand via `POST /api/update/check`), compares the latest
release tag against the running version, and exposes the result two ways:

- **Web UI** — the System tab's *Software update* card shows installed/latest
  versions, a **Check for updates** button, and a one-click **Install update**
  button (which downloads the release's `mitsubishi-heatpump*.bin` asset through
  GitHub's CDN redirect — see the enlarged HTTP buffers in `ota.cpp`).
- **Home Assistant** — a native MQTT `update` entity
  (`homeassistant/update/<friendly_name>/config`) reports installed/latest
  versions to `.../update/state`; HA shows an *update available* badge and an
  **Install** button that publishes `install` to `.../update/install`, routed to
  `ota::install_latest()`.

`/releases/latest` ignores drafts **and pre-releases**, so only full releases
(cut by `create-release.yml` with `draft:false`) are offered; the device reports
*"no published release yet"* until one exists.

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
