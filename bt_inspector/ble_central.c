#include "ble_central.h"
#include <furi_hal.h>
#include <furi_ble/event_dispatcher.h>
#include <ble/ble.h>
#include <interface/patterns/ble_thread/tl/tl.h>
#include <string.h>
#include <stdlib.h>

#define TAG "BtInspector"

#define OWN_ADDR_TYPE GAP_PUBLIC_ADDR // firmware programs a public identity address
#define PROC_TIMEOUT_MS 5000

typedef enum {
    FlagConnected = 1 << 0,
    FlagConnFailed = 1 << 1,
    FlagDisconnected = 1 << 2,
    FlagDone = 1 << 3,
} BcFlag;

struct BleCentral {
    FuriMutex* mutex; // guards devices[]
    FuriEventFlag* flags;
    GapSvcEventHandler* handler;
    BcNotifyCb notify_cb;
    void* notify_ctx;

    BcDevice devices[BC_MAX_DEVICES];
    bool scanning;

    volatile bool connecting;
    volatile bool connected;
    volatile uint16_t conn_handle;
    uint16_t mtu;
    uint8_t proc_error; // from ACI_GATT_PROC_COMPLETE
    uint8_t att_error; // from ACI_GATT_ERROR_RESP
    uint8_t last_error;

    // procedure output buffers, owned by the caller of the running procedure
    BcService* svc_out;
    size_t svc_max, svc_count;
    BcChar* chr_out;
    size_t chr_max, chr_count;
    uint8_t* val_out;
    size_t val_max, val_len;
    uint16_t found_cccd;
};

// ---- advertising -----------------------------------------------------------

const uint8_t* bc_adv_find(const uint8_t* data, uint8_t len, uint8_t type, uint8_t* out_len) {
    uint8_t i = 0;
    while(i < len) {
        uint8_t l = data[i];
        if(l == 0 || i + 1 + l > len) break;
        if(data[i + 1] == type) {
            *out_len = l - 1;
            return data + i + 2;
        }
        i += 1 + l;
    }
    return NULL;
}

static void update_name(BcDevice* d) {
    uint8_t l = 0;
    const uint8_t* n = bc_adv_find(d->adv, d->adv_len, 0x09, &l);
    if(!n) n = bc_adv_find(d->rsp, d->rsp_len, 0x09, &l);
    if(!n) n = bc_adv_find(d->adv, d->adv_len, 0x08, &l);
    if(!n) n = bc_adv_find(d->rsp, d->rsp_len, 0x08, &l);
    if(!n) return;
    if(l > BC_NAME_MAX) l = BC_NAME_MAX;
    if(strlen(d->name) == l && memcmp(d->name, n, l) == 0) return;
    memcpy(d->name, n, l);
    d->name[l] = 0;
    d->logged = false; // re-log on name change
}

static void handle_adv_report(BleCentral* bc, const uint8_t* p, size_t len) {
    if(len < 1) return;
    uint8_t n = p[0];
    size_t i = 1;
    uint32_t now = furi_get_tick();
    furi_mutex_acquire(bc->mutex, FuriWaitForever);
    while(n-- && i + 9 <= len) {
        uint8_t evt = p[i], at = p[i + 1];
        const uint8_t* addr = p + i + 2;
        uint8_t dl = p[i + 8];
        const uint8_t* data = p + i + 9;
        if(i + 9 + dl + 1 > len) break;
        int8_t rssi = (int8_t)p[i + 9 + dl];
        i += 10 + dl;
        if(at > 1) at -= 2; // resolved private -> underlying type

        BcDevice* d = NULL;
        BcDevice* victim = NULL;
        for(size_t k = 0; k < BC_MAX_DEVICES; k++) {
            BcDevice* c = &bc->devices[k];
            if(c->used && c->addr_type == at && memcmp(c->addr, addr, 6) == 0) {
                d = c;
                break;
            }
            if(!victim || !c->used || (victim->used && c->last_seen < victim->last_seen)) victim = c;
        }
        if(!d) {
            if(evt == SCAN_RSP) continue; // never seen its advertisement; wait for it
            d = victim;
            memset(d, 0, sizeof(*d));
            d->used = true;
            memcpy(d->addr, addr, 6);
            d->addr_type = at;
            d->first_seen = now;
            d->rssi_max = rssi;
        }
        d->last_seen = now;
        d->packets++;
        d->rssi = rssi;
        if(rssi > d->rssi_max) d->rssi_max = rssi;
        if(dl > BC_ADV_MAX) dl = BC_ADV_MAX;
        if(evt == SCAN_RSP) {
            memcpy(d->rsp, data, dl);
            d->rsp_len = dl;
        } else {
            d->evt_type = evt;
            memcpy(d->adv, data, dl);
            d->adv_len = dl;
        }
        update_name(d);
    }
    furi_mutex_release(bc->mutex);
}

