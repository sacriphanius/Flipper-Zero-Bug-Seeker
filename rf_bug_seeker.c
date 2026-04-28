#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <subghz/devices/devices.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <applications/drivers/subghz/cc1101_ext/cc1101_ext_interconnect.h>
#include <applications/services/power/power_service/power.h>
#include <notification/notification_messages.h>
#include <math.h>

#define TAG "RFBugSeeker"

static const uint32_t common_frequencies[] = {
    315000000, 318000000, 390000000, 433075000, 433420000, 
    433889000, 433920000, 434420000, 434775000, 438900000,
    868350000, 868850000, 868950000, 915000000, 925000000
};
#define COMMON_FREQ_COUNT (sizeof(common_frequencies) / sizeof(common_frequencies[0]))

typedef struct {
    uint32_t start;
    uint32_t end;
} FreqRange;

static const FreqRange scan_ranges[] = {
    {300000000, 348000000},
    {387000000, 464000000},
    {779000000, 928000000},
};
#define RANGE_COUNT (sizeof(scan_ranges) / sizeof(scan_ranges[0]))
#define SCAN_STEP 250000

typedef struct {
    uint32_t frequency;
    float rssi;
    bool is_locked;
    float threshold;
    uint8_t range_index;
    uint8_t common_index;
    bool scan_mode_common;
    const SubGhzDevice* device;
    float rssi_history[32]; 
    uint8_t history_index;
    FuriMutex* mutex;
    bool running;
} AppState;

static void draw_gauge(Canvas* canvas, float rssi, int* level) {
    int cx = 32;
    int cy = 52;
    int r = 26; 

    float normalized = (rssi + 110.0f) / 90.0f;
    if(normalized < 0.0f) normalized = 0.0f;
    if(normalized > 1.0f) normalized = 1.0f;

    *level = (int)(normalized * 4.99f) + 1;

    for(float a = M_PI; a <= 2.0f * M_PI; a += 0.05f) {
        int x1 = cx + (int)(cosf(a) * r);
        int y1 = cy + (int)(sinf(a) * r);
        int x2 = cx + (int)(cosf(a + 0.05f) * r);
        int y2 = cy + (int)(sinf(a + 0.05f) * r);
        canvas_draw_line(canvas, x1, y1, x2, y2);
        canvas_draw_line(canvas, x1, y1-1, x2, y2-1);
    }

    for(int i = 0; i <= 5; i++) {
        float a = M_PI + (i * M_PI / 5.0f);
        int x1 = cx + (int)(cosf(a) * r);
        int y1 = cy + (int)(sinf(a) * r);
        int x2 = cx + (int)(cosf(a) * (r - 3)); 
        int y2 = cy + (int)(sinf(a) * (r - 3));
        canvas_draw_line(canvas, x1, y1, x2, y2); 

        if(i < 5) { 
            float mid_a = M_PI + (i * M_PI / 5.0f) + (M_PI / 10.0f);
            int x_num = cx + (int)(cosf(mid_a) * (r - 9));
            int y_num = cy + (int)(sinf(mid_a) * (r - 9));
            char n_str[4];
            snprintf(n_str, sizeof(n_str), "%u", i + 1);
            canvas_draw_str_aligned(canvas, x_num, y_num, AlignCenter, AlignCenter, n_str);
        }
    }

    float needle_angle = M_PI + (normalized * M_PI);
    int nx = cx + (int)(cosf(needle_angle) * (r - 5));
    int ny = cy + (int)(sinf(needle_angle) * (r - 5));
    canvas_draw_line(canvas, cx, cy, nx, ny);
    canvas_draw_circle(canvas, cx, cy, 3);
}

