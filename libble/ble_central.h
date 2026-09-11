#pragma once
#include <furi.h>
#include <stdint.h>
#include <stdbool.h>

#define BC_MAX_DEVICES 32
#define BC_ADV_MAX 31
#define BC_NAME_MAX 29
#define BC_MAX_SERVICES 16
#define BC_MAX_CHARS 48
#define BC_VALUE_MAX 256
#define BC_STALE_MS 60000

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type; // 0 public, 1 random (resolved 2/3 mapped down)
    uint8_t evt_type; // last non-SCAN_RSP advertising type
    int8_t rssi;
    int8_t rssi_max;
    uint32_t first_seen; // furi ticks
    uint32_t last_seen;
    uint32_t packets;
    uint8_t adv[BC_ADV_MAX];
    uint8_t adv_len;
    uint8_t rsp[BC_ADV_MAX];
    uint8_t rsp_len;
    char name[BC_NAME_MAX + 1];
    bool used;
    bool logged; // false until the UI has written it to the log
} BcDevice;

typedef struct {
    uint16_t start, end;
    uint8_t uuid_len;
    uint8_t uuid[16];
} BcService;

typedef struct {
    uint16_t decl_handle, value_handle, cccd_handle;
    uint8_t props; // CHAR_PROP_* bits
    uint8_t uuid_len;
    uint8_t uuid[16];
} BcChar;

typedef void (*BcNotifyCb)(uint16_t value_handle, const uint8_t* data, uint8_t len, void* ctx);

typedef struct BleCentral BleCentral;

bool bc_supported(void); // true when the copro runs the full BLE stack
BleCentral* bc_alloc(BcNotifyCb cb, void* ctx);
void bc_free(BleCentral* bc);

bool bc_scan_start(BleCentral* bc);
void bc_scan_stop(BleCentral* bc);
// Copies live devices sorted by RSSI, evicts stale ones, marks returned ones as logged.
size_t bc_snapshot(BleCentral* bc, BcDevice* out, size_t max);
bool bc_get_device(BleCentral* bc, const uint8_t* addr, BcDevice* out);

bool bc_connect(BleCentral* bc, const BcDevice* dev, uint32_t timeout_ms);
void bc_disconnect(BleCentral* bc);
bool bc_is_connected(BleCentral* bc);
uint16_t bc_mtu(BleCentral* bc);
uint8_t bc_last_error(BleCentral* bc); // ATT/BLE error, 0xFE timeout, 0xFD disconnected

int bc_discover_services(BleCentral* bc, BcService* out, size_t max);
int bc_discover_chars(BleCentral* bc, const BcService* svc, BcChar* out, size_t max);
int bc_read(BleCentral* bc, uint16_t value_handle, uint8_t* out, size_t max);
bool bc_write(BleCentral* bc, uint16_t value_handle, const uint8_t* data, uint8_t len, bool with_resp);
// end_handle: last handle that can belong to this characteristic (next decl - 1, or service end)
bool bc_set_notify(BleCentral* bc, BcChar* c, uint16_t end_handle, bool enable);

// First AD structure of `type` in adv data; returns payload pointer or NULL.
const uint8_t* bc_adv_find(const uint8_t* data, uint8_t len, uint8_t type, uint8_t* out_len);
