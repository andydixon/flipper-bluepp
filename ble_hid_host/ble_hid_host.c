// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andy Dixon <andy@dixon.cx>
// BLE HID Host: connect to a Bluetooth Low Energy keyboard or mouse and show
// its input reports live. Reuses the libble central/GATT layer. Needs the
// full BLE stack + exported ST command API (see ../build.sh).
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/elements.h>
#include <gui/modules/popup.h>
#include <storage/storage.h>
#include <ble/core/ble_defs.h>
#include <string.h>
#include <stdarg.h>
#include "ble_central.h"
#include "ble_names.h"
#include "hid_decode.h"

#define TAG "BleHidHost"
#define LOG_DIR EXT_PATH("apps_data/ble_hid_host")
#define ROWS 3
#define EVENTS_MAX 32
#define SUBS_MAX 8
#define HID_NOTIFY_MAX 32

#define UUID_HID 0x1812
#define UUID_PROTO_MODE 0x2A4E
#define UUID_REPORT 0x2A4D
#define UUID_BOOT_KBD 0x2A22
#define UUID_BOOT_MOUSE 0x2A33
#define UUID_CTRL_POINT 0x2A4C

typedef enum { ViewScan, ViewMonitor, ViewPopup } ViewId;
typedef enum {
    EvtTick,
    EvtSelect,
    EvtNotify,
    EvtJobDone,
    EvtPopupDone,
} AppEvent;
typedef enum { JobConnect, JobDisconnect, JobQuit } JobType;
typedef enum { KindAuto, KindKbd, KindMouse } ReportKind;

typedef struct {
    BcDevice devs[BC_MAX_DEVICES];
    size_t count, sel, top;
    bool scanning;
} ScanModel;

typedef struct {
    char lines[EVENTS_MAX][40];
    int count; // total ever, index with % EVENTS_MAX
    char header[40];
    char status[40];
    size_t scroll;
} MonModel;

typedef struct {
    uint16_t handle;
    ReportKind kind;
} Sub;

typedef struct {
    Gui* gui;
    ViewDispatcher* vd;
    View* scan_view;
    View* mon_view;
    Popup* popup;
    FuriTimer* timer;
    Storage* storage;
    File* log;

    BleCentral* bc;
    FuriThread* worker;
    FuriMessageQueue* jobs;
    FuriMessageQueue* notify_q;
    ViewId cur_view;

    BcDevice dev;
    BcService services[BC_MAX_SERVICES];
    BcChar chars[BC_MAX_CHARS];
    Sub subs[SUBS_MAX];
    int sub_count;
    bool connected_ok;
    char setup_msg[48];
} App;

typedef struct {
    uint16_t handle;
    uint8_t len;
    uint8_t data[HID_NOTIFY_MAX];
} NotifyMsg;

// ---- logging ----------------------------------------------------------------

