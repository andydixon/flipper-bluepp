#!/usr/bin/env bash
# Build a Flipper Zero firmware update package that contains BT Inspector.
#
# Why a firmware build and not a plain .fap:
#   * The stock radio coprocessor image is ST's "BLE Light" stack: peripheral/
#     broadcaster only. Scanning and connecting as a central need the "full"
#     stack (COPRO_STACK_TYPE=ble_full, already supported by fbt).
#   * The firmware does not export the ST GAP/GATT/HCI command API to external
#     apps, so the app is compiled in as a built-in ("--extra-int-apps").
#
# Usage:
#   ./build.sh            build  -> flipperzero-firmware/dist/f7-C/f7-update-*/ (update package)
#   ./build.sh flash      build and flash over USB (Flipper connected, qFlipper closed)
#   FW_TAG=1.4.3 ./build.sh   pin a different firmware tag (default 1.4.3)
#   JOBS=2 ./build.sh         limit parallel compile jobs (default 4)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_SRC="$HERE/bt_inspector"
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

# Link (or refresh) the app into the user apps directory.
mkdir -p applications_user
rm -rf applications_user/bt_inspector
cp -r "$APP_SRC" applications_user/bt_inspector

FBT_ARGS=(
    COMPACT=1 DEBUG=0 # release build, same as official images
    COPRO_STACK_TYPE=ble_full
    COPRO_STACK_BIN=stm32wb5x_BLE_Stack_full_fw.bin
    --extra-int-apps=bt_inspector
)

echo ">> building update package (full BLE stack + BT Inspector)"
./fbt -j"${JOBS:-4}" "${FBT_ARGS[@]}" updater_package

PKG=$(ls -d dist/f7-C/f7-update-* 2>/dev/null | head -1)
echo
echo ">> update package: $PKG"
echo ">> radio stack in package:"
grep -E "^Radio" "$PKG/update.fuf" || true
echo ">> firmware size:"
ls -l "$PKG/firmware.dfu"
echo
echo "Install: copy '$PKG' to the SD card (e.g. /ext/update/) and run it from"
echo "the file browser, or use: ./build.sh flash"

if [ "${1:-}" = "flash" ]; then
    echo ">> flashing over USB"
    ./fbt -j"${JOBS:-4}" "${FBT_ARGS[@]}" flash_usb_full
fi
