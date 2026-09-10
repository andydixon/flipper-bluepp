// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andy Dixon <andy@dixon.cx>
// BT Inspector: BLE scanner + GATT explorer for Flipper Zero (full-stack firmware build).
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/elements.h>
#include <gui/modules/submenu.h>
#include <gui/modules/popup.h>
#include <gui/modules/text_input.h>
#include <gui/modules/number_input.h>
#include <storage/storage.h>
#include <ble/core/ble_defs.h>
#include <string.h>
#include <stdarg.h>
#include "ble_central.h"
#include "ble_names.h"

#define TAG "BtInspector"
#define LOG_DIR EXT_PATH("apps_data/bt_inspector")
#define HIST_MAX 16 // ponytail: history kept for the open characteristic only; the .log has everything
#define HIST_DATA 24
#define ROWS 3u
#define INFO_ITEM 0xFFFF

typedef enum {
    ViewScan,
    ViewPage,
    ViewPopup,
    ViewServices,
    ViewChars,
    ViewWriteMenu,
    ViewTextInput,
    ViewNumberInput,
} ViewId;

typedef enum {
    EvtTick,
    EvtScanSelect,
    EvtPageLeft,
    EvtPageCenter,
    EvtPageRight,
    EvtJobDone,
    EvtNotify,
    EvtPopupDone,
} AppEvent;

typedef enum {
    JobConnect,
    JobServices,
    JobChars,
    JobRead,
    JobWrite,
    JobNotify,
    JobInfo,
    JobDisconnect,
    JobQuit,
} JobType;

typedef enum { PageError, PageDetails, PageChar, PageInfo } PageKind;
typedef enum { AfterPopupDetails, AfterPopupDisconnect, AfterPopupServices } AfterPopup;

typedef struct {
    BcDevice devs[BC_MAX_DEVICES];
    size_t count, sel, top;
    bool scanning;
    uint8_t scan_err;
} ScanModel;

typedef struct {
    FuriString* text;
    size_t scroll, max_scroll;
    const char *left, *center, *right;
} PageModel;

typedef struct {
    char time[12];
    uint8_t len;
    uint8_t data[HIST_DATA];
} HistEntry;

typedef struct {
    uint16_t handle;
    uint8_t len;
    uint8_t data[HIST_DATA];
} NotifyMsg;

typedef struct {
    Gui* gui;
    ViewDispatcher* vd;
    View* scan_view;
    View* page_view;
    Popup* popup;
    Submenu* services_menu;
    Submenu* chars_menu;
    Submenu* write_menu;
    TextInput* text_input;
    NumberInput* number_input;
    FuriTimer* timer;
    Storage* storage;
    File* log;

    BleCentral* bc;
    FuriThread* worker;
    FuriMessageQueue* jobs;
    FuriMessageQueue* notify_q;

    ViewId cur_view;
    PageKind page_kind;
    AfterPopup after_popup;
    bool was_advertising;

    BcDevice dev;
    BcService services[BC_MAX_SERVICES];
    int svc_count, svc_sel;
    BcChar chars[BC_MAX_CHARS];
    int chr_count, chr_sel;
    uint8_t value[BC_VALUE_MAX];
    int value_len;
    bool notify_on;
    HistEntry hist[HIST_MAX];
    int hist_count;
    uint8_t write_buf[BC_VALUE_MAX];
    uint8_t write_len;
    int write_mode; // 0 text, 1 hex, 2 number
    char text_buf[128];

    JobType job;
    bool job_ok;
    volatile bool cancel;
    FuriString* status;
    FuriString* info_text;
} App;

static App* nav_app; // navigation callbacks only receive the view's own context

// ---- logging -----------------------------------------------------------------

static void now_str(char* out, size_t n) {
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    snprintf(out, n, "%02u:%02u:%02u", dt.hour, dt.minute, dt.second);
}

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
        LOG_DIR "/bti_%04u%02u%02u_%02u%02u%02u.log", dt.year, dt.month, dt.day, dt.hour, dt.minute,
        dt.second);
    app->log = storage_file_alloc(app->storage);
    if(!storage_file_open(app->log, furi_string_get_cstr(p), FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        FURI_LOG_W(TAG, "log open failed: %s", furi_string_get_cstr(p));
        storage_file_free(app->log);
        app->log = NULL;
    }
    furi_string_free(p);
}

// ---- text helpers ------------------------------------------------------------

static const char* vendor_of(const BcDevice* d, uint16_t* id) {
    uint8_t l = 0;
    const uint8_t* m = bc_adv_find(d->adv, d->adv_len, 0xFF, &l);
    if(!m) m = bc_adv_find(d->rsp, d->rsp_len, 0xFF, &l);
    if(!m || l < 2) return NULL;
    *id = m[0] | (m[1] << 8);
    return ble_company_name(*id);
}

static const char* dev_label(const BcDevice* d) {
    return d->name[0] ? d->name : "(no name)";
}

static void char_label(FuriString* out, const BcChar* c) {
    const char* n = ble_uuid_name(c->uuid, c->uuid_len);
    if(n) {
        furi_string_set_str(out, n);
    } else {
        furi_string_reset(out);
        ble_uuid_str(out, c->uuid, c->uuid_len);
    }
}

static void cat_uuid_list(FuriString* out, const uint8_t* p, uint8_t len, uint8_t ulen) {
    for(uint8_t i = 0; i + ulen <= len; i += ulen) {
        const char* n = ulen == 4 ? NULL : ble_uuid_name(p + i, ulen);
        furi_string_cat_str(out, "  ");
        if(ulen == 4)
            furi_string_cat_printf(out, "%02X%02X%02X%02X", p[i + 3], p[i + 2], p[i + 1], p[i]);
        else
            ble_uuid_str(out, p + i, ulen);
        if(n) furi_string_cat_printf(out, " %s", n);
        furi_string_cat_str(out, "\n");
    }
}

