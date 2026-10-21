#include "ui_alarm_menu.h"

#include "manager_audio.h"
#include "manager_led.h"
#include "manager_storage.h"

#include <Arduino.h>
#include <cstdio>
#include <string.h>

namespace {

static constexpr int DISP_W = 428;
static constexpr int DISP_H = 142;
static constexpr int HEADER_H = 34;
static constexpr int CONTENT_Y = 36;
static constexpr int CONTENT_H = DISP_H - CONTENT_Y;

static constexpr uint8_t FIELD_COUNT = 14;
static constexpr uint8_t VISIBLE_COLS = 4;
static constexpr uint8_t LEVEL_COUNT = (FIELD_COUNT + VISIBLE_COLS - 1) / VISIBLE_COLS;
static constexpr uint8_t REPEAT_ONCE = 0x80;

static constexpr int PAGE_DOT_W = 16;
static constexpr int CONTENT_PAD_X = 6;
static constexpr int CONTENT_GAP_X = 4;
static constexpr int MAIN_COL_START_X = CONTENT_PAD_X + PAGE_DOT_W;
static constexpr int MAIN_COL_AVAILABLE_W = DISP_W - MAIN_COL_START_X - CONTENT_PAD_X;
static constexpr int COL_W = (MAIN_COL_AVAILABLE_W - ((VISIBLE_COLS - 1) * CONTENT_GAP_X)) / VISIBLE_COLS;
static constexpr int TITLE_H = 34;
static constexpr int VALUE_W = 90;
static constexpr int VALUE_H = 68;
static constexpr int VALUE_Y = 36;

static constexpr uint8_t BROWSER_MAX_ENTRIES = 24;
static constexpr uint8_t BROWSER_VISIBLE_ROWS = 5;
static constexpr uint32_t PREVIEW_TIMEOUT_MS = 2000;

enum class AlarmField : uint8_t {
    ENABLED = 0,
    HOUR,
    MINUTE,
    SOUND,
    END_VOL,
    END_SUN,
    VOL_RAMP,
    SUN_RAMP,
    SNOOZE,
    SNOOZE_DURATION,
    HOLD_TO_DISMISS,
    REPEAT,
    AUTO_LOUD,
    AUTO_DISMISS,
};

struct UiAlarmState {
    bool enabled;
    uint8_t hour;
    uint8_t minute;
    uint8_t end_vol_pct;
    uint8_t end_sun_pct;
    uint8_t vol_ramp_min;
    uint8_t sun_ramp_min;
    bool snooze_enabled;
    uint8_t snooze_min;
    uint8_t hold_sec;
    uint8_t repeat_mask;
    uint8_t auto_loud_min;
    uint8_t auto_dismiss_min;
    char sound_path[64];
    AlarmField selected;
};

struct SoundBrowserState {
    bool open;
    char dir_path[64];
    char original_sound_path[64];
    char pending_sound_path[64];
    AudioBrowserEntry entries[BROWSER_MAX_ENTRIES];
    uint8_t entry_count;
    uint8_t selected;
    uint8_t scroll_start;
};

static lv_obj_t *g_screen = nullptr;
static lv_obj_t *g_alarm_header_cancel_bg = nullptr;
static lv_obj_t *g_alarm_header_accept_bg = nullptr;
static lv_obj_t *g_repeat_header_cancel_bg = nullptr;
static lv_obj_t *g_repeat_header_accept_bg = nullptr;
static lv_obj_t *g_sound_header_cancel_bg = nullptr;
static lv_obj_t *g_sound_header_accept_bg = nullptr;
static lv_timer_t *g_header_flash_timer = nullptr;
static lv_obj_t *g_value_widgets[VISIBLE_COLS] = {nullptr};
static lv_obj_t *g_title_labels[VISIBLE_COLS] = {nullptr};
static lv_obj_t *g_value_labels[VISIBLE_COLS] = {nullptr};
static lv_obj_t *g_value_arcs[VISIBLE_COLS] = {nullptr};
static lv_obj_t *g_level_dots[LEVEL_COUNT] = {nullptr};

static lv_obj_t *g_repeat_screen = nullptr;
static lv_obj_t *g_repeat_chip[8] = {nullptr};
static lv_obj_t *g_repeat_chip_lbl[8] = {nullptr};
static bool g_repeat_open = false;
static uint8_t g_repeat_focus = 0;
static uint8_t g_repeat_edit_mask = REPEAT_ONCE;

static lv_obj_t *g_sound_screen = nullptr;
static lv_obj_t *g_sound_dir_label = nullptr;
static lv_obj_t *g_sound_rows[BROWSER_VISIBLE_ROWS] = {nullptr};
static lv_obj_t *g_sound_row_labels[BROWSER_VISIBLE_ROWS] = {nullptr};
static SoundBrowserState g_sound_state = {};
static lv_timer_t *g_sound_accept_timer = nullptr;
static bool g_sound_accept_pending = false;
static UiAlarmAction g_pending_action = UiAlarmAction::NONE;
static lv_timer_t *g_pending_action_timer = nullptr;
static UiAlarmAction g_deferred_action = UiAlarmAction::NONE;
static lv_timer_t *g_modal_close_timer = nullptr;
static lv_timer_t *g_realtime_preview_timer = nullptr;
static bool g_preview_led_active = false;
static uint8_t g_preview_led_front = 0;
static uint8_t g_preview_led_back = 0;
static bool g_preview_led_front_on = false;
static bool g_preview_led_back_on = false;
static uint8_t g_preview_volume = 0;
static bool g_preview_audio_active = false;
static uint32_t g_last_end_vol_beep_ms = 0;
static constexpr uint32_t END_VOL_BEEP_MIN_INTERVAL_MS = 200;

static UiAlarmState g_state = {
    false, 7, 0, 80, 78, 5, 20, true, 9, 3, REPEAT_ONCE, 0, 0, "/test.mp3", AlarmField::ENABLED
};

static uint8_t g_window_start = 0;

enum class ModalCloseAction : uint8_t {
    NONE = 0,
    REPEAT_CANCEL,
    REPEAT_ACCEPT,
    SOUND_CANCEL,
    SOUND_ACCEPT,
};

static ModalCloseAction g_modal_close_action = ModalCloseAction::NONE;

// Throttle render_fields() to prevent excessive arc redraws during live adjustments.
// Must exceed the render's own cost (~50ms observed) or the loop period never
// advances far enough between checks for any iteration to actually be skipped.
static uint32_t g_last_render_time_us = 0;
static constexpr uint32_t RENDER_THROTTLE_US = 150000;  // 150ms throttle

static void close_repeat_window(bool apply_changes);
static void close_sound_window(bool apply_changes);

static lv_obj_t *current_cancel_bg() {
    if (g_sound_state.open) return g_sound_header_cancel_bg;
    if (g_repeat_open) return g_repeat_header_cancel_bg;
    return g_alarm_header_cancel_bg;
}

static lv_obj_t *current_accept_bg() {
    if (g_sound_state.open) return g_sound_header_accept_bg;
    if (g_repeat_open) return g_repeat_header_accept_bg;
    return g_alarm_header_accept_bg;
}

static void hide_header_flash() {
    lv_obj_t *cancel_bg = current_cancel_bg();
    lv_obj_t *accept_bg = current_accept_bg();
    if (cancel_bg) {
        lv_obj_set_style_bg_opa(cancel_bg, LV_OPA_TRANSP, 0);
    }
    if (accept_bg) {
        lv_obj_set_style_bg_opa(accept_bg, LV_OPA_TRANSP, 0);
    }
}

static void header_flash_timer_cb(lv_timer_t *timer) {
    (void)timer;
    hide_header_flash();
    g_header_flash_timer = nullptr;
}

static void trigger_header_flash(bool accept) {
    hide_header_flash();
    lv_obj_t *cancel_bg = current_cancel_bg();
    lv_obj_t *accept_bg = current_accept_bg();
    if (accept) {
        if (accept_bg) lv_obj_set_style_bg_opa(accept_bg, LV_OPA_COVER, 0);
    } else {
        if (cancel_bg) lv_obj_set_style_bg_opa(cancel_bg, LV_OPA_COVER, 0);
    }

    if (g_header_flash_timer) {
        lv_timer_del(g_header_flash_timer);
    }
    g_header_flash_timer = lv_timer_create(header_flash_timer_cb, 120, nullptr);
    lv_timer_set_repeat_count(g_header_flash_timer, 1);
}

static void pending_action_timer_cb(lv_timer_t *timer) {
    (void)timer;
    g_pending_action_timer = nullptr;
    g_pending_action = g_deferred_action;
    g_deferred_action = UiAlarmAction::NONE;
}

static void queue_action_after_flash(UiAlarmAction action) {
    g_deferred_action = action;
    if (g_pending_action_timer) {
        lv_timer_del(g_pending_action_timer);
    }
    g_pending_action_timer = lv_timer_create(pending_action_timer_cb, 120, nullptr);
    lv_timer_set_repeat_count(g_pending_action_timer, 1);
}

static void modal_close_timer_cb(lv_timer_t *timer) {
    (void)timer;

    ModalCloseAction action = g_modal_close_action;
    g_modal_close_action = ModalCloseAction::NONE;
    g_modal_close_timer = nullptr;

    switch (action) {
        case ModalCloseAction::REPEAT_CANCEL:
            close_repeat_window(false);
            break;
        case ModalCloseAction::REPEAT_ACCEPT:
            close_repeat_window(true);
            break;
        case ModalCloseAction::SOUND_CANCEL:
            close_sound_window(false);
            break;
        case ModalCloseAction::SOUND_ACCEPT:
            close_sound_window(true);
            break;
        case ModalCloseAction::NONE:
        default:
            break;
    }
}

static void queue_modal_close_after_flash(ModalCloseAction action) {
    g_modal_close_action = action;
    if (g_modal_close_timer) {
        lv_timer_del(g_modal_close_timer);
    }
    g_modal_close_timer = lv_timer_create(modal_close_timer_cb, 120, nullptr);
    lv_timer_set_repeat_count(g_modal_close_timer, 1);
}

static void sound_accept_timer_cb(lv_timer_t *timer) {
    (void)timer;
    if (!g_sound_state.open || !g_sound_accept_pending) {
        g_sound_accept_timer = nullptr;
        g_sound_accept_pending = false;
        return;
    }

    g_sound_accept_timer = nullptr;
    g_sound_accept_pending = false;
    close_sound_window(true);
}

static uint8_t clamp_u8(uint8_t v, uint8_t lo, uint8_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint8_t wrap_u8(uint8_t value, int32_t delta, uint8_t min_v, uint8_t max_v) {
    const int32_t range = (int32_t)max_v - (int32_t)min_v + 1;
    int32_t normalized = ((int32_t)value - (int32_t)min_v + delta) % range;
    if (normalized < 0) normalized += range;
    return (uint8_t)((int32_t)min_v + normalized);
}

static void copy_str(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    if (!src) return;
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static const char *basename_from_path(const char *path) {
    if (!path || path[0] == '\0') return "-";
    const char *slash = strrchr(path, '/');
    if (!slash) return path;
    if (*(slash + 1) == '\0') return "/";
    return slash + 1;
}

static void basename_without_extension(const char *path, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';

    const char *base = basename_from_path(path);
    copy_str(out, out_len, base);

    char *dot = strrchr(out, '.');
    if (dot && dot != out) {
        *dot = '\0';
    }
}

static void parent_dir_from_file(const char *path, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';

    if (!path || path[0] != '/') {
        copy_str(out, out_len, "/");
        return;
    }

    char tmp[64];
    copy_str(tmp, sizeof(tmp), path);
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp) {
        copy_str(out, out_len, "/");
        return;
    }

    *slash = '\0';
    copy_str(out, out_len, tmp);
}

static uint8_t raw_to_percent(uint8_t raw_value) {
    if (raw_value == 0) return 0;
    const int scaled = (int)((raw_value * 100U + 127U) / 255U);
    if (scaled < 0) return 0;
    if (scaled > 100) return 100;
    return (uint8_t)scaled;
}

static uint8_t percent_to_raw(uint8_t percent_value) {
    if (percent_value == 0) return 0;
    const int scaled = (int)((percent_value * 255U + 50U) / 100U);
    if (scaled < 0) return 0;
    if (scaled > 255) return 255;
    return (uint8_t)scaled;
}

static void stop_realtime_preview();

static void realtime_preview_timer_cb(lv_timer_t *timer) {
    (void)timer;
    g_realtime_preview_timer = nullptr;
    stop_realtime_preview();
}

static void restart_realtime_preview_timer() {
    if (g_realtime_preview_timer) {
        lv_timer_reset(g_realtime_preview_timer);
        return;
    }
    g_realtime_preview_timer = lv_timer_create(realtime_preview_timer_cb, PREVIEW_TIMEOUT_MS, nullptr);
    lv_timer_set_repeat_count(g_realtime_preview_timer, 1);
}

static void start_led_preview_if_needed() {
    if (g_preview_led_active) return;

    g_preview_led_front = led_manager_get_front();
    g_preview_led_back = led_manager_get_back();
    g_preview_led_front_on = led_manager_is_front_on();
    g_preview_led_back_on = led_manager_is_back_on();

    if (!g_preview_led_front_on) led_manager_toggle_front();
    if (!g_preview_led_back_on) led_manager_toggle_back();

    g_preview_led_active = true;
}

static void preview_end_sun_level() {
    start_led_preview_if_needed();
    const uint8_t raw = percent_to_raw(g_state.end_sun_pct);
    led_manager_set_front(raw);
    led_manager_set_back(raw);
    restart_realtime_preview_timer();
}

static void preview_end_volume_level() {
    if (!g_preview_audio_active) {
        g_preview_volume = audio_manager_get_volume();
        g_preview_audio_active = true;
        g_last_end_vol_beep_ms = 0; // force an immediate beep on first change
    }

    audio_manager_set_volume(g_state.end_vol_pct);

    const uint32_t now = lv_tick_get();
    if ((now - g_last_end_vol_beep_ms) >= END_VOL_BEEP_MIN_INTERVAL_MS) {
        audio_manager_play("/beeps/700hz.mp3");
        g_last_end_vol_beep_ms = now;
    }

    restart_realtime_preview_timer();
}

static void stop_realtime_preview() {
    if (g_realtime_preview_timer) {
        lv_timer_del(g_realtime_preview_timer);
        g_realtime_preview_timer = nullptr;
    }

    if (g_preview_led_active) {
        led_manager_set_front(g_preview_led_front);
        led_manager_set_back(g_preview_led_back);

        const bool front_now = led_manager_is_front_on();
        const bool back_now = led_manager_is_back_on();

        if (front_now != g_preview_led_front_on) led_manager_toggle_front();
        if (back_now != g_preview_led_back_on) led_manager_toggle_back();

        g_preview_led_active = false;
    }

    if (g_preview_audio_active && audio_manager_is_playing()) {
        audio_manager_stop();
    }
    if (g_preview_audio_active) {
        audio_manager_set_volume(g_preview_volume);
    }
    g_preview_audio_active = false;
    g_preview_volume = 0;
}

static void clamp_state() {
    g_state.hour = clamp_u8(g_state.hour, 0, 23);
    g_state.minute = clamp_u8(g_state.minute, 0, 59);
    g_state.end_vol_pct = clamp_u8(g_state.end_vol_pct, 0, 100);
    g_state.end_sun_pct = clamp_u8(g_state.end_sun_pct, 0, 100);
    g_state.vol_ramp_min = clamp_u8(g_state.vol_ramp_min, 0, 30);
    g_state.sun_ramp_min = clamp_u8(g_state.sun_ramp_min, 0, 30);
    g_state.snooze_min = clamp_u8(g_state.snooze_min, 1, 30);
    g_state.hold_sec = clamp_u8(g_state.hold_sec, 1, 10);
    g_state.auto_loud_min = clamp_u8(g_state.auto_loud_min, 0, 5);
    g_state.auto_dismiss_min = clamp_u8(g_state.auto_dismiss_min, 0, 5);
    if (g_state.sound_path[0] == '\0') {
        copy_str(g_state.sound_path, sizeof(g_state.sound_path), "/test.mp3");
    }
}

static void repeat_mask_normalize(uint8_t &mask) {
    const uint8_t day_bits = (uint8_t)(mask & 0x7F);
    if (mask & REPEAT_ONCE) {
        mask = REPEAT_ONCE;
    } else if (day_bits == 0) {
        mask = REPEAT_ONCE;
    }
}

static void repeat_mask_normalize() {
    repeat_mask_normalize(g_state.repeat_mask);
}

static const char *field_title(uint8_t idx) {
    static const char *titles[FIELD_COUNT] = {
        "Alarm", "Hour", "Minute", "Sound",
        "End\nVol lvl", "End\nSun lvl", "Vol\nRamp", "Sun\nRamp",
        "Snooze", "Snooze\nDuration", "Hold to\nDismiss", "Repeat",
        "Auto\nLoud", "Auto\nDismiss"
    };
    return titles[idx];
}

static lv_color_t toggle_color(bool on) {
    return on ? lv_color_make(0x00, 0x9A, 0x3A) : lv_color_make(0xB0, 0x20, 0x20);
}

static bool field_uses_arc(AlarmField field) {
    return field == AlarmField::HOUR ||
           field == AlarmField::MINUTE ||
           field == AlarmField::END_VOL ||
           field == AlarmField::END_SUN;
}

static uint8_t repeat_slot_to_day_bit(uint8_t slot) {
    switch (slot) {
        case 1: return 1; // Mon
        case 2: return 2; // Tue
        case 3: return 3; // Wed
        case 4: return 4; // Thu
        case 5: return 5; // Fri
        case 6: return 6; // Sat
        case 7: return 0; // Sun
        default: return 0;
    }
}

static void field_value_text(uint8_t idx, char *buf, size_t len, lv_color_t &color) {
    color = lv_color_white();

    switch ((AlarmField)idx) {
        case AlarmField::ENABLED:
            std::snprintf(buf, len, "%s", g_state.enabled ? "ON" : "OFF");
            break;
        case AlarmField::HOUR:
            std::snprintf(buf, len, "%02u", (unsigned)g_state.hour);
            break;
        case AlarmField::MINUTE:
            std::snprintf(buf, len, "%02u", (unsigned)g_state.minute);
            break;
        case AlarmField::SOUND:
            basename_without_extension(g_state.sound_path, buf, len);
            color = lv_color_make(0x6D, 0xC4, 0xFF);
            break;
        case AlarmField::END_VOL:
            std::snprintf(buf, len, "%u", (unsigned)g_state.end_vol_pct);
            break;
        case AlarmField::END_SUN:
            std::snprintf(buf, len, "%u", (unsigned)g_state.end_sun_pct);
            break;
        case AlarmField::VOL_RAMP:
            std::snprintf(buf, len, "%u min", (unsigned)g_state.vol_ramp_min);
            break;
        case AlarmField::SUN_RAMP:
            std::snprintf(buf, len, "%u min", (unsigned)g_state.sun_ramp_min);
            break;
        case AlarmField::SNOOZE:
            std::snprintf(buf, len, "%s", g_state.snooze_enabled ? "ON" : "OFF");
            break;
        case AlarmField::SNOOZE_DURATION:
            std::snprintf(buf, len, "%u min", (unsigned)g_state.snooze_min);
            break;
        case AlarmField::HOLD_TO_DISMISS:
            std::snprintf(buf, len, "%u sec", (unsigned)g_state.hold_sec);
            break;
        case AlarmField::REPEAT:
            std::snprintf(buf, len, "Select");
            color = lv_color_make(0x6D, 0xC4, 0xFF);
            break;
        case AlarmField::AUTO_LOUD:
            if (g_state.auto_loud_min == 0) std::snprintf(buf, len, "Off");
            else std::snprintf(buf, len, "%u min", (unsigned)g_state.auto_loud_min);
            break;
        case AlarmField::AUTO_DISMISS:
            if (g_state.auto_dismiss_min == 0) std::snprintf(buf, len, "Off");
            else std::snprintf(buf, len, "%u min", (unsigned)g_state.auto_dismiss_min);
            break;
    }
}

static uint8_t selected_index() {
    return (uint8_t)g_state.selected;
}

static uint8_t current_level() {
    return (uint8_t)(selected_index() / VISIBLE_COLS);
}

static void update_window_for_selection() {
    g_window_start = (uint8_t)(current_level() * VISIBLE_COLS);
}

static void update_level_dots() {
    const uint8_t active = current_level();
    for (uint8_t i = 0; i < LEVEL_COUNT; ++i) {
        if (!g_level_dots[i]) continue;
        lv_obj_set_style_bg_color(g_level_dots[i],
                                  (i == active) ? lv_color_white() : lv_color_make(0x42, 0x42, 0x42),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(g_level_dots[i], LV_OPA_COVER, LV_PART_MAIN);
    }
}

static void update_focus() {
    const uint8_t sel = selected_index();
    for (uint8_t i = 0; i < VISIBLE_COLS; ++i) {
        if (!g_value_widgets[i]) continue;
        const uint8_t idx = (uint8_t)(g_window_start + i);
        if (idx == sel) lv_obj_add_state(g_value_widgets[i], LV_STATE_FOCUSED);
        else lv_obj_clear_state(g_value_widgets[i], LV_STATE_FOCUSED);
    }
    update_level_dots();
}

static void update_repeat_widgets() {
    if (!g_repeat_screen) return;

    repeat_mask_normalize(g_repeat_edit_mask);

    for (uint8_t i = 0; i < 8; ++i) {
        if (!g_repeat_chip[i] || !g_repeat_chip_lbl[i]) continue;

        bool on = false;
        if (i == 0) {
            on = (g_repeat_edit_mask & REPEAT_ONCE) != 0;
        } else {
            const uint8_t bit = repeat_slot_to_day_bit(i);
            on = ((g_repeat_edit_mask & REPEAT_ONCE) == 0) && ((g_repeat_edit_mask & (1U << bit)) != 0);
        }

        lv_obj_set_style_bg_color(g_repeat_chip[i],
                                  on ? lv_color_make(0x00, 0x88, 0x36) : lv_color_make(0x30, 0x30, 0x30),
                                  LV_PART_MAIN);
        lv_obj_set_style_border_width(g_repeat_chip[i], (i == g_repeat_focus) ? 2 : 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(g_repeat_chip[i],
                                      (i == g_repeat_focus) ? lv_color_white() : lv_color_make(0x66, 0x66, 0x66),
                                      LV_PART_MAIN);
    }
}

static void toggle_repeat_focus() {
    if (g_repeat_focus == 0) {
        g_repeat_edit_mask = REPEAT_ONCE;
    } else {
        g_repeat_edit_mask &= (uint8_t)~REPEAT_ONCE;
        const uint8_t bit = repeat_slot_to_day_bit(g_repeat_focus);
        const uint8_t mask_bit = (uint8_t)(1U << bit);
        if (g_repeat_edit_mask & mask_bit) g_repeat_edit_mask &= (uint8_t)~mask_bit;
        else g_repeat_edit_mask |= mask_bit;

        if ((g_repeat_edit_mask & 0x7F) == 0) {
            g_repeat_edit_mask = REPEAT_ONCE;
        }
    }
}

static void save_state() {
    AppSettings &s = storage_manager_get();
    s.alarm_enabled = g_state.enabled;
    s.alarm_hour = g_state.hour;
    s.alarm_minute = g_state.minute;
    s.alarm_volume = g_state.end_vol_pct;
    s.alarm_end_brightness = percent_to_raw(g_state.end_sun_pct);
    s.alarm_vol_ramp_min = g_state.vol_ramp_min;
    s.alarm_sun_ramp_min = g_state.sun_ramp_min;
    s.alarm_sun_lead_min = g_state.sun_ramp_min;
    s.snooze_enabled = g_state.snooze_enabled;
    s.snooze_duration_min = g_state.snooze_min;
    s.hold_dismiss_sec = g_state.hold_sec;
    s.repeat_mode = g_state.repeat_mask;
    s.alarm_auto_loud_min = g_state.auto_loud_min;
    s.alarm_auto_dismiss_min = g_state.auto_dismiss_min;

    copy_str(s.alarm_mp3, sizeof(s.alarm_mp3), g_state.sound_path);

    storage_manager_save_alarm();
}

static void adjust_selected_field(int32_t delta) {
    if (delta == 0) return;

    switch (g_state.selected) {
        case AlarmField::ENABLED:
            g_state.enabled = (delta > 0);
            break;
        case AlarmField::HOUR:
            g_state.hour = wrap_u8(g_state.hour, delta, 0, 23);
            break;
        case AlarmField::MINUTE:
            g_state.minute = wrap_u8(g_state.minute, delta, 0, 59);
            break;
        case AlarmField::SOUND:
            break;
        case AlarmField::END_VOL: {
            int16_t v = (int16_t)g_state.end_vol_pct + (int16_t)(delta * 5);
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            v = (int16_t)((v / 5) * 5);
            g_state.end_vol_pct = (uint8_t)v;
            preview_end_volume_level();
            break;
        }
        case AlarmField::END_SUN: {
            int16_t v = (int16_t)g_state.end_sun_pct + (int16_t)(delta * 5);
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            v = (int16_t)((v / 5) * 5);
            g_state.end_sun_pct = (uint8_t)v;
            preview_end_sun_level();
            break;
        }
        case AlarmField::VOL_RAMP: {
            int16_t v = (int16_t)g_state.vol_ramp_min + (int16_t)delta;
            if (v < 0) v = 0;
            if (v > 30) v = 30;
            g_state.vol_ramp_min = (uint8_t)v;
            break;
        }
        case AlarmField::SUN_RAMP: {
            int16_t v = (int16_t)g_state.sun_ramp_min + (int16_t)delta;
            if (v < 0) v = 0;
            if (v > 30) v = 30;
            g_state.sun_ramp_min = (uint8_t)v;
            break;
        }
        case AlarmField::SNOOZE:
            g_state.snooze_enabled = (delta > 0);
            break;
        case AlarmField::SNOOZE_DURATION: {
            int16_t v = (int16_t)g_state.snooze_min + (int16_t)delta;
            if (v < 1) v = 1;
            if (v > 30) v = 30;
            g_state.snooze_min = (uint8_t)v;
            break;
        }
        case AlarmField::HOLD_TO_DISMISS: {
            int16_t v = (int16_t)g_state.hold_sec + (int16_t)delta;
            if (v < 1) v = 1;
            if (v > 10) v = 10;
            g_state.hold_sec = (uint8_t)v;
            break;
        }
        case AlarmField::REPEAT:
            break;
        case AlarmField::AUTO_LOUD: {
            int16_t v = (int16_t)g_state.auto_loud_min + (int16_t)delta;
            if (v < 0) v = 0;
            if (v > 5) v = 5;
            g_state.auto_loud_min = (uint8_t)v;
            break;
        }
        case AlarmField::AUTO_DISMISS: {
            int16_t v = (int16_t)g_state.auto_dismiss_min + (int16_t)delta;
            if (v < 0) v = 0;
            if (v > 5) v = 5;
            g_state.auto_dismiss_min = (uint8_t)v;
            break;
        }
    }
}

static void render_fields() {
    clamp_state();
    update_window_for_selection();

    for (uint8_t slot = 0; slot < VISIBLE_COLS; ++slot) {
        const uint8_t idx = (uint8_t)(g_window_start + slot);
        if (!g_title_labels[slot] || !g_value_labels[slot] || !g_value_arcs[slot]) continue;
        if (idx >= FIELD_COUNT) {
            lv_label_set_text(g_title_labels[slot], "");
            lv_label_set_text(g_value_labels[slot], "");
            lv_obj_set_hidden(g_value_arcs[slot], true);
            lv_obj_set_style_bg_opa(g_value_widgets[slot], LV_OPA_TRANSP, LV_PART_MAIN);
            continue;
        }

        const char *title = field_title(idx);
        char title_buf[32];
        if (strchr(title, '\n')) {
            copy_str(title_buf, sizeof(title_buf), title);
        } else {
            std::snprintf(title_buf, sizeof(title_buf), "\n%s", title);
        }
        lv_label_set_text(g_title_labels[slot], title_buf);

        const AlarmField field = (AlarmField)idx;
        const bool use_arc = field_uses_arc(field);
        lv_obj_set_hidden(g_value_arcs[slot], !use_arc);
        lv_obj_set_style_bg_opa(g_value_widgets[slot], LV_OPA_COVER, LV_PART_MAIN);

        lv_color_t shell_color = lv_color_make(0x16, 0x16, 0x16);
        if (field == AlarmField::ENABLED) {
            shell_color = toggle_color(g_state.enabled);
        } else if (field == AlarmField::SNOOZE) {
            shell_color = toggle_color(g_state.snooze_enabled);
        }
        lv_obj_set_style_bg_color(g_value_widgets[slot], shell_color, LV_PART_MAIN);

        char value[48];
        lv_color_t value_color;
        field_value_text(idx, value, sizeof(value), value_color);

        if (use_arc) {
            int32_t arc_min = 0;
            int32_t arc_max = 100;
            int32_t arc_value = 0;

            switch (field) {
                case AlarmField::HOUR:
                    arc_min = 0;
                    arc_max = 12;
                    arc_value = (int32_t)(g_state.hour % 12);
                    lv_arc_set_rotation(g_value_arcs[slot], 270);
                    std::snprintf(value, sizeof(value), "%02u", (unsigned)g_state.hour);
                    break;
                case AlarmField::MINUTE:
                    arc_min = 0;
                    arc_max = 59;
                    arc_value = g_state.minute;
                    lv_arc_set_rotation(g_value_arcs[slot], 270);
                    std::snprintf(value, sizeof(value), "%02u", (unsigned)g_state.minute);
                    break;
                case AlarmField::END_VOL:
                    arc_min = 0;
                    arc_max = 100;
                    arc_value = g_state.end_vol_pct;
                    lv_arc_set_rotation(g_value_arcs[slot], 270);
                    std::snprintf(value, sizeof(value), "%u", (unsigned)g_state.end_vol_pct);
                    break;
                case AlarmField::END_SUN:
                    arc_min = 0;
                    arc_max = 100;
                    arc_value = g_state.end_sun_pct;
                    lv_arc_set_rotation(g_value_arcs[slot], 270);
                    std::snprintf(value, sizeof(value), "%u", (unsigned)g_state.end_sun_pct);
                    break;
                default:
                    break;
            }

            lv_arc_set_range(g_value_arcs[slot], arc_min, arc_max);
            lv_arc_set_value(g_value_arcs[slot], arc_value);
            lv_obj_set_style_text_font(g_value_labels[slot], &lv_font_montserrat_20, 0);
            lv_obj_set_style_text_color(g_value_labels[slot], lv_color_white(), 0);
            lv_label_set_text(g_value_labels[slot], value);
        } else {
            lv_obj_set_style_text_font(g_value_labels[slot], &lv_font_montserrat_20, 0);
            lv_label_set_text(g_value_labels[slot], value);
            lv_obj_set_style_text_color(g_value_labels[slot], value_color, 0);
        }
    }

    update_focus();
}

static void apply_header_base(lv_obj_t *screen, const char *title) {
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_set_size(header, DISP_W, HEADER_H);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_top(header, 2, 0);
    lv_obj_set_style_pad_left(header, 2, 0);
    lv_obj_set_style_pad_right(header, 2, 0);
    lv_obj_set_scrollable(header, false);

    lv_obj_t *cancel_bg = lv_obj_create(header);
    lv_obj_set_size(cancel_bg, 96, 24);
    lv_obj_align(cancel_bg, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_bg_color(cancel_bg, lv_color_make(0xB0, 0x20, 0x20), 0);
    lv_obj_set_style_bg_opa(cancel_bg, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(cancel_bg, 4, 0);
    lv_obj_set_style_border_width(cancel_bg, 0, 0);
    lv_obj_set_style_pad_all(cancel_bg, 0, 0);
    lv_obj_set_scrollable(cancel_bg, false);

    lv_obj_t *lbl_cancel = lv_label_create(cancel_bg);
    lv_label_set_text(lbl_cancel, "Cancel");
    lv_obj_set_style_text_font(lbl_cancel, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_cancel, lv_color_white(), 0);
    lv_obj_center(lbl_cancel);

    lv_obj_t *lbl_title = lv_label_create(header);
    lv_label_set_text(lbl_title, title);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_white(), 0);
    lv_obj_align(lbl_title, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *accept_bg = lv_obj_create(header);
    lv_obj_set_size(accept_bg, 96, 24);
    lv_obj_align(accept_bg, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_set_style_bg_color(accept_bg, lv_color_make(0x00, 0x9A, 0x3A), 0);
    lv_obj_set_style_bg_opa(accept_bg, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(accept_bg, 4, 0);
    lv_obj_set_style_border_width(accept_bg, 0, 0);
    lv_obj_set_style_pad_all(accept_bg, 0, 0);
    lv_obj_set_scrollable(accept_bg, false);

    lv_obj_t *lbl_accept = lv_label_create(accept_bg);
    lv_label_set_text(lbl_accept, "Accept");
    lv_obj_set_style_text_font(lbl_accept, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_accept, lv_color_white(), 0);
    lv_obj_center(lbl_accept);

    if (screen == g_screen) {
        g_alarm_header_cancel_bg = cancel_bg;
        g_alarm_header_accept_bg = accept_bg;
    } else if (screen == g_repeat_screen) {
        g_repeat_header_cancel_bg = cancel_bg;
        g_repeat_header_accept_bg = accept_bg;
    } else if (screen == g_sound_screen) {
        g_sound_header_cancel_bg = cancel_bg;
        g_sound_header_accept_bg = accept_bg;
    }
}

static lv_obj_t *create_value_shell(lv_obj_t *parent, int x) {
    lv_obj_t *widget = lv_obj_create(parent);
    lv_obj_set_size(widget, VALUE_W, VALUE_H);
    lv_obj_set_pos(widget, x + ((COL_W - VALUE_W) / 2), VALUE_Y);
    lv_obj_set_style_bg_color(widget, lv_color_make(0x16, 0x16, 0x16), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(widget, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(widget, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(widget, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(widget, lv_color_make(0x3A, 0x3A, 0x3A), LV_PART_MAIN);
    lv_obj_set_style_outline_width(widget, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(widget, 0, LV_PART_MAIN);
    lv_obj_set_scrollable(widget, false);

    lv_obj_set_style_border_width(widget, 3, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(widget, lv_color_white(), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(widget, 1, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(widget, lv_color_white(), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_outline_opa(widget, LV_OPA_70, LV_PART_MAIN | LV_STATE_FOCUSED);

    return widget;
}

static void create_column(lv_obj_t *parent, int x, uint8_t slot) {
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_set_size(col, COL_W, CONTENT_H);
    lv_obj_set_pos(col, x, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_scrollable(col, false);

    lv_obj_t *title_label = lv_label_create(col);
    lv_label_set_text(title_label, "-");
    lv_obj_set_size(title_label, COL_W, TITLE_H);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title_label, lv_color_make(0xD0, 0xD0, 0xD0), 0);
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_clickable(title_label, false);
    lv_obj_set_click_focusable(title_label, false);

    lv_obj_t *widget = create_value_shell(col, 0);

    lv_obj_t *arc = lv_arc_create(widget);
    lv_obj_set_size(arc, 58, 58);
    lv_obj_center(arc);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_mode(arc, LV_ARC_MODE_NORMAL);
    lv_obj_set_style_arc_width(arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_make(0x34, 0x34, 0x34), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_make(0x4D, 0xB1, 0xFF), LV_PART_INDICATOR);
    lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
    lv_obj_set_clickable(arc, false);
    lv_obj_set_scrollable(arc, false);

    lv_obj_t *value_label = lv_label_create(widget);
    lv_label_set_text(value_label, "-");
    lv_obj_set_style_text_font(value_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(value_label, lv_color_white(), 0);
    lv_obj_set_width(value_label, VALUE_W - 8);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(value_label);

    g_value_widgets[slot] = widget;
    g_value_arcs[slot] = arc;
    g_title_labels[slot] = title_label;
    g_value_labels[slot] = value_label;
}

static void create_level_dots(lv_obj_t *parent) {
    const int dot_h = 8;
    const int gap = 12;
    const int total_h = (LEVEL_COUNT * dot_h) + ((LEVEL_COUNT - 1) * gap);
    const int start_y = (CONTENT_H - total_h) / 2;

    for (uint8_t i = 0; i < LEVEL_COUNT; ++i) {
        lv_obj_t *dot = lv_obj_create(parent);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_pos(dot, 6, start_y + i * (dot_h + gap));
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(dot, 0, LV_PART_MAIN);
        lv_obj_set_scrollable(dot, false);
        g_level_dots[i] = dot;
    }
}

static void build_repeat_screen() {
    g_repeat_screen = lv_obj_create(nullptr);
    lv_obj_set_size(g_repeat_screen, DISP_W, DISP_H);
    lv_obj_set_style_pad_all(g_repeat_screen, 0, 0);
    lv_obj_set_style_border_width(g_repeat_screen, 0, 0);
    lv_obj_set_scrollable(g_repeat_screen, false);

    apply_header_base(g_repeat_screen, "Repeat Alarm");

    lv_obj_t *content = lv_obj_create(g_repeat_screen);
    lv_obj_set_size(content, DISP_W, CONTENT_H);
    lv_obj_set_pos(content, 0, CONTENT_Y);
    lv_obj_set_style_bg_color(content, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_scrollable(content, false);

    static const char *chip_text[8] = {"Once", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};

    g_repeat_chip[0] = lv_obj_create(content);
    lv_obj_set_size(g_repeat_chip[0], 92, 26);
    lv_obj_align(g_repeat_chip[0], LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_radius(g_repeat_chip[0], 6, LV_PART_MAIN);
    lv_obj_set_scrollable(g_repeat_chip[0], false);

    g_repeat_chip_lbl[0] = lv_label_create(g_repeat_chip[0]);
    lv_label_set_text(g_repeat_chip_lbl[0], chip_text[0]);
    lv_obj_set_style_text_font(g_repeat_chip_lbl[0], &lv_font_montserrat_16, 0);
    lv_obj_center(g_repeat_chip_lbl[0]);

    const int day_gap = 2;
    const int day_w = (DISP_W - 12 - (day_gap * 6)) / 7;
    const int day_y = 54;

    for (uint8_t i = 1; i < 8; ++i) {
        g_repeat_chip[i] = lv_obj_create(content);
        lv_obj_set_size(g_repeat_chip[i], day_w, 26);
        lv_obj_set_pos(g_repeat_chip[i], 6 + (int)(i - 1) * (day_w + day_gap), day_y);
        lv_obj_set_style_radius(g_repeat_chip[i], 6, LV_PART_MAIN);
        lv_obj_set_scrollable(g_repeat_chip[i], false);

        g_repeat_chip_lbl[i] = lv_label_create(g_repeat_chip[i]);
        lv_label_set_text(g_repeat_chip_lbl[i], chip_text[i]);
        lv_obj_set_style_text_font(g_repeat_chip_lbl[i], &lv_font_montserrat_14, 0);
        lv_obj_center(g_repeat_chip_lbl[i]);
    }
}

static void refresh_sound_browser_entries() {
    size_t count = 0;
    if (!audio_manager_browse_dir(g_sound_state.dir_path,
                                  g_sound_state.entries,
                                  BROWSER_MAX_ENTRIES,
                                  &count)) {
        g_sound_state.entry_count = 0;
        g_sound_state.selected = 0;
        g_sound_state.scroll_start = 0;
        return;
    }

    if (count > BROWSER_MAX_ENTRIES) count = BROWSER_MAX_ENTRIES;
    g_sound_state.entry_count = (uint8_t)count;

    if (g_sound_state.entry_count == 0) {
        g_sound_state.selected = 0;
        g_sound_state.scroll_start = 0;
    } else if (g_sound_state.selected >= g_sound_state.entry_count) {
        g_sound_state.selected = 0;
    }
}

static void ensure_sound_browser_scroll() {
    if (g_sound_state.entry_count == 0) {
        g_sound_state.scroll_start = 0;
        return;
    }

    if (g_sound_state.selected < g_sound_state.scroll_start) {
        g_sound_state.scroll_start = g_sound_state.selected;
    } else if (g_sound_state.selected >= (uint8_t)(g_sound_state.scroll_start + BROWSER_VISIBLE_ROWS)) {
        g_sound_state.scroll_start = (uint8_t)(g_sound_state.selected - (BROWSER_VISIBLE_ROWS - 1));
    }
}

static void render_sound_browser() {
    if (!g_sound_screen || !g_sound_dir_label) return;

    char dir_buf[72];
    std::snprintf(dir_buf, sizeof(dir_buf), "Dir: %s", g_sound_state.dir_path);
    lv_label_set_text(g_sound_dir_label, dir_buf);

    ensure_sound_browser_scroll();

    for (uint8_t row = 0; row < BROWSER_VISIBLE_ROWS; ++row) {
        if (!g_sound_rows[row] || !g_sound_row_labels[row]) continue;

        const uint8_t idx = (uint8_t)(g_sound_state.scroll_start + row);
        if (idx >= g_sound_state.entry_count) {
            lv_label_set_text(g_sound_row_labels[row], "");
            lv_obj_set_style_bg_opa(g_sound_rows[row], LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_clear_state(g_sound_rows[row], LV_STATE_FOCUSED);
            continue;
        }

        const AudioBrowserEntry &entry = g_sound_state.entries[idx];
        char row_text[64];

        if (entry.is_directory) {
            std::snprintf(row_text, sizeof(row_text), "[%s]", entry.name);
            lv_obj_set_style_text_color(g_sound_row_labels[row], lv_color_make(0x7A, 0xBE, 0xFF), 0);
            lv_obj_set_style_bg_opa(g_sound_rows[row], LV_OPA_TRANSP, LV_PART_MAIN);
        } else if (entry.is_playable) {
            const bool previewing = audio_manager_is_previewing(entry.path);
            std::snprintf(row_text, sizeof(row_text), "%s%s", previewing ? "|| " : "   ", entry.name);
            lv_obj_set_style_text_color(g_sound_row_labels[row], lv_color_white(), 0);
            if (g_sound_accept_pending && idx == g_sound_state.selected) {
                lv_obj_set_style_bg_color(g_sound_rows[row], lv_color_make(0x00, 0x9A, 0x3A), LV_PART_MAIN);
                lv_obj_set_style_bg_opa(g_sound_rows[row], LV_OPA_COVER, LV_PART_MAIN);
            } else {
                lv_obj_set_style_bg_opa(g_sound_rows[row], LV_OPA_TRANSP, LV_PART_MAIN);
            }
        } else {
            std::snprintf(row_text, sizeof(row_text), "- %s", entry.name);
            lv_obj_set_style_text_color(g_sound_row_labels[row], lv_color_make(0xA0, 0xA0, 0xA0), 0);
            lv_obj_set_style_bg_opa(g_sound_rows[row], LV_OPA_TRANSP, LV_PART_MAIN);
        }

        lv_label_set_text(g_sound_row_labels[row], row_text);

        if (idx == g_sound_state.selected) {
            lv_obj_add_state(g_sound_rows[row], LV_STATE_FOCUSED);
        } else {
            lv_obj_clear_state(g_sound_rows[row], LV_STATE_FOCUSED);
        }
    }
}

static void build_sound_screen() {
    g_sound_screen = lv_obj_create(nullptr);
    lv_obj_set_size(g_sound_screen, DISP_W, DISP_H);
    lv_obj_set_style_pad_all(g_sound_screen, 0, 0);
    lv_obj_set_style_border_width(g_sound_screen, 0, 0);
    lv_obj_set_scrollable(g_sound_screen, false);

    apply_header_base(g_sound_screen, "Select Sound");

    lv_obj_t *content = lv_obj_create(g_sound_screen);
    lv_obj_set_size(content, DISP_W, CONTENT_H);
    lv_obj_set_pos(content, 0, CONTENT_Y);
    lv_obj_set_style_bg_color(content, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_scrollable(content, false);

    g_sound_dir_label = lv_label_create(content);
    lv_label_set_text(g_sound_dir_label, "Dir: /");
    lv_obj_set_style_text_font(g_sound_dir_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(g_sound_dir_label, lv_color_make(0xD0, 0xD0, 0xD0), 0);
    lv_obj_set_width(g_sound_dir_label, DISP_W - 12);
    lv_obj_set_style_text_align(g_sound_dir_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(g_sound_dir_label, LV_ALIGN_TOP_LEFT, 6, 2);

    const int row_h = 16;
    const int row_gap = 2;
    const int row_y_start = 22;
    for (uint8_t i = 0; i < BROWSER_VISIBLE_ROWS; ++i) {
        lv_obj_t *row = lv_obj_create(content);
        lv_obj_set_size(row, DISP_W - 12, row_h);
        lv_obj_set_pos(row, 6, row_y_start + (int)i * (row_h + row_gap));
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_left(row, 3, LV_PART_MAIN);
        lv_obj_set_style_pad_right(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_top(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_bottom(row, 0, LV_PART_MAIN);
        lv_obj_set_scrollable(row, false);

        lv_obj_set_style_border_width(row, 1, LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(row, lv_color_white(), LV_PART_MAIN | LV_STATE_FOCUSED);

        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, "");
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_set_width(label, DISP_W - 20);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

        g_sound_rows[i] = row;
        g_sound_row_labels[i] = label;
    }

}

static void open_repeat_window() {
    stop_realtime_preview();
    g_repeat_open = true;
    g_repeat_focus = 0;
    g_repeat_edit_mask = g_state.repeat_mask;
    repeat_mask_normalize(g_repeat_edit_mask);
    update_repeat_widgets();
    lv_screen_load(g_repeat_screen);
}

static void close_repeat_window(bool apply_changes) {
    hide_header_flash();
    if (apply_changes) {
        g_state.repeat_mask = g_repeat_edit_mask;
        repeat_mask_normalize();
    }
    g_repeat_open = false;
    lv_screen_load(g_screen);
    render_fields();
    g_last_render_time_us = micros();  // Reset throttle on major screen change
}

static void open_sound_window() {
    stop_realtime_preview();
    g_sound_state.open = true;
    g_sound_accept_pending = false;
    if (g_sound_accept_timer) {
        lv_timer_del(g_sound_accept_timer);
        g_sound_accept_timer = nullptr;
    }
    copy_str(g_sound_state.original_sound_path,
             sizeof(g_sound_state.original_sound_path),
             g_state.sound_path);
    copy_str(g_sound_state.pending_sound_path,
             sizeof(g_sound_state.pending_sound_path),
             g_state.sound_path);

    parent_dir_from_file(g_state.sound_path, g_sound_state.dir_path, sizeof(g_sound_state.dir_path));
    g_sound_state.selected = 0;
    g_sound_state.scroll_start = 0;

    refresh_sound_browser_entries();

    for (uint8_t i = 0; i < g_sound_state.entry_count; ++i) {
        if (!g_sound_state.entries[i].is_directory &&
            strcmp(g_sound_state.entries[i].path, g_sound_state.pending_sound_path) == 0) {
            g_sound_state.selected = i;
            break;
        }
    }

    render_sound_browser();
    lv_screen_load(g_sound_screen);
}

static void close_sound_window(bool apply_changes) {
    hide_header_flash();
    g_sound_accept_pending = false;
    if (g_sound_accept_timer) {
        lv_timer_del(g_sound_accept_timer);
        g_sound_accept_timer = nullptr;
    }

    if (apply_changes && g_sound_state.pending_sound_path[0] != '\0') {
        copy_str(g_state.sound_path, sizeof(g_state.sound_path), g_sound_state.pending_sound_path);
    } else {
        copy_str(g_state.sound_path, sizeof(g_state.sound_path), g_sound_state.original_sound_path);
    }

    audio_manager_stop_preview();
    g_sound_state.open = false;
    lv_screen_load(g_screen);
    render_fields();
    g_last_render_time_us = micros();  // Reset throttle on major screen change
}

static void build_alarm_screen() {
    g_screen = lv_obj_create(nullptr);
    lv_obj_set_size(g_screen, DISP_W, DISP_H);
    lv_obj_set_style_pad_all(g_screen, 0, 0);
    lv_obj_set_style_border_width(g_screen, 0, 0);
    lv_obj_set_scrollable(g_screen, false);

    apply_header_base(g_screen, "Alarm");

    lv_obj_t *content = lv_obj_create(g_screen);
    lv_obj_set_size(content, DISP_W, CONTENT_H);
    lv_obj_set_pos(content, 0, CONTENT_Y);
    lv_obj_set_style_bg_color(content, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_scrollable(content, false);

    create_level_dots(content);

    for (uint8_t i = 0; i < VISIBLE_COLS; ++i) {
        const int x = MAIN_COL_START_X + i * (COL_W + CONTENT_GAP_X);
        create_column(content, x, i);
    }

    render_fields();
}

} // namespace

void ui_alarm_init() {
    if (g_screen) return;

    build_alarm_screen();
    build_repeat_screen();
    build_sound_screen();
}

lv_obj_t *ui_alarm_get_screen() {
    return g_screen;
}

void ui_alarm_on_enter() {
    const AppSettings &s = storage_manager_get();
    g_state.enabled = s.alarm_enabled;
    g_state.hour = s.alarm_hour;
    g_state.minute = s.alarm_minute;
    g_state.end_vol_pct = clamp_u8(s.alarm_volume, 0, 100);
    g_state.end_sun_pct = raw_to_percent(s.alarm_end_brightness);
    g_state.vol_ramp_min = (uint8_t)((s.alarm_vol_ramp_min > 30) ? 30 : s.alarm_vol_ramp_min);
    g_state.sun_ramp_min = (uint8_t)((s.alarm_sun_ramp_min > 30) ? 30 : s.alarm_sun_ramp_min);
    g_state.snooze_enabled = s.snooze_enabled;
    g_state.snooze_min = (uint8_t)((s.snooze_duration_min > 30) ? 30 : s.snooze_duration_min);
    g_state.hold_sec = s.hold_dismiss_sec;
    g_state.repeat_mask = s.repeat_mode;
    g_state.auto_loud_min = clamp_u8(s.alarm_auto_loud_min, 0, 5);
    g_state.auto_dismiss_min = clamp_u8(s.alarm_auto_dismiss_min, 0, 5);
    copy_str(g_state.sound_path,
             sizeof(g_state.sound_path),
             (s.alarm_mp3[0] != '\0') ? s.alarm_mp3 : "/test.mp3");

    g_state.selected = AlarmField::ENABLED;
    g_window_start = 0;
    g_repeat_open = false;
    g_sound_state.open = false;
    stop_realtime_preview();
    repeat_mask_normalize();
    audio_manager_stop_preview();
    hide_header_flash();
    render_fields();
    g_last_render_time_us = micros();  // Reset throttle on screen enter
}

UiAlarmAction ui_alarm_handle_inputs(int32_t enc1_delta,
                                     int32_t enc2_delta,
                                     bool enc1_pressed,
                                     bool enc2_pressed) {
    if (g_repeat_open) {
        if (g_modal_close_timer) {
            return UiAlarmAction::NONE;
        }

        if (enc1_pressed) {
            trigger_header_flash(false);
            queue_modal_close_after_flash(ModalCloseAction::REPEAT_CANCEL);
            return UiAlarmAction::NONE;
        }

        if (enc2_pressed) {
            trigger_header_flash(true);
            queue_modal_close_after_flash(ModalCloseAction::REPEAT_ACCEPT);
            return UiAlarmAction::NONE;
        }

        if (enc1_delta != 0) {
            const int8_t direction = (enc1_delta > 0) ? 1 : -1;
            int32_t steps = (enc1_delta > 0) ? enc1_delta : -enc1_delta;
            while (steps-- > 0) {
                int16_t next = (int16_t)g_repeat_focus + direction;
                if (next < 0) next = 7;
                if (next > 7) next = 0;
                g_repeat_focus = (uint8_t)next;
            }
            update_repeat_widgets();
        }

        if (enc2_delta != 0) {
            toggle_repeat_focus();
            update_repeat_widgets();
        }

        return UiAlarmAction::NONE;
    }

    if (g_sound_state.open) {
        if (g_modal_close_timer) {
            return UiAlarmAction::NONE;
        }

        if (enc1_pressed) {
            trigger_header_flash(false);
            queue_modal_close_after_flash(ModalCloseAction::SOUND_CANCEL);
            return UiAlarmAction::NONE;
        }

        if (g_sound_accept_pending) {
            return UiAlarmAction::NONE;
        }

        if (enc2_pressed) {
            if (g_sound_state.entry_count > 0) {
                const AudioBrowserEntry &entry = g_sound_state.entries[g_sound_state.selected];
                if (entry.is_playable) {
                    copy_str(g_sound_state.pending_sound_path,
                             sizeof(g_sound_state.pending_sound_path),
                             entry.path);
                    trigger_header_flash(true);
                    g_sound_accept_pending = true;
                    render_sound_browser();

                    if (g_sound_accept_timer) {
                        lv_timer_del(g_sound_accept_timer);
                    }
                    g_sound_accept_timer = lv_timer_create(sound_accept_timer_cb, 300, nullptr);
                    lv_timer_set_repeat_count(g_sound_accept_timer, 1);
                    return UiAlarmAction::NONE;
                }
            }

            trigger_header_flash(true);
            queue_modal_close_after_flash(ModalCloseAction::SOUND_ACCEPT);
            return UiAlarmAction::NONE;
        }

        if (enc1_delta != 0 && g_sound_state.entry_count > 0) {
            const int8_t direction = (enc1_delta > 0) ? 1 : -1;
            int32_t steps = (enc1_delta > 0) ? enc1_delta : -enc1_delta;
            while (steps-- > 0) {
                int16_t next = (int16_t)g_sound_state.selected + direction;
                if (next < 0) next = g_sound_state.entry_count - 1;
                if (next >= g_sound_state.entry_count) next = 0;
                g_sound_state.selected = (uint8_t)next;
            }
            render_sound_browser();
        }

        if (enc2_delta != 0 && g_sound_state.entry_count > 0) {
            const AudioBrowserEntry &entry = g_sound_state.entries[g_sound_state.selected];
            if (entry.is_directory) {
                copy_str(g_sound_state.dir_path, sizeof(g_sound_state.dir_path), entry.path);
                g_sound_state.selected = 0;
                g_sound_state.scroll_start = 0;
                refresh_sound_browser_entries();
            } else if (entry.is_playable) {
                copy_str(g_sound_state.pending_sound_path,
                         sizeof(g_sound_state.pending_sound_path),
                         entry.path);
                audio_manager_toggle_preview(entry.path);
            }
            render_sound_browser();
        }

        return UiAlarmAction::NONE;
    }

    if (g_pending_action != UiAlarmAction::NONE) {
        UiAlarmAction out = g_pending_action;
        g_pending_action = UiAlarmAction::NONE;
        return out;
    }

    if (g_pending_action_timer) {
        return UiAlarmAction::NONE;
    }

    if (enc1_pressed) {
        stop_realtime_preview();
        trigger_header_flash(false);
        queue_action_after_flash(UiAlarmAction::CANCEL);
        return UiAlarmAction::NONE;
    }

    if (enc1_delta != 0) {
        const int8_t direction = (enc1_delta > 0) ? 1 : -1;
        int32_t steps = (enc1_delta > 0) ? enc1_delta : -enc1_delta;
        while (steps-- > 0) {
            int16_t next = (int16_t)selected_index() + direction;
            if (next < 0) next = FIELD_COUNT - 1;
            if (next >= FIELD_COUNT) next = 0;
            g_state.selected = (AlarmField)next;
        }
    }

    if (enc2_delta != 0) {
        if (g_state.selected == AlarmField::REPEAT) {
            open_repeat_window();
            return UiAlarmAction::NONE;
        }
        if (g_state.selected == AlarmField::SOUND) {
            open_sound_window();
            return UiAlarmAction::NONE;
        }
        adjust_selected_field(enc2_delta);
    }

    if (enc2_pressed) {
        stop_realtime_preview();
        trigger_header_flash(true);
        save_state();
        queue_action_after_flash(UiAlarmAction::ACCEPT);
        return UiAlarmAction::NONE;
    }

    // Throttle render_fields() during live adjustments to prevent audio starvation.
    // Force immediate render on focus changes (enc1_delta), throttle value changes.
    bool should_render = false;
    if (enc1_delta != 0) {
        // Focus changed - render immediately
        should_render = true;
    } else {
        // Check throttle for other updates
        const uint32_t now_us = micros();
        if ((now_us - g_last_render_time_us) >= RENDER_THROTTLE_US) {
            should_render = true;
        }
    }

    if (should_render) {
#if AUDIO_DEBUG_TIMING
        {
            const uint32_t start_us = micros();
            render_fields();
            const uint32_t duration_us = micros() - start_us;
            if (duration_us > 5000) {
                Serial.printf("[render_fields] took %luus\n", (unsigned long)duration_us);
            }
            g_last_render_time_us = micros();
        }
#else
        render_fields();
        g_last_render_time_us = micros();
#endif
    }
    return UiAlarmAction::NONE;
}
