#!/usr/bin/env bash
# Build a Flipper Zero firmware update package that contains BT Inspector.
#
# Why a firmware build and not a plain .fap:
#   * The stock radio coprocessor image is ST's "BLE Light" stack: peripheral/
#     broadcaster only. Scanning and connecting as a central need the "full"
#     stack (COPRO_STACK_TYPE=ble_full, already supported by fbt).
#   * The firmware does not export the ST GAP/GATT/HCI command API to external
#     apps. This script adds those headers to the SDK and accepts the new
#     aci_*/hci_* symbols, so the app builds as a normal .fap (bumps the API
#     minor version: the .fap only loads on this firmware).
#
# Usage:
#   ./build.sh            build  -> flipperzero-firmware/dist/f7-C/f7-update-*/ (update package,
#                         includes the .fap under apps/Bluetooth) + SDK zip for ufbt
#   ./build.sh flash      build and flash over USB (Flipper connected, qFlipper closed)
#   ./build.sh fap        rebuild only the .fap (firmware already built once)
#   BLE_API=all|min|hci,gap,gatt,hal,l2cap   which ST API to export (default hci,gatt)
#   FW_TAG=1.4.3 ./build.sh   pin a different firmware tag (default 1.4.3)
#   JOBS=2 ./build.sh         limit parallel compile jobs (default 4)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APPS=(bt_inspector ble_hid_host) # app dirs under $HERE, each staged with libble/
APP_SRC="$HERE/bt_inspector" # used only for the BLE_API=min symbol scan
FW_DIR="${FW_DIR:-$HERE/flipperzero-firmware}"
FW_TAG="${FW_TAG:-1.4.3}"
FW_REPO="https://github.com/flipperdevices/flipperzero-firmware.git"

if [ ! -d "$FW_DIR/.git" ]; then
    echo ">> cloning firmware $FW_TAG into $FW_DIR"
    git clone --depth 1 --branch "$FW_TAG" --recurse-submodules --shallow-submodules "$FW_REPO" "$FW_DIR"
fi

cd "$FW_DIR"

# Let GAP take the central/observer roles as well as peripheral (harmless on the
# full stack; the app itself uses HCI-level scan/connect commands).
GAP_C=targets/f7/ble_glue/gap.c
if grep -q "^        GAP_PERIPHERAL_ROLE,$" "$GAP_C"; then
    sed -i 's/^        GAP_PERIPHERAL_ROLE,$/        GAP_PERIPHERAL_ROLE | GAP_CENTRAL_ROLE | GAP_OBSERVER_ROLE,/' "$GAP_C"
    echo ">> patched $GAP_C: GAP roles = peripheral|central|observer"
fi

# Export the ST BLE command API (aci_*/hci_*) to external apps.
WB_SCONS=lib/stm32wb.scons
python3 - "$WB_SCONS" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()
block = """# BT Inspector: expose the ST BLE command API to .fap apps
env.Append(
    SDK_HEADERS=[
        File("stm32wb_copro/wpan/ble/core/auto/ble_hci_le.h"),
        File("stm32wb_copro/wpan/ble/core/auto/ble_gap_aci.h"),
        File("stm32wb_copro/wpan/ble/core/auto/ble_gatt_aci.h"),
        File("stm32wb_copro/wpan/ble/core/auto/ble_hal_aci.h"),
        File("stm32wb_copro/wpan/ble/core/auto/ble_l2cap_aci.h"),
        File("stm32wb_copro/wpan/ble/core/auto/ble_vs_codes.h"),
    ],
)

"""
s = re.sub(r"\n*# BT Inspector: expose.*?\n\)\n", "\n", s, flags=re.S)  # drop any earlier copy
if block not in s:
    s = s.replace('Return("lib")', block + 'Return("lib")', 1)  # must run before Return
    open(p, "w").write(s)
    print(">> patched " + p + ": ST BLE headers added to SDK")
PY

# ST's generated headers have no extern "C" guards; the API table is C++.
python3 - <<'PY'
import re
for name in ["ble_hci_le", "ble_gap_aci", "ble_gatt_aci", "ble_hal_aci", "ble_l2cap_aci"]:
    p = f"lib/stm32wb_copro/wpan/ble/core/auto/{name}.h"
    s = open(p).read()
    if "__cplusplus" in s:
        continue
    guard = re.search(r"#define \w+_H__\n", s).group(0)
    s = s.replace(guard, guard + '#ifdef __cplusplus\nextern "C" {\n#endif\n', 1)
    i = s.rstrip().rfind("#endif")
    s = s[:i] + "#ifdef __cplusplus\n}\n#endif\n" + s[i:]
    open(p, "w").write(s)
    print(">> patched " + p + ': extern "C" guards')