static void walk_ad(FuriString* out, const uint8_t* data, uint8_t len) {
    uint8_t i = 0;
    while(i < len) {
        uint8_t l = data[i];
        if(!l || i + 1 + l > len) break;
        uint8_t t = data[i + 1], pl = l - 1;
        const uint8_t* p = data + i + 2;
        switch(t) {
        case 0x01:
            if(pl) {
                furi_string_cat_printf(out, "Flags: %02X", p[0]);
                if(p[0] & 0x01) furi_string_cat_str(out, " LtdDisc");
                if(p[0] & 0x02) furi_string_cat_str(out, " GenDisc");
                if(p[0] & 0x04) furi_string_cat_str(out, " NoBR/EDR");
                furi_string_cat_str(out, "\n");
            }
            break;
        case 0x02:
        case 0x03:
            furi_string_cat_str(out, "Services (16-bit):\n");
            cat_uuid_list(out, p, pl, 2);
            break;
        case 0x04:
        case 0x05:
            furi_string_cat_str(out, "Services (32-bit):\n");
            cat_uuid_list(out, p, pl, 4);
            break;
        case 0x06:
        case 0x07:
            furi_string_cat_str(out, "Services (128-bit):\n");
            cat_uuid_list(out, p, pl, 16);
            break;
        case 0x08:
        case 0x09: break; // name shown in the title
        case 0x0A:
            if(pl) furi_string_cat_printf(out, "Tx power: %d dBm\n", (int8_t)p[0]);
            break;
        case 0x16:
            if(pl >= 2) {
                uint16_t u = p[0] | (p[1] << 8);
                const char* n = ble_uuid_name(p, 2);
                furi_string_cat_printf(out, "Svc data %04X%s%s: ", u, n ? " " : "", n ? n : "");
                ble_hex(out, p + 2, pl - 2);
                furi_string_cat_str(out, "\n");
                ble_decode_svc_data(out, u, p + 2, pl - 2);
            }
            break;
        case 0x19:
            if(pl >= 2) {
                uint16_t a = p[0] | (p[1] << 8);
                const char* n = ble_appearance_name(a);
                furi_string_cat_printf(out, "Appearance: %s (%04X)\n", n ? n : "?", a);
            }
            break;
        case 0xFF:
            if(pl >= 2) {
                uint16_t c = p[0] | (p[1] << 8);
                const char* n = ble_company_name(c);
                furi_string_cat_printf(out, "Mfg %04X %s: ", c, n ? n : "?");
                ble_hex(out, p + 2, pl - 2);
                furi_string_cat_str(out, "\n");
                ble_decode_mfg(out, c, p + 2, pl - 2);
            }
            break;
        default:
            furi_string_cat_printf(out, "AD %02X: ", t);
            ble_hex(out, p, pl);
            furi_string_cat_str(out, "\n");
            break;
        }
        i += 1 + l;
    }
}

static void build_details(FuriString* out, const BcDevice* d) {
    furi_string_printf(out, "%s\n", dev_label(d));
    ble_addr_str(out, d->addr);
    furi_string_cat_printf(out, " %s\n", d->addr_type ? "random" : "public");
    furi_string_cat_printf(
        out, "RSSI %d dBm (best %d) %lu pkts\n", d->rssi, d->rssi_max, (unsigned long)d->packets);
    furi_string_cat_printf(
        out, "%s%s, %lus ago\n", ble_adv_type_name(d->evt_type), d->evt_type <= 1 ? " connectable" : "",
        (unsigned long)((furi_get_tick() - d->last_seen) / 1000));
    walk_ad(out, d->adv, d->adv_len);
    walk_ad(out, d->rsp, d->rsp_len);
    furi_string_cat_str(out, "Raw adv: ");
    ble_hex(out, d->adv, d->adv_len);
    if(d->rsp_len) {
        furi_string_cat_str(out, "\nRaw rsp: ");
        ble_hex(out, d->rsp, d->rsp_len);
    }
}

static void log_device(App* app, const BcDevice* d) {
    FuriString* s = furi_string_alloc();
    ble_addr_str(s, d->addr);
    uint16_t cid = 0;
    const char* v = vendor_of(d, &cid);
    FuriString* adv = furi_string_alloc();
    ble_hex(adv, d->adv, d->adv_len);
    app_log(
        app, "DEVICE %s %s rssi=%d name=\"%s\" vendor=%s adv=%s", furi_string_get_cstr(s),
        ble_adv_type_name(d->evt_type), d->rssi, d->name, v ? v : "?", furi_string_get_cstr(adv));
    furi_string_free(adv);
    furi_string_free(s);
}

// ---- scan view ---------------------------------------------------------------

static void scan_draw(Canvas* c, void* m_) {
    ScanModel* m = m_;
    FuriString* s = furi_string_alloc();
    canvas_clear(c);
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 2, 9, "BT Inspector");
    canvas_set_font(c, FontSecondary);
    if(m->scanning)
        furi_string_printf(s, "%u found", (unsigned)m->count);
    else
        furi_string_printf(s, "err 0x%02X", m->scan_err);
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
        int bars = d->rssi > -55 ? 4 : d->rssi > -65 ? 3 : d->rssi > -75 ? 2 : d->rssi > -85 ? 1 : 0;
        for(int b = 0; b < bars; b++) canvas_draw_box(c, 96 + b * 3, y + 8 - (b + 1) * 2, 2, (b + 1) * 2);
        furi_string_reset(s);
        ble_addr_str(s, d->addr);
        uint16_t cid = 0;
        const char* v = vendor_of(d, &cid);
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
    if(e->key == InputKeyOk) {
        if(e->type == InputTypeShort) view_dispatcher_send_custom_event(app->vd, EvtScanSelect);
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
    if(!ok) app_log(app, "SCAN START FAILED 0x%02X", bc_last_error(app->bc));
    uint8_t err = ok ? 0 : bc_last_error(app->bc);
    with_view_model(app->scan_view, ScanModel * m, { m->scanning = ok; m->scan_err = err; }, true);
}

