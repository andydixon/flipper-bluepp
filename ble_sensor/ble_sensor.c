// BLE Sensor Dashboard: connect to a selected BLE peripheral, find its readable
// and notifying characteristics with known SIG meanings (battery, heart rate,
// temperature, humidity, pressure, Tx power, ...), and show them decoded and
// updating live. Reuses the libble central/GATT layer and value decoders.
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/elements.h>
#include <gui/modules/popup.h>
#include <ble/core/ble_defs.h>
#include <string.h>
#include "ble_central.h"
#include "ble_names.h"

#define TAG "BleSensor"
#define ROWS 3
#define MONITOR_MAX 12
#define VAL_MAX 32

typedef enum { ViewScan, ViewReadings, ViewPopup } ViewId;
typedef enum { EvtTick, EvtSelect, EvtNotify, EvtJobDone, EvtPopupDone } AppEvent;
typedef enum { JobConnect, JobDisconnect, JobQuit } JobType;

typedef struct {
    BcDevice devs[BC_MAX_DEVICES];
    size_t count, sel, top;
} ScanModel;

typedef struct {
    uint16_t handle;
    uint8_t uuid[16];
    uint8_t uuid_len;
    char name[24];
    uint8_t val[VAL_MAX];
    uint8_t val_len;
    bool have;
} Reading;

typedef struct {
    char header[40];
    Reading r[MONITOR_MAX];
    int count;
    size_t scroll;
} ReadingsModel;

typedef struct {
    uint16_t handle;
    uint8_t len;
    uint8_t data[VAL_MAX];
} NotifyMsg;

typedef struct {
    Gui* gui;
    ViewDispatcher* vd;
    View* scan_view;
    View* readings_view;
    Popup* popup;
    FuriTimer* timer;
    BleCentral* bc;
    FuriThread* worker;
    FuriMessageQueue* jobs;
    FuriMessageQueue* notify_q;
    ViewId cur;

    BcDevice dev;
    BcService services[BC_MAX_SERVICES];
    BcChar chars[BC_MAX_CHARS];
    bool connected_ok;
    char setup_msg[40];
} App;

static const char* dev_label(const BcDevice* d) {
    return d->name[0] ? d->name : "(no name)";
}

// ---- worker: connect + collect readable/notify known sensor chars ----------

static uint16_t char_end(App* app, int svc, int idx, int nch) {
    if(idx + 1 < nch) return app->chars[idx + 1].decl_handle - 1;
    return app->services[svc].end;
}

static bool sensor_setup(App* app) {
    int nsvc = bc_discover_services(app->bc, app->services, BC_MAX_SERVICES);
    if(nsvc < 0) {
        strncpy(app->setup_msg, "discovery failed", sizeof(app->setup_msg) - 1);
        return false;
    }
    with_view_model(
        app->readings_view, ReadingsModel * m, { m->count = 0; m->scroll = 0; }, false);
    int found = 0;
    for(int s = 0; s < nsvc; s++) {
        int nch = bc_discover_chars(app->bc, &app->services[s], app->chars, BC_MAX_CHARS);
        for(int i = 0; i < nch && found < MONITOR_MAX; i++) {
            BcChar* c = &app->chars[i];
            const char* name = ble_uuid_name(c->uuid, c->uuid_len);
            if(!name) continue; // only known SIG characteristics
            bool readable = c->props & CHAR_PROP_READ;
            bool notif = c->props & (CHAR_PROP_NOTIFY | CHAR_PROP_INDICATE);
            if(!readable && !notif) continue;
            Reading rd;
            memset(&rd, 0, sizeof(rd));
            rd.handle = c->value_handle;
            rd.uuid_len = c->uuid_len;
            memcpy(rd.uuid, c->uuid, c->uuid_len);
            strncpy(rd.name, name, sizeof(rd.name) - 1);
            if(readable) {
                int n = bc_read(app->bc, c->value_handle, rd.val, VAL_MAX);
                if(n >= 0) {
                    rd.val_len = n;
                    rd.have = true;
                }
            }
            if(notif) bc_set_notify(app->bc, c, char_end(app, s, i, nch), true);
            with_view_model(
                app->readings_view,
                ReadingsModel * m,
                {
                    if(m->count < MONITOR_MAX) m->r[m->count++] = rd;
                },
                false);
            found++;
        }
    }
    if(!found) {
        strncpy(app->setup_msg, "no known sensor chars", sizeof(app->setup_msg) - 1);
        return false;
    }
    snprintf(app->setup_msg, sizeof(app->setup_msg), "%d reading(s)", found);
    return true;
}