static void app_log(App* app, const char* fmt, ...) {
    if(!app->log) return;
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    FuriString* s = furi_string_alloc_printf(
        "[%04u-%02u-%02u %02u:%02u:%02u] ", dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    va_list va;
    va_start(va, fmt);
    furi_string_cat_vprintf(s, fmt, va);
    va_end(va);
    furi_string_cat_str(s, "\n");
    storage_file_write(app->log, furi_string_get_cstr(s), furi_string_size(s));
    storage_file_sync(app->log);
    furi_string_free(s);
}

static void log_open(App* app) {
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    storage_simply_mkdir(app->storage, LOG_DIR);
    FuriString* p = furi_string_alloc_printf(
        LOG_DIR "/hid_%04u%02u%02u_%02u%02u%02u.log", dt.year, dt.month, dt.day, dt.hour, dt.minute,
        dt.second);
    app->log = storage_file_alloc(app->storage);
    if(!storage_file_open(app->log, furi_string_get_cstr(p), FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_free(app->log);
        app->log = NULL;
    }
    furi_string_free(p);
}

// ---- monitor event feed -----------------------------------------------------

static void mon_add(App* app, const char* text) {
    with_view_model(
        app->mon_view,
        MonModel * m,
        {
            strncpy(m->lines[m->count % EVENTS_MAX], text, sizeof(m->lines[0]) - 1);
            m->lines[m->count % EVENTS_MAX][sizeof(m->lines[0]) - 1] = 0;
            m->count++;
            m->scroll = 0; // jump to newest
        },
        app->cur_view == ViewMonitor);
}

static void mon_set_status(App* app, const char* header, const char* status) {
    with_view_model(
        app->mon_view,
        MonModel * m,
        {
            if(header) strncpy(m->header, header, sizeof(m->header) - 1);
            if(status) strncpy(m->status, status, sizeof(m->status) - 1);
        },
        app->cur_view == ViewMonitor);
}

static const char* dev_label(const BcDevice* d) {
    return d->name[0] ? d->name : "(no name)";
}

// ---- HID setup (worker thread) ----------------------------------------------

static uint16_t char_end_handle(App* app, int svc, int idx, int nchars) {
    if(idx + 1 < nchars) return app->chars[idx + 1].decl_handle - 1;
    return app->services[svc].end;
}

static uint16_t uuid16(const BcChar* c) {
    return c->uuid_len == 2 ? (c->uuid[0] | (c->uuid[1] << 8)) : 0;
}

static bool hid_setup(App* app) {
    app->sub_count = 0;
    int nsvc = bc_discover_services(app->bc, app->services, BC_MAX_SERVICES);
    if(nsvc < 0) {
        snprintf(app->setup_msg, sizeof(app->setup_msg), "Service discovery failed");
        return false;
    }
    int hid = -1;
    for(int i = 0; i < nsvc; i++)
        if(app->services[i].uuid_len == 2 &&
           (app->services[i].uuid[0] | (app->services[i].uuid[1] << 8)) == UUID_HID)
            hid = i;
    if(hid < 0) {
        snprintf(app->setup_msg, sizeof(app->setup_msg), "No HID service (0x1812)");
        return false;
    }
    int nch = bc_discover_chars(app->bc, &app->services[hid], app->chars, BC_MAX_CHARS);
    if(nch < 0) {
        snprintf(app->setup_msg, sizeof(app->setup_msg), "Char discovery failed");
        return false;
    }

    bool boot_available = false;
    int proto_idx = -1, ctrl_idx = -1;
    for(int i = 0; i < nch; i++) {
        uint16_t u = uuid16(&app->chars[i]);
        if(u == UUID_BOOT_KBD || u == UUID_BOOT_MOUSE) boot_available = true;
        if(u == UUID_PROTO_MODE) proto_idx = i;
        if(u == UUID_CTRL_POINT) ctrl_idx = i;
    }
    // Protocol mode: prefer boot (fixed report layout) when the device offers it.
    if(proto_idx >= 0) {
        uint8_t mode = boot_available ? 0 : 1;
        bc_write(app->bc, app->chars[proto_idx].value_handle, &mode, 1, false);
        app_log(app, "SET protocol mode = %s", mode ? "report" : "boot");
    }
    // Keep the device out of suspend.
    if(ctrl_idx >= 0) {
        uint8_t exit_suspend = 1;
        bc_write(app->bc, app->chars[ctrl_idx].value_handle, &exit_suspend, 1, false);
    }

    for(int i = 0; i < nch && app->sub_count < SUBS_MAX; i++) {
        BcChar* c = &app->chars[i];
        uint16_t u = uuid16(c);
        bool is_input = (u == UUID_BOOT_KBD || u == UUID_BOOT_MOUSE || u == UUID_REPORT);
        bool boot = (u == UUID_BOOT_KBD || u == UUID_BOOT_MOUSE);
        if(!is_input || !(c->props & (CHAR_PROP_NOTIFY | CHAR_PROP_INDICATE))) continue;
        if(boot_available && u == UUID_REPORT) continue; // using boot chars instead
        if(!boot_available && boot) continue;
        uint16_t end = char_end_handle(app, hid, i, nch);
        if(bc_set_notify(app->bc, c, end, true)) {
            ReportKind k = u == UUID_BOOT_KBD ? KindKbd : u == UUID_BOOT_MOUSE ? KindMouse : KindAuto;
            app->subs[app->sub_count++] = (Sub){.handle = c->value_handle, .kind = k};
            app_log(app, "SUBSCRIBE handle 0x%04X uuid 0x%04X", c->value_handle, u);
        }
    }
    if(!app->sub_count) {
        snprintf(app->setup_msg, sizeof(app->setup_msg), "HID service has no input reports");
        return false;
    }
    snprintf(
        app->setup_msg, sizeof(app->setup_msg), "%s mode, %d report(s)",
        boot_available ? "boot" : "report", app->sub_count);
    return true;
}

static int32_t worker_thread(void* ctx) {
    App* app = ctx;
    JobType job;
    while(furi_message_queue_get(app->jobs, &job, FuriWaitForever) == FuriStatusOk) {
        if(job == JobQuit) break;
        if(job == JobConnect) {
            app->connected_ok = bc_connect(app->bc, &app->dev, 8000) && hid_setup(app);
        } else if(job == JobDisconnect) {
            bc_disconnect(app->bc);
            bc_scan_start(app->bc);
        }
        view_dispatcher_send_custom_event(app->vd, EvtJobDone);
    }
    return 0;
}

static void notify_cb(uint16_t handle, const uint8_t* data, uint8_t len, void* ctx) {
    App* app = ctx;
    NotifyMsg m = {.handle = handle, .len = len > HID_NOTIFY_MAX ? HID_NOTIFY_MAX : len};
    memcpy(m.data, data, m.len);
    if(furi_message_queue_put(app->notify_q, &m, 0) == FuriStatusOk)
        view_dispatcher_send_custom_event(app->vd, EvtNotify);
}

// ---- report decoding --------------------------------------------------------

static ReportKind kind_for(App* app, uint16_t handle, uint8_t len) {
    for(int i = 0; i < app->sub_count; i++)
        if(app->subs[i].handle == handle) {
            if(app->subs[i].kind != KindAuto) return app->subs[i].kind;
            break;
        }
    return len >= 8 ? KindKbd : KindMouse; // boot-report heuristic for report-mode inputs
}

static void decode_kbd(FuriString* out, const uint8_t* d, uint8_t len) {
    if(len < 3) {
        furi_string_cat_str(out, "(empty)");
        return;
    }
    hid_mods_str(out, d[0]);
    bool any = false;
    for(uint8_t i = 2; i < len && i < 8; i++) {
        if(!d[i]) continue;
        const char* n = hid_key_name(d[i]);
        if(n)
            furi_string_cat_printf(out, "%s ", n);
        else
            furi_string_cat_printf(out, "[%02X] ", d[i]);
        any = true;
    }
    if(!any && d[0] == 0) furi_string_cat_str(out, "(release)");
}

static void decode_mouse(FuriString* out, const uint8_t* d, uint8_t len) {
    if(len < 1) return;
    uint8_t b = d[0];
    furi_string_cat_printf(
        out, "%s%s%s", (b & 1) ? "L" : "-", (b & 2) ? "R" : "-", (b & 4) ? "M" : "-");
    if(len >= 3) furi_string_cat_printf(out, " dx%+d dy%+d", (int8_t)d[1], (int8_t)d[2]);
    if(len >= 4 && d[3]) furi_string_cat_printf(out, " whl%+d", (int8_t)d[3]);
}

static void on_notify(App* app) {
    NotifyMsg m;
    while(furi_message_queue_get(app->notify_q, &m, 0) == FuriStatusOk) {
        FuriString* line = furi_string_alloc();
        FuriString* raw = furi_string_alloc();
        ble_hex(raw, m.data, m.len);
        if(kind_for(app, m.handle, m.len) == KindKbd)
            decode_kbd(line, m.data, m.len);
        else
            decode_mouse(line, m.data, m.len);
        app_log(app, "REPORT 0x%04X %s | %s", m.handle, furi_string_get_cstr(line), furi_string_get_cstr(raw));
        mon_add(app, furi_string_get_cstr(line));
        furi_string_free(raw);
        furi_string_free(line);
    }
}

// ---- scan view --------------------------------------------------------------

static const char* vendor_of(const BcDevice* d) {
    uint8_t l = 0;
    const uint8_t* mm = bc_adv_find(d->adv, d->adv_len, 0xFF, &l);
    if(!mm) mm = bc_adv_find(d->rsp, d->rsp_len, 0xFF, &l);
    if(!mm || l < 2) return NULL;
    return ble_company_name(mm[0] | (mm[1] << 8));
}

static void scan_draw(Canvas* c, void* m_) {
    ScanModel* m = m_;
    FuriString* s = furi_string_alloc();
    canvas_clear(c);
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 2, 9, "BLE HID Host");
    canvas_set_font(c, FontSecondary);
    furi_string_printf(s, "%u dev", (unsigned)m->count);
    canvas_draw_str_aligned(c, 126, 9, AlignRight, AlignBottom, furi_string_get_cstr(s));
    canvas_draw_line(c, 0, 11, 127, 11);
    if(!m->count) canvas_draw_str_aligned(c, 64, 40, AlignCenter, AlignCenter, "Scanning...");
    for(size_t r = 0; r < ROWS && m->top + r < m->count; r++) {
        const BcDevice* d = &m->devs[m->top + r];
        int y = 13 + r * 17;
        bool sel = (m->top + r == m->sel);
        if(sel) {
            canvas_draw_rbox(c, 0, y, 122, 17, 2);
            canvas_set_color(c, ColorWhite);
        }
        canvas_set_font(c, FontPrimary);
        furi_string_set_str(s, dev_label(d));
        elements_string_fit_width(c, s, 90);
        canvas_draw_str(c, 3, y + 8, furi_string_get_cstr(s));
        canvas_set_font(c, FontSecondary);
        furi_string_printf(s, "%d", d->rssi);
        canvas_draw_str_aligned(c, 119, y + 8, AlignRight, AlignBottom, furi_string_get_cstr(s));
        furi_string_reset(s);
        ble_addr_str(s, d->addr);
        const char* v = vendor_of(d);
        if(v) furi_string_cat_printf(s, " %s", v);
        elements_string_fit_width(c, s, 116);
        canvas_draw_str(c, 3, y + 16, furi_string_get_cstr(s));
        if(sel) canvas_set_color(c, ColorBlack);
    }
    if(m->count > ROWS) elements_scrollbar_pos(c, 125, 13, 51, m->sel, m->count);
    furi_string_free(s);
}

static bool scan_input(InputEvent* e, void* ctx) {
    App* app = ctx;
    if(e->type != InputTypeShort && e->type != InputTypeRepeat) return false;
    if(e->key == InputKeyOk && e->type == InputTypeShort) {
        view_dispatcher_send_custom_event(app->vd, EvtSelect);
        return true;
    }
    if(e->key != InputKeyUp && e->key != InputKeyDown) return false;
    with_view_model(
        app->scan_view,
        ScanModel * m,
        {
            if(e->key == InputKeyUp && m->sel > 0) m->sel--;
            if(e->key == InputKeyDown && m->sel + 1 < m->count) m->sel++;
            if(m->sel < m->top) m->top = m->sel;
            if(m->sel >= m->top + ROWS) m->top = m->sel - ROWS + 1;
        },
        true);
    return true;
}

static void scan_enter(void* ctx) {
    App* app = ctx;
    app->cur_view = ViewScan;
    bool ok = bc_scan_start(app->bc);
    with_view_model(app->scan_view, ScanModel * m, { m->scanning = ok; }, true);
}

// ---- monitor view -----------------------------------------------------------

static void mon_draw(Canvas* c, void* m_) {
    MonModel* m = m_;
    canvas_clear(c);
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 2, 9, m->header[0] ? m->header : "HID");
    canvas_set_font(c, FontSecondary);
    canvas_draw_line(c, 0, 11, 127, 11);
    canvas_draw_str(c, 2, 21, m->status);

    int total = m->count;
    int visible = 4; // event lines
    int shown = total < visible ? total : visible;
    for(int r = 0; r < shown; r++) {
        // newest at bottom; scroll shifts the window back into history
        int idx = total - 1 - (int)m->scroll - (shown - 1 - r);
        if(idx < 0 || idx < total - EVENTS_MAX) continue;
        canvas_draw_str(c, 2, 33 + r * 10, m->lines[idx % EVENTS_MAX]);
    }
    if(total > visible) elements_scrollbar_pos(c, 126, 23, 41, m->scroll, total);
}