static void render_callback(Canvas* canvas, void* ctx) {
    AppState* state = ctx;
    furi_mutex_acquire(state->mutex, FuriWaitForever);

    int level = 1;
    draw_gauge(canvas, state->rssi, &level);

    int bar_w = 4;
    int bar_h = 18;
    int bar_gap = 2;
    int start_y = 107; 

    for(int i = 0; i < 5; i++) {
        int y = start_y - (i * (bar_h + bar_gap));
        if(i < level) {
            canvas_draw_box(canvas, 0, y, bar_w, bar_h); 
            canvas_draw_box(canvas, 60, y, bar_w, bar_h); 
        } else {
            canvas_draw_frame(canvas, 0, y, bar_w, bar_h); 
            canvas_draw_frame(canvas, 60, y, bar_w, bar_h); 
        }
    }

    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 1, 1, 21, 9);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontSecondary);
    char lvl_str[8];
    snprintf(lvl_str, sizeof(lvl_str), "L:%u", (unsigned int)level);
    canvas_draw_str_aligned(canvas, 11, 6, AlignCenter, AlignCenter, lvl_str);

    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 42, 1, 21, 9);
    canvas_set_color(canvas, ColorWhite);
    bool is_ext = (state->device == subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_EXT_NAME));
    canvas_draw_str_aligned(canvas, 52, 6, AlignCenter, AlignCenter, is_ext ? "EXT" : "INT");

    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);

    int graph_x = 16;
    int graph_y = 58;
    int graph_w = 32;
    int graph_h = 7;
    canvas_draw_frame(canvas, graph_x - 1, graph_y - 1, graph_w + 2, graph_h + 2);
    for(int i = 0; i < 32; i++) {
        float h_rssi = state->rssi_history[(state->history_index + i) % 32];
        float h_norm = (h_rssi + 110.0f) / 90.0f;
        if(h_norm < 0.0f) h_norm = 0.0f;
        if(h_norm > 1.0f) h_norm = 1.0f;
        int bar_h = (int)(h_norm * graph_h);
        canvas_draw_line(canvas, graph_x + i, graph_y + graph_h - 1, 
                         graph_x + i, graph_y + graph_h - 1 - bar_h);
    }

    char freq_str[32];
    snprintf(freq_str, sizeof(freq_str), "%lu.%02lu", 
             state->frequency / 1000000, (state->frequency % 1000000) / 10000);

    canvas_set_font(canvas, FontPrimary); // Bold Frequency
    canvas_draw_str_aligned(canvas, 32, 92, AlignCenter, AlignBottom, freq_str);
    canvas_set_font(canvas, FontSecondary); // Normal MHz
    canvas_draw_str_aligned(canvas, 32, 102, AlignCenter, AlignBottom, "MHz");

    bool blink = (furi_get_tick() / 500) % 2;
    if(blink || state->is_locked) {
        // Blinking Box with smaller text (stays away from side bars)
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_box(canvas, 6, 111, 52, 11);
        canvas_set_color(canvas, ColorWhite);
        canvas_set_font(canvas, FontSecondary);
        const char* status = state->is_locked ? "LOCKED" : "SCANNING";
        canvas_draw_str_aligned(canvas, 32, 119, AlignCenter, AlignBottom, status);
        canvas_set_color(canvas, ColorBlack); // Reset color
    }

    furi_mutex_release(state->mutex);
}

static void input_callback(InputEvent* input_event, void* ctx) {
    FuriMessageQueue* event_queue = ctx;
    furi_message_queue_put(event_queue, input_event, FuriWaitForever);
}

static int32_t worker_thread(void* context) {
    AppState* state = context;

    furi_mutex_acquire(state->mutex, FuriWaitForever);
    subghz_devices_begin(state->device);
    subghz_devices_idle(state->device);
    subghz_devices_load_preset(state->device, FuriHalSubGhzPresetOok650Async, NULL);
    furi_mutex_release(state->mutex);

    while(state->running) {
        furi_mutex_acquire(state->mutex, FuriWaitForever);

        if(!state->is_locked) {
            uint32_t target_freq;
            if(state->scan_mode_common) {
                target_freq = common_frequencies[state->common_index];
                state->common_index = (state->common_index + 1) % COMMON_FREQ_COUNT;
                if(state->common_index == 0) state->scan_mode_common = false;
            } else {
                state->frequency += SCAN_STEP;
                if(state->frequency > scan_ranges[state->range_index].end) {
                    state->range_index = (state->range_index + 1) % RANGE_COUNT;
                    state->frequency = scan_ranges[state->range_index].start;
                    state->scan_mode_common = true;
                }
                target_freq = state->frequency;
            }

            subghz_devices_idle(state->device);
            subghz_devices_set_frequency(state->device, target_freq);
            subghz_devices_set_rx(state->device);
            furi_delay_ms(20);

            float rssi = subghz_devices_get_rssi(state->device);
            state->rssi = rssi;
            state->frequency = target_freq;

            state->rssi_history[state->history_index] = rssi;
            state->history_index = (state->history_index + 1) % 32;

            float current_threshold = state->threshold;

            if(target_freq >= 924000000 && target_freq <= 928000000) {
                current_threshold = -45.0f; 
            }

            if(rssi > current_threshold) {

                furi_delay_ms(30);
                float confirm_rssi = subghz_devices_get_rssi(state->device);

                if(confirm_rssi > current_threshold - 5.0f) { 
                    state->is_locked = true;
                    NotificationApp* notifications = furi_record_open(RECORD_NOTIFICATION);
                    notification_message(notifications, &sequence_set_vibro_on);
                    furi_delay_ms(100);
                    notification_message(notifications, &sequence_reset_vibro);
                    furi_record_close(RECORD_NOTIFICATION);
                }
            }
        } else {
            state->rssi = subghz_devices_get_rssi(state->device);
        }

        NotificationApp* notifications = furi_record_open(RECORD_NOTIFICATION);
        if(!state->is_locked) {
            static bool blue_toggle = false;
            blue_toggle = !blue_toggle;
            notification_message(notifications, blue_toggle ? &sequence_set_blue_255 : &sequence_reset_blue);
        } else {
            float norm = (state->rssi + 110.0f) / 90.0f;
            int level = (int)(norm * 4.99f) + 1;
            notification_message(notifications, &sequence_reset_blue);
            if(level == 1) { notification_message(notifications, &sequence_set_green_255); }
            else if(level <= 3) { notification_message(notifications, &sequence_set_green_255); notification_message(notifications, &sequence_set_red_255); }
            else { notification_message(notifications, &sequence_set_red_255); }

            int audio_delay = 500 - (int)(norm * 450);
            if(audio_delay < 20) audio_delay = 20;

            if(furi_hal_speaker_acquire(100)) {
                furi_hal_speaker_start(2000.0f, 1.0f); 
                notification_message(notifications, &sequence_set_vibro_on);
                furi_delay_ms(4); 
                furi_hal_speaker_stop();
                notification_message(notifications, &sequence_reset_vibro);
                furi_hal_speaker_release();
            }
            furi_delay_ms(audio_delay);
        }
        furi_record_close(RECORD_NOTIFICATION);

        furi_mutex_release(state->mutex);
        furi_delay_ms(10); 
    }
    return 0;
}

