// BLE GATT Fuzzer / logger: connect to a selected peripheral, walk every
// service and characteristic, read all readable values, and (optionally) write
// a set of boundary payloads to writable characteristics, logging every
// response and error. For authorised testing of devices you own. Reuses the
// libble central/GATT layer.
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/elements.h>
#include <storage/storage.h>
#include <ble/core/ble_defs.h>
#include <string.h>
#include <stdarg.h>
#include "ble_central.h"
#include "ble_names.h"

#define TAG "BleGattFuzz"
#define LOG_DIR EXT_PATH("apps_data/ble_gattfuzz")
#define ROWS 3
#define FEED_MAX 64
#define FEED_LINE 40

typedef enum { ViewScan, ViewFeed } ViewId;
typedef enum { EvtTick, EvtSelect, EvtFeed, EvtJobDone } AppEvent;
typedef enum { JobRun, JobFuzz, JobDisconnect, JobQuit } JobType;

typedef struct {
    BcDevice devs[BC_MAX_DEVICES];
    size_t count, sel, top;
} ScanModel;

typedef struct {
    char lines[FEED_MAX][FEED_LINE];
    int count;
    size_t scroll;
    char header[40];
    char status[28];
} FeedModel;

typedef struct {
    Gui* gui;
    ViewDispatcher* vd;
    View* scan_view;
    View* feed_view;
    FuriTimer* timer;
    Storage* storage;
    File* log;
    BleCentral* bc;
    FuriThread* worker;
    FuriMessageQueue* jobs;
    ViewId cur;
    BcDevice dev;
    BcService services[BC_MAX_SERVICES];
    BcChar chars[BC_MAX_CHARS];
    bool fuzz_enabled;
} App;

static const char* dev_label(const BcDevice* d) {
    return d->name[0] ? d->name : "(no name)";
}

static void feed(App* app, const char* fmt, ...) {
    char line[FEED_LINE];
    va_list va;
    va_start(va, fmt);
    vsnprintf(line, sizeof(line), fmt, va);
    va_end(va);
    if(app->log) {
        storage_file_write(app->log, line, strlen(line));
        storage_file_write(app->log, "\n", 1);
        storage_file_sync(app->log);
    }
    with_view_model(
        app->feed_view,
        FeedModel * m,
        {
            strncpy(m->lines[m->count % FEED_MAX], line, FEED_LINE - 1);
            m->lines[m->count % FEED_MAX][FEED_LINE - 1] = 0;
            m->count++;
            m->scroll = 0;
        },
        app->cur == ViewFeed);
}

static void feed_status(App* app, const char* status) {
    with_view_model(
        app->feed_view, FeedModel * m, { strncpy(m->status, status, sizeof(m->status) - 1); },
        app->cur == ViewFeed);
}

static uint16_t char_end(App* app, int svc, int idx, int nch) {
    if(idx + 1 < nch) return app->chars[idx + 1].decl_handle - 1;
    return app->services[svc].end;
}

// ---- worker: enumerate + read (+ optional fuzz) -----------------------------

static void run_enum(App* app, bool fuzz) {
    if(!bc_is_connected(app->bc)) {
        if(!bc_connect(app->bc, &app->dev, 8000)) {
            feed(app, "connect failed");
            feed_status(app, "connect failed");
            return;
        }
        feed(app, "connected, mtu %u", bc_mtu(app->bc));
    }
    int nsvc = bc_discover_services(app->bc, app->services, BC_MAX_SERVICES);
    if(nsvc < 0) {
        feed(app, "service discovery failed");
        return;
    }
    feed_status(app, fuzz ? "fuzzing..." : "reading...");
    // Boundary payloads for the fuzz pass.
    static const uint8_t p_zero[1] = {0x00};
    static const uint8_t p_ff[1] = {0xFF};
    static const uint8_t p_long[20] =
        {0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
         0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41};
    struct {
        const uint8_t* d;
        uint8_t len;
        const char* tag;
    } pats[] = {{NULL, 0, "empty"}, {p_zero, 1, "0x00"}, {p_ff, 1, "0xFF"}, {p_long, 20, "20xA"}};

    uint8_t val[BC_VALUE_MAX];
    FuriString* s = furi_string_alloc();
    for(int si = 0; si < nsvc; si++) {
        furi_string_reset(s);
        ble_uuid_str(s, app->services[si].uuid, app->services[si].uuid_len);
        const char* sn = ble_uuid_name(app->services[si].uuid, app->services[si].uuid_len);
        feed(app, "SVC %s %s", furi_string_get_cstr(s), sn ? sn : "");
        int nch = bc_discover_chars(app->bc, &app->services[si], app->chars, BC_MAX_CHARS);
        for(int ci = 0; ci < nch; ci++) {
            BcChar* c = &app->chars[ci];
            furi_string_reset(s);
            ble_uuid_str(s, c->uuid, c->uuid_len);
            if(c->props & CHAR_PROP_READ) {
                int n = bc_read(app->bc, c->value_handle, val, BC_VALUE_MAX);
                if(n >= 0) {
                    FuriString* h = furi_string_alloc();
                    ble_hex(h, val, n < 8 ? n : 8);
                    feed(app, " R %04X=%s", c->value_handle, furi_string_get_cstr(h));
                    furi_string_free(h);
                } else {
                    feed(app, " R %04X err:%s", c->value_handle,
                        ble_att_error_name(bc_last_error(app->bc)) ? ble_att_error_name(bc_last_error(app->bc)) : "?");
                }
            }
            bool writable = c->props & (CHAR_PROP_WRITE | CHAR_PROP_WRITE_WITHOUT_RESP);
            if(fuzz && writable) {
                bool wr = c->props & CHAR_PROP_WRITE;
                for(size_t pi = 0; pi < COUNT_OF(pats); pi++) {
                    bool ok = bc_write(app->bc, c->value_handle, pats[pi].d, pats[pi].len, wr);
                    feed(app, " W %04X %s:%s", c->value_handle, pats[pi].tag, ok ? "ok" : "rej");
                }
            }
            UNUSED(char_end);
        }
    }
    furi_string_free(s);
    feed(app, fuzz ? "fuzz pass done" : "read pass done");
    feed_status(app, "done (OK=fuzz)");
}