// ---- event handler (runs on the BLE glue thread) ---------------------------

static BleEventAckStatus bc_event_handler(void* event, void* context) {
    BleCentral* bc = context;
    hci_event_pckt* pkt = (hci_event_pckt*)((hci_uart_pckt*)event)->data;

    switch(pkt->evt) {
    case HCI_LE_META_EVT_CODE: {
        evt_le_meta_event* meta = (evt_le_meta_event*)pkt->data;
        if(meta->subevent == HCI_LE_ADVERTISING_REPORT_SUBEVT_CODE) {
            handle_adv_report(bc, meta->data, pkt->plen - 1);
            return BleEventAckFlowEnable;
        }
        if(meta->subevent == HCI_LE_CONNECTION_COMPLETE_SUBEVT_CODE ||
           meta->subevent == HCI_LE_ENHANCED_CONNECTION_COMPLETE_SUBEVT_CODE) {
            // both layouts start with Status, Connection_Handle, Role
            hci_le_connection_complete_event_rp0* e = (void*)meta->data;
            if(!bc->connecting) return BleEventNotAck;
            if(e->Status == 0 && e->Role != 0) return BleEventNotAck; // we are slave: not ours
            bc->connecting = false;
            if(e->Status == 0) {
                bc->conn_handle = e->Connection_Handle;
                bc->connected = true;
                furi_event_flag_set(bc->flags, FlagConnected);
            } else {
                bc->last_error = e->Status;
                furi_event_flag_set(bc->flags, FlagConnFailed);
            }
            return BleEventAckFlowEnable;
        }
        return BleEventNotAck;
    }
    case HCI_DISCONNECTION_COMPLETE_EVT_CODE: {
        hci_disconnection_complete_event_rp0* e = (void*)pkt->data;
        if(bc->connected && e->Connection_Handle == bc->conn_handle) {
            bc->connected = false;
            bc->conn_handle = 0;
            furi_event_flag_set(bc->flags, FlagDisconnected);
            return BleEventAckFlowEnable;
        }
        return BleEventNotAck;
    }
    case HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE: {
        evt_blecore_aci* aci = (evt_blecore_aci*)pkt->data;
        uint16_t ch = aci->data[0] | (aci->data[1] << 8); // every event below starts with it
        if(!bc->connected || ch != bc->conn_handle) return BleEventNotAck;

        switch(aci->ecode) {
        case ACI_ATT_READ_BY_GROUP_TYPE_RESP_VSEVT_CODE: {
            aci_att_read_by_group_type_resp_event_rp0* e = (void*)aci->data;
            uint8_t rl = e->Attribute_Data_Length;
            if(!bc->svc_out || (rl != 6 && rl != 20)) break;
            for(uint8_t i = 0; i + rl <= e->Data_Length && bc->svc_count < bc->svc_max; i += rl) {
                const uint8_t* r = e->Attribute_Data_List + i;
                BcService* s = &bc->svc_out[bc->svc_count++];
                s->start = r[0] | (r[1] << 8);
                s->end = r[2] | (r[3] << 8);
                s->uuid_len = rl - 4;
                memcpy(s->uuid, r + 4, s->uuid_len);
            }
        } break;
        case ACI_ATT_READ_BY_TYPE_RESP_VSEVT_CODE: {
            aci_att_read_by_type_resp_event_rp0* e = (void*)aci->data;
            uint8_t rl = e->Handle_Value_Pair_Length;
            if(!bc->chr_out || (rl != 7 && rl != 21)) break;
            for(uint8_t i = 0; i + rl <= e->Data_Length && bc->chr_count < bc->chr_max; i += rl) {
                const uint8_t* r = e->Handle_Value_Pair_Data + i;
                BcChar* c = &bc->chr_out[bc->chr_count++];
                memset(c, 0, sizeof(*c));
                c->decl_handle = r[0] | (r[1] << 8);
                c->props = r[2];
                c->value_handle = r[3] | (r[4] << 8);
                c->uuid_len = rl - 5;
                memcpy(c->uuid, r + 5, c->uuid_len);
            }
        } break;
        case ACI_ATT_FIND_INFO_RESP_VSEVT_CODE: {
            aci_att_find_info_resp_event_rp0* e = (void*)aci->data;
            uint8_t rl = e->Format == 1 ? 4 : 18;
            for(uint8_t i = 0; i + rl <= e->Event_Data_Length; i += rl) {
                const uint8_t* r = e->Handle_UUID_Pair + i;
                if(e->Format == 1 && r[2] == 0x02 && r[3] == 0x29 && !bc->found_cccd)
                    bc->found_cccd = r[0] | (r[1] << 8);
            }
        } break;
        case ACI_ATT_READ_RESP_VSEVT_CODE:
        case ACI_ATT_READ_BLOB_RESP_VSEVT_CODE: {
            aci_att_read_resp_event_rp0* e = (void*)aci->data;
            if(!bc->val_out) break;
            size_t n = e->Event_Data_Length;
            if(bc->val_len + n > bc->val_max) n = bc->val_max - bc->val_len;
            memcpy(bc->val_out + bc->val_len, e->Attribute_Value, n);
            bc->val_len += n;
        } break;
        case ACI_ATT_EXCHANGE_MTU_RESP_VSEVT_CODE: {
            aci_att_exchange_mtu_resp_event_rp0* e = (void*)aci->data;
            bc->mtu = e->Server_RX_MTU;
        } break;
        case ACI_GATT_ERROR_RESP_VSEVT_CODE: {
            aci_gatt_error_resp_event_rp0* e = (void*)aci->data;
            bc->att_error = e->Error_Code;
        } break;
        case ACI_GATT_PROC_COMPLETE_VSEVT_CODE: {
            aci_gatt_proc_complete_event_rp0* e = (void*)aci->data;
            bc->proc_error = e->Error_Code;
            furi_event_flag_set(bc->flags, FlagDone);
        } break;
        case ACI_GATT_PROC_TIMEOUT_VSEVT_CODE:
            bc->proc_error = 0xFE;
            furi_event_flag_set(bc->flags, FlagDone);
            break;
        case ACI_GATT_INDICATION_VSEVT_CODE:
        case ACI_GATT_NOTIFICATION_VSEVT_CODE: { // same layout
            if(aci->ecode == ACI_GATT_INDICATION_VSEVT_CODE) aci_gatt_confirm_indication(bc->conn_handle);
            aci_gatt_notification_event_rp0* e = (void*)aci->data;
            if(bc->notify_cb)
                bc->notify_cb(
                    e->Attribute_Handle, e->Attribute_Value, e->Attribute_Value_Length,
                    bc->notify_ctx);
        } break;
        default: break;
        }
        return BleEventAckFlowEnable;
    }
    default: return BleEventNotAck;
    }
}