int32_t rf_bug_seeker_app(void* p) {
    UNUSED(p);
    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    AppState* state = malloc(sizeof(AppState));
    state->mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    subghz_devices_init();

    Power* power = furi_record_open(RECORD_POWER);
    bool otg_was_enabled = furi_hal_power_is_otg_enabled();
    if(!otg_was_enabled) {
        power_enable_otg(power, true);
        furi_delay_ms(10); 
    }

    const SubGhzDevice* ext_device = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_EXT_NAME);
    bool using_ext_power = false;
    if(ext_device && subghz_devices_is_connect(ext_device)) {
        state->device = ext_device;
        using_ext_power = true;
    } else {
        if(!otg_was_enabled) {
            power_enable_otg(power, false); 
        }
        state->device = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
    }
    furi_record_close(RECORD_POWER);

    state->frequency = scan_ranges[0].start;
    state->rssi = -120.0f;
    state->is_locked = false;
    state->threshold = -65.0f; 
    state->range_index = 0;
    state->common_index = 0;
    state->scan_mode_common = true;
    state->running = true;

    ViewPort* view_port = view_port_alloc();
    view_port_set_orientation(view_port, ViewPortOrientationVertical);
    view_port_draw_callback_set(view_port, render_callback, state);
    view_port_input_callback_set(view_port, input_callback, event_queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    FuriThread* worker = furi_thread_alloc();
    furi_thread_set_name(worker, "RFBugWorker");
    furi_thread_set_stack_size(worker, 2048);
    furi_thread_set_context(worker, state);
    furi_thread_set_callback(worker, worker_thread);
    furi_thread_start(worker);

    InputEvent event;
    while(state->running) {
        if(furi_message_queue_get(event_queue, &event, 100) == FuriStatusOk) {
            if(event.type == InputTypeShort || event.type == InputTypeLong) {
                if(event.key == InputKeyBack) {
                    state->running = false;
                } else if(event.key == InputKeyOk) {
                    furi_mutex_acquire(state->mutex, FuriWaitForever);
                    state->is_locked = false;
                    state->scan_mode_common = true;
                    state->common_index = 0;
                    furi_mutex_release(state->mutex);
                } else if(event.key == InputKeyUp) {
                    state->threshold += 1.0f;
                } else if(event.key == InputKeyDown) {
                    state->threshold -= 1.0f;
                }
            }
        }
        view_port_update(view_port);
    }

    furi_thread_join(worker);
    furi_thread_free(worker);

    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);

    subghz_devices_idle(state->device);
    subghz_devices_sleep(state->device);
    subghz_devices_end(state->device);
    subghz_devices_deinit();
    if(using_ext_power) {
        Power* power = furi_record_open(RECORD_POWER);
        power_enable_otg(power, false);
        furi_record_close(RECORD_POWER);
    }

    NotificationApp* notifications = furi_record_open(RECORD_NOTIFICATION);
    notification_message(notifications, &sequence_reset_rgb);
    furi_record_close(RECORD_NOTIFICATION);

    furi_mutex_free(state->mutex);
    furi_message_queue_free(event_queue);
    free(state);

    return 0;
}