static bool mon_input(InputEvent* e, void* ctx) {
    App* app = ctx;
    if(e->type != InputTypeShort && e->type != InputTypeRepeat) return false;
    if(e->key != InputKeyUp && e->key != InputKeyDown) return false;
    with_view_model(
        app->mon_view,
        MonModel * m,
        {
            int maxs = m->count > 4 ? m->count - 4 : 0;
            if(e->key == InputKeyDown && (int)m->scroll < maxs) m->scroll++; // into history
            if(e->key == InputKeyUp && m->scroll > 0) m->scroll--;
        },
        true);
    return true;
}

static uint32_t nav_exit(void* ctx) {
    UNUSED(ctx);
    return VIEW_NONE;
}

// Back from the monitor: disconnect and return to scanning.
static uint32_t nav_mon_back(void* ctx) {
    App* app = ctx;
    furi_message_queue_put(app->jobs, &(JobType){JobDisconnect}, 0);
    app->cur_view = ViewScan;
    return ViewScan;
}

static uint32_t nav_stay_popup(void* ctx) {
    UNUSED(ctx);
    return ViewPopup;
}

// ---- events -----------------------------------------------------------------

static void popup_cb(void* ctx) {
    App* app = ctx;
    view_dispatcher_send_custom_event(app->vd, EvtPopupDone);
}