// ---- lifecycle ---------------------------------------------------------------

bool bc_supported(void) {
    return furi_hal_bt_get_radio_stack() == FuriHalBtStackFull;
}

BleCentral* bc_alloc(BcNotifyCb cb, void* ctx) {
    BleCentral* bc = malloc(sizeof(BleCentral));
    bc->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    bc->flags = furi_event_flag_alloc();
    bc->notify_cb = cb;
    bc->notify_ctx = ctx;
    bc->mtu = 23;
    bc->handler = ble_event_dispatcher_register_svc_handler(bc_event_handler, bc);
    return bc;
}

void bc_free(BleCentral* bc) {
    bc_scan_stop(bc);
    if(bc->connected) bc_disconnect(bc);
    ble_event_dispatcher_unregister_svc_handler(bc->handler);
    furi_event_flag_free(bc->flags);
    furi_mutex_free(bc->mutex);
    free(bc);
}

// ---- scanning ----------------------------------------------------------------

bool bc_scan_start(BleCentral* bc) {
    if(bc->scanning) return true;
    // active scan, 50 ms interval / 30 ms window; no duplicate filtering so RSSI stays live
    tBleStatus st = hci_le_set_scan_parameters(0x01, 0x50, 0x30, OWN_ADDR_TYPE, 0x00);
    if(st == BLE_STATUS_SUCCESS) st = hci_le_set_scan_enable(0x01, 0x00);
    if(st != BLE_STATUS_SUCCESS) {
        FURI_LOG_E(TAG, "scan start failed: 0x%02X", st);
        bc->last_error = st;
        return false;
    }
    bc->scanning = true;
    return true;
}