static int32_t worker_thread(void* ctx) {
    App* app = ctx;
    JobType job;
    while(furi_message_queue_get(app->jobs, &job, FuriWaitForever) == FuriStatusOk) {
        if(job == JobQuit) break;
        if(job == JobConnect)
            app->connected_ok = bc_connect(app->bc, &app->dev, 8000) && sensor_setup(app);
        else if(job == JobDisconnect) {
            bc_disconnect(app->bc);
            bc_scan_start(app->bc);
        }
        view_dispatcher_send_custom_event(app->vd, EvtJobDone);
    }
    return 0;
}

static void notify_cb(uint16_t handle, const uint8_t* data, uint8_t len, void* ctx) {
    App* app = ctx;
    NotifyMsg m = {.handle = handle, .len = len > VAL_MAX ? VAL_MAX : len};
    memcpy(m.data, data, m.len);
    if(furi_message_queue_put(app->notify_q, &m, 0) == FuriStatusOk)
        view_dispatcher_send_custom_event(app->vd, EvtNotify);
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
    canvas_draw_str(c, 2, 9, "Sensor Dashboard");
    canvas_set_font(c, FontSecondary);
    furi_string_printf(s, "%u", (unsigned)m->count);
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
    app->cur = ViewScan;
    bc_scan_start(app->bc);
}

// ---- readings view ----------------------------------------------------------

static void readings_draw(Canvas* c, void* m_) {
    ReadingsModel* m = m_;
    FuriString* s = furi_string_alloc();
    canvas_clear(c);
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 2, 9, m->header[0] ? m->header : "Readings");
    canvas_set_font(c, FontSecondary);
    canvas_draw_line(c, 0, 11, 127, 11);
    int visible = 5;
    for(int r = 0; r < visible; r++) {
        int idx = (int)m->scroll + r;
        if(idx >= m->count) break;
        Reading* rd = &m->r[idx];
        furi_string_printf(s, "%s: ", rd->name);
        if(rd->have)
            ble_decode_value(s, rd->uuid, rd->uuid_len, rd->val, rd->val_len);
        else
            furi_string_cat_str(s, "...\n");
        size_t nl = furi_string_search_char(s, '\n', 0);
        if(nl != FURI_STRING_FAILURE) furi_string_left(s, nl);
        elements_string_fit_width(c, s, 124);
        canvas_draw_str(c, 2, 22 + r * 10, furi_string_get_cstr(s));
    }
    if(m->count > visible) elements_scrollbar_pos(c, 126, 12, 52, m->scroll, m->count);
    furi_string_free(s);
}

static bool readings_input(InputEvent* e, void* ctx) {
    App* app = ctx;
    if(e->type != InputTypeShort && e->type != InputTypeRepeat) return false;
    if(e->key != InputKeyUp && e->key != InputKeyDown) return false;
    with_view_model(
        app->readings_view,
        ReadingsModel * m,
        {
            int maxs = m->count > 5 ? m->count - 5 : 0;
            if(e->key == InputKeyDown && (int)m->scroll < maxs) m->scroll++;
            if(e->key == InputKeyUp && m->scroll > 0) m->scroll--;
        },
        true);
    return true;
}

static uint32_t readings_back(void* ctx) {
    App* app = ctx;
    furi_message_queue_put(app->jobs, &(JobType){JobDisconnect}, 0);
    app->cur = ViewScan;
    return ViewScan;
}
static uint32_t scan_exit(void* ctx) {
    UNUSED(ctx);
    return VIEW_NONE;
}
static uint32_t stay_popup(void* ctx) {
    UNUSED(ctx);
    return ViewPopup;
}

static void popup_cb(void* ctx) {
    App* app = ctx;
    view_dispatcher_send_custom_event(app->vd, EvtPopupDone);
}

static void on_notify(App* app) {
    NotifyMsg msg;
    while(furi_message_queue_get(app->notify_q, &msg, 0) == FuriStatusOk) {
        with_view_model(
            app->readings_view,
            ReadingsModel * m,
            {
                for(int i = 0; i < m->count; i++)
                    if(m->r[i].handle == msg.handle) {
                        m->r[i].val_len = msg.len;
                        memcpy(m->r[i].val, msg.data, msg.len);
                        m->r[i].have = true;
                    }
            },
            app->cur == ViewReadings);
    }
}

