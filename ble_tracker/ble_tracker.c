// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andy Dixon <andy@dixon.cx>
// BLE Tracker Detector: passively scan and flag nearby item trackers by their
// advertising signatures (Apple Find My / AirTag, Tile, Samsung SmartTag), plus
// a "following" warning for a tracker seen persistently over time. Read-only.
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/elements.h>
#include <storage/storage.h>
#include <string.h>
#include <stdarg.h>
#include "ble_central.h"
#include "ble_names.h"

#define TAG "BleTracker"
#define LOG_DIR EXT_PATH("apps_data/ble_tracker")
#define ROWS 4
#define FOLLOW_SECS 120 // seen this long => "following" warning
#define FOLLOW_HITS 8

typedef enum { TrackNone, TrackAirTag, TrackFindMy, TrackTile, TrackSmartTag } TrackerType;

static const char* type_name(TrackerType t) {
    switch(t) {
    case TrackAirTag: return "AirTag";
    case TrackFindMy: return "Find My";
    case TrackTile: return "Tile";
    case TrackSmartTag: return "SmartTag";
    default: return "?";
    }
}

// Classify a device from its advertisement. Returns TrackNone if not a tracker.
static TrackerType classify(const BcDevice* d) {
    uint8_t l = 0;
    const uint8_t* mfg = bc_adv_find(d->adv, d->adv_len, 0xFF, &l);
    if(!mfg) mfg = bc_adv_find(d->rsp, d->rsp_len, 0xFF, &l);
    if(mfg && l >= 3 && mfg[0] == 0x4C && mfg[1] == 0x00) {
        if(mfg[2] == 0x12) return TrackAirTag; // Find My "offline finding"
        if(mfg[2] == 0x07) return TrackFindMy; // proximity pairing / nearby
    }
    // Service data / 16-bit service UUID lists.
    for(int which = 0; which < 2; which++) {
        const uint8_t* adv = which ? d->rsp : d->adv;
        uint8_t alen = which ? d->rsp_len : d->adv_len;
        uint8_t sl = 0;
        const uint8_t* sd = bc_adv_find(adv, alen, 0x16, &sl); // service data 16-bit
        if(sd && sl >= 2) {
            uint16_t u = sd[0] | (sd[1] << 8);
            if(u == 0xFEED || u == 0xFEEC) return TrackTile;
            if(u == 0xFD5A) return TrackSmartTag;
        }
        for(uint8_t t = 0x02; t <= 0x03; t++) {
            uint8_t ul = 0;
            const uint8_t* us = bc_adv_find(adv, alen, t, &ul);
            for(uint8_t i = 0; us && i + 2 <= ul; i += 2) {
                uint16_t u = us[i] | (us[i + 1] << 8);
                if(u == 0xFEED || u == 0xFEEC) return TrackTile;
                if(u == 0xFD5A) return TrackSmartTag;
            }
        }
    }
    return TrackNone;
}

typedef struct {
    uint8_t addr[6];
    TrackerType type;
    int8_t rssi;
    uint32_t first_seen, last_seen, hits;
    bool warned;
} Tracker;

typedef struct {
    Tracker items[BC_MAX_DEVICES];
    size_t count, sel, top;
} ListModel;

typedef struct {
    Gui* gui;
    ViewDispatcher* vd;
    View* view;
    FuriTimer* timer;
    Storage* storage;
    File* log;
    BleCentral* bc;
    Tracker db[BC_MAX_DEVICES];
    size_t db_count;
    BcDevice snap[BC_MAX_DEVICES]; // snapshot scratch (kept off the stack)
} App;

static void app_log(App* app, const char* fmt, ...) {
    if(!app->log) return;
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    FuriString* s = furi_string_alloc_printf("[%02u:%02u:%02u] ", dt.hour, dt.minute, dt.second);
    va_list va;
    va_start(va, fmt);
    furi_string_cat_vprintf(s, fmt, va);
    va_end(va);
    furi_string_cat_str(s, "\n");
    storage_file_write(app->log, furi_string_get_cstr(s), furi_string_size(s));
    storage_file_sync(app->log);
    furi_string_free(s);
}