// ---- page view (scrollable text with optional buttons) -----------------------

static void page_draw(Canvas* c, void* m_) {
    PageModel* m = m_;
    canvas_clear(c);
    canvas_set_font(c, FontSecondary);
    bool buttons = m->left || m->center || m->right;
    size_t visible = buttons ? 5 : 6;
    const char* p = furi_string_get_cstr(m->text);
    char line[64];
    size_t idx = 0, drawn = 0;
    while(*p) {
        size_t n = 0, last_space = 0;
        int w = 0;
        while(p[n] && p[n] != '\n' && n < sizeof(line) - 1) {
            int cw = canvas_glyph_width(c, p[n]);
            if(w + cw > 123 && n > 0) break;
            if(p[n] == ' ') last_space = n;
            w += cw;
            n++;
        }
        bool mid = p[n] && p[n] != '\n';
        if(mid && last_space) n = last_space;
        if(idx >= m->scroll && drawn < visible) {
            memcpy(line, p, n);
            line[n] = 0;
            canvas_draw_str(c, 1, 9 + drawn * 10, line);
            drawn++;
        }
        idx++;
        p += n;
        if(*p == '\n' || (*p == ' ' && mid)) p++;
    }
    m->max_scroll = idx > visible ? idx - visible : 0;
    if(idx > visible) elements_scrollbar_pos(c, 126, 0, buttons ? 50 : 63, m->scroll, idx);
    if(m->left) elements_button_left(c, m->left);
    if(m->center) elements_button_center(c, m->center);
    if(m->right) elements_button_right(c, m->right);
}

static bool page_input(InputEvent* e, void* ctx) {
    App* app = ctx;
    if(e->type != InputTypeShort && e->type != InputTypeRepeat) return false;
    bool consumed = false, send = false;
    AppEvent ev = EvtPageCenter;
    with_view_model(
        app->page_view,
        PageModel * m,
        {
            switch(e->key) {
            case InputKeyUp:
                if(m->scroll) m->scroll--;
                consumed = true;
                break;
            case InputKeyDown:
                if(m->scroll < m->max_scroll) m->scroll++;
                consumed = true;
                break;
            case InputKeyLeft:
                send = m->left != NULL;
                ev = EvtPageLeft;
                break;
            case InputKeyOk:
                send = m->center != NULL;
                break;
            case InputKeyRight:
                send = m->right != NULL;
                ev = EvtPageRight;
                break;
            default: break;
            }
        },
        consumed);
    if(send && e->type == InputTypeShort) {
        view_dispatcher_send_custom_event(app->vd, ev);
        return true;
    }
    return consumed;
}

static void page_set(
    App* app,
    PageKind kind,
    FuriString* text,
    const char* left,
    const char* center,
    const char* right,
    bool reset_scroll) {
    app->page_kind = kind;
    with_view_model(
        app->page_view,
        PageModel * m,
        {
            furi_string_set(m->text, text);
            if(reset_scroll) m->scroll = 0;
            m->left = left;
            m->center = center;
            m->right = right;
        },
        true);
}

static void show_view(App* app, ViewId id) {
    app->cur_view = id;
    view_dispatcher_switch_to_view(app->vd, id);
}

static void show_details(App* app, bool reset_scroll) {
    FuriString* s = furi_string_alloc();
    build_details(s, &app->dev);
    page_set(app, PageDetails, s, NULL, app->dev.evt_type <= 1 ? "Connect" : NULL, NULL, reset_scroll);
    furi_string_free(s);
}

static void props_str(FuriString* out, uint8_t p) {
    if(p & CHAR_PROP_READ) furi_string_cat_str(out, "R");
    if(p & CHAR_PROP_WRITE) furi_string_cat_str(out, "W");
    if(p & CHAR_PROP_WRITE_WITHOUT_RESP) furi_string_cat_str(out, "w");
    if(p & CHAR_PROP_NOTIFY) furi_string_cat_str(out, "N");
    if(p & CHAR_PROP_INDICATE) furi_string_cat_str(out, "I");
    if(p & CHAR_PROP_BROADCAST) furi_string_cat_str(out, "B");
}

static void show_char(App* app, bool reset_scroll) {
    BcChar* c = &app->chars[app->chr_sel];
    FuriString* s = furi_string_alloc();
    FuriString* t = furi_string_alloc();
    char_label(t, c);
    furi_string_printf(s, "%s\nUUID ", furi_string_get_cstr(t));
    ble_uuid_str(s, c->uuid, c->uuid_len);
    furi_string_cat_printf(s, "\nHandle 0x%04X  Props ", c->value_handle);
    props_str(s, c->props);
    furi_string_cat_printf(s, "\n%s\n", furi_string_get_cstr(app->status));
    if(app->value_len >= 0 && (app->value_len > 0 || app->hist_count)) {
        furi_string_cat_printf(s, "Value (%d B): ", app->value_len);
        ble_hex(s, app->value, app->value_len);
        furi_string_cat_str(s, "\n");
        ble_decode_value(s, c->uuid, c->uuid_len, app->value, app->value_len);
    }
    if(app->hist_count) furi_string_cat_str(s, "History:\n");
    for(int i = app->hist_count - 1; i >= 0; i--) {
        furi_string_cat_printf(s, "%s ", app->hist[i].time);
        ble_hex(s, app->hist[i].data, app->hist[i].len);
        furi_string_cat_str(s, "\n");
    }
    bool can_sub = c->props & (CHAR_PROP_NOTIFY | CHAR_PROP_INDICATE);
    bool can_write = c->props & (CHAR_PROP_WRITE | CHAR_PROP_WRITE_WITHOUT_RESP);
    page_set(
        app, PageChar, s, can_sub ? (app->notify_on ? "Unsub" : "Notify") : NULL,
        (c->props & CHAR_PROP_READ) ? "Read" : NULL, can_write ? "Write" : NULL, reset_scroll);
    furi_string_free(t);
    furi_string_free(s);
}