PY

# Remove the sub-GHz region / frequency transmit limits (RESEARCH USE ONLY).
# furi_hal_region_is_frequency_allowed gates TX vs RX-only per the provisioned
# region; furi_hal_region_is_provisioned makes apps refuse to start unprovisioned.
# Forcing both true allows transmit on any frequency the CC1101 hardware can
# tune (the ~300-348 / 387-464 / 779-928 MHz PLL bands remain: they are physical,
# not regulatory). Transmitting outside your local allocation may be illegal;
# you are responsible for legal operation. Build with SUBGHZ_UNLOCK=0 to skip.
if [ "${SUBGHZ_UNLOCK:-1}" = "1" ]; then
python3 - targets/f7/furi_hal/furi_hal_region.c <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
patches = [
    ("bool furi_hal_region_is_frequency_allowed(uint32_t frequency) {\n    return furi_hal_region_get_band(frequency) != NULL;\n}",
     "bool furi_hal_region_is_frequency_allowed(uint32_t frequency) {\n    UNUSED(frequency);\n    return true; /* region limits removed (research build) */\n}"),
    ("bool furi_hal_region_is_provisioned(void) {\n    return furi_hal_region_get() != NULL;\n}",
     "bool furi_hal_region_is_provisioned(void) {\n    return true; /* region limits removed (research build) */\n}"),
]
changed = False
for old, new in patches:
    if old in s:
        s = s.replace(old, new, 1)
        changed = True
if changed:
    open(p, "w").write(s)
    print(">> patched " + p + ": sub-GHz region/frequency TX limits removed")
PY
fi