static void tick(App* app) {
    BcDevice* snap = app->snap;
    size_t n = bc_snapshot(app->bc, snap, BC_MAX_DEVICES);
    uint32_t now = furi_get_tick();
    for(size_t i = 0; i < n; i++) {
        TrackerType t = classify(&snap[i]);
        if(t == TrackNone) continue;
        Tracker* rec = NULL;
        for(size_t k = 0; k < app->db_count; k++)
            if(memcmp(app->db[k].addr, snap[i].addr, 6) == 0) rec = &app->db[k];
        if(!rec && app->db_count < BC_MAX_DEVICES) {
            rec = &app->db[app->db_count++];
            memset(rec, 0, sizeof(*rec));
            memcpy(rec->addr, snap[i].addr, 6);
            rec->first_seen = now;
            FuriString* a = furi_string_alloc();
            ble_addr_str(a, snap[i].addr);
            app_log(app, "TRACKER %s %s rssi=%d", type_name(t), furi_string_get_cstr(a), snap[i].rssi);
            furi_string_free(a);
        }
        if(!rec) continue;
        rec->type = t;
        rec->rssi = snap[i].rssi;
        rec->last_seen = now;
        rec->hits++;
        if(!rec->warned && rec->hits >= FOLLOW_HITS &&
           (now - rec->first_seen) >= furi_ms_to_ticks(FOLLOW_SECS * 1000)) {
            rec->warned = true;
            FuriString* a = furi_string_alloc();
            ble_addr_str(a, rec->addr);
            app_log(app, "FOLLOWING? %s %s for %lus", type_name(t), furi_string_get_cstr(a),
                (unsigned long)((now - rec->first_seen) / 1000));
            furi_string_free(a);
        }
    }
    // Publish sorted-by-rssi copy to the view model.
    with_view_model(
        app->view,
        ListModel * m,
        {
            m->count = app->db_count;
            for(size_t i = 0; i < app->db_count; i++) {
                m->items[i].type = app->db[i].type;
                memcpy(m->items[i].addr, app->db[i].addr, 6);
                m->items[i].rssi = app->db[i].rssi;
                m->items[i].first_seen = app->db[i].first_seen;
                m->items[i].last_seen = app->db[i].last_seen;
                m->items[i].hits = app->db[i].hits;
                m->items[i].warned = app->db[i].warned;
            }
            if(m->sel >= m->count) m->sel = m->count ? m->count - 1 : 0;
            if(m->sel < m->top) m->top = m->sel;
            if(m->sel >= m->top + ROWS) m->top = m->sel - ROWS + 1;
        },
        true);
}

static void draw_cb(Canvas* c, void* m_) {
    ListModel* m = m_;
    FuriString* s = furi_string_alloc();
    canvas_clear(c);
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 2, 9, "Tracker Detector");
    canvas_set_font(c, FontSecondary);
    furi_string_printf(s, "%u", (unsigned)m->count);
    canvas_draw_str_aligned(c, 126, 9, AlignRight, AlignBottom, furi_string_get_cstr(s));
    canvas_draw_line(c, 0, 11, 127, 11);
    if(!m->count) canvas_draw_str_aligned(c, 64, 40, AlignCenter, AlignCenter, "Scanning...");
    uint32_t now = furi_get_tick();
    for(size_t r = 0; r < ROWS && m->top + r < m->count; r++) {
        Tracker* t = &m->items[m->top + r];
        int y = 13 + r * 13;
        bool sel = (m->top + r == m->sel);
        if(sel) {
            canvas_draw_rbox(c, 0, y, 122, 13, 2);
            canvas_set_color(c, ColorWhite);
        }
        furi_string_printf(s, "%s%s ", t->warned ? "! " : "", type_name(t->type));
        ble_addr_str(s, t->addr);
        elements_string_fit_width(c, s, 100);
        canvas_draw_str(c, 3, y + 9, furi_string_get_cstr(s));
        furi_string_printf(s, "%d", t->rssi);
        canvas_draw_str_aligned(c, 119, y + 9, AlignRight, AlignBottom, furi_string_get_cstr(s));
        if(sel) canvas_set_color(c, ColorBlack);
        UNUSED(now);
    }
    if(m->count > ROWS) elements_scrollbar_pos(c, 125, 13, 51, m->sel, m->count);
    furi_string_free(s);
}