static void hist_push(App* app, const uint8_t* d, uint8_t len) {
    if(app->hist_count == HIST_MAX) {
        memmove(app->hist, app->hist + 1, sizeof(HistEntry) * (HIST_MAX - 1));
        app->hist_count--;
    }
    HistEntry* h = &app->hist[app->hist_count++];
    now_str(h->time, sizeof(h->time));
    h->len = len > HIST_DATA ? HIST_DATA : len;
    memcpy(h->data, d, h->len);
}

static void popup_cb(void* ctx);
static void number_input_cb(void* ctx, int32_t number);

static void show_popup(App* app, const char* header, const char* text, bool timeout) {
    popup_reset(app->popup); // clears callback/context too
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
    show_view(app, ViewPopup);
}

static void post_job(App* app, JobType job) {
    furi_message_queue_put(app->jobs, &job, 0);
}

static const char* err_name(App* app) {
    const char* n = ble_att_error_name(bc_last_error(app->bc));
    return n ? n : "error";
}

// ---- worker thread -----------------------------------------------------------

static uint16_t char_end_handle(App* app) {
    if(app->chr_sel + 1 < app->chr_count) return app->chars[app->chr_sel + 1].decl_handle - 1;
    return app->services[app->svc_sel].end;
}

// If the device advertised no name, read the GAP Device Name (0x2A00) over the
// connection so the device page and service list show its real name, not just
// the vendor. Runs on the worker thread right after service discovery.
static void resolve_name(App* app) {
    if(app->dev.name[0]) return; // already have an advertised name
    for(int i = 0; i < app->svc_count; i++) {
        BcService* s = &app->services[i];
        if(!(s->uuid_len == 2 && (s->uuid[0] | (s->uuid[1] << 8)) == 0x1800)) continue; // GAP
        BcChar tmp[8];
        int n = bc_discover_chars(app->bc, s, tmp, COUNT_OF(tmp));
        for(int k = 0; k < n; k++) {
            uint16_t u = tmp[k].uuid_len == 2 ? (tmp[k].uuid[0] | (tmp[k].uuid[1] << 8)) : 0;
            if(u != 0x2A00 || !(tmp[k].props & CHAR_PROP_READ)) continue;
            uint8_t val[BC_NAME_MAX + 1];
            int len = bc_read(app->bc, tmp[k].value_handle, val, BC_NAME_MAX);
            if(len > 0) {
                memcpy(app->dev.name, val, len);
                app->dev.name[len] = 0;
                app_log(app, "NAME resolved: %s", app->dev.name);
            }
            return;
        }
        return; // found GAP but no readable name
    }
}

static void do_info(App* app) {
    furi_string_printf(app->info_text, "%s\nMTU %u\n", dev_label(&app->dev), bc_mtu(app->bc));
    FuriString* label = furi_string_alloc();
    FuriString* dec = furi_string_alloc();
    bool any = false;
    for(int i = 0; i < app->svc_count; i++) {
        BcService* s = &app->services[i];
        if(s->uuid_len != 2) continue;
        uint16_t u = s->uuid[0] | (s->uuid[1] << 8);
        if(u != 0x1800 && u != 0x180A && u != 0x180F) continue;
        BcChar tmp[12];
        int n = bc_discover_chars(app->bc, s, tmp, COUNT_OF(tmp));
        for(int k = 0; k < n; k++) {
            if(!(tmp[k].props & CHAR_PROP_READ)) continue;
            char_label(label, &tmp[k]);
            int len = bc_read(app->bc, tmp[k].value_handle, app->value, BC_VALUE_MAX);
            furi_string_reset(dec);
            if(len < 0) {
                furi_string_set_str(dec, err_name(app));
            } else {
                ble_decode_value(dec, tmp[k].uuid, tmp[k].uuid_len, app->value, len);
                size_t nl = furi_string_search_char(dec, '\n', 0);
                if(nl != FURI_STRING_FAILURE) furi_string_left(dec, nl);
                if(furi_string_empty(dec)) ble_hex(dec, app->value, len);
            }
            furi_string_cat_printf(
                app->info_text, "%s: %s\n", furi_string_get_cstr(label), furi_string_get_cstr(dec));
            app_log(app, "INFO %s = %s", furi_string_get_cstr(label), furi_string_get_cstr(dec));
            any = true;
        }
    }
    if(!any) furi_string_cat_str(app->info_text, "No GAP / Device Info / Battery service");
    furi_string_free(dec);
    furi_string_free(label);
}