void bc_scan_stop(BleCentral* bc) {
    if(!bc->scanning) return;
    hci_le_set_scan_enable(0x00, 0x00);
    bc->scanning = false;
}

static int cmp_rssi(const void* a, const void* b) {
    return ((const BcDevice*)b)->rssi - ((const BcDevice*)a)->rssi;
}

size_t bc_snapshot(BleCentral* bc, BcDevice* out, size_t max) {
    size_t n = 0;
    uint32_t now = furi_get_tick();
    furi_mutex_acquire(bc->mutex, FuriWaitForever);
    for(size_t k = 0; k < BC_MAX_DEVICES && n < max; k++) {
        BcDevice* d = &bc->devices[k];
        if(!d->used) continue;
        if(now - d->last_seen > BC_STALE_MS) {
            d->used = false;
            continue;
        }
        out[n++] = *d;
        d->logged = true;
    }
    furi_mutex_release(bc->mutex);
    qsort(out, n, sizeof(BcDevice), cmp_rssi);
    return n;
}

bool bc_get_device(BleCentral* bc, const uint8_t* addr, BcDevice* out) {
    bool found = false;
    furi_mutex_acquire(bc->mutex, FuriWaitForever);
    for(size_t k = 0; k < BC_MAX_DEVICES; k++) {
        BcDevice* d = &bc->devices[k];
        if(d->used && memcmp(d->addr, addr, 6) == 0) {
            *out = *d;
            found = true;
            break;
        }
    }
    furi_mutex_release(bc->mutex);
    return found;
}

// ---- connection --------------------------------------------------------------

static bool wait_done(BleCentral* bc, uint32_t timeout) {
    uint32_t f = furi_event_flag_wait(bc->flags, FlagDone | FlagDisconnected, FuriFlagWaitAny, timeout);
    if(f & FuriFlagError) {
        bc->last_error = 0xFE;
        return false;
    }
    if(f & FlagDisconnected) {
        bc->last_error = 0xFD;
        return false;
    }
    if(bc->proc_error) {
        bc->last_error = bc->att_error ? bc->att_error : bc->proc_error;
        return false;
    }
    return true;
}

static void proc_begin(BleCentral* bc) {
    furi_event_flag_clear(bc->flags, FlagDone);
    bc->proc_error = 0;
    bc->att_error = 0;
    bc->last_error = 0;
}

bool bc_connect(BleCentral* bc, const BcDevice* dev, uint32_t timeout_ms) {
    if(bc->connected) return true;
    bc_scan_stop(bc);
    furi_event_flag_clear(bc->flags, FlagConnected | FlagConnFailed | FlagDisconnected | FlagDone);
    bc->connecting = true;
    bc->mtu = 23;
    // scan 60/30 ms, conn interval 30-50 ms, latency 0, supervision 5 s
    tBleStatus st = hci_le_create_connection(
        0x60, 0x30, 0x00, dev->addr_type, dev->addr, OWN_ADDR_TYPE, 0x18, 0x28, 0, 0x01F4, 0, 0);
    if(st != BLE_STATUS_SUCCESS) {
        FURI_LOG_E(TAG, "create_connection failed: 0x%02X", st);
        bc->connecting = false;
        bc->last_error = st;
        return false;
    }
    uint32_t f = furi_event_flag_wait(bc->flags, FlagConnected | FlagConnFailed, FuriFlagWaitAny, timeout_ms);
    if(f & FuriFlagError) {
        hci_le_create_connection_cancel();
        f = furi_event_flag_wait(bc->flags, FlagConnected | FlagConnFailed, FuriFlagWaitAny, 2000);
        if(!(f & FlagConnected)) {
            bc->connecting = false;
            bc->last_error = 0xFE;
            return false;
        }
    }
    if(!(f & FlagConnected)) return false;

    proc_begin(bc);
    if(aci_gatt_exchange_config(bc->conn_handle) == BLE_STATUS_SUCCESS) wait_done(bc, 2000);
    return true;
}

void bc_disconnect(BleCentral* bc) {
    if(!bc->connected) return;
    furi_event_flag_clear(bc->flags, FlagDisconnected);
    if(hci_disconnect(bc->conn_handle, 0x13) == BLE_STATUS_SUCCESS)
        furi_event_flag_wait(bc->flags, FlagDisconnected, FuriFlagWaitAny, 3000);
    bc->connected = false;
    bc->conn_handle = 0;
}