static void show_popup(App* app, const char* header, const char* text, bool timeout) {
    popup_reset(app->popup);
    popup_set_callback(app->popup, popup_cb);
    popup_set_context(app->popup, app);
    popup_set_header(app->popup, header, 64, 18, AlignCenter, AlignCenter);
    popup_set_text(app->popup, text, 64, 40, AlignCenter, AlignCenter);
    if(timeout) {
        popup_set_timeout(app->popup, 1800);
        popup_enable_timeout(app->popup);
    } else {
        popup_disable_timeout(app->popup);
    }
    app->cur_view = ViewPopup;
    view_dispatcher_switch_to_view(app->vd, ViewPopup);
}

static void on_tick(App* app) {
    if(app->cur_view != ViewScan) return;
    with_view_model(
        app->scan_view,
        ScanModel * m,
        {
            uint8_t sel_addr[6];
            bool had = m->count > 0;
            if(had) memcpy(sel_addr, m->devs[m->sel].addr, 6);
            m->count = bc_snapshot(app->bc, m->devs, BC_MAX_DEVICES);
            if(had)
                for(size_t i = 0; i < m->count; i++)
                    if(memcmp(m->devs[i].addr, sel_addr, 6) == 0) m->sel = i;
            if(m->sel >= m->count) m->sel = m->count ? m->count - 1 : 0;
            if(m->sel < m->top) m->top = m->sel;
            if(m->sel >= m->top + ROWS) m->top = m->sel - ROWS + 1;
        },
        true);
}