static int32_t worker_thread(void* ctx) {
    App* app = ctx;
    JobType job;
    while(furi_message_queue_get(app->jobs, &job, FuriWaitForever) == FuriStatusOk) {
        if(job == JobQuit) break;
        bool ok = false;
        switch(job) {
        case JobConnect: ok = bc_connect(app->bc, &app->dev, 8000); break;
        case JobServices: {
            int n = bc_discover_services(app->bc, app->services, BC_MAX_SERVICES);
            app->svc_count = n < 0 ? 0 : n;
            ok = n >= 0;
            if(ok) resolve_name(app);
        } break;
        case JobChars: {
            int n = bc_discover_chars(app->bc, &app->services[app->svc_sel], app->chars, BC_MAX_CHARS);
            app->chr_count = n < 0 ? 0 : n;
            ok = n >= 0;
        } break;
        case JobRead: {
            int n = bc_read(app->bc, app->chars[app->chr_sel].value_handle, app->value, BC_VALUE_MAX);
            app->value_len = n < 0 ? 0 : n;
            ok = n >= 0;
        } break;
        case JobWrite: {
            BcChar* c = &app->chars[app->chr_sel];
            ok = bc_write(
                app->bc, c->value_handle, app->write_buf, app->write_len, c->props & CHAR_PROP_WRITE);
        } break;
        case JobNotify:
            ok = bc_set_notify(app->bc, &app->chars[app->chr_sel], char_end_handle(app), !app->notify_on);
            if(ok) app->notify_on = !app->notify_on;
            break;
        case JobInfo:
            do_info(app);
            ok = true;
            break;
        case JobDisconnect:
            bc_disconnect(app->bc);
            ok = true;
            break;
        default: break;
        }
        app->job = job;
        app->job_ok = ok;
        view_dispatcher_send_custom_event(app->vd, EvtJobDone);
    }
    return 0;
}

static void notify_cb(uint16_t handle, const uint8_t* data, uint8_t len, void* ctx) {
    App* app = ctx;
    NotifyMsg m = {.handle = handle, .len = len > HIST_DATA ? HIST_DATA : len};
    memcpy(m.data, data, m.len);
    if(furi_message_queue_put(app->notify_q, &m, 0) == FuriStatusOk)
        view_dispatcher_send_custom_event(app->vd, EvtNotify);
}

// ---- menus & inputs ----------------------------------------------------------

static void services_cb(void* ctx, uint32_t index);
static void chars_cb(void* ctx, uint32_t index);

static void fill_services_menu(App* app) {
    submenu_reset(app->services_menu);
    submenu_set_header(app->services_menu, dev_label(&app->dev));
    submenu_add_item(app->services_menu, "* Read device info", INFO_ITEM, services_cb, app);
    FuriString* s = furi_string_alloc();
    for(int i = 0; i < app->svc_count; i++) {
        BcService* sv = &app->services[i];
        const char* n = ble_uuid_name(sv->uuid, sv->uuid_len);
        furi_string_reset(s);
        if(n) {
            furi_string_set_str(s, n);
        } else {
            ble_uuid_str(s, sv->uuid, sv->uuid_len);
        }
        submenu_add_item(app->services_menu, furi_string_get_cstr(s), i, services_cb, app);
        FuriString* u = furi_string_alloc();
        ble_uuid_str(u, sv->uuid, sv->uuid_len);
        app_log(
            app, "SERVICE %s %s handles 0x%04X-0x%04X", furi_string_get_cstr(u), n ? n : "",
            sv->start, sv->end);
        furi_string_free(u);
    }
    furi_string_free(s);
}

static void fill_chars_menu(App* app) {
    submenu_reset(app->chars_menu);
    BcService* sv = &app->services[app->svc_sel];
    const char* sn = ble_uuid_name(sv->uuid, sv->uuid_len);
    submenu_set_header(app->chars_menu, sn ? sn : "Characteristics");
    FuriString* s = furi_string_alloc();
    FuriString* u = furi_string_alloc();
    for(int i = 0; i < app->chr_count; i++) {
        BcChar* c = &app->chars[i];
        char_label(s, c);
        furi_string_cat_str(s, " [");
        props_str(s, c->props);
        furi_string_cat_str(s, "]");
        submenu_add_item(app->chars_menu, furi_string_get_cstr(s), i, chars_cb, app);
        furi_string_reset(u);
        ble_uuid_str(u, c->uuid, c->uuid_len);
        app_log(app, "CHAR %s handle 0x%04X props 0x%02X", furi_string_get_cstr(u), c->value_handle, c->props);
    }
    furi_string_free(u);
    furi_string_free(s);
}

static void services_cb(void* ctx, uint32_t index) {
    App* app = ctx;
    if(index == INFO_ITEM) {
        show_popup(app, "Reading info...", "", false);
        post_job(app, JobInfo);
        return;
    }
    app->svc_sel = index;
    show_popup(app, "Discovering...", "characteristics", false);
    post_job(app, JobChars);
}

static void chars_cb(void* ctx, uint32_t index) {
    App* app = ctx;
    app->chr_sel = index;
    app->hist_count = 0;
    app->value_len = 0;
    app->notify_on = false;
    bool readable = app->chars[index].props & CHAR_PROP_READ;
    furi_string_set_str(app->status, readable ? "Reading..." : "");
    show_char(app, true);
    show_view(app, ViewPage);
    if(readable) post_job(app, JobRead);
}

static void write_menu_cb(void* ctx, uint32_t index) {
    App* app = ctx;
    app->write_mode = index;
    if(index == 2) {
        number_input_set_header_text(app->number_input, "Value (LE int)");
        number_input_set_result_callback(app->number_input, number_input_cb, app, 0, INT32_MIN, INT32_MAX);
        show_view(app, ViewNumberInput);
        return;
    }
    app->text_buf[0] = 0;
    text_input_set_header_text(app->text_input, index == 0 ? "Text value" : "Hex bytes (e.g. 01FF)");
    text_input_set_minimum_length(app->text_input, 1);
    show_view(app, ViewTextInput);
}