# Stage each app into applications_user, with the shared libble sources copied
# alongside so the fap glob (*.c) picks them up.
mkdir -p applications_user
for app in "${APPS[@]}"; do
    rm -rf "applications_user/$app"
    cp -r "$HERE/$app" "applications_user/$app"
    cp "$HERE"/libble/*.c "$HERE"/libble/*.h "applications_user/$app/"
done

FBT_ARGS=(
    COMPACT=1 DEBUG=0 # release build, same as official images
    COPRO_STACK_TYPE=ble_full
    COPRO_STACK_BIN=stm32wb5x_BLE_Stack_full_fw.bin
)

API_CSV=targets/f7/api_symbols.csv
# BLE_API selects which ST command headers are exported to apps (each exported
# function is kept in the firmware image):
#   hci,gatt  scanning, connections, GATT client/server  (~12 KB, default)
#   all       also gap, hal, l2cap                       (~20 KB)
#   min       only the functions BT Inspector imports    (~1.5 KB)
BLE_API="${BLE_API:-hci,gatt}"

# --- Firmware API exported to .fap apps (in addition to the ST BLE commands) ---
# Each area is appended below; every enabled symbol stays in the firmware image.
# BLE peripheral internals: GAP control (advertised name, custom GATT server,
# extra-beacon config, connection state) and BLE-glue radio-stack status.
EXPORT_ENABLE="${EXPORT_ENABLE:-}"
EXPORT_ENABLE+='
^gap_
^ble_glue_(start|stop|get_c2_status|get_hardfault_info|force_c2_mode)$
^furi_hal_bt_init$
'
# Power / battery internals: the gas-gauge readings (voltage, current,
# temperature, capacity, health, charge state) are already exported by stock
# firmware; this adds the remaining safe reader. furi_hal_power_init stays
# disabled (re-initialising the power HAL from an app is unsafe).
EXPORT_ENABLE+='
^furi_hal_power_insomnia_level$
'
# Flash + option-byte internals: geometry, free-page info, and raw program /
# erase / write / option-byte access. These let an app read and modify internal
# flash directly. WARNING: misuse can brick the device or wipe storage; enabled
# here for research. Set EXPORT_FLASH=0 before building to leave them disabled.
if [ "${EXPORT_FLASH:-1}" = "1" ]; then
EXPORT_ENABLE+='
^furi_hal_flash_
'
fi
# RPC + loader internals: the public rpc_* / loader_* APIs (session open/feed/
# close, app data exchange, launch/enqueue/lock/menu) are already exported by
# stock firmware, so nothing is force-enabled here; this build only adds the
# internal loader header set so apps can use the internal Loader types. The
# internal helper functions stay unexported (see the "?" policy below): they are
# not linked into the API table, and exporting them breaks the firmware link.
export EXPORT_ENABLE

# Add the loader internal headers to the SDK (rpc_i.h is skipped: it pulls the
# protobuf headers, which are not part of the app SDK).
python3 - applications/services/loader/application.fam <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
want = '''        "loader.h",
        "loader_i.h",
        "loader_menu.h",
        "loader_queue.h",
        "loader_applications.h",
        "firmware_api/firmware_api.h",'''
have = '''        "loader.h",
        "firmware_api/firmware_api.h",'''
if "loader_i.h" not in s:
    s = s.replace(have, want, 1)
    open(p, "w").write(s)
    print(">> patched " + p + ": loader internal headers added to SDK")
PY
if grep -q "^Function,?," "$API_CSV" || ! grep -qE "^Header,\+,.*ble_hci_le\.h" "$API_CSV" \
   || ! grep -qE "^Header,\+,.*loader_i\.h" "$API_CSV"; then
    # fbt notices the new headers, rewrites the csv with '?' entries and stops.
    echo ">> syncing API symbol table (expected to stop once)"
    ./fbt -j"${JOBS:-4}" "${FBT_ARGS[@]}" api_check || true
fi
python3 - "$API_CSV" "$BLE_API" "$APP_SRC" <<'PY'
import os, re, sys, glob
csv_path, mode, app_src = sys.argv[1:4]
hdrs = {"hci": "ble_hci_le", "gap": "ble_gap_aci", "gatt": "ble_gatt_aci", "hal": "ble_hal_aci", "l2cap": "ble_l2cap_aci"}
owner = {}
for key, name in hdrs.items():
    src = open(f"lib/stm32wb_copro/wpan/ble/core/auto/{name}.h").read()
    for fn in re.findall(r"^tBleStatus (\w+)\(", src, re.M):
        owner[fn] = key
if mode == "all":
    selected = set(owner)
elif mode == "min":
    selected = set()
    for f in glob.glob(app_src + "/*.c"):
        selected |= set(re.findall(r"\b((?:aci_|hci_)\w+)\(", open(f).read()))
else:
    want = set(mode.split(","))
    selected = {fn for fn, key in owner.items() if key in want}
# Firmware symbols to force-enable, one regex per line, from $EXPORT_ENABLE.
enable_res = [re.compile(p) for p in os.environ.get("EXPORT_ENABLE", "").split() if p]
out, n_on, n_fw = [], 0, 0
for line in open(csv_path).read().splitlines():
    parts = line.split(",")
    if len(parts) >= 3 and parts[0] == "Function" and parts[2] in owner:
        parts[1] = "+" if parts[2] in selected else "-"
        n_on += parts[1] == "+"
    elif len(parts) >= 3 and parts[0] in ("Function", "Variable") and any(r.search(parts[2]) for r in enable_res):
        if parts[1] != "+":
            n_fw += 1
        parts[1] = "+"
    elif len(parts) >= 2 and parts[1] == "?":
        # Headers are always enabled (their declarations must be available to the
        # API table). New functions/variables default to disabled: only the ones
        # explicitly enabled (BLE commands, EXPORT_ENABLE) are exported, so
        # internal helpers pulled in by added headers stay out of the table.
        parts[1] = "+" if parts[0] == "Header" else "-"
    out.append(",".join(parts))
open(csv_path, "w").write("\n".join(out) + "\n")
print(f">> BLE API export ({mode}): {n_on} of {len(owner)} aci_*/hci_* functions enabled")
if enable_res:
    print(f">> firmware API export: {n_fw} extra symbol(s) newly enabled")
PY

if [ "${1:-}" = "fap" ]; then
    ./fbt -j"${JOBS:-4}" "${FBT_ARGS[@]}" "${APPS[@]/#/fap_}"
    ls build/f7-firmware-C/.extapps/*.fap
    exit 0
fi

echo ">> building update package (full BLE stack + apps)"
./fbt -j"${JOBS:-4}" "${FBT_ARGS[@]}" updater_package

PKG=$(ls -d dist/f7-C/f7-update-* 2>/dev/null | head -1)
echo
echo ">> update package: $PKG"
echo ">> radio stack in package:"
grep -E "^Radio" "$PKG/update.fuf" || true
echo ">> firmware size:"
ls -l "$PKG/firmware.dfu"
echo ">> apps (also inside the package's resources, installed to apps/Bluetooth/):"
ls -l build/f7-firmware-C/.extapps/*.fap
echo ">> SDK for building the apps with ufbt: $(ls dist/f7-C/flipper-z-f7-sdk-*.zip)"
echo
echo "Install: copy '$PKG' to the SD card (e.g. /ext/update/) and run it from"
echo "the file browser, or use: ./build.sh flash"

if [ "${1:-}" = "flash" ]; then
    echo ">> flashing over USB"
    ./fbt -j"${JOBS:-4}" "${FBT_ARGS[@]}" flash_usb_full
fi