static bool custom_event_cb(void* ctx, uint32_t event) {
    App* app = ctx;
    switch(event) {
    case EvtTick: on_tick(app); break;
    case EvtSelect: {
        bool have = false;
        with_view_model(
            app->scan_view,
            ScanModel * m,
            {
                if(m->count) {
                    app->dev = m->devs[m->sel];
                    have = true;
                }
            },
            false);
        if(have) {
            app_log(app, "CONNECTING %s", dev_label(&app->dev));
            show_popup(app, "Connecting...", dev_label(&app->dev), false);
            furi_message_queue_put(app->jobs, &(JobType){JobConnect}, 0);
        }
    } break;
    case EvtNotify: on_notify(app); break;
    case EvtJobDone:
        if(app->connected_ok) {
            char hdr[40];
            snprintf(hdr, sizeof(hdr), "%s", dev_label(&app->dev));
            with_view_model(app->mon_view, MonModel * m, { m->count = 0; m->scroll = 0; }, false);
            mon_set_status(app, hdr, app->setup_msg);
            app_log(app, "CONNECTED %s: %s", dev_label(&app->dev), app->setup_msg);
            app->cur_view = ViewMonitor;
            view_dispatcher_switch_to_view(app->vd, ViewMonitor);
        } else if(bc_is_connected(app->bc)) {
            // connected but HID setup failed: report and drop
            app_log(app, "SETUP FAILED %s: %s", dev_label(&app->dev), app->setup_msg);
            bc_disconnect(app->bc);
            show_popup(app, "Not a HID device", app->setup_msg, true);
        } else {
            const char* n = ble_att_error_name(bc_last_error(app->bc));
            app_log(app, "CONNECT FAILED %s", dev_label(&app->dev));
            show_popup(app, "Connect failed", n ? n : "timeout", true);
        }
        break;
    case EvtPopupDone:
        bc_scan_start(app->bc);
        app->cur_view = ViewScan;
        view_dispatcher_switch_to_view(app->vd, ViewScan);
        break;
    default: return false;
    }
    return true;
}