static bool input_cb(InputEvent* e, void* ctx) {
    App* app = ctx;
    if(e->type != InputTypeShort && e->type != InputTypeRepeat) return false;
    if(e->key != InputKeyUp && e->key != InputKeyDown) return false;
    with_view_model(
        app->view,
        ListModel * m,
        {
            if(e->key == InputKeyUp && m->sel > 0) m->sel--;
            if(e->key == InputKeyDown && m->sel + 1 < m->count) m->sel++;
            if(m->sel < m->top) m->top = m->sel;
            if(m->sel >= m->top + ROWS) m->top = m->sel - ROWS + 1;
        },
        true);
    return true;
}

static uint32_t exit_cb(void* ctx) {
    UNUSED(ctx);
    return VIEW_NONE;
}

static bool tracker_custom(void* ctx, uint32_t event) {
    if(event == 1) tick((App*)ctx);
    return true;
}

static void timer_cb(void* ctx) {
    App* app = ctx;
    view_dispatcher_send_custom_event(app->vd, 1);
}

int32_t ble_tracker_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);

    app->vd = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->vd, app);
    view_dispatcher_set_custom_event_callback(app->vd, tracker_custom);
    view_dispatcher_attach_to_gui(app->vd, app->gui, ViewDispatcherTypeFullscreen);
    app->view = view_alloc();
    view_allocate_model(app->view, ViewModelTypeLocking, sizeof(ListModel));
    view_set_context(app->view, app);
    view_set_draw_callback(app->view, draw_cb);
    view_set_input_callback(app->view, input_cb);
    view_set_previous_callback(app->view, exit_cb);
    view_dispatcher_add_view(app->vd, 0, app->view);

    if(!bc_supported()) {
        // No full stack: show nothing meaningful, just run empty (Back exits).
        view_dispatcher_switch_to_view(app->vd, 0);
        view_dispatcher_run(app->vd);
        goto teardown;
    }

    storage_simply_mkdir(app->storage, LOG_DIR);
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    FuriString* path = furi_string_alloc_printf(
        LOG_DIR "/track_%02u%02u%02u.log", dt.hour, dt.minute, dt.second);
    app->log = storage_file_alloc(app->storage);
    if(!storage_file_open(app->log, furi_string_get_cstr(path), FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_free(app->log);
        app->log = NULL;
    }
    furi_string_free(path);

    bool was_adv = furi_hal_bt_is_active();
    furi_hal_bt_stop_advertising();
    furi_delay_ms(100);
    app->bc = bc_alloc(NULL, NULL);
    bc_scan_start(app->bc);
    app->timer = furi_timer_alloc(timer_cb, FuriTimerTypePeriodic, app);
    furi_timer_start(app->timer, furi_ms_to_ticks(500));

    view_dispatcher_switch_to_view(app->vd, 0);
    view_dispatcher_run(app->vd);

    furi_timer_stop(app->timer);
    furi_timer_free(app->timer);
    bc_free(app->bc);
    if(was_adv) furi_hal_bt_start_advertising();
    if(app->log) {
        storage_file_close(app->log);
        storage_file_free(app->log);
    }

teardown:
    view_dispatcher_remove_view(app->vd, 0);
    view_free(app->view);
    view_dispatcher_free(app->vd);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
