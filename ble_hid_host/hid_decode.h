// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andy Dixon <andy@dixon.cx>
#pragma once
#include <furi.h>
#include <stdint.h>

// Human-readable name for a USB HID keyboard usage id (0 => NULL).
const char* hid_key_name(uint8_t usage);
// Append active modifier names ("Ctrl+Shift+") for a boot-keyboard modifier byte.
void hid_mods_str(FuriString* out, uint8_t mods);
