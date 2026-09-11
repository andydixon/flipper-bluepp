// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andy Dixon <andy@dixon.cx>
#include "ble_names.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

typedef struct {
    uint16_t id;
    const char* name;
} IdName;

static const char* lookup(const IdName* t, size_t n, uint16_t id) {
    for(size_t i = 0; i < n; i++)
        if(t[i].id == id) return t[i].name;
    return NULL;
}
#define LOOKUP(t, id) lookup(t, COUNT_OF(t), id)

// ponytail: curated subset of the SIG company table; unknown IDs print as hex.
static const IdName companies[] = {
    {0x0000, "Ericsson"},      {0x0001, "Nokia"},         {0x0002, "Intel"},
    {0x0003, "IBM"},           {0x0004, "Toshiba"},       {0x0006, "Microsoft"},
    {0x0008, "Motorola"},      {0x000A, "CSR"},           {0x000D, "Texas Instr."},
    {0x000F, "Broadcom"},      {0x001D, "Qualcomm"},      {0x0025, "NXP"},
    {0x0030, "ST Micro"},      {0x0046, "MediaTek"},      {0x0047, "Bluegiga"},
    {0x004C, "Apple"},         {0x004F, "Nike"},          {0x0059, "Nordic Semi"},
    {0x005D, "Realtek"},       {0x0065, "HP"},            {0x006B, "Polar"},
    {0x0075, "Samsung"},       {0x0087, "Garmin"},        {0x009E, "Bose"},
    {0x00C4, "LG"},            {0x00D2, "Dialog Semi"},   {0x00E0, "Google"},
    {0x0110, "Nikon"},         {0x012D, "Sony"},          {0x0131, "Cypress"},
    {0x0157, "Huami/Amazfit"}, {0x0171, "Amazon"},        {0x01DA, "Logitech"},
    {0x02E5, "Espressif"},     {0x038F, "Xiaomi"},        {0x0499, "Ruuvi"},
    {0x05A7, "Sonos"},         {0x0822, "Adafruit"},
};