static void timer_cb(void* ctx) {
    App* app = ctx;
    view_dispatcher_send_custom_event(app->vd, EvtTick);
}

// ---- setup / teardown -------------------------------------------------------

int32_t ble_hid_host_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->jobs = furi_message_queue_alloc(8, sizeof(JobType));
    app->notify_q = furi_message_queue_alloc(16, sizeof(NotifyMsg));

    app->vd = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->vd, app);
    view_dispatcher_set_custom_event_callback(app->vd, custom_event_cb);
    view_dispatcher_attach_to_gui(app->vd, app->gui, ViewDispatcherTypeFullscreen);

    app->scan_view = view_alloc();
    view_allocate_model(app->scan_view, ViewModelTypeLocking, sizeof(ScanModel));
    view_set_context(app->scan_view, app);
    view_set_draw_callback(app->scan_view, scan_draw);
    view_set_input_callback(app->scan_view, scan_input);
    view_set_enter_callback(app->scan_view, scan_enter);
    view_set_previous_callback(app->scan_view, nav_exit);
    view_dispatcher_add_view(app->vd, ViewScan, app->scan_view);

    app->mon_view = view_alloc();
    view_allocate_model(app->mon_view, ViewModelTypeLocking, sizeof(MonModel));
    view_set_context(app->mon_view, app);
    view_set_draw_callback(app->mon_view, mon_draw);
    view_set_input_callback(app->mon_view, mon_input);
    view_set_previous_callback(app->mon_view, nav_mon_back);
    view_dispatcher_add_view(app->vd, ViewMonitor, app->mon_view);

    app->popup = popup_alloc();
    view_set_previous_callback(popup_get_view(app->popup), nav_stay_popup);
    view_dispatcher_add_view(app->vd, ViewPopup, popup_get_view(app->popup));

    app->timer = furi_timer_alloc(timer_cb, FuriTimerTypePeriodic, app);

    if(!bc_supported()) {
        show_popup(
            app, "Full BLE stack needed",
            "This firmware runs BLE Light. Build with ble_full (see build.sh).", false);
        view_set_previous_callback(popup_get_view(app->popup), nav_exit);
        view_dispatcher_run(app->vd);
        goto cleanup;
    }

    log_open(app);
    bool was_adv = furi_hal_bt_is_active();
    furi_hal_bt_stop_advertising();
    furi_delay_ms(100);
    app->bc = bc_alloc(notify_cb, app);
    app_log(app, "START");

    app->worker = furi_thread_alloc_ex("HidHostWorker", 3072, worker_thread, app);
    furi_thread_start(app->worker);
    furi_timer_start(app->timer, furi_ms_to_ticks(300));

    view_dispatcher_switch_to_view(app->vd, ViewScan);
    view_dispatcher_run(app->vd);

    furi_timer_stop(app->timer);
    furi_message_queue_put(app->jobs, &(JobType){JobQuit}, 0);
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);
    bc_free(app->bc);
    if(was_adv) furi_hal_bt_start_advertising();
    app_log(app, "STOP");
    if(app->log) {
        storage_file_close(app->log);
        storage_file_free(app->log);
    }

cleanup:
    furi_timer_free(app->timer);
    view_dispatcher_remove_view(app->vd, ViewScan);
    view_dispatcher_remove_view(app->vd, ViewMonitor);
    view_dispatcher_remove_view(app->vd, ViewPopup);
    view_free(app->scan_view);
    view_free(app->mon_view);
    popup_free(app->popup);
    view_dispatcher_free(app->vd);
    furi_message_queue_free(app->jobs);
    furi_message_queue_free(app->notify_q);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