static int hexval(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void start_write(App* app) {
    furi_string_set_str(app->status, "Writing...");
    show_char(app, false);
    show_view(app, ViewPage);
    post_job(app, JobWrite);
}

static void text_input_cb(void* ctx) {
    App* app = ctx;
    if(app->write_mode == 0) {
        app->write_len = strlen(app->text_buf);
        memcpy(app->write_buf, app->text_buf, app->write_len);
    } else {
        int n = 0, hi = -1;
        for(char* p = app->text_buf; *p; p++) {
            if(*p == ' ' || *p == ':' || *p == ',') continue;
            int v = hexval(*p);
            if(v < 0) {
                n = -1;
                break;
            }
            if(hi < 0) {
                hi = v;
            } else {
                app->write_buf[n++] = (hi << 4) | v;
                hi = -1;
            }
        }
        if(n <= 0 || hi >= 0) {
            furi_string_set_str(app->status, "Bad hex, nothing written");
            show_char(app, false);
            show_view(app, ViewPage);
            return;
        }
        app->write_len = n;
    }
    start_write(app);
}

static void number_input_cb(void* ctx, int32_t number) {
    App* app = ctx;
    int len = app->value_len == 1 || app->value_len == 2 || app->value_len == 4 ? app->value_len :
              (number >= -128 && number <= 255)                                ? 1 :
              (number >= -32768 && number <= 65535)                            ? 2 :
                                                                                 4;
    uint32_t v = (uint32_t)number;
    for(int i = 0; i < len; i++) app->write_buf[i] = (v >> (8 * i)) & 0xFF;
    app->write_len = len;
    start_write(app);
}

static void popup_cb(void* ctx) {
    App* app = ctx;
    view_dispatcher_send_custom_event(app->vd, EvtPopupDone);
}

// ---- navigation --------------------------------------------------------------

static uint32_t nav_exit(void* ctx) {
    UNUSED(ctx);
    return VIEW_NONE;
}

static uint32_t nav_stay_popup(void* ctx) {
    UNUSED(ctx);
    // Back on a connect/discover popup: abort and return to the scan list.
    nav_app->cancel = true;
    nav_app->cur_view = ViewScan;
    return ViewScan;
}

static uint32_t nav_page_back(void* ctx) {
    UNUSED(ctx);
    App* app = nav_app;
    switch(app->page_kind) {
    case PageDetails: app->cur_view = ViewScan; return ViewScan;
    case PageChar: app->cur_view = ViewChars; return ViewChars;
    case PageInfo: app->cur_view = ViewServices; return ViewServices;
    default: return VIEW_NONE;
    }
}

static uint32_t nav_services_back(void* ctx) {
    UNUSED(ctx);
    App* app = nav_app;
    app_log(app, "DISCONNECT");
    post_job(app, JobDisconnect);
    show_details(app, true);
    app->cur_view = ViewPage;
    return ViewPage;
}

static uint32_t nav_to_services(void* ctx) {
    UNUSED(ctx);
    nav_app->cur_view = ViewServices;
    return ViewServices;
}

static uint32_t nav_to_page(void* ctx) {
    UNUSED(ctx);
    nav_app->cur_view = ViewPage;
    return ViewPage;
}

// ---- event handling ----------------------------------------------------------

static void on_tick(App* app) {
    with_view_model(
        app->scan_view,
        ScanModel * m,
        {
            uint8_t sel_addr[6];
            bool had = m->count > 0;
            if(had) memcpy(sel_addr, m->devs[m->sel].addr, 6);
            m->count = bc_snapshot(app->bc, m->devs, BC_MAX_DEVICES);
            for(size_t i = 0; i < m->count; i++)
                if(!m->devs[i].logged) log_device(app, &m->devs[i]);
            if(had)
                for(size_t i = 0; i < m->count; i++)
                    if(memcmp(m->devs[i].addr, sel_addr, 6) == 0) m->sel = i;
            if(m->sel >= m->count) m->sel = m->count ? m->count - 1 : 0;
            if(m->sel < m->top) m->top = m->sel;
            if(m->sel >= m->top + ROWS) m->top = m->sel - ROWS + 1;
        },
        app->cur_view == ViewScan);
    if(app->cur_view == ViewPage && app->page_kind == PageDetails && !bc_is_connected(app->bc)) {
        if(bc_get_device(app->bc, app->dev.addr, &app->dev)) show_details(app, false);
    }
}

static void on_job_done(App* app) {
    if(app->cancel) { // user backed out of the connect/discover popup
        app->cancel = false;
        if(app->job != JobDisconnect) {
            if(bc_is_connected(app->bc))
                post_job(app, JobDisconnect);
            else
                bc_scan_start(app->bc);
        }
        return;
    }
    switch(app->job) {
    case JobConnect:
        if(app->job_ok) {
            app_log(app, "CONNECTED %s mtu=%u", dev_label(&app->dev), bc_mtu(app->bc));
            popup_set_header(app->popup, "Discovering...", 64, 18, AlignCenter, AlignCenter);
            popup_set_text(app->popup, "services", 64, 40, AlignCenter, AlignCenter);
            post_job(app, JobServices);
        } else {
            app_log(app, "CONNECT FAILED %s: %s", dev_label(&app->dev), err_name(app));
            app->after_popup = AfterPopupDetails;
            show_popup(app, "Connect failed", err_name(app), true);
        }
        break;
    case JobServices:
        if(app->job_ok) {
            fill_services_menu(app);
            show_view(app, ViewServices);
        } else {
            app->after_popup = AfterPopupDisconnect;
            show_popup(app, "Discovery failed", err_name(app), true);
        }
        break;
    case JobChars:
        if(app->job_ok) {
            fill_chars_menu(app);
            show_view(app, ViewChars);
        } else {
            app->after_popup = AfterPopupServices;
            show_popup(app, "Discovery failed", err_name(app), true);
        }
        break;
    case JobRead: {
        FuriString* h = furi_string_alloc();
        ble_hex(h, app->value, app->value_len);
        if(app->job_ok) {
            furi_string_printf(app->status, "Read OK (%d B)", app->value_len);
            hist_push(app, app->value, app->value_len);
            app_log(app, "READ 0x%04X = %s", app->chars[app->chr_sel].value_handle, furi_string_get_cstr(h));
        } else {
            furi_string_printf(app->status, "Read failed: %s", err_name(app));
            app_log(app, "READ 0x%04X failed: %s", app->chars[app->chr_sel].value_handle, err_name(app));
        }
        furi_string_free(h);
        if(app->cur_view == ViewPage && app->page_kind == PageChar) show_char(app, false);
    } break;
    case JobWrite: {
        FuriString* h = furi_string_alloc();
        ble_hex(h, app->write_buf, app->write_len);
        if(app->job_ok) {
            furi_string_printf(app->status, "Write OK (%u B)", app->write_len);
            app_log(app, "WRITE 0x%04X = %s", app->chars[app->chr_sel].value_handle, furi_string_get_cstr(h));
            if(app->chars[app->chr_sel].props & CHAR_PROP_READ) post_job(app, JobRead);
        } else {
            furi_string_printf(app->status, "Write failed: %s", err_name(app));
            app_log(app, "WRITE 0x%04X failed: %s", app->chars[app->chr_sel].value_handle, err_name(app));
        }
        furi_string_free(h);
        if(app->cur_view == ViewPage && app->page_kind == PageChar) show_char(app, false);
    } break;
    case JobNotify:
        if(app->job_ok) {
            furi_string_set_str(app->status, app->notify_on ? "Notifications on" : "Notifications off");
        } else {
            furi_string_printf(app->status, "Subscribe failed: %s", err_name(app));
        }
        app_log(app, "NOTIFY 0x%04X %s", app->chars[app->chr_sel].value_handle, furi_string_get_cstr(app->status));
        if(app->cur_view == ViewPage && app->page_kind == PageChar) show_char(app, false);
        break;
    case JobInfo:
        page_set(app, PageInfo, app->info_text, NULL, NULL, NULL, true);
        show_view(app, ViewPage);
        break;
    case JobDisconnect:
        app_log(app, "DISCONNECTED");
        if(app->cur_view == ViewPage || app->cur_view == ViewScan) bc_scan_start(app->bc);
        break;
    default: break;
    }
}

static void on_notify(App* app) {
    NotifyMsg m;
    while(furi_message_queue_get(app->notify_q, &m, 0) == FuriStatusOk) {
        FuriString* h = furi_string_alloc();
        ble_hex(h, m.data, m.len);
        app_log(app, "NOTIFY 0x%04X = %s", m.handle, furi_string_get_cstr(h));
        furi_string_free(h);
        if(app->chr_sel >= 0 && app->chr_sel < app->chr_count &&
           app->chars[app->chr_sel].value_handle == m.handle) {
            memcpy(app->value, m.data, m.len);
            app->value_len = m.len;
            hist_push(app, m.data, m.len);
            furi_string_set_str(app->status, "Notification");
            if(app->cur_view == ViewPage && app->page_kind == PageChar) show_char(app, false);
        }
    }
}

static bool custom_event_cb(void* ctx, uint32_t event) {
    App* app = ctx;
    switch(event) {
    case EvtTick: on_tick(app); break;
    case EvtScanSelect: {
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
            show_details(app, true);
            show_view(app, ViewPage);
        }
    } break;
    case EvtPageCenter:
        if(app->page_kind == PageDetails) {
            app_log(app, "CONNECTING %s", dev_label(&app->dev));
            app->cancel = false;
            show_popup(app, "Connecting...", dev_label(&app->dev), false);
            post_job(app, JobConnect);
        } else if(app->page_kind == PageChar) {
            furi_string_set_str(app->status, "Reading...");
            show_char(app, false);
            post_job(app, JobRead);
        }
        break;
    case EvtPageLeft:
        if(app->page_kind == PageChar) {
            furi_string_set_str(app->status, app->notify_on ? "Unsubscribing..." : "Subscribing...");
            show_char(app, false);
            post_job(app, JobNotify);
        }
        break;
    case EvtPageRight:
        if(app->page_kind == PageChar) show_view(app, ViewWriteMenu);
        break;
    case EvtJobDone: on_job_done(app); break;
    case EvtNotify: on_notify(app); break;
    case EvtPopupDone:
        if(app->cancel) {
            app->cancel = false;
            bc_scan_start(app->bc);
            show_view(app, ViewScan);
            break;
        }
        if(app->after_popup == AfterPopupServices) {
            show_view(app, ViewServices);
        } else {
            if(app->after_popup == AfterPopupDisconnect) post_job(app, JobDisconnect);
            bc_scan_start(app->bc);
            show_details(app, true);
            show_view(app, ViewPage);
        }
        break;
    default: return false;
    }
    return true;
}