static const IdName uuids[] = {
    // Services
    {0x1800, "Generic Access"},         {0x1801, "Generic Attribute"},
    {0x1802, "Immediate Alert"},        {0x1803, "Link Loss"},
    {0x1804, "Tx Power"},               {0x1805, "Current Time"},
    {0x1808, "Glucose"},                {0x1809, "Health Thermometer"},
    {0x180A, "Device Information"},     {0x180D, "Heart Rate"},
    {0x180F, "Battery"},                {0x1810, "Blood Pressure"},
    {0x1811, "Alert Notification"},     {0x1812, "HID"},
    {0x1813, "Scan Parameters"},        {0x1814, "Running Speed/Cadence"},
    {0x1815, "Automation IO"},          {0x1816, "Cycling Speed/Cadence"},
    {0x1818, "Cycling Power"},          {0x1819, "Location/Navigation"},
    {0x181A, "Environmental Sensing"},  {0x181B, "Body Composition"},
    {0x181C, "User Data"},              {0x181D, "Weight Scale"},
    {0x181E, "Bond Management"},        {0x181F, "Cont. Glucose Monitor"},
    {0x1822, "Pulse Oximeter"},         {0x1824, "Transport Discovery"},
    {0x1826, "Fitness Machine"},        {0x1827, "Mesh Provisioning"},
    {0x1828, "Mesh Proxy"},             {0x1843, "Audio Input Control"},
    {0x1844, "Volume Control"},         {0x1846, "Coordinated Set Id"},
    {0x1848, "Media Control"},          {0x184E, "Audio Stream Control"},
    {0x1850, "Published Audio Cap."},   {0x1854, "Hearing Access"},
    {0x1855, "Telephony/Media Audio"},
    {0xFE2C, "Google Fast Pair"},       {0xFEAA, "Eddystone"},
    {0xFD6F, "Exposure Notification"},  {0xFE95, "Xiaomi Mi"},
    {0xFDAB, "Xiaomi"},                 {0xFEE7, "Tencent"},
    {0xFE59, "Nordic DFU"},             {0xFEF5, "Dialog"},
    {0xFE07, "Sonos"},                  {0xFEBE, "Bose"},
    {0xFEED, "Tile"},                   {0xFEEC, "Tile"},
    {0xFE9F, "Google"},                 {0xFEF3, "Google Nearby"},
    // Characteristics
    {0x2A00, "Device Name"},            {0x2A01, "Appearance"},
    {0x2A02, "Peripheral Privacy Flag"},{0x2A03, "Reconnection Address"},
    {0x2A04, "Pref. Conn. Parameters"}, {0x2A05, "Service Changed"},
    {0x2A06, "Alert Level"},            {0x2A07, "Tx Power Level"},
    {0x2A08, "Date Time"},              {0x2A19, "Battery Level"},
    {0x2A1C, "Temperature Measurement"},{0x2A23, "System ID"},
    {0x2A24, "Model Number"},           {0x2A25, "Serial Number"},
    {0x2A26, "Firmware Revision"},      {0x2A27, "Hardware Revision"},
    {0x2A28, "Software Revision"},      {0x2A29, "Manufacturer Name"},
    {0x2A2A, "IEEE Regulatory Cert"},   {0x2A2B, "Current Time"},
    {0x2A37, "Heart Rate Measurement"}, {0x2A38, "Body Sensor Location"},
    {0x2A39, "Heart Rate Control Pt"},  {0x2A4A, "HID Information"},
    {0x2A4B, "Report Map"},             {0x2A4C, "HID Control Point"},
    {0x2A4D, "Report"},                 {0x2A4E, "Protocol Mode"},
    {0x2A50, "PnP ID"},                 {0x2A53, "RSC Measurement"},
    {0x2A5B, "CSC Measurement"},        {0x2A63, "Cycling Power Meas."},
    {0x2A6D, "Pressure"},               {0x2A6E, "Temperature"},
    {0x2A6F, "Humidity"},               {0x2A9D, "Weight Measurement"},
    {0x2AA6, "Central Addr Resolution"},{0x2AC9, "Resolvable Priv Addr Only"},
    {0x2AD2, "Indoor Bike Data"},       {0x2B29, "Client Supported Features"},
    {0x2B2A, "Database Hash"},          {0x2B3A, "Server Supported Features"},
    // Descriptors
    {0x2900, "Extended Properties"},    {0x2901, "User Description"},
    {0x2902, "Client Char Config"},     {0x2903, "Server Char Config"},
    {0x2904, "Presentation Format"},    {0x2906, "Valid Range"},
    {0x2908, "Report Reference"},
};

static const char* appearances[] = {
    "Unknown",         "Phone",          "Computer",        "Watch",
    "Clock",           "Display",        "Remote Control",  "Eye-glasses",
    "Tag",             "Keyring",        "Media Player",    "Barcode Scanner",
    "Thermometer",     "Heart Rate Sensor", "Blood Pressure", "HID",
    "Glucose Meter",   "Running Sensor", "Cycling",         "Control Device",
    "Network Device",  "Sensor",         "Light Fixture",   "Fan",
    "HVAC",            "Air Conditioning", "Humidifier",    "Heating",
    "Access Control",  "Motorized Device", "Power Device",  "Light Source",
    "Window Covering", "Audio Sink",     "Audio Source",    "Motorized Vehicle",
    "Domestic Appliance", "Wearable Audio", "Aircraft",     "AV Equipment",
    "Display Equipment", "Hearing Aid",  "Gaming",          "Signage",
};

static const char* hid_sub[] = {
    "HID", "Keyboard", "Mouse", "Joystick", "Gamepad", "Digitizer",
    "Card Reader", "Digital Pen", "Barcode Scanner", "Touchpad", "Presentation Remote"};

const char* ble_company_name(uint16_t id) {
    return LOOKUP(companies, id);
}

static const uint8_t sig_base[16] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0, 0, 0x00, 0x00};

