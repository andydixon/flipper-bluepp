// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andy Dixon <andy@dixon.cx>
// Sub-GHz Spectrum: sweep a centre +/- span window, drawing a live spectrum bar
// graph and a scrolling waterfall of RSSI over time. Left/Right retune the
// centre, Up/Down change the span. Region-unlocked so any hardware-tunable
// centre works.
#include <furi.h>
#include <furi_hal_subghz.h>
#include <furi_hal_region.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <lib/subghz/devices/cc1101_configs.h>
#include <stdio.h>

#define COLS 128
#define WF_ROWS 28
#define SETTLE_US 3000

typedef struct {
    uint32_t center, span;
    float bins[COLS];
    uint8_t waterfall[WF_ROWS][COLS]; // 0..4 intensity, ring buffer by wf_head
    int wf_head;
    uint32_t peak_freq;
    float peak_rssi;
} Model;

typedef struct {
    Gui* gui;
    ViewPort* vp;
    FuriMutex* mutex;
    FuriMessageQueue* input;
    Model m;
    volatile bool active;
} App;

static int32_t sweep_thread(void* ctx) {
    App* app = ctx;
    furi_hal_subghz_reset();
    furi_hal_subghz_idle();
    furi_hal_subghz_load_custom_preset(subghz_device_cc1101_preset_ook_650khz_async_regs);

    while(app->active) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        uint32_t center = app->m.center, span = app->m.span;
        furi_mutex_release(app->mutex);
        uint32_t start = center - span / 2;
        uint32_t step = span / COLS;
        if(step == 0) step = 1;
        float peak = -127.0f;
        uint32_t peak_f = center;
        uint8_t row[COLS];

        for(int i = 0; i < COLS && app->active; i++) {
            uint32_t f = start + (uint32_t)i * step;
            float rssi = -127.0f;
            if(furi_hal_subghz_is_frequency_valid(f)) {
                furi_hal_subghz_idle();
                furi_hal_subghz_set_frequency_and_path(f);
                furi_hal_subghz_flush_rx();
                furi_hal_subghz_rx();
                furi_delay_us(SETTLE_US);
                rssi = furi_hal_subghz_get_rssi();
            }
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->m.bins[i] = rssi;
            furi_mutex_release(app->mutex);
            float clamped = rssi < -110 ? -110 : rssi > -50 ? -50 : rssi;
            row[i] = (uint8_t)((clamped + 110) / 60.0f * 4);
            if(rssi > peak) {
                peak = rssi;
                peak_f = f;
            }
        }
        furi_hal_subghz_idle();
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        memcpy(app->m.waterfall[app->m.wf_head], row, COLS);
        app->m.wf_head = (app->m.wf_head + 1) % WF_ROWS;
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
    canvas_set_font(c, FontSecondary);
    snprintf(
        buf, sizeof(buf), "%lu.%03lu MHz +-%lu kHz", (unsigned long)(m->center / 1000000),
        (unsigned long)(m->center / 1000 % 1000), (unsigned long)(m->span / 2000));
    canvas_draw_str(c, 1, 8, buf);
    snprintf(
        buf, sizeof(buf), "pk %lu.%03lu %d", (unsigned long)(m->peak_freq / 1000000),
        (unsigned long)(m->peak_freq / 1000 % 1000), (int)m->peak_rssi);
    canvas_draw_str_aligned(c, 127, 8, AlignRight, AlignBottom, buf);

    // spectrum, top band 10..30
    for(int i = 0; i < COLS; i++) {
        float r = m->bins[i];
        if(r < -110) r = -110;
        if(r > -50) r = -50;
        int h = (int)((r + 110) / 60.0f * 20);
        canvas_draw_line(c, i, 30 - h, i, 30);
    }
    canvas_draw_line(c, 0, 32, 127, 32);
    // waterfall, rows 34..62, newest at top
    for(int r = 0; r < WF_ROWS; r++) {
        int src = (m->wf_head - 1 - r + WF_ROWS * 2) % WF_ROWS;
        int y = 34 + r;
        if(y > 63) break;
        for(int i = 0; i < COLS; i++) {
            uint8_t v = m->waterfall[src][i];
            if(v >= 3)
                canvas_draw_dot(c, i, y);
            else if(v == 2 && (i % 2 == 0))
                canvas_draw_dot(c, i, y);
            else if(v == 1 && (i % 4 == 0))
                canvas_draw_dot(c, i, y);
        }
    }
    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* e, void* ctx) {
    App* app = ctx;
    furi_message_queue_put(app->input, e, FuriWaitForever);
}

int32_t subghz_spectrum_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->input = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->m.center = 433920000;
    app->m.span = 2000000; // +-1 MHz

    app->gui = furi_record_open(RECORD_GUI);
    app->vp = view_port_alloc();
    view_port_draw_callback_set(app->vp, draw_cb, app);
    view_port_input_callback_set(app->vp, input_cb, app);
    gui_add_view_port(app->gui, app->vp, GuiLayerFullscreen);

    app->active = true;
    FuriThread* th = furi_thread_alloc_ex("SubGhzSpec", 2048, sweep_thread, app);
    furi_thread_start(th);

    bool run = true;
    while(run) {
        InputEvent e;
        if(furi_message_queue_get(app->input, &e, FuriWaitForever) != FuriStatusOk) continue;
        if(e.type != InputTypeShort && e.type != InputTypeRepeat) continue;
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        switch(e.key) {
        case InputKeyBack:
            run = false;
            break;
        case InputKeyLeft:
            app->m.center -= app->m.span / 4;
            break;
        case InputKeyRight:
            app->m.center += app->m.span / 4;
            break;
        case InputKeyUp:
            if(app->m.span < 8000000) app->m.span *= 2;
            break;
        case InputKeyDown:
            if(app->m.span > 250000) app->m.span /= 2;
            break;
        default:
            break;
        }
        furi_mutex_release(app->mutex);
    }

    app->active = false;
    furi_thread_join(th);
    furi_thread_free(th);
    gui_remove_view_port(app->gui, app->vp);
    view_port_free(app->vp);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app->input);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
