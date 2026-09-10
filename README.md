# Flipper Blue++

A custom Flipper Zero firmware build that turns the Bluetooth radio from an
advertise-only peripheral into a full Bluetooth Low Energy platform, plus a
suite of apps that use it.

Stock Flipper firmware runs ST's "BLE Light" coprocessor stack, which can only
advertise. Flipper Blue++ swaps in ST's **full** BLE stack (observer + central +
GATT client), **exports the ST BLE command API to apps** so ordinary `.fap`
files can scan, connect and run GATT, and throws in a few related unlocks
(selected firmware internals, and the sub-GHz region gate). On top of that
firmware it ships ten apps: a BLE scanner/explorer, a HID host, a sensor
dashboard, a tracker detector, a GATT fuzzer, a beacon broadcaster, a GATT
server, two sub-GHz tools and a battery monitor.

Repo: `github.com/andydixon/flipper-bluepp` (the slug drops the `++` because
GitHub repo names cannot contain `+`).

> **Read this first.** These apps do **not** run on stock Flipper firmware. They
> need the firmware `build.sh` produces (full BLE stack + exported command API,
> and for the sub-GHz tools the region unlock). Battery Health is the only app
> that would also run on stock firmware. Flashing this firmware also installs a
> different radio stack; going back to official firmware restores the stock one.

---

## Contents