typedef struct {
    const char* uuid; // canonical text, upper case
    const char* name;
} UuidName;
static const UuidName uuids128[] = {
    {"6E400001-B5A3-F393-E0A9-E50E24DCCA9E", "Nordic UART"},
    {"6E400002-B5A3-F393-E0A9-E50E24DCCA9E", "Nordic UART RX"},
    {"6E400003-B5A3-F393-E0A9-E50E24DCCA9E", "Nordic UART TX"},
    {"7905F431-B5CE-4E99-A40F-4B1E122D00D0", "Apple Notification Center"},
    {"89D3502B-0F36-433A-8EF4-C502AD55F8DC", "Apple Media"},
    {"D0611E78-BBB4-4591-A5F8-487910AE4366", "Apple Continuity"},
    {"9FA480E0-4967-4542-9390-D343DC5D04AE", "Apple Nearby"},
    {"8667556C-9A37-4C91-84ED-54EE27D90049", "Apple Continuity Char"},
    {"AF0BADB1-5B99-43CD-917A-A77BC549E3CC", "Apple Nearby Char"},
};

const char* ble_uuid_name(const uint8_t* uuid, uint8_t len) {
    if(len == 2) return LOOKUP(uuids, uuid[0] | (uuid[1] << 8));
    if(len != 16) return NULL;
    if(memcmp(uuid, sig_base, 12) == 0 && uuid[14] == 0 && uuid[15] == 0)
        return LOOKUP(uuids, uuid[12] | (uuid[13] << 8));
    FuriString* s = furi_string_alloc();
    ble_uuid_str(s, uuid, 16);
    const char* r = NULL;
    for(size_t i = 0; i < COUNT_OF(uuids128); i++)
        if(furi_string_cmp_str(s, uuids128[i].uuid) == 0) {
            r = uuids128[i].name;
            break;
        }
    furi_string_free(s);
    return r;
}

const char* ble_appearance_name(uint16_t a) {
    uint16_t cat = a >> 6, sub = a & 0x3F;
    if(cat == 15 && sub < COUNT_OF(hid_sub)) return hid_sub[sub];
    if(cat < COUNT_OF(appearances)) return appearances[cat];
    if(cat == 49) return "Pulse Oximeter";
    if(cat == 50) return "Weight Scale";
    if(cat == 52) return "Glucose Monitor";
    return NULL;
}

const char* ble_adv_type_name(uint8_t t) {
    static const char* n[] = {"ADV_IND", "ADV_DIRECT_IND", "ADV_SCAN_IND", "ADV_NONCONN_IND", "SCAN_RSP"};
    return t < COUNT_OF(n) ? n[t] : "?";
}

const char* ble_att_error_name(uint8_t c) {
    static const IdName e[] = {
        {0x01, "Invalid handle"},     {0x02, "Read not permitted"},
        {0x03, "Write not permitted"},{0x04, "Invalid PDU"},
        {0x05, "Insufficient auth"},  {0x06, "Not supported"},
        {0x07, "Invalid offset"},     {0x08, "Insufficient authz"},
        {0x0A, "Attribute not found"},{0x0D, "Invalid length"},
        {0x0E, "Unlikely error"},     {0x0F, "Insufficient encryption"},
        {0x11, "Insufficient resources"},
        {0xFD, "Disconnected"},       {0xFE, "Timeout"},
    };
    return LOOKUP(e, c);
}

void ble_hex(FuriString* out, const uint8_t* data, size_t len) {
    for(size_t i = 0; i < len; i++) furi_string_cat_printf(out, "%s%02X", i ? " " : "", data[i]);
}

void ble_uuid_str(FuriString* out, const uint8_t* u, uint8_t len) {
    if(len == 2) {
        furi_string_cat_printf(out, "%04X", u[0] | (u[1] << 8));
        return;
    }
    // 128-bit UUIDs are little-endian on the wire
    for(int i = 15; i >= 0; i--) {
        furi_string_cat_printf(out, "%02X", u[i]);
        if(i == 12 || i == 10 || i == 8 || i == 6) furi_string_cat_str(out, "-");
    }
}