static int32_t worker_thread(void* ctx) {
    App* app = ctx;
    JobType job;
    while(furi_message_queue_get(app->jobs, &job, FuriWaitForever) == FuriStatusOk) {
        if(job == JobQuit) break;
        if(job == JobRun)
            run_enum(app, false);
        else if(job == JobFuzz)
            run_enum(app, true);
        else if(job == JobDisconnect) {
            bc_disconnect(app->bc);
            bc_scan_start(app->bc);
        }
        view_dispatcher_send_custom_event(app->vd, EvtJobDone);
    }
    return 0;
}

// ---- scan view --------------------------------------------------------------

static void scan_draw(Canvas* c, void* m_) {
    ScanModel* m = m_;
    FuriString* s = furi_string_alloc();
    canvas_clear(c);
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 2, 9, "GATT Fuzzer");
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

// ---- feed view --------------------------------------------------------------

static void feed_draw(Canvas* c, void* m_) {
    FeedModel* m = m_;
    canvas_clear(c);
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 2, 9, m->header[0] ? m->header : "GATT");
    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(c, 126, 9, AlignRight, AlignBottom, m->status);
    canvas_draw_line(c, 0, 11, 127, 11);
    int visible = 5;
    int total = m->count;
    int shown = total < visible ? total : visible;
    for(int r = 0; r < shown; r++) {
        int idx = total - 1 - (int)m->scroll - (shown - 1 - r);
        if(idx < 0 || idx < total - FEED_MAX) continue;
        canvas_draw_str(c, 2, 22 + r * 9, m->lines[idx % FEED_MAX]);
    }
    if(total > visible) elements_scrollbar_pos(c, 126, 12, 52, m->scroll, total);
}

static bool feed_input(InputEvent* e, void* ctx) {
    App* app = ctx;
    if(e->type != InputTypeShort && e->type != InputTypeRepeat) return false;
    if(e->key == InputKeyOk && e->type == InputTypeShort) {
        furi_message_queue_put(app->jobs, &(JobType){JobFuzz}, 0);
        return true;
    }
    if(e->key != InputKeyUp && e->key != InputKeyDown) return false;
    with_view_model(
        app->feed_view,
        FeedModel * m,
        {
            int maxs = m->count > 5 ? m->count - 5 : 0;
            if(e->key == InputKeyDown && (int)m->scroll < maxs) m->scroll++;
            if(e->key == InputKeyUp && m->scroll > 0) m->scroll--;
        },
        true);
    return true;
}

static uint32_t feed_back(void* ctx) {
    App* app = ctx;
    furi_message_queue_put(app->jobs, &(JobType){JobDisconnect}, 0);
    app->cur = ViewScan;
    return ViewScan;
}
static uint32_t scan_exit(void* ctx) {
    UNUSED(ctx);
    return VIEW_NONE;
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
            with_view_model(
                app->feed_view,
                FeedModel * m,
                {
                    m->count = 0;
                    m->scroll = 0;
                    snprintf(m->header, sizeof(m->header), "%s", dev_label(&app->dev));
                    strncpy(m->status, "working", sizeof(m->status) - 1);
                },
                false);
            app->cur = ViewFeed;
            view_dispatcher_switch_to_view(app->vd, ViewFeed);
            furi_message_queue_put(app->jobs, &(JobType){JobRun}, 0);
        }
    } break;
    case EvtJobDone: break;
    default: return false;
    }
    return true;
}

static void timer_cb(void* ctx) {
    App* app = ctx;
    view_dispatcher_send_custom_event(app->vd, EvtTick);
}

int32_t ble_gattfuzz_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->jobs = furi_message_queue_alloc(8, sizeof(JobType));

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

    app->feed_view = view_alloc();
    view_allocate_model(app->feed_view, ViewModelTypeLocking, sizeof(FeedModel));
    view_set_context(app->feed_view, app);
    view_set_draw_callback(app->feed_view, feed_draw);
    view_set_input_callback(app->feed_view, feed_input);
    view_set_previous_callback(app->feed_view, feed_back);
    view_dispatcher_add_view(app->vd, ViewFeed, app->feed_view);

    app->timer = furi_timer_alloc(timer_cb, FuriTimerTypePeriodic, app);

    if(!bc_supported()) {
        view_dispatcher_switch_to_view(app->vd, ViewScan);
        view_dispatcher_run(app->vd);
        goto teardown;
    }

    storage_simply_mkdir(app->storage, LOG_DIR);
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    FuriString* path = furi_string_alloc_printf(
        LOG_DIR "/fuzz_%02u%02u%02u.log", dt.hour, dt.minute, dt.second);
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
    app->worker = furi_thread_alloc_ex("BleFuzzW", 3072, worker_thread, app);
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
    if(app->log) {
        storage_file_close(app->log);
        storage_file_free(app->log);
    }

teardown:
    furi_timer_free(app->timer);
    view_dispatcher_remove_view(app->vd, ViewScan);
    view_dispatcher_remove_view(app->vd, ViewFeed);
    view_free(app->scan_view);
    view_free(app->feed_view);
    view_dispatcher_free(app->vd);
    furi_message_queue_free(app->jobs);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