static void timer_cb(void* ctx) {
    App* app = ctx;
    view_dispatcher_send_custom_event(app->vd, EvtTick);
}

// ---- setup / teardown --------------------------------------------------------

static App* app_alloc(void) {
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    nav_app = app;
    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->status = furi_string_alloc();
    app->info_text = furi_string_alloc();
    app->chr_sel = -1;
    app->jobs = furi_message_queue_alloc(8, sizeof(JobType));
    app->notify_q = furi_message_queue_alloc(8, sizeof(NotifyMsg));

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

    app->page_view = view_alloc();
    view_allocate_model(app->page_view, ViewModelTypeLocking, sizeof(PageModel));
    with_view_model(app->page_view, PageModel * m, { m->text = furi_string_alloc(); }, false);
    view_set_context(app->page_view, app);
    view_set_draw_callback(app->page_view, page_draw);
    view_set_input_callback(app->page_view, page_input);
    view_set_previous_callback(app->page_view, nav_page_back);
    view_dispatcher_add_view(app->vd, ViewPage, app->page_view);

    app->popup = popup_alloc();
    popup_set_callback(app->popup, popup_cb);
    popup_set_context(app->popup, app);
    view_set_previous_callback(popup_get_view(app->popup), nav_stay_popup);
    view_dispatcher_add_view(app->vd, ViewPopup, popup_get_view(app->popup));

    app->services_menu = submenu_alloc();
    view_set_previous_callback(submenu_get_view(app->services_menu), nav_services_back);
    view_dispatcher_add_view(app->vd, ViewServices, submenu_get_view(app->services_menu));

    app->chars_menu = submenu_alloc();
    view_set_previous_callback(submenu_get_view(app->chars_menu), nav_to_services);
    view_dispatcher_add_view(app->vd, ViewChars, submenu_get_view(app->chars_menu));

    app->write_menu = submenu_alloc();
    submenu_set_header(app->write_menu, "Write value as");
    submenu_add_item(app->write_menu, "Text", 0, write_menu_cb, app);
    submenu_add_item(app->write_menu, "Hex bytes", 1, write_menu_cb, app);
    submenu_add_item(app->write_menu, "Number", 2, write_menu_cb, app);
    view_set_previous_callback(submenu_get_view(app->write_menu), nav_to_page);
    view_dispatcher_add_view(app->vd, ViewWriteMenu, submenu_get_view(app->write_menu));

    app->text_input = text_input_alloc();
    text_input_set_result_callback(
        app->text_input, text_input_cb, app, app->text_buf, sizeof(app->text_buf), true);
    view_set_previous_callback(text_input_get_view(app->text_input), nav_to_page);
    view_dispatcher_add_view(app->vd, ViewTextInput, text_input_get_view(app->text_input));

    app->number_input = number_input_alloc();
    number_input_set_result_callback(app->number_input, number_input_cb, app, 0, INT32_MIN, INT32_MAX);
    view_set_previous_callback(number_input_get_view(app->number_input), nav_to_page);
    view_dispatcher_add_view(app->vd, ViewNumberInput, number_input_get_view(app->number_input));

    app->timer = furi_timer_alloc(timer_cb, FuriTimerTypePeriodic, app);
    return app;
}