- [Building and installing](#building-and-installing)
- [The apps](#the-apps) — detailed per-app docs
- [The firmware](#the-firmware) — full stack, exported APIs, sub-GHz unlock
- [How the BLE apps work](#how-the-ble-apps-work)
- [Limitations](#limitations)
- [Troubleshooting](#troubleshooting)
- [Repository layout](#repository-layout)

---

## Building and installing

Requirements: Linux or macOS with `git`, `python3` and a few GB of disk. The
firmware build system downloads its own ARM toolchain.

```sh
./build.sh          # clone firmware 1.4.3, apply the patches, build firmware + apps
./build.sh flash    # same, then flash over USB (Flipper connected, qFlipper closed)
./build.sh fap      # rebuild only the apps (firmware already built once)
```

Environment variables:

| Var | Default | Effect |
|---|---|---|
| `BLE_API` | `hci,gatt` | Which ST command groups to export (`min`, `all`, or any comma list of `hci,gap,gatt,hal,l2cap`). |
| `EXPORT_FLASH` | `1` | Export the raw flash / option-byte API (set `0` to omit). |
| `SUBGHZ_UNLOCK` | `1` | Remove the sub-GHz region gate (set `0` to keep it). |
| `FW_TAG` | `1.4.3` | Firmware tag to clone. |
| `FW_DIR` | `./flipperzero-firmware` | Where to clone. |
| `JOBS` | `4` | Parallel compile jobs. |

The result is a standard update package in
`flipperzero-firmware/dist/f7-C/f7-update-local/`, containing the firmware, the
**full radio stack**, and every app under `apps/` in its resources. To install
without USB flashing, copy that folder to the SD card (e.g. `SD:/update/`) and
open it in the Flipper's file browser. The individual `.fap` files are in
`flipperzero-firmware/build/f7-firmware-C/.extapps/`; once the firmware is
installed, app updates are just a file copy to the SD card.

To iterate on an app with `ufbt` instead of the whole firmware tree, point it at
the SDK the build emits:

```sh
ufbt update --local=flipperzero-firmware/dist/f7-C/flipper-z-f7-sdk-local.zip
cd bt_inspector && ufbt        # -> dist/bt_inspector.fap ; `ufbt launch` installs it
```

The first build takes 5-15 minutes; later builds are incremental.

While any BLE app runs, the Flipper's own advertising is stopped (and any phone
link dropped), then restored on exit. Only one app owns the radio at a time.

---

## The apps

All ten install to the Flipper's app menu. The BLE apps share one
central/GATT-client layer (`libble/`) and follow the same pattern: a live scan
list you pick a device from, then a per-app action. On the scan list, **Up/Down**
move the selection, **OK** acts on the highlighted device, **Back** exits.

### BT Inspector

A full BLE scanner and GATT explorer, in the spirit of BTInspector for iOS.

- **Scan list** — every advertiser in range, sorted by RSSI and refreshed every
  300 ms, showing name (or "(no name)"), RSSI in dBm with signal bars, address
  and vendor. Devices unseen for 60 s drop off.
- **Device page** (OK on a device) — address and address type, current and best
  RSSI, packet count, advertising type (and whether it is connectable), and a
  decode of everything in the advertisement and scan response:
  - flags; advertised service UUIDs (16/32/128-bit, named when known); service
    data; Tx power; appearance;
  - manufacturer data, with decoders for **Apple Continuity** (iBeacon
    UUID/major/minor/Tx; Proximity Pairing with AirPods/Beats **model name** and
    **left / right / case battery** and charging state; Nearby Info and Action;
    Find My; AirDrop; Handoff; AirPlay; tethering), **Microsoft CDP** device
    type, **Google Fast Pair**, **Eddystone** (UID/URL/TLM, URL expanded), and
    **Exposure Notification**;
  - the raw advertisement and scan-response bytes.
- **Connect** (OK on a connectable device) — negotiates MTU, discovers all
  primary services, and lists them. The first entry, **Read device info**, reads
  every readable characteristic of the Generic Access, Device Information and
  Battery services at once (name, appearance, model/serial numbers, firmware /
  hardware / software revisions, manufacturer, PnP ID, battery level).
- **Characteristics** — each service opens to its characteristics with property
  flags: `R`ead, `W`rite, `w`rite-without-response, `N`otify, `I`ndicate,
  `B`roadcast.
- **Characteristic page** — Read (OK), including custom 128-bit characteristics
  and long values (read-long). Values show as hex plus decoded forms: printable
  string, u8/i8, u16, u32, and SIG formats (battery %, appearance, preferred
  connection parameters, PnP ID, heart rate, temperature, humidity, pressure, Tx
  power, date/time). **Write** (Right) a value as text, hex bytes, or a number;
  **Notify/Unsub** (Left) subscribes to notifications/indications, locating the
  CCCD automatically. A timestamped **history** of every value seen for the open
  characteristic is kept, newest first.
- **Log** — everything is written with timestamps to
  `SD:/apps_data/bt_inspector/bti_<date>_<time>.log`: devices, connects,
  services, characteristics, reads, writes, notifications, errors.

| Screen | Up/Down | OK | Left | Right | Back |
|---|---|---|---|---|---|
| Scan list | select | device page | | | exit |
| Device page | scroll | Connect (if connectable) | | | scan list |
| Services | select | open / read device info | | | disconnect |
| Characteristics | select | open (auto-read) | | | services |
| Characteristic | scroll | Read | Notify/Unsub | Write | characteristics |

### BLE HID Host

Connect a Bluetooth keyboard or mouse and watch its input decoded live.

Pick a device; the app connects, finds the HID service (0x1812), sets the
protocol mode (Boot when the device offers boot reports, otherwise Report),
subscribes to the input-report characteristics, and keeps the device out of
suspend. Keyboard reports decode to modifier names plus key names (common USB
HID usage page; media/vendor keys show as raw `[XX]`); mouse reports decode to
button state and dx/dy/wheel. Events scroll on screen (Up/Down reviews history)
and are logged with the raw bytes to `SD:/apps_data/ble_hid_host/`. Back
disconnects and returns to scanning.

It cannot pair, so keyboards that require an encrypted link before sending
reports connect but stay silent; open keyboards and most mice work.

### BLE Sensor Dashboard

Connect a sensor and watch its readings decoded and updating live.

On connect it discovers all services, then picks out the characteristics with a
known SIG meaning that are readable or notifying, reads each once, and subscribes
to the notifying ones. The readings screen shows each as `Name: value`, decoded
by the shared value decoders — battery %, heart rate, temperature, humidity,
pressure, Tx power, time, appearance, PnP ID and more — and updates as
notifications arrive. Up to 12 readings; Up/Down scrolls, Back disconnects.
Unknown or vendor-specific characteristics are ignored (use BT Inspector for
those).

### BLE Tracker Detector

Passively flag nearby item trackers and warn if one seems to be following you.

It scans continuously and classifies advertisers by signature: Apple Find My /
AirTag (manufacturer-data type), Tile and Samsung SmartTag (service UUIDs). The
list shows each suspected tracker with its type, address and RSSI, sorted by
signal. A tracker seen repeatedly over roughly two minutes gets a `!` "following"
flag. Everything is logged to `SD:/apps_data/ble_tracker/` with first-seen and
follow events. It never connects to a tracker — detection only — and it does not
de-anonymise or disable anything.

### BLE GATT Fuzzer

Enumerate, read and (optionally) fuzz a peripheral's GATT database, logging
every response. **For authorised testing of devices you own.**

Pick a device; it connects and walks every service and characteristic, reading
each readable value and logging the service/characteristic UUIDs, handles,
properties, and read results (or ATT errors). Press **OK** on the report screen
to run a fuzz pass: for every writable characteristic it writes a small fixed set
of boundary payloads (empty, `0x00`, `0xFF`, twenty `0x41` bytes) and logs
whether each was accepted or rejected. The running report scrolls on screen
(Up/Down) and the full detail goes to `SD:/apps_data/ble_gattfuzz/`. Back
disconnects.

### BLE Beacon Toolkit

Broadcast a BLE advertisement with a chosen random MAC.

Two modes. **iBeacon** takes 21 entered bytes (16-byte UUID + 2 major + 2 minor +
1 Tx) and wraps them in the Apple manufacturer AD. **Raw** sends the entered
bytes verbatim as the advertising payload (up to 31). The menu lets you toggle
the mode, edit the MAC (six little-endian bytes) and the payload via the byte
editor, and Start/Stop the beacon; the header shows RUNNING or stopped. Uses the
firmware's extra-beacon API, so it advertises alongside the normal stack.
Changing a field stops the beacon; press Start again to re-broadcast.

### BLE GATT Server

Turn the Flipper into a connectable BLE peripheral you can talk to.

It brings up the firmware's serial GATT service (a 128-bit service with RX and TX
characteristics) via the BT service's profile API and echoes back whatever a
central writes, as a TX notification. Connect from a phone (e.g. nRF Connect) to
see the service, write to RX and receive the echo. The screen shows connection
state and running RX/TX byte counters and the last received text; **OK** toggles
echo on/off; **Back** restores the default (phone/RPC) profile and exits. It
exposes this fixed service rather than an arbitrary schema, because building
custom services from a `.fap` is not supported by the exported API.

### Sub-GHz Scanner

Sweep RSSI across the CC1101's range and log active frequencies. Needs the
sub-GHz region unlock (default on).

It tunes across a band in 250 kHz steps, drawing a live spectrum bar graph and
tracking the peak. Any frequency whose RSSI crosses the threshold is logged with
a timestamp to `SD:/apps_data/subghz_scan/`. **Left/Right** cycle between the
three hardware bands and an all-bands sweep; **OK** pauses; **Back** exits. RSSI
is read one frequency at a time, so a sweep is not instantaneous and can miss
very brief transmissions.

### Sub-GHz Spectrum

A live spectrum and waterfall over a tunable window. Needs the region unlock.

It sweeps a centre frequency ± span, drawing a spectrum bar graph on top and a
scrolling intensity waterfall below, so you can watch activity over time.
**Left/Right** retune the centre by a quarter-span; **Up/Down** widen or narrow
the span (250 kHz to 8 MHz); **Back** exits. Same one-frequency-at-a-time
sampling caveat as the scanner.

### Battery Health

Live fuel-gauge readings with a voltage graph. Read-only; the one app that also
runs on stock firmware.

It polls the gauge every two seconds and shows charge percentage and state,
voltage, current, temperature, remaining/full/design capacity, health, a derived
wear estimate, and USB (VBUS) voltage, with a rolling voltage graph across the
bottom. It never changes charge settings. Back exits.

---

## The firmware

The whole suite rests on three firmware changes `build.sh` applies to a clean
clone of official firmware 1.4.3.

### 1. The full BLE stack

The STM32WB55's second core runs a prebuilt ST BLE stack. Flipper ships
`stm32wb5x_BLE_Stack_light_fw.bin` (peripheral / broadcaster only). ST's
`stm32wb5x_BLE_Stack_full_fw.bin` adds the observer and central roles and the
GATT client. The firmware build system already supports it
(`COPRO_STACK_TYPE=ble_full`) and the firmware recognises it at runtime
(`FuriHalBtStackFull`); `build.sh` selects it and also widens the GAP roles the
firmware initialises to `PERIPHERAL | CENTRAL | OBSERVER`.

### 2. The BLE command API, exported to apps

External `.fap` apps can only call functions in the firmware's API table, and
the ST `aci_*` / `hci_*` command functions are not in it — so a stock `.fap`
cannot start a scan or a connection. `build.sh` adds ST's command headers to the
SDK, gives them `extern "C"` guards (the API table is compiled as C++), lets
`fbt` regenerate `api_symbols.csv`, and enables the selected functions. This
bumps the API minor version, which is why these apps load only on this firmware.

Every exported function stays in the firmware image, so `BLE_API` trades reach
for flash. The full stack loads at `0x080CE000` (light: `0x080D7000`); the gap
between the firmware and the stack is the Flipper's internal storage:

| `BLE_API` | exported | firmware | pages free, full stack | pages free, extended stack |
|---|---|---|---|---|
| `min` (only what the apps import) | 15 | 770 KB | 18 | 9 |
| `hci,gatt` (default) | 127 | 779 KB | 15 | 6 |
| `all` | 211 | ~788 KB | 13 | 4 |
| stock light-stack firmware | 0 | 768 KB | 27 | n/a |

`extended` refers to `stm32wb5x_BLE_Stack_full_extended_fw.bin` (BLE 5 extended
advertising, Coded PHY) at `0x080C5000`; it is not selected by this script.

Beyond the ST BLE commands, `build.sh` also exports a set of firmware functions
that stock firmware keeps internal (the list lives in `EXPORT_ENABLE`):

- **BLE peripheral internals** (`gap_*`, safe `ble_glue_*`, `furi_hal_bt_init`) —
  drive GAP directly, run a custom GATT server, set the advertised name and the
  extra-beacon config, read connection and radio-stack status. The coprocessor
  firmware-update calls (`ble_glue_fus_stack_delete/install`) stay disabled, as
  they can erase the radio stack.
- **Power / battery internals** — the gauge readings are already exported by
  stock firmware; this adds `furi_hal_power_insomnia_level`.
  `furi_hal_power_init` stays disabled.
- **Flash + option-byte internals** (`furi_hal_flash_*`) — geometry, free-page
  info, and raw program / erase / write / option-byte access. **This can brick
  the device or wipe storage;** enabled for research, `EXPORT_FLASH=0` opts out.
  No app in this repo uses it.
- **RPC + loader** — the public `rpc_*` / `loader_*` APIs are already exported by
  stock firmware; the internal loader headers are added so apps can use the
  internal types, but the internal helper *functions* stay unexported (they are
  not in the API-table link).

### 3. The sub-GHz region unlock

`build.sh` patches the region gate (`furi_hal_region_is_frequency_allowed` and
`furi_hal_region_is_provisioned` forced true) so transmit is allowed on any
frequency the CC1101 can tune, regardless of the device's provisioned region —
what most "unlocked" Flipper firmwares do. The CC1101 PLL bands (about 300-348,
387-464 and 779-928 MHz) remain, as they are a hardware limit, not a regulatory
one.

> **Transmitting outside the frequencies and power levels allocated to you may be
> illegal where you are. This is for research and authorised testing only; you
> are responsible for operating within the law.** `SUBGHZ_UNLOCK=0` keeps the
> region limits in place.

---

## How the BLE apps work

```
libble/
  ble_central.[ch]   scan / connect / GATT client on top of the ST stack:
                     registers a handler with the firmware's BLE event
                     dispatcher, consumes its own events (advertising reports,
                     its connection's events) so the firmware's peripheral GAP
                     never sees them, and turns async ATT events into
                     synchronous calls behind a worker thread.
  ble_names.[ch]     vendor / UUID / appearance tables, advertisement decoders
                     (Apple/Microsoft/Fast Pair/Eddystone), and characteristic
                     value decoders.
```

Each BLE app registers for BLE events, stops the Flipper's own advertising, then
issues `hci_le_set_scan_*`, `hci_le_create_connection` and `aci_gatt_*` commands
directly. Its own connection-complete events are told apart from the firmware's
peripheral connections by the role field. Advertising reports are parsed from the
raw HCI payload. `build.sh` copies `libble/*.{c,h}` into each app that includes
`ble_central.h` at build time, so every app compiles against one source of truth.

---

## Limitations

**Shared / hardware**

- **BLE only.** The STM32WB55 has no Bluetooth Classic (BR/EDR), so classic
  devices cannot be seen or connected from any Flipper.
- **No pairing or bonding as central.** None of the central apps pair, so
  anything gated behind an encrypted link is out of reach: encrypted
  characteristics report "Insufficient auth" / "Insufficient encryption", and
  keyboards that only send reports after bonding stay silent.
- At most two concurrent links (the firmware's BLE config); a BLE app stops the
  Flipper's advertising and drops any phone link while it runs, restoring both on
  exit. Only one app owns the radio at a time.
- Built and checked against firmware 1.4.3 as `.fap` apps. On-device behaviour
  needs a Flipper running the `build.sh` firmware; the apps will not load on
  stock firmware (missing exported symbols). Everything here is compile-verified,
  not yet exercised on hardware.

**BT Inspector** — Apple battery levels come from what AirPods/Beats broadcast;
iPhone/Mac battery is only exposed to paired Apple devices. Vendor, UUID and
Apple model tables are curated subsets (unknown IDs shown as hex) and Continuity
layouts are community-documented. Value history is per open characteristic; the
log has all of it. iOS-only BTInspector features (Shortcuts, Live Activities,
location-mapped background scanning) have no Flipper equivalent.

**BLE HID Host** — report-mode devices are classified heuristically (8+ byte
reports as keyboard, shorter as mouse), not by parsing the Report Map, so unusual
layouts (consumer keys, multi-touch, gamepads) may decode oddly. Keyboard
decoding covers the common usage page; other usages show as raw `[XX]`. Only
input reports are shown; output (LEDs) and feature reports are not driven.

**BLE Sensor Dashboard** — only known-SIG characteristics are shown; each is a
single decoded field (no graphs or extra unit conversion), up to 12.

**BLE Tracker Detector** — signature-based, so trackers using other schemes or
rotating their advertising identity may be missed; an address change counts as a
new device. The "following" flag is a heuristic, not proof. Passive only.

**BLE GATT Fuzzer** — a fixed set of boundary payloads, not a coverage-guided or
mutation fuzzer. **Writing arbitrary values can misconfigure or brick the
target;** authorised use only. Reads reflect what the peripheral returns; the
on-screen feed shows the first bytes, the log has the run.

**BLE Beacon Toolkit** — iBeacon expects exactly 21 bytes; Raw payloads are not
validated as well-formed AD. Legacy advertising only (no extended / BLE 5 long
range). Config changes apply on the next Start.

**BLE GATT Server** — exposes the fixed serial GATT service and echoes writes;
not an arbitrary user-defined schema. It takes over the BLE profile while open
and restores the default on exit.

**Sub-GHz Scanner / Spectrum** — RSSI is read one frequency at a time with a
fixed OOK preset and a short settle delay, so sweeps are slow and can miss brief
bursts; the reading reflects that preset's bandwidth, not a true FFT.
Frequencies are limited to the CC1101 PLL bands. Do not run alongside the stock
Sub-GHz app (both drive the shared radio). Neither transmits; see the region
unlock note on legality.

**Battery Health** — figures come straight from the gauge IC; "wear" is a rough
full-vs-design estimate. Read-only.

**Exported firmware APIs (unlocks)** — every exported function costs flash and
shrinks internal storage (see the table above). Flash / option-byte access is
raw and unguarded (brick/wipe risk); RPC/loader expose internal types only, not
the internal helper functions. The sub-GHz unlock removes the region check only;
the PLL bands and the law still apply. All of this is compile-verified, not
hardware-tested.

---

## Troubleshooting

- **"Radio stack has no scanning" / an app says it needs the full stack** — the
  light stack is installed. Install the package from `build.sh`, which includes
  the full `radio.bin`.
- **App refuses to load ("API mismatch" / unresolved symbol)** — the device
  isn't running the `build.sh` firmware, or it was built with a smaller
  `BLE_API` than the app needs. Reinstall the update package.
- **Build killed / out of memory** — `JOBS=2 ./build.sh`.
- **"API version is still WIP" during a build** — `fbt` found symbols the script
  did not classify; open `targets/f7/api_symbols.csv`, change any remaining `?`
  to `+` or `-`, and rerun.
- **Connect failed: Timeout** — the device may not be connectable (check the
  advertising type on BT Inspector's device page) or is out of range; a
  rotating-address device may need re-selecting from the list.

---

## Repository layout

```
libble/            shared BLE central / GATT client + name & value decoders
bt_inspector/      BT Inspector          ble_beacon/      BLE Beacon Toolkit
ble_hid_host/      BLE HID Host          ble_gattsrv/     BLE GATT Server
ble_sensor/        BLE Sensor Dashboard  subghz_scan/     Sub-GHz Scanner
ble_tracker/       BLE Tracker Detector  subghz_spectrum/ Sub-GHz Spectrum
ble_gattfuzz/      BLE GATT Fuzzer       battery_health/  Battery Health
build.sh           firmware patch + build + flash script
flipperzero-firmware/   created by build.sh (git-ignored)
```

Each app directory holds its `application.fam`, `icon.png` and sources. To add
an app, drop a directory next to these and add its name to `APPS=(...)` in
`build.sh`; if it includes `ble_central.h`, `build.sh` stages `libble` into it
automatically.
