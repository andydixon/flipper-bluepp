// BLE Beacon Toolkit: broadcast an iBeacon or a raw advertising payload with a
// chosen (random) MAC, using the firmware's extra-beacon API. Needs the full
// BLE stack. iBeacon mode wraps 21 entered bytes (16 UUID + 2 major + 2 minor +
// 1 Tx) into an Apple manufacturer AD; Raw mode sends the entered bytes as-is.
#include <furi.h>
#include <furi_hal_bt.h>
#include <extra_beacon.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/byte_input.h>
#include <string.h>

typedef enum { ViewMenu, ViewByteInput } ViewId;
typedef enum { ModeRaw, ModeIBeacon } Mode;
typedef enum { EditMac, EditPayload } EditWhat;

// Menu row indices.
enum { RowStartStop, RowState, RowMode, RowMac, RowPayload };

typedef struct {
    Gui* gui;
    ViewDispatcher* vd;
    VariableItemList* menu;
    ByteInput* byte_input;
    VariableItem* it_mode;
    VariableItem* it_state;

    Mode mode;
    EditWhat editing;
    bool running;
    uint8_t mac[EXTRA_BEACON_MAC_ADDR_SIZE]; // little-endian, as the stack wants
    uint8_t payload[EXTRA_BEACON_MAX_DATA_SIZE];
    uint8_t payload_len; // Raw: full AD; iBeacon: 21 significant bytes
} App;

static void beacon_stop(App* app) {
    if(app->running) {
        furi_hal_bt_extra_beacon_stop();
        app->running = false;
    }
}

static uint8_t build_data(App* app, uint8_t* out) {
    if(app->mode == ModeIBeacon) {
        static const uint8_t hdr[] = {0x1A, 0xFF, 0x4C, 0x00, 0x02, 0x15};
        memcpy(out, hdr, sizeof(hdr));
        memcpy(out + sizeof(hdr), app->payload, 21);
        return sizeof(hdr) + 21; // 27
    }
    memcpy(out, app->payload, app->payload_len);
    return app->payload_len;
}

static bool beacon_start(App* app) {
    GapExtraBeaconConfig cfg = {
        .min_adv_interval_ms = 100,
        .max_adv_interval_ms = 150,
        .adv_channel_map = GapAdvChannelMapAll,
        .adv_power_level = GapAdvPowerLevel_0dBm,
        .address_type = GapAddressTypeRandom,
    };
    memcpy(cfg.address, app->mac, EXTRA_BEACON_MAC_ADDR_SIZE);
    if(!furi_hal_bt_extra_beacon_set_config(&cfg)) return false;
    uint8_t data[EXTRA_BEACON_MAX_DATA_SIZE];
    uint8_t len = build_data(app, data);
    if(len > EXTRA_BEACON_MAX_DATA_SIZE) len = EXTRA_BEACON_MAX_DATA_SIZE;
    if(!furi_hal_bt_extra_beacon_set_data(data, len)) return false;
    if(!furi_hal_bt_extra_beacon_start()) return false;
    app->running = true;
    return true;
}

static void refresh_state(App* app) {
    variable_item_set_current_value_text(app->it_state, app->running ? "RUNNING" : "stopped");
    variable_item_set_current_value_text(app->it_mode, app->mode == ModeIBeacon ? "iBeacon" : "Raw");
}

static void mode_changed(VariableItem* item) {
    App* app = variable_item_get_context(item);
    app->mode = variable_item_get_current_value_index(item);
    beacon_stop(app);
    refresh_state(app);
}

static void open_byte_input(App* app, EditWhat what) {
    app->editing = what;
    if(what == EditMac) {
        byte_input_set_header_text(app->byte_input, "MAC (LE, 6 bytes)");
        byte_input_set_result_callback(
            app->byte_input, NULL, NULL, app, app->mac, EXTRA_BEACON_MAC_ADDR_SIZE);
    } else {
        if(app->mode == ModeIBeacon) {
            byte_input_set_header_text(app->byte_input, "UUID16 major2 minor2 tx1");
            app->payload_len = 21;
        } else {
            byte_input_set_header_text(app->byte_input, "Raw AD payload");
            if(app->payload_len == 0 || app->payload_len > EXTRA_BEACON_MAX_DATA_SIZE)
                app->payload_len = 8;
        }
        byte_input_set_result_callback(
            app->byte_input, NULL, NULL, app, app->payload, app->payload_len);
    }
    view_dispatcher_switch_to_view(app->vd, ViewByteInput);
}

static void menu_enter(void* ctx, uint32_t index) {
    App* app = ctx;
    switch(index) {
    case RowStartStop:
        if(app->running)
            beacon_stop(app);
        else
            beacon_start(app);
        refresh_state(app);
        break;
    case RowMac: open_byte_input(app, EditMac); break;
    case RowPayload: open_byte_input(app, EditPayload); break;
    default: break; // State/Mode rows: no enter action
    }
}

static uint32_t to_menu(void* ctx) {
    UNUSED(ctx);
    return ViewMenu;
}
static uint32_t exit_cb(void* ctx) {
    UNUSED(ctx);
    return VIEW_NONE;
}

int32_t ble_beacon_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    uint8_t mac[] = {0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    memcpy(app->mac, mac, 6);
    uint8_t demo[] = {0x02, 0x01, 0x06, 0x08, 0x09, 'F', 'l', 'i', 'p', 'p', 'e', 'r'};
    memcpy(app->payload, demo, sizeof(demo));
    app->payload_len = sizeof(demo);

    app->gui = furi_record_open(RECORD_GUI);
    app->vd = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->vd, app->gui, ViewDispatcherTypeFullscreen);

    app->menu = variable_item_list_alloc();
    variable_item_list_add(app->menu, "Start / Stop", 1, NULL, NULL);
    app->it_state = variable_item_list_add(app->menu, "State", 1, NULL, NULL);
    app->it_mode = variable_item_list_add(app->menu, "Mode", 2, mode_changed, app);
    variable_item_set_values_count(app->it_mode, 2);
    variable_item_list_add(app->menu, "Edit MAC", 1, NULL, NULL);
    variable_item_list_add(app->menu, "Edit payload", 1, NULL, NULL);
    variable_item_list_set_enter_callback(app->menu, menu_enter, app);
    refresh_state(app);
    view_set_previous_callback(variable_item_list_get_view(app->menu), exit_cb);
    view_dispatcher_add_view(app->vd, ViewMenu, variable_item_list_get_view(app->menu));

    app->byte_input = byte_input_alloc();
    view_set_previous_callback(byte_input_get_view(app->byte_input), to_menu);
    view_dispatcher_add_view(app->vd, ViewByteInput, byte_input_get_view(app->byte_input));

    view_dispatcher_switch_to_view(app->vd, ViewMenu);
    view_dispatcher_run(app->vd);

    beacon_stop(app);
    view_dispatcher_remove_view(app->vd, ViewMenu);
    view_dispatcher_remove_view(app->vd, ViewByteInput);
    variable_item_list_free(app->menu);
    byte_input_free(app->byte_input);
    view_dispatcher_free(app->vd);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