bool bc_is_connected(BleCentral* bc) {
    return bc->connected;
}
uint16_t bc_mtu(BleCentral* bc) {
    return bc->mtu;
}
uint8_t bc_last_error(BleCentral* bc) {
    return bc->last_error;
}

// ---- GATT client -------------------------------------------------------------

int bc_discover_services(BleCentral* bc, BcService* out, size_t max) {
    if(!bc->connected) return -1;
    proc_begin(bc);
    bc->svc_out = out;
    bc->svc_max = max;
    bc->svc_count = 0;
    bool ok = aci_gatt_disc_all_primary_services(bc->conn_handle) == BLE_STATUS_SUCCESS &&
              wait_done(bc, PROC_TIMEOUT_MS);
    bc->svc_out = NULL;
    return (ok || bc->svc_count) ? (int)bc->svc_count : -1;
}

int bc_discover_chars(BleCentral* bc, const BcService* svc, BcChar* out, size_t max) {
    if(!bc->connected) return -1;
    proc_begin(bc);
    bc->chr_out = out;
    bc->chr_max = max;
    bc->chr_count = 0;
    bool ok = aci_gatt_disc_all_char_of_service(bc->conn_handle, svc->start, svc->end) ==
                  BLE_STATUS_SUCCESS &&
              wait_done(bc, PROC_TIMEOUT_MS);
    bc->chr_out = NULL;
    return (ok || bc->chr_count) ? (int)bc->chr_count : -1;
}

int bc_read(BleCentral* bc, uint16_t value_handle, uint8_t* out, size_t max) {
    if(!bc->connected) return -1;
    bc->val_out = out;
    bc->val_max = max;
    bc->val_len = 0;
    proc_begin(bc);
    bool ok = aci_gatt_read_char_value(bc->conn_handle, value_handle) == BLE_STATUS_SUCCESS &&
              wait_done(bc, PROC_TIMEOUT_MS);
    // A full-size response means there may be more: fetch the rest with read-long.
    size_t chunk = bc->mtu - 1;
    while(ok && bc->val_len % chunk == 0 && bc->val_len > 0 && bc->val_len < max) {
        size_t before = bc->val_len;
        proc_begin(bc);
        bool more = aci_gatt_read_long_char_value(bc->conn_handle, value_handle, before) ==
                        BLE_STATUS_SUCCESS &&
                    wait_done(bc, PROC_TIMEOUT_MS);
        if(!more || bc->val_len == before) break; // short/failed continuation: keep what we have
    }
    bc->val_out = NULL;
    return ok ? (int)bc->val_len : -1;
}

bool bc_write(BleCentral* bc, uint16_t value_handle, const uint8_t* data, uint8_t len, bool with_resp) {
    if(!bc->connected) return false;
    proc_begin(bc);
    tBleStatus st;
    if(with_resp) {
        st = aci_gatt_write_char_value(bc->conn_handle, value_handle, len, data);
        if(st != BLE_STATUS_SUCCESS) {
            bc->last_error = st;
            return false;
        }
        return wait_done(bc, PROC_TIMEOUT_MS);
    }
    st = aci_gatt_write_without_resp(bc->conn_handle, value_handle, len, data);
    bc->last_error = st;
    return st == BLE_STATUS_SUCCESS;
}

bool bc_set_notify(BleCentral* bc, BcChar* c, uint16_t end_handle, bool enable) {
    if(!bc->connected) return false;
    if(!c->cccd_handle) {
        if(end_handle <= c->value_handle) return false;
        proc_begin(bc);
        bc->found_cccd = 0;
        bool ok = aci_gatt_disc_all_char_desc(bc->conn_handle, c->value_handle + 1, end_handle) ==
                      BLE_STATUS_SUCCESS &&
                  wait_done(bc, PROC_TIMEOUT_MS);
        if(!bc->found_cccd) {
            if(ok) bc->last_error = 0x0A;
            return false;
        }
        c->cccd_handle = bc->found_cccd;
    }
    uint8_t v[2] = {0, 0};
    if(enable) v[0] = (c->props & CHAR_PROP_NOTIFY) ? 0x01 : 0x02;
    proc_begin(bc);
    tBleStatus st = aci_gatt_write_char_desc(bc->conn_handle, c->cccd_handle, 2, v);
    if(st != BLE_STATUS_SUCCESS) {
        bc->last_error = st;
        return false;
    }
    return wait_done(bc, PROC_TIMEOUT_MS);
}