static void app_free(App* app) {
    furi_timer_free(app->timer);
    view_dispatcher_remove_view(app->vd, ViewScan);
    view_dispatcher_remove_view(app->vd, ViewPage);
    view_dispatcher_remove_view(app->vd, ViewPopup);
    view_dispatcher_remove_view(app->vd, ViewServices);
    view_dispatcher_remove_view(app->vd, ViewChars);
    view_dispatcher_remove_view(app->vd, ViewWriteMenu);
    view_dispatcher_remove_view(app->vd, ViewTextInput);
    view_dispatcher_remove_view(app->vd, ViewNumberInput);
    with_view_model(app->page_view, PageModel * m, { furi_string_free(m->text); }, false);
    view_free(app->scan_view);
    view_free(app->page_view);
    popup_free(app->popup);
    submenu_free(app->services_menu);
    submenu_free(app->chars_menu);
    submenu_free(app->write_menu);
    text_input_free(app->text_input);
    number_input_free(app->number_input);
    view_dispatcher_free(app->vd);
    furi_message_queue_free(app->jobs);
    furi_message_queue_free(app->notify_q);
    furi_string_free(app->status);
    furi_string_free(app->info_text);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t bt_inspector_app(void* p) {
    UNUSED(p);
    App* app = app_alloc();

    if(!bc_supported()) {
        FuriString* s = furi_string_alloc_set_str(
            "Radio stack has no scanning\n"
            "This Flipper runs the ST \"BLE Light\" stack (peripheral only). "
            "BT Inspector needs firmware built with COPRO_STACK_TYPE=ble_full. "
            "See build.sh in the app source.");
        page_set(app, PageError, s, NULL, NULL, NULL, true);
        furi_string_free(s);
        show_view(app, ViewPage);
        view_dispatcher_run(app->vd);
        app_free(app);
        return 0;
    }

    log_open(app);
    app->was_advertising = furi_hal_bt_is_active();
    furi_hal_bt_stop_advertising(); // also drops any phone connection; restored on exit
    furi_delay_ms(100); // let the resulting disconnect event drain before hooking the dispatcher
    app->bc = bc_alloc(notify_cb, app);
    app_log(app, "START stack=full advertising_was=%d", app->was_advertising);

    app->worker = furi_thread_alloc_ex("BtInspWorker", 3072, worker_thread, app);
    furi_thread_start(app->worker);
    furi_timer_start(app->timer, furi_ms_to_ticks(300));

    show_view(app, ViewScan); // enter callback starts the scan
    view_dispatcher_run(app->vd);

    furi_timer_stop(app->timer);
    post_job(app, JobQuit);
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);
    bc_free(app->bc); // stops scan, disconnects, unregisters the event handler
    if(app->was_advertising) furi_hal_bt_start_advertising();
    app_log(app, "STOP");
    if(app->log) {
        storage_file_close(app->log);
        storage_file_free(app->log);
    }
    app_free(app);
    return 0;
}
