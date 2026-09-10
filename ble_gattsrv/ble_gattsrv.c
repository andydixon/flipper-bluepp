// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andy Dixon <andy@dixon.cx>
// BLE GATT Server / emulator: bring up the Flipper as a connectable BLE serial
// peripheral (a custom 128-bit GATT service with RX/TX characteristics) via the
// bt service's profile API, and echo whatever a central writes back to it as a
// notification. Connect from a phone (e.g. nRF Connect) to see the service,
// write to RX and receive the echo on TX. Restores the default profile on exit.
#include <furi.h>
#include <bt/bt_service/bt.h>
#include <profiles/serial_profile.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <string.h>

typedef struct {
    Gui* gui;
    ViewPort* vp;
    FuriMutex* mutex;
    FuriMessageQueue* input;
    Bt* bt;
    FuriHalBleProfileBase* profile;

    BtStatus status;
    uint32_t rx_bytes, tx_bytes;
    char last[40];
    bool echo;
} App;

static void status_cb(BtStatus status, void* ctx) {
    App* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->status = status;
    furi_mutex_release(app->mutex);
    if(app->vp) view_port_update(app->vp);
}

// Runs on the BLE stack thread: echo received data back as a TX notification.
static uint16_t serial_cb(SerialServiceEvent event, void* ctx) {
    App* app = ctx;
    if(event.event == SerialServiceEventTypeDataReceived) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->rx_bytes += event.data.size;
        uint16_t n = event.data.size < sizeof(app->last) - 1 ? event.data.size : sizeof(app->last) - 1;
        memcpy(app->last, event.data.buffer, n);
        app->last[n] = 0;
        for(uint16_t i = 0; i < n; i++)
            if(app->last[i] < 32 || app->last[i] > 126) app->last[i] = '.';
        bool echo = app->echo;
        furi_mutex_release(app->mutex);
        if(echo && app->profile) {
            if(ble_profile_serial_tx(app->profile, event.data.buffer, event.data.size))
                app->tx_bytes += event.data.size;
        }
        if(app->vp) view_port_update(app->vp);
    }
    return event.data.size;
}

static void draw_cb(Canvas* c, void* ctx) {
    App* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    char buf[48];
    canvas_clear(c);
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 2, 9, "GATT Server");
    canvas_set_font(c, FontSecondary);
    const char* st = app->status == BtStatusConnected      ? "connected" :
                     app->status == BtStatusAdvertising     ? "advertising" :
                     app->status == BtStatusOff             ? "off" :
                                                              "unavailable";
    canvas_draw_str_aligned(c, 126, 9, AlignRight, AlignBottom, st);
    canvas_draw_line(c, 0, 11, 127, 11);
    canvas_draw_str(c, 2, 22, "Serial GATT service (RX/TX)");
    snprintf(buf, sizeof(buf), "RX %lu B   TX %lu B", (unsigned long)app->rx_bytes, (unsigned long)app->tx_bytes);
    canvas_draw_str(c, 2, 34, buf);
    snprintf(buf, sizeof(buf), "Echo: %s (OK toggles)", app->echo ? "on" : "off");
    canvas_draw_str(c, 2, 45, buf);
    if(app->last[0]) {
        snprintf(buf, sizeof(buf), "last: %s", app->last);
        canvas_draw_str(c, 2, 57, buf);
    }
    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* e, void* ctx) {
    App* app = ctx;
    furi_message_queue_put(app->input, e, FuriWaitForever);
}

int32_t ble_gattsrv_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->input = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->echo = true;

    app->bt = furi_record_open(RECORD_BT);
    bt_set_status_changed_callback(app->bt, status_cb, app);
    // Swap the default RPC serial profile for our own instance so we own RX/TX.
    app->profile = bt_profile_start(app->bt, ble_profile_serial, NULL);
    if(app->profile)
        ble_profile_serial_set_event_callback(
            app->profile, BLE_PROFILE_SERIAL_PACKET_SIZE_MAX, serial_cb, app);

    app->gui = furi_record_open(RECORD_GUI);
    app->vp = view_port_alloc();
    view_port_draw_callback_set(app->vp, draw_cb, app);
    view_port_input_callback_set(app->vp, input_cb, app);
    gui_add_view_port(app->gui, app->vp, GuiLayerFullscreen);

    bool run = app->profile != NULL;
    if(!run) {
        // Could not start the profile (wrong stack?). Show a hint until Back.
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        strncpy(app->last, "profile start failed", sizeof(app->last) - 1);
        furi_mutex_release(app->mutex);
        view_port_update(app->vp);
    }
    bool active = true;
    while(active) {
        InputEvent e;
        if(furi_message_queue_get(app->input, &e, FuriWaitForever) != FuriStatusOk) continue;
        if(e.type != InputTypeShort) continue;
        if(e.key == InputKeyBack) {
            active = false;
        } else if(e.key == InputKeyOk) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->echo = !app->echo;
            furi_mutex_release(app->mutex);
            view_port_update(app->vp);
        }
    }

    // Unhook every callback that runs on the BLE/BT thread BEFORE freeing the
    // view port, or serial_cb/status_cb can call view_port_update on freed memory.
    if(app->profile) {
        ble_profile_serial_set_event_callback(app->profile, 0, NULL, NULL);
        bt_profile_restore_default(app->bt);
    }
    bt_set_status_changed_callback(app->bt, NULL, NULL);
    furi_record_close(RECORD_BT);
    gui_remove_view_port(app->gui, app->vp);
    view_port_free(app->vp);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app->input);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