static bool custom_cb(void* ctx, uint32_t event) {
    App* app = ctx;
    switch(event) {
    case EvtTick:
        if(app->cur == ViewScan)
            with_view_model(
                app->scan_view,
                ScanModel * m,
                {
                    m->count = bc_snapshot(app->bc, m->devs, BC_MAX_DEVICES);
                    if(m->sel >= m->count) m->sel = m->count ? m->count - 1 : 0;
                    if(m->sel < m->top) m->top = m->sel;
                    if(m->sel >= m->top + ROWS) m->top = m->sel - ROWS + 1;
                },
                true);
        break;
    case EvtSelect: {
        bool have = false;
        with_view_model(
            app->scan_view, ScanModel * m,
            { if(m->count) { app->dev = m->devs[m->sel]; have = true; } }, false);
        if(have) {
            popup_reset(app->popup);
            popup_set_callback(app->popup, popup_cb);
            popup_set_context(app->popup, app);
            popup_set_header(app->popup, "Connecting...", 64, 26, AlignCenter, AlignCenter);
            popup_set_text(app->popup, dev_label(&app->dev), 64, 40, AlignCenter, AlignCenter);
            app->cur = ViewPopup;
            view_dispatcher_switch_to_view(app->vd, ViewPopup);
            furi_message_queue_put(app->jobs, &(JobType){JobConnect}, 0);
        }
    } break;
    case EvtNotify: on_notify(app); break;
    case EvtJobDone:
        if(app->connected_ok) {
            with_view_model(
                app->readings_view, ReadingsModel * m,
                { snprintf(m->header, sizeof(m->header), "%s", dev_label(&app->dev)); }, false);
            app->cur = ViewReadings;
            view_dispatcher_switch_to_view(app->vd, ViewReadings);
        } else {
            if(bc_is_connected(app->bc)) bc_disconnect(app->bc);
            popup_set_header(app->popup, "No sensor data", 64, 26, AlignCenter, AlignCenter);
            popup_set_text(app->popup, app->setup_msg, 64, 40, AlignCenter, AlignCenter);
            popup_set_timeout(app->popup, 1800);
            popup_enable_timeout(app->popup);
        }
        break;
    case EvtPopupDone:
        bc_scan_start(app->bc);
        app->cur = ViewScan;
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

int32_t ble_sensor_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->gui = furi_record_open(RECORD_GUI);
    app->jobs = furi_message_queue_alloc(8, sizeof(JobType));
    app->notify_q = furi_message_queue_alloc(16, sizeof(NotifyMsg));

    app->vd = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->vd, app);
    view_dispatcher_set_custom_event_callback(app->vd, custom_cb);
    view_dispatcher_attach_to_gui(app->vd, app->gui, ViewDispatcherTypeFullscreen);

    app->scan_view = view_alloc();
    view_allocate_model(app->scan_view, ViewModelTypeLocking, sizeof(ScanModel));
    view_set_context(app->scan_view, app);
    view_set_draw_callback(app->scan_view, scan_draw);
    view_set_input_callback(app->scan_view, scan_input);
    view_set_enter_callback(app->scan_view, scan_enter);
    view_set_previous_callback(app->scan_view, scan_exit);
    view_dispatcher_add_view(app->vd, ViewScan, app->scan_view);

    app->readings_view = view_alloc();
    view_allocate_model(app->readings_view, ViewModelTypeLocking, sizeof(ReadingsModel));
    view_set_context(app->readings_view, app);
    view_set_draw_callback(app->readings_view, readings_draw);
    view_set_input_callback(app->readings_view, readings_input);
    view_set_previous_callback(app->readings_view, readings_back);
    view_dispatcher_add_view(app->vd, ViewReadings, app->readings_view);

    app->popup = popup_alloc();
    view_set_previous_callback(popup_get_view(app->popup), stay_popup);
    view_dispatcher_add_view(app->vd, ViewPopup, popup_get_view(app->popup));

    app->timer = furi_timer_alloc(timer_cb, FuriTimerTypePeriodic, app);

    if(!bc_supported()) {
        popup_reset(app->popup);
        popup_set_header(app->popup, "Full BLE stack needed", 64, 26, AlignCenter, AlignCenter);
        popup_set_text(app->popup, "Build with ble_full", 64, 40, AlignCenter, AlignCenter);
        view_set_previous_callback(popup_get_view(app->popup), scan_exit);
        view_dispatcher_switch_to_view(app->vd, ViewPopup);
        view_dispatcher_run(app->vd);
        goto teardown;
    }

    bool was_adv = furi_hal_bt_is_active();
    furi_hal_bt_stop_advertising();
    furi_delay_ms(100);
    app->bc = bc_alloc(notify_cb, app);
    app->worker = furi_thread_alloc_ex("BleSensorW", 3072, worker_thread, app);
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

teardown:
    furi_timer_free(app->timer);
    view_dispatcher_remove_view(app->vd, ViewScan);
    view_dispatcher_remove_view(app->vd, ViewReadings);
    view_dispatcher_remove_view(app->vd, ViewPopup);
    view_free(app->scan_view);
    view_free(app->readings_view);
    popup_free(app->popup);
    view_dispatcher_free(app->vd);
    furi_message_queue_free(app->jobs);
    furi_message_queue_free(app->notify_q);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
