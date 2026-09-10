// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andy Dixon <andy@dixon.cx>
// Sub-GHz Scanner: sweep RSSI across a frequency range in fixed steps, show the
// strongest bins, and log any frequency whose RSSI crosses a threshold. Uses the
// region-unlocked firmware so the whole hardware-tunable range is available.
#include <furi.h>
#include <furi_hal_subghz.h>
#include <furi_hal_region.h>
#include <furi_hal_rtc.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <storage/storage.h>
#include <lib/subghz/devices/cc1101_configs.h>
#include <stdio.h>

#define LOG_DIR EXT_PATH("apps_data/subghz_scan")
#define STEP_HZ 250000u
#define SETTLE_US 3500
#define THRESH_DBM (-90.0f)

// Three CC1101 hardware bands (the region unlock removes the regulatory gate;
// these are the PLL limits and remain).
typedef struct {
    uint32_t start, end;
} Band;
static const Band bands[] = {
    {300000000, 348000000},
    {387000000, 464000000},
    {779000000, 928000000},
};

typedef struct {
    uint32_t peak_freq;
    float peak_rssi;
    uint32_t cur_freq;
    float bins[128]; // one column per screen x
    int nbins;
    uint32_t range_start, range_end;
    bool running, paused;
} Model;

typedef struct {
    Gui* gui;
    ViewPort* vp;
    FuriMutex* mutex;
    FuriMessageQueue* input;
    Storage* storage;
    File* log;
    Model m;
    int band_idx; // -1 = all bands
    volatile bool active;
} App;

static void log_line(App* app, const char* fmt, ...) {
    if(!app->log) return;
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    FuriString* s = furi_string_alloc_printf(
        "[%02u:%02u:%02u] ", dt.hour, dt.minute, dt.second);
    va_list va;
    va_start(va, fmt);
    furi_string_cat_vprintf(s, fmt, va);
    va_end(va);
    furi_string_cat_str(s, "\n");
    storage_file_write(app->log, furi_string_get_cstr(s), furi_string_size(s));
    storage_file_sync(app->log);
    furi_string_free(s);
}

static int32_t scan_thread(void* ctx) {
    App* app = ctx;
    furi_hal_subghz_reset();
    furi_hal_subghz_idle();
    furi_hal_subghz_load_custom_preset(subghz_device_cc1101_preset_ook_650khz_async_regs);

    while(app->active) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        bool paused = app->m.paused;
        uint32_t rs = app->m.range_start, re = app->m.range_end;
        furi_mutex_release(app->mutex);
        if(paused) {
            furi_delay_ms(100);
            continue;
        }

        int nb = (int)((re - rs) / STEP_HZ) + 1;
        if(nb > 128) nb = 128;
        uint32_t step = (re - rs) / (nb > 1 ? nb - 1 : 1);
        uint32_t peak_f = rs;
        float peak = -127.0f;
        for(int i = 0; i < nb && app->active; i++) {
            uint32_t f = rs + (uint32_t)i * step;
            if(!furi_hal_subghz_is_frequency_valid(f)) {
                // Band gaps (e.g. in the all-bands sweep): report a floor, not a
                // stale reading left in the bin from a previous pass.
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                app->m.bins[i] = -127.0f;
                furi_mutex_release(app->mutex);
                continue;
            }
            furi_hal_subghz_idle();
            furi_hal_subghz_set_frequency_and_path(f);
            furi_hal_subghz_flush_rx();
            furi_hal_subghz_rx();
            furi_delay_us(SETTLE_US);
            float rssi = furi_hal_subghz_get_rssi();
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->m.bins[i] = rssi;
            app->m.nbins = nb;
            app->m.cur_freq = f;
            furi_mutex_release(app->mutex);
            if(rssi > peak) {
                peak = rssi;
                peak_f = f;
            }
            if(rssi > THRESH_DBM) log_line(app, "%lu Hz  %.0f dBm", (unsigned long)f, (double)rssi);
        }
        furi_hal_subghz_idle();
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->m.peak_freq = peak_f;
        app->m.peak_rssi = peak;
        furi_mutex_release(app->mutex);
        view_port_update(app->vp);
    }
    furi_hal_subghz_idle();
    furi_hal_subghz_sleep();
    return 0;
}

