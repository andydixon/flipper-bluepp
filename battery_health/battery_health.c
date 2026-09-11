// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andy Dixon <andy@dixon.cx>
// Battery Health: live fuel-gauge readings (voltage, current, temperature,
// capacity, health, charge state) with a rolling voltage graph. Read-only.
#include <furi.h>
#include <furi_hal_power.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <stdio.h>

#define GRAPH_N 96 // one sample column per ~2 s across the screen width

typedef struct {
    float v, i, temp, vbus;
    uint32_t rem_mah, full_mah, design_mah;
    uint8_t pct, health;
    bool charging, charged;
    float graph[GRAPH_N];
    int graph_count;
} Model;

typedef struct {
    Gui* gui;
    ViewPort* vp;
    FuriMutex* mutex;
    FuriMessageQueue* input;
    Model m;
    bool running;
} App;

static void sample(App* app) {
    Model* m = &app->m;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    m->v = furi_hal_power_get_battery_voltage(FuriHalPowerICFuelGauge);
    m->i = furi_hal_power_get_battery_current(FuriHalPowerICFuelGauge);
    m->temp = furi_hal_power_get_battery_temperature(FuriHalPowerICFuelGauge);
    m->vbus = furi_hal_power_get_usb_voltage();
    m->rem_mah = furi_hal_power_get_battery_remaining_capacity();
    m->full_mah = furi_hal_power_get_battery_full_capacity();
    m->design_mah = furi_hal_power_get_battery_design_capacity();
    m->pct = furi_hal_power_get_pct();
    m->health = furi_hal_power_get_bat_health_pct();
    m->charging = furi_hal_power_is_charging();
    m->charged = furi_hal_power_is_charging_done();
    if(m->graph_count < GRAPH_N) {
        m->graph[m->graph_count++] = m->v;
    } else {
        memmove(m->graph, m->graph + 1, sizeof(float) * (GRAPH_N - 1));
        m->graph[GRAPH_N - 1] = m->v;
    }
    furi_mutex_release(app->mutex);
}

static void draw_cb(Canvas* c, void* ctx) {
    App* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    Model* m = &app->m;
    char buf[64];
    canvas_clear(c);
    canvas_set_font(c, FontPrimary);
    snprintf(buf, sizeof(buf), "Battery %u%%", m->pct);
    canvas_draw_str(c, 2, 9, buf);
    const char* st = m->charged ? "full" : m->charging ? "charging" : "discharging";
    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(c, 126, 9, AlignRight, AlignBottom, st);
    canvas_draw_line(c, 0, 11, 127, 11);

    snprintf(buf, sizeof(buf), "%.3f V   %+.0f mA   %.1f C", (double)m->v, (double)(m->i * 1000), (double)m->temp);
    canvas_draw_str(c, 2, 21, buf);
    snprintf(
        buf, sizeof(buf), "%lu/%lu mAh (design %lu)", (unsigned long)m->rem_mah,
        (unsigned long)m->full_mah, (unsigned long)m->design_mah);
    canvas_draw_str(c, 2, 31, buf);
    int wear = m->design_mah ? 100 - (int)(m->full_mah * 100 / m->design_mah) : 0;
    snprintf(buf, sizeof(buf), "Health %u%%  Wear %d%%  VBUS %.2fV", m->health, wear, (double)m->vbus);
    canvas_draw_str(c, 2, 41, buf);

    // Voltage graph, auto-scaled to 3.3-4.2 V window.
    int gx = 2, gy = 45, gw = GRAPH_N, gh = 17;
    canvas_draw_frame(c, gx, gy, gw + 2, gh + 2);
    for(int k = 0; k < m->graph_count; k++) {
        float v = m->graph[k];
        if(v < 3.3f) v = 3.3f;
        if(v > 4.2f) v = 4.2f;
        int h = (int)((v - 3.3f) / 0.9f * gh);
        canvas_draw_line(c, gx + 1 + k, gy + 1 + gh - h, gx + 1 + k, gy + 1 + gh);
    }
    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* e, void* ctx) {
    App* app = ctx;
    furi_message_queue_put(app->input, e, FuriWaitForever);
}

int32_t battery_health_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->input = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->gui = furi_record_open(RECORD_GUI);
    app->vp = view_port_alloc();
    view_port_draw_callback_set(app->vp, draw_cb, app);
    view_port_input_callback_set(app->vp, input_cb, app);
    gui_add_view_port(app->gui, app->vp, GuiLayerFullscreen);

    app->running = true;
    sample(app);
    uint32_t last = furi_get_tick();
    while(app->running) {
        InputEvent e;
        if(furi_message_queue_get(app->input, &e, 200) == FuriStatusOk) {
            if(e.type == InputTypeShort && e.key == InputKeyBack) app->running = false;
        }
        if(furi_get_tick() - last >= furi_ms_to_ticks(2000)) {
            sample(app);
            last = furi_get_tick();
            view_port_update(app->vp);
        }
    }

    gui_remove_view_port(app->gui, app->vp);
    view_port_free(app->vp);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app->input);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
