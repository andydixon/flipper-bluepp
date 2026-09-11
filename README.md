# Flipper Zero BLE tools

Two BLE apps for the Flipper Zero, sharing one central/GATT layer (`libble/`):

* **BT Inspector** — a BLE scanner and GATT explorer, in the spirit of
  BTInspector for iOS: discover nearby Bluetooth Low Energy devices, see their
  signal strength and everything they broadcast, then connect and inspect their
  services and characteristics: read, write, subscribe, and log it all.
* **BLE HID Host** — connect to a Bluetooth keyboard or mouse and watch its
  input reports decoded live (keys, modifiers, mouse buttons and movement).

> **Read this first.** This app cannot run on stock Flipper firmware. The
> Flipper's radio coprocessor ships with ST's "BLE Light" stack, which can only
> advertise (peripheral role). It cannot scan for or connect to other devices.
> BT Inspector is therefore delivered as a **custom firmware build** that swaps
> in ST's full BLE stack and exports the BLE command API to apps. The app itself
> is a normal `.fap`. `build.sh` does all of this.
> See [Why a custom firmware build](#why-a-custom-firmware-build).

## Features

* **Live scan list.** Every BLE advertiser in range, sorted by RSSI, refreshed
  every 300 ms as you move around. Shows name (or "(no name)"), RSSI in dBm
  with signal bars, address and vendor. Devices unseen for 60 s drop off.
* **Device page.** Address and address type, current/best RSSI and packet
  count, advertising type (connectable or not), flags, advertised service UUIDs
  (16/32/128-bit, named when known), service data, Tx power, appearance,
  manufacturer data and raw advertisement / scan-response bytes. Updates live.
* **Identification.** Manufacturer IDs map to vendor names. Decoders for:
  * Apple Continuity: iBeacon (UUID/major/minor/Tx), Proximity Pairing with
    AirPods/Beats **model name** and **left / right / case battery** and
    charging state, Nearby Info (device activity), Nearby Action, Find My,
    AirDrop, Handoff, AirPlay, tethering.
  * Microsoft Connected Devices beacon: device type (Windows desktop/laptop,
    Xbox, Surface Hub, Android, iPhone, ...).
  * Google Fast Pair model ID, Eddystone UID/URL/TLM (URL decoded), COVID
    Exposure Notification.
* **Connect and interrogate.** OK on a connectable device connects (MTU is
  negotiated), discovers all primary services, then characteristics with their
  properties (`R`ead, `W`rite, `w`rite-without-response, `N`otify, `I`ndicate,
  `B`roadcast).
* **Read device info** (first item in the service list): reads every readable
  characteristic of the Generic Access, Device Information and Battery services
  in one go: device name, appearance, model number, serial number, firmware /
  hardware / software revision, manufacturer name, PnP ID, battery level.
* **Characteristic page.** Read any characteristic, including custom 128-bit
  ones. Values are shown as hex plus decoded forms: printable string, u8/i8,
  u16, u32, and SIG-specific formats (battery %, appearance, preferred
  connection parameters, PnP ID, heart rate, temperature, humidity, pressure,
  Tx power, date/time). Long values are fetched with read-long.
* **Write** a value back as text, hex bytes or a number (little-endian, sized to
  the current value or the magnitude). Uses write-with-response when the
  characteristic supports it, otherwise write-without-response. A readable
  characteristic is re-read after a write.
* **Subscribe** to notifications / indications (the CCCD is located
  automatically). Incoming values update the page and the history.
* **Value history.** Each value seen for the open characteristic (reads and
  notifications) is kept with a timestamp, newest first.
* **Session log.** Every event is written with a date/time stamp to
  `SD:/apps_data/bt_inspector/bti_YYYYMMDD_HHMMSS.log`: devices found (and
  renamed), connects, services, characteristics, reads, writes, notifications,
  errors. Copy it off the SD card (or via qFlipper) for analysis.

## Building and installing

Requirements: Linux or macOS with `git`, `python3` and a few GB of disk. The
firmware build system downloads its own ARM toolchain.

```sh
./build.sh          # clone firmware 1.4.3, export the BLE API, build firmware + .fap
./build.sh flash    # same, then flash over USB (Flipper connected, qFlipper closed)
./build.sh fap      # rebuild only the .fap after editing the app
```

Options via environment variables: `FW_TAG` (firmware tag, default `1.4.3`),
`FW_DIR` (where to clone), `JOBS` (parallel compile jobs, default 4),
`BLE_API` (which ST API to export, see below).

The result is a standard update package in
`flipperzero-firmware/dist/f7-C/f7-update-local/`. It contains the firmware,
the **full radio stack**, and the app as `apps/Bluetooth/bt_inspector.fap` in
its resources. To install without USB flashing, copy that folder to the SD card
(for example `SD:/update/`) and open it in the Flipper's file browser.
Installing an official firmware later restores the light stack the same way.

The `.fap` alone is at `flipperzero-firmware/build/f7-firmware-C/.extapps/`;
after the firmware is installed once, app updates are just a file copy to
`SD:/apps/Bluetooth/`. It only loads on this firmware (API version 87.2 with
the exported symbols); the stock loader rejects it with an API mismatch.

To build the app with `ufbt` instead of the firmware tree, point `ufbt` at the
SDK the build produces:

```sh
ufbt update --local=flipperzero-firmware/dist/f7-C/flipper-z-f7-sdk-local.zip
cd bt_inspector && ufbt        # -> dist/bt_inspector.fap ; `ufbt launch` installs it
```

The first build takes 5-15 minutes; later builds are incremental.

## Using the app

BT Inspector appears in the Flipper's main menu. While it runs, the Flipper's
own BLE advertising is stopped (and any phone connection dropped); it is
restored on exit.

| Screen | Up / Down | OK | Left | Right | Back |
|---|---|---|---|---|---|
| Scan list | select device | device page | | | exit app |
| Device page | scroll | Connect (if connectable) | | | scan list |
| Services | select | open service / read device info | | | disconnect, device page |
| Characteristics | select | open characteristic (auto-read) | | | services |
| Characteristic | scroll | Read | Notify / Unsub | Write | characteristics |
| Write menu | select | Text / Hex bytes / Number | | | characteristic |

Status messages ("Read OK (5 B)", "Write failed: Write not permitted",
"Notifications on", ...) appear on the characteristic page.

On stock firmware the app opens with an explanation instead of scanning.

## Log format

One event per line:

```
[2026-09-11 09:12:03] START stack=full advertising_was=1
[2026-09-11 09:12:04] DEVICE 5C:F3:70:9A:11:20 ADV_IND rssi=-58 name="Living Room" vendor=Apple adv=02 01 1A 0B FF 4C 00 ...
[2026-09-11 09:12:20] CONNECTING Living Room
[2026-09-11 09:12:21] CONNECTED Living Room mtu=247
[2026-09-11 09:12:21] SERVICE 180A Device Information handles 0x0010-0x001C
[2026-09-11 09:12:25] CHAR 2A26 handle 0x0016 props 0x02
[2026-09-11 09:12:26] READ 0x0016 = 31 2E 32 2E 30
[2026-09-11 09:12:40] WRITE 0x0021 = 01
[2026-09-11 09:12:41] NOTIFY 0x0024 = 64
[2026-09-11 09:13:00] DISCONNECT
```

A `DEVICE` line is written when a device is first seen and again when its name
changes. `INFO` lines come from "Read device info".

## Why a custom firmware build

Three facts about the Flipper Zero drive the design:

1. The STM32WB55's second core runs a prebuilt ST BLE stack. Flipper ships
   `stm32wb5x_BLE_Stack_light_fw.bin` (peripheral + broadcaster only). ST's
   `stm32wb5x_BLE_Stack_full_fw.bin` adds observer and central roles. The
   firmware build system already supports it (`COPRO_STACK_TYPE=ble_full`) and
   the firmware recognises it at runtime (`FuriHalBtStackFull`).
2. External `.fap` apps can only call functions listed in the firmware's API
   table. The ST `aci_*` / `hci_*` command functions are not in it, so a stock
   `.fap` cannot start a scan or a connection. `build.sh` adds ST's command
   headers to the SDK (`lib/stm32wb.scons`), adds `extern "C"` guards to them
   (the API table is C++), lets `fbt` regenerate `api_symbols.csv`, and enables
   the selected functions. The API minor version becomes 87.2.
3. Every exported function is kept in the firmware image, so `BLE_API` trades
   generality for flash. The full stack loads at 0x080CE000 (light: 0x080D7000);
   the space between the firmware and the stack is the Flipper's internal
   storage:

   | `BLE_API` | exported | firmware | pages left, full stack | pages left, extended stack |
   |---|---|---|---|---|
   | `min` (app's own 15 imports) | 15 | 770 KB | 18 | 9 |
   | `hci,gatt` (default) | 127 | 779 KB | 15 | 6 |
   | `all` | 211 | ~788 KB | 13 | 4 |
   | stock light-stack firmware | 0 | 768 KB | 27 | n/a |

   Any comma list of `hci,gap,gatt,hal,l2cap` works. `extended` refers to
   `stm32wb5x_BLE_Stack_full_extended_fw.bin` (BLE 5 extended advertising,
   Coded PHY), which loads at 0x080C5000; it is not selected by this script.

### Extra firmware APIs exported to apps

Beyond the ST BLE commands, `build.sh` enables a set of firmware functions that
stock firmware keeps internal, so any `.fap` on this firmware can use them. The
set is a list of name patterns in `build.sh` (`EXPORT_ENABLE`); each enabled
symbol costs one API-table entry.

* **BLE peripheral internals** (`gap_*`, safe `ble_glue_*`, `furi_hal_bt_init`):
  drive GAP directly, run a custom GATT server, change the advertised name and
  the extra-beacon config, read connection state and radio-stack status,
  without going through the firmware's serial profile. The coprocessor
  firmware-update calls (`ble_glue_fus_stack_delete/install`) are deliberately
  left disabled, since they can erase the radio stack.
* **Power / battery internals**: the gas-gauge readings (voltage, current,
  temperature, remaining/full/design capacity, health, charge state) are
  already exported by stock firmware. This build also enables
  `furi_hal_power_insomnia_level`; `furi_hal_power_init` stays disabled.
* **Flash + option-byte internals** (`furi_hal_flash_*`): flash geometry,
  free-page info, and raw program / erase / dword-write / option-byte access.
  An app can read and modify internal flash directly. **This can brick the
  device or wipe storage.** Enabled here for research; build with
  `EXPORT_FLASH=0 ./build.sh` to leave these disabled.
* **RPC + loader internals**: the public `rpc_*` and `loader_*` APIs (RPC
  session open/feed/close, app data exchange, launching and enqueuing apps,
  lock/unlock, showing the app menu) are already exported by stock firmware and
  are the supported way to script the Flipper from a host or launch other apps;
  this build enables them explicitly and adds the internal loader headers
  (`loader_i.h`, `loader_menu.h`, `loader_queue.h`, `loader_applications.h`) so
  the internal loader/menu/queue helpers are reachable too. `rpc_i.h` is not
  exported because it depends on the protobuf headers, which are not in the SDK;
  the public RPC API covers host scripting.

`build.sh` also widens the GAP roles the firmware initialises
(`GAP_PERIPHERAL_ROLE | GAP_CENTRAL_ROLE | GAP_OBSERVER_ROLE`) in
`targets/f7/ble_glue/gap.c`. The app itself uses HCI-level scan and connect
commands, so this is belt-and-braces.

## How it works

```
bt_inspector.c   UI (ViewDispatcher: scan list, text page, menus, inputs),
                 worker thread for blocking BLE operations, session log
ble_central.c    scan / connect / GATT client on top of the ST stack.
                 Registers a handler with the firmware's BLE event dispatcher,
                 acknowledges its own events (advertising reports, our
                 connection's events) so the firmware's peripheral GAP never
                 sees them, and turns async ATT events into synchronous calls
ble_names.c      vendor / UUID / appearance tables and value decoders
```

The app registers for BLE events, stops the Flipper's own advertising, then
issues `hci_le_set_scan_*`, `hci_le_create_connection` and `aci_gatt_*`
commands directly. Connection-complete events are distinguished from the
firmware's peripheral connections by the role field. Advertising reports are
parsed from the raw HCI payload (the ST header's `Advertising_Report_t` is not a
wire overlay).

## Limitations

* **BLE only.** The STM32WB55 has no Bluetooth Classic (BR/EDR), so classic
  devices cannot be seen from any Flipper.
* Apple battery levels come from what AirPods/Beats broadcast. Battery of
  iPhones/Macs is only exposed to paired Apple devices.
* Vendor, UUID and Apple model tables are curated subsets; unknown IDs are
  shown as hex. Apple Continuity layouts are community-documented and may drift.
* Value history is kept for the open characteristic; the log file has all of it.
* iOS-only BTInspector features (Shortcuts, Live Activities, background scanning
  mapped by location) have no Flipper equivalent.
* Pairing/bonding with the target is not implemented; characteristics that
  require encryption report "Insufficient auth" / "Insufficient encryption".
* Tested by compiling into firmware 1.4.3; on-device behaviour needs a Flipper
  with the full stack installed.

## Troubleshooting

* **"Radio stack has no scanning"** on launch: the light stack is installed.
  Install the package produced by `build.sh`, which includes `radio.bin` with
  the full stack.
* **Build killed / out of memory**: `JOBS=2 ./build.sh`.
* **App refuses to load ("API mismatch" / unresolved symbol)**: the firmware on
  the device is not the one from `build.sh`, or it was built with a smaller
  `BLE_API` than the app needs. Reinstall the update package.
* **"API version is still WIP"** during a build: `fbt` found symbols the script
  did not classify. Open `targets/f7/api_symbols.csv`, replace the remaining
  `?` marks with `+` or `-`, rerun.
* **Connect failed: Timeout**: the device may not be connectable (see the
  advertising type on the device page), or it is out of range. Random-address
  devices that rotate their address may need re-selecting from the list.

## The BLE HID Host app

Select a device from the scan list; the app connects, finds the HID service
(0x1812), sets the protocol mode (boot when the device offers boot reports,
otherwise report mode), and subscribes to its input-report characteristics.
Keyboard reports are decoded to modifier + key names, mouse reports to button
state and dx/dy/wheel. Events scroll on screen (Up/Down to review history) and
are logged to `SD:/apps_data/ble_hid_host/hid_*.log` with the raw bytes. Back
disconnects and returns to scanning.

It cannot pair, so keyboards that require an encrypted link before sending
reports will connect but stay silent; open ones and most mice work.

## Layout

```
libble/            shared BLE central / GATT client + name & value decoders
bt_inspector/      BT Inspector app (application.fam, icon.png, bt_inspector.c)
ble_hid_host/      BLE HID Host app (HID-over-GATT setup + report decoding)
build.sh           firmware build / flash script (stages libble into each app)
flipperzero-firmware/   created by build.sh (git-ignored)
```

`build.sh` copies `libble/*.{c,h}` into each app directory at build time, so
both apps compile against one source of truth. Add an app by dropping its
directory next to these and adding its name to `APPS=(...)` in `build.sh`.