static void draw_cb(Canvas* c, void* ctx) {
    App* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    Model* m = &app->m;
    char buf[48];
    canvas_clear(c);
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 2, 9, "Sub-GHz Scanner");
    canvas_set_font(c, FontSecondary);
    if(m->paused) canvas_draw_str_aligned(c, 126, 9, AlignRight, AlignBottom, "PAUSED");
    canvas_draw_line(c, 0, 11, 127, 11);

    snprintf(
        buf, sizeof(buf), "%lu.%03lu-%lu.%03lu MHz", (unsigned long)(m->range_start / 1000000),
        (unsigned long)(m->range_start / 1000 % 1000), (unsigned long)(m->range_end / 1000000),
        (unsigned long)(m->range_end / 1000 % 1000));
    canvas_draw_str(c, 2, 20, buf);
    snprintf(
        buf, sizeof(buf), "pk %lu.%03luM %ddBm", (unsigned long)(m->peak_freq / 1000000),
        (unsigned long)(m->peak_freq / 1000 % 1000), (int)m->peak_rssi);
    canvas_draw_str(c, 2, 29, buf);

    // spectrum bars, rssi -110..-40 mapped to 0..24 px
    int gy = 32, gh = 24;
    for(int i = 0; i < m->nbins && i < 128; i++) {
        float r = m->bins[i];
        if(r < -110) r = -110;
        if(r > -40) r = -40;
        int h = (int)((r + 110) / 70.0f * gh);
        canvas_draw_line(c, i, gy + gh - h, i, gy + gh);
    }
    canvas_draw_str(c, 2, 63, "OK:pause  <>:band");
    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* e, void* ctx) {
    App* app = ctx;
    furi_message_queue_put(app->input, e, FuriWaitForever);
}

static void set_range(App* app) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    if(app->band_idx < 0) {
        app->m.range_start = bands[0].start;
        app->m.range_end = bands[COUNT_OF(bands) - 1].end;
    } else {
        app->m.range_start = bands[app->band_idx].start;
        app->m.range_end = bands[app->band_idx].end;
    }
    app->m.nbins = 0;
    furi_mutex_release(app->mutex);
}

int32_t subghz_scan_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->input = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->storage = furi_record_open(RECORD_STORAGE);
    app->band_idx = -1;
    set_range(app);

    storage_simply_mkdir(app->storage, LOG_DIR);
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    FuriString* path = furi_string_alloc_printf(
        LOG_DIR "/scan_%02u%02u%02u.log", dt.hour, dt.minute, dt.second);
    app->log = storage_file_alloc(app->storage);
    if(!storage_file_open(app->log, furi_string_get_cstr(path), FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_free(app->log);
        app->log = NULL;
    }
    furi_string_free(path);

    app->gui = furi_record_open(RECORD_GUI);
    app->vp = view_port_alloc();
    view_port_draw_callback_set(app->vp, draw_cb, app);
    view_port_input_callback_set(app->vp, input_cb, app);
    gui_add_view_port(app->gui, app->vp, GuiLayerFullscreen);

    app->active = true;
    FuriThread* th = furi_thread_alloc_ex("SubGhzScan", 2048, scan_thread, app);
    furi_thread_start(th);

    bool run = true;
    while(run) {
        InputEvent e;
        if(furi_message_queue_get(app->input, &e, FuriWaitForever) != FuriStatusOk) continue;
        if(e.type != InputTypeShort) continue;
        if(e.key == InputKeyBack) {
            run = false;
        } else if(e.key == InputKeyOk) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->m.paused = !app->m.paused;
            furi_mutex_release(app->mutex);
        } else if(e.key == InputKeyRight) {
            app->band_idx = app->band_idx + 1 >= (int)COUNT_OF(bands) ? -1 : app->band_idx + 1;
            set_range(app);
        } else if(e.key == InputKeyLeft) {
            app->band_idx = app->band_idx - 1 < -1 ? (int)COUNT_OF(bands) - 1 : app->band_idx - 1;
            set_range(app);
        }
        view_port_update(app->vp);
    }

    app->active = false;
    furi_thread_join(th);
    furi_thread_free(th);
    gui_remove_view_port(app->gui, app->vp);
    view_port_free(app->vp);
    furi_record_close(RECORD_GUI);
    if(app->log) {
        storage_file_close(app->log);
        storage_file_free(app->log);
    }
    furi_record_close(RECORD_STORAGE);
    furi_message_queue_free(app->input);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