void ble_addr_str(FuriString* out, const uint8_t* a) {
    furi_string_cat_printf(
        out, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

// ---- Apple Continuity -------------------------------------------------------
// ponytail: community-documented, unofficial; model/battery layouts may drift.
static const IdName apple_models[] = {
    {0x0220, "AirPods"},           {0x0F20, "AirPods 2"},        {0x1320, "AirPods 3"},
    {0x1920, "AirPods 4"},         {0x1B20, "AirPods 4 ANC"},    {0x0E20, "AirPods Pro"},
    {0x1420, "AirPods Pro 2"},     {0x2420, "AirPods Pro 2 USB-C"},
    {0x0A20, "AirPods Max"},       {0x1F20, "AirPods Max USB-C"},
    {0x0320, "Powerbeats3"},       {0x0B20, "Powerbeats Pro"},   {0x1220, "Powerbeats 4"},
    {0x0520, "BeatsX"},            {0x0620, "Beats Solo3"},      {0x0920, "Beats Studio3"},
    {0x1020, "Beats Flex"},        {0x1120, "Beats Studio Buds"},{0x1620, "Beats Fit Pro"},
    {0x1720, "Beats Studio Buds+"},{0x1820, "Beats Studio Pro"}, {0x1C20, "Beats Solo Buds"},
};

static const IdName apple_nearby_actions[] = {
    {0x01, "Apple TV Setup"},   {0x04, "Mobile Backup"},  {0x05, "Watch Setup"},
    {0x06, "Apple TV Pair"},    {0x07, "Internet Relay"}, {0x08, "WiFi Password"},
    {0x09, "iOS Setup"},        {0x0A, "Repair"},         {0x0B, "Speaker Setup"},
    {0x0C, "Apple Pay"},        {0x0D, "Whole Home Audio"}, {0x0E, "Dev Tools Pairing"},
    {0x0F, "Answered Call"},    {0x10, "Ended Call"},     {0x11, "DD Ping"},
    {0x12, "DD Pong"},          {0x13, "Remote AutoFill"},{0x14, "Companion Link"},
    {0x15, "Remote Mgmt"},      {0x17, "Remote Display"},
};

static const IdName apple_nearby_info[] = {
    {0x01, "Activity reporting off"}, {0x03, "Idle"},
    {0x05, "Audio, screen off"},      {0x07, "Screen on"},
    {0x09, "Video playing"},          {0x0A, "Watch on wrist"},
    {0x0B, "Recent interaction"},     {0x0D, "Driving"},
    {0x0E, "Phone call"},
};

static void pct(FuriString* out, const char* label, uint8_t nib) {
    if(nib == 0xF)
        furi_string_cat_printf(out, " %s:?", label);
    else
        furi_string_cat_printf(out, " %s:%u%%", label, nib * 10);
}

static void decode_apple(FuriString* out, const uint8_t* d, uint8_t len) {
    uint8_t i = 0;
    while(i + 2 <= len) {
        uint8_t type = d[i], l = d[i + 1];
        const uint8_t* p = d + i + 2;
        if(i + 2 + l > len) l = len - i - 2;
        switch(type) {
        case 0x02:
            furi_string_cat_str(out, "  iBeacon");
            if(l >= 21) { // 16 UUID + 2 major + 2 minor + 1 Tx; p[20] is the Tx byte
                furi_string_cat_str(out, " ");
                for(int k = 0; k < 16; k++) furi_string_cat_printf(out, "%02X", p[k]);
                furi_string_cat_printf(
                    out, "\n  major %u minor %u tx %d", (p[16] << 8) | p[17], (p[18] << 8) | p[19],
                    (int8_t)p[20]);
            }
            furi_string_cat_str(out, "\n");
            break;
        case 0x05: furi_string_cat_str(out, "  AirDrop\n"); break;
        case 0x07: {
            furi_string_cat_str(out, "  Proximity Pairing");
            if(l >= 3) {
                uint16_t model = (p[1] << 8) | p[2];
                const char* m = LOOKUP(apple_models, model);
                if(m)
                    furi_string_cat_printf(out, ": %s", m);
                else
                    furi_string_cat_printf(out, ": model %04X", model);
            }
            if(l >= 6) {
                furi_string_cat_str(out, "\n  Battery");
                pct(out, "L", p[4] >> 4);
                pct(out, "R", p[4] & 0xF);
                pct(out, "Case", p[5] & 0xF);
                if(p[5] & 0x70)
                    furi_string_cat_printf(
                        out, " chg:%s%s%s", (p[5] & 0x20) ? "L" : "", (p[5] & 0x10) ? "R" : "",
                        (p[5] & 0x40) ? "C" : "");
            }
            furi_string_cat_str(out, "\n");
        } break;
        case 0x09: furi_string_cat_str(out, "  AirPlay Target\n"); break;
        case 0x0A: furi_string_cat_str(out, "  AirPlay Source\n"); break;
        case 0x0B: furi_string_cat_str(out, "  Magic Switch (Watch)\n"); break;
        case 0x0C: furi_string_cat_str(out, "  Handoff\n"); break;
        case 0x0D: furi_string_cat_str(out, "  Tethering Target\n"); break;
        case 0x0E: furi_string_cat_str(out, "  Tethering Source\n"); break;
        case 0x0F: {
            const char* a = l >= 2 ? LOOKUP(apple_nearby_actions, p[1]) : NULL;
            furi_string_cat_printf(out, "  Nearby Action: %s\n", a ? a : "?");
        } break;
        case 0x10: {
            const char* s = l >= 1 ? LOOKUP(apple_nearby_info, p[0] & 0x0F) : NULL;
            furi_string_cat_printf(out, "  Nearby Info: %s\n", s ? s : "iOS device");
        } break;
        case 0x12: furi_string_cat_str(out, "  Find My\n"); break;
        default: furi_string_cat_printf(out, "  Type %02X (%u B)\n", type, l); break;
        }
        i += 2 + l;
    }
}

// ---- Microsoft Connected Devices Platform beacon ---------------------------
static const IdName ms_types[] = {
    {1, "Xbox One"},        {6, "iPhone"},          {7, "iPad"},
    {8, "Android"},         {9, "Windows Desktop"}, {11, "Windows Phone"},
    {12, "Linux"},          {13, "Windows IoT"},    {14, "Surface Hub"},
    {15, "Windows Laptop"}, {16, "Windows Tablet"},
};

void ble_decode_mfg(FuriString* out, uint16_t company, const uint8_t* d, uint8_t len) {
    if(company == 0x004C) {
        decode_apple(out, d, len);
    } else if(company == 0x0006 && len >= 2 && d[0] == 0x01) {
        const char* t = LOOKUP(ms_types, d[1] & 0x1F);
        furi_string_cat_printf(out, "  Windows CDP: %s\n", t ? t : "?");
    }
}

void ble_decode_svc_data(FuriString* out, uint16_t uuid, const uint8_t* d, uint8_t len) {
    if(uuid == 0xFE2C && len >= 3) {
        furi_string_cat_printf(out, "  Fast Pair model %02X%02X%02X\n", d[0], d[1], d[2]);
    } else if(uuid == 0xFEAA && len >= 1) {
        const char* f = d[0] == 0x00 ? "UID" : d[0] == 0x10 ? "URL" : d[0] == 0x20 ? "TLM" : "?";
        furi_string_cat_printf(out, "  Eddystone %s\n", f);
        if(d[0] == 0x10 && len > 2) {
            static const char* pre[] = {"http://www.", "https://www.", "http://", "https://"};
            furi_string_cat_printf(out, "  %s", d[2] < 4 ? pre[d[2]] : "");
            for(uint8_t i = 3; i < len; i++)
                if(isprint(d[i])) furi_string_push_back(out, d[i]);
            furi_string_cat_str(out, "\n");
        }
    } else if(uuid == 0xFD6F) {
        furi_string_cat_str(out, "  COVID Exposure Notification\n");
    }
}

static bool all_printable(const uint8_t* d, size_t len) {
    if(!len) return false;
    for(size_t i = 0; i < len; i++)
        if(!isprint(d[i]) && d[i] != '\n' && d[i] != '\r' && d[i] != '\t') return false;
    return true;
}

void ble_decode_value(FuriString* out, const uint8_t* uuid, uint8_t uuid_len, const uint8_t* d, size_t len) {
    uint16_t u = 0;
    if(uuid_len == 2) u = uuid[0] | (uuid[1] << 8);
    else if(uuid_len == 16 && memcmp(uuid, sig_base, 12) == 0) u = uuid[12] | (uuid[13] << 8);

    switch(u) {
    case 0x2A19:
        if(len >= 1) furi_string_cat_printf(out, "%u%%\n", d[0]);
        return;
    case 0x2A01:
        if(len >= 2) {
            uint16_t a = d[0] | (d[1] << 8);
            const char* n = ble_appearance_name(a);
            furi_string_cat_printf(out, "%s (0x%04X)\n", n ? n : "?", a);
        }
        return;
    case 0x2A04:
        if(len >= 8)
            furi_string_cat_printf(
                out, "int %u-%u ms, lat %u, to %u ms\n", (d[0] | (d[1] << 8)) * 5 / 4,
                (d[2] | (d[3] << 8)) * 5 / 4, d[4] | (d[5] << 8), (d[6] | (d[7] << 8)) * 10);
        return;
    case 0x2A50:
        if(len >= 7)
            furi_string_cat_printf(
                out, "%s VID %04X PID %04X ver %04X\n", d[0] == 1 ? "SIG" : d[0] == 2 ? "USB" : "?",
                d[1] | (d[2] << 8), d[3] | (d[4] << 8), d[5] | (d[6] << 8));
        return;
    case 0x2A37:
        if(len >= 2) {
            uint16_t bpm = (d[0] & 1) ? (d[1] | (d[2] << 8)) : d[1];
            furi_string_cat_printf(out, "%u bpm\n", bpm);
        }
        return;
    case 0x2A6E:
        if(len >= 2) {
            int16_t t = d[0] | (d[1] << 8);
            furi_string_cat_printf(out, "%d.%02d C\n", t / 100, abs(t % 100));
        }
        return;
    case 0x2A6F:
        if(len >= 2) {
            uint16_t h = d[0] | (d[1] << 8);
            furi_string_cat_printf(out, "%u.%02u %%RH\n", h / 100, h % 100);
        }
        return;
    case 0x2A6D:
        if(len >= 4) {
            uint32_t p = d[0] | (d[1] << 8) | (d[2] << 16) | ((uint32_t)d[3] << 24);
            furi_string_cat_printf(out, "%lu.%lu hPa\n", p / 1000, (p / 100) % 10);
        }
        return;
    case 0x2A07:
        if(len >= 1) furi_string_cat_printf(out, "%d dBm\n", (int8_t)d[0]);
        return;
    case 0x2A2B:
    case 0x2A08:
        if(len >= 7)
            furi_string_cat_printf(
                out, "%04u-%02u-%02u %02u:%02u:%02u\n", d[0] | (d[1] << 8), d[2], d[3], d[4], d[5],
                d[6]);
        return;
    default: break;
    }
    // Generic: string if printable, plus small integer interpretations.
    if(all_printable(d, len)) {
        furi_string_cat_str(out, "\"");
        for(size_t i = 0; i < len; i++) furi_string_push_back(out, d[i]);
        furi_string_cat_str(out, "\"\n");
    }
    if(len == 1) furi_string_cat_printf(out, "u8 %u / i8 %d\n", d[0], (int8_t)d[0]);
    if(len == 2) furi_string_cat_printf(out, "u16 %u\n", d[0] | (d[1] << 8));
    if(len == 4)
        furi_string_cat_printf(
            out, "u32 %lu\n", (uint32_t)(d[0] | (d[1] << 8) | (d[2] << 16) | ((uint32_t)d[3] << 24)));
}
