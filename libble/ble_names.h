// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andy Dixon <andy@dixon.cx>
#pragma once
#include <furi.h>
#include <stdint.h>

// Lookup tables and decoders. All return static strings or NULL when unknown.
const char* ble_company_name(uint16_t id);
const char* ble_uuid_name(const uint8_t* uuid, uint8_t len); // len 2 or 16, little-endian
const char* ble_appearance_name(uint16_t appearance);
const char* ble_adv_type_name(uint8_t evt_type);
const char* ble_att_error_name(uint8_t code);

void ble_hex(FuriString* out, const uint8_t* data, size_t len);
void ble_uuid_str(FuriString* out, const uint8_t* uuid, uint8_t len);
void ble_addr_str(FuriString* out, const uint8_t* addr);

// Manufacturer-specific data decoders (Apple Continuity, Microsoft CDP, ...)
void ble_decode_mfg(FuriString* out, uint16_t company, const uint8_t* d, uint8_t len);
// Service data decoders (Fast Pair, Eddystone, Exposure Notification, ...)
void ble_decode_svc_data(FuriString* out, uint16_t uuid, const uint8_t* d, uint8_t len);
// Characteristic value decoder: appends "decoded" representation(s) of a value.
void ble_decode_value(FuriString* out, const uint8_t* uuid, uint8_t uuid_len, const uint8_t* d, size_t len);
