#include "manager_alarm.h"

#include "manager_audio.h"
#include "manager_led.h"
#include "manager_rtc.h"
#include "manager_storage.h"
#include "ui_alarm_active_window.h"
#include "ui_main_screen.h"
#include "ui_settings_menu.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

namespace {

static AlarmState g_state = AlarmState::IDLE;

static uint8_t g_base_audio_volume = 0;
static uint8_t g_base_led_front = 0;
static uint8_t g_base_led_back = 0;
static bool g_base_led_front_on = false;
static bool g_base_led_back_on = false;
static uint16_t g_base_led_front_hue = 0;
static uint8_t g_base_led_front_sat = 100;
static uint16_t g_base_led_back_hue = 0;
static uint8_t g_base_led_back_sat = 100;

static uint32_t g_ramp_start_ms = 0;
static uint32_t g_active_start_ms = 0;
static uint32_t g_auto_loud_start_ms = 0;
static bool g_auto_loud_triggered = false;
static uint32_t g_last_match_stamp = 0;
static uint32_t g_snooze_until_epoch = 0;
static constexpr uint32_t TAP_SNOOZE_MS = 300U;
static uint32_t g_alarm_target_epoch = 0;
static uint16_t g_flow_vol_ramp_min = 0;
static uint16_t g_flow_sun_ramp_min = 0;
static uint8_t g_flow_end_volume = 0;
static uint8_t g_flow_end_led = 0;
static bool g_audio_started = false;
static bool g_outputs_paused_for_hold = false;
static char g_alarm_path[64] = "/test.mp3";

static bool g_any_btn_prev_held = false;
static uint32_t g_press_start_ms = 0;
static bool g_hold_dismiss_latched = false;

static uint8_t clamp_u8(uint8_t v, uint8_t lo, uint8_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint16_t clamp_u16(uint16_t v, uint16_t lo, uint16_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint8_t ramp_from_one_to_end(uint8_t end_value, uint32_t elapsed_ms, uint32_t total_ms) {
    if (total_ms == 0U) return end_value;
    if (elapsed_ms >= total_ms) return end_value;
    if (end_value <= 1U) return end_value;

    return (uint8_t)(1U + ((uint32_t)(end_value - 1U) * elapsed_ms) / total_ms);
}

static uint8_t led_ramp_from_one_to_end(uint8_t end_value, uint32_t elapsed_ms, uint32_t total_ms) {
    if (total_ms == 0U) return end_value;
    if (elapsed_ms >= total_ms) return end_value;
    static constexpr float MIN_VISIBLE = 20.0f;
    if (end_value <= MIN_VISIBLE) return end_value;

    const float normalized = (float)elapsed_ms / (float)total_ms;
    const float curved = powf(normalized, 2.2f);
    return (uint8_t)lroundf(MIN_VISIBLE + ((float)end_value - MIN_VISIBLE) * curved);
}

static bool repeat_allows_today(uint8_t repeat_mask, uint8_t rtc_dow) {
    // repeat_mask: bit7=once, bit0..6=Sun..Sat
    if (repeat_mask & 0x80U) {
        return true;
    }
    if (rtc_dow > 6) return false;
    return (repeat_mask & (1U << rtc_dow)) != 0;
}

static uint32_t epoch_minutes(const DateTime &dt) {
    return dt.unixtime() / 60U;
}

static uint32_t days_from_civil(int32_t year, uint32_t month, uint32_t day) {
    year -= (month <= 2U) ? 1 : 0;
    const int32_t era = (year >= 0 ? year : year - 399) / 400;
    const uint32_t yoe = (uint32_t)(year - era * 400);
    const uint32_t doy = (153U * (month + (month > 2U ? (uint32_t)-3 : 9U)) + 2U) / 5U + day - 1U;
    const uint32_t doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return (uint32_t)(era * 146097 + (int32_t)doe - 719468);
}

static bool is_same_calendar_day(const DateTime &a, const DateTime &b) {
    return a.year() == b.year() && a.month() == b.month() && a.day() == b.day();
}

static DateTime add_days(const DateTime &dt, uint8_t days) {
    const uint32_t shifted = dt.unixtime() + ((uint32_t)days * 24U * 60U * 60U);
    return DateTime(shifted);
}

static void copy_str(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    if (!src) return;
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static void set_front_output(bool enabled, uint8_t value) {
    if (led_manager_is_front_on() != enabled) {
        led_manager_toggle_front();
    }
    led_manager_set_front(value);
}

static void set_back_output(bool enabled, uint8_t value) {
    if (led_manager_is_back_on() != enabled) {
        led_manager_toggle_back();
    }
    led_manager_set_back(value);
}

static bool compute_next_scheduled_alarm_epoch(const AppSettings &s, const DateTime &now, uint32_t *out_epoch) {
    if (!s.alarm_enabled || !out_epoch) {
        return false;
    }

    const uint32_t now_minute = epoch_minutes(now);
    const DateTime today_alarm(now.year(), now.month(), now.day(), s.alarm_hour, s.alarm_minute, 0);

    if (s.repeat_mode & 0x80U) {
        uint32_t candidate = today_alarm.unixtime();
        if ((candidate / 60U) < now_minute) {
            candidate += 24U * 60U * 60U;
        }
        *out_epoch = candidate;
        return true;
    }

    for (uint8_t day_offset = 0; day_offset < 7; ++day_offset) {
        const uint8_t candidate_dow = (uint8_t)((now.dayOfTheWeek() + day_offset) % 7U);
        if ((s.repeat_mode & (1U << candidate_dow)) == 0) {
            continue;
        }

        DateTime candidate_dt = add_days(today_alarm, day_offset);
        const uint32_t candidate = candidate_dt.unixtime();
        if ((candidate / 60U) < now_minute) {
            continue;
        }

        *out_epoch = candidate;
        return true;
    }

    return false;
}

static void restore_pre_alarm_outputs() {
    audio_manager_set_volume(g_base_audio_volume);

    led_manager_set_hue_sat_front(g_base_led_front_hue, g_base_led_front_sat);
    led_manager_set_hue_sat_back(g_base_led_back_hue, g_base_led_back_sat);

    set_front_output(g_base_led_front_on, g_base_led_front);
    set_back_output(g_base_led_back_on, g_base_led_back);
}

static void capture_pre_alarm_outputs() {
    g_base_audio_volume = audio_manager_get_volume();
    g_base_led_front = led_manager_get_front();
    g_base_led_back = led_manager_get_back();
    g_base_led_front_on = led_manager_is_front_on();
    g_base_led_back_on = led_manager_is_back_on();
    g_base_led_front_hue = led_manager_get_hue_front();
    g_base_led_front_sat = led_manager_get_sat_front();
    g_base_led_back_hue = led_manager_get_hue_back();
    g_base_led_back_sat = led_manager_get_sat_back();
}

static void apply_alarm_outputs(bool audio_active,
                                uint8_t volume_pct,
                                bool led_active,
                                uint8_t led_value,
                                bool pause_front_and_audio) {
    if (led_active) {
        set_back_output(true, led_value);
        set_front_output(!pause_front_and_audio, led_value);
    } else if (pause_front_and_audio) {
        set_front_output(false, 0);
    }

    if (audio_active) {
        audio_manager_set_volume(pause_front_and_audio ? 0 : volume_pct);
    }
}

static void stop_alarm(bool return_home, bool leave_back_at_end) {
    audio_manager_stop();
    if (leave_back_at_end) {
        set_front_output(false, 0);
        set_back_output(true, g_flow_end_led);
    } else {
        restore_pre_alarm_outputs();
    }

    const AppSettings &s = storage_manager_get();
    g_state = s.alarm_enabled ? AlarmState::ARMED : AlarmState::IDLE;
    g_alarm_target_epoch = 0;
    g_flow_vol_ramp_min = 0;
    g_flow_sun_ramp_min = 0;
    g_flow_end_volume = 0;
    g_flow_end_led = 0;
    g_audio_started = false;
    g_auto_loud_triggered = false;
    g_auto_loud_start_ms = 0;
    g_outputs_paused_for_hold = false;
    g_any_btn_prev_held = false;
    g_hold_dismiss_latched = false;
    ui_alarm_active_window_set_hold_progress(0.0f);

    if (return_home) {
        settings_menu_return_home();
    }
}

static void start_alarm_flow(uint32_t target_epoch, bool skip_ramp) {
    const AppSettings &s = storage_manager_get();

    capture_pre_alarm_outputs();

    led_manager_set_hue_sat_front(s.led1_hue, s.led1_sat);
    led_manager_set_hue_sat_back(s.led2_hue, s.led2_sat);

    g_alarm_target_epoch = target_epoch;
    g_ramp_start_ms = millis();
    g_active_start_ms = 0;
    g_auto_loud_start_ms = 0;
    g_auto_loud_triggered = false;
    g_flow_vol_ramp_min = skip_ramp ? 0 : clamp_u16(s.alarm_vol_ramp_min, 0, 30);
    g_flow_sun_ramp_min = skip_ramp ? 0 : clamp_u16(s.alarm_sun_ramp_min, 0, 30);
    g_flow_end_volume = clamp_u8(s.alarm_volume, 0, 100);
    g_flow_end_led = clamp_u8(s.alarm_end_brightness, 0, 255);
    g_audio_started = false;
    g_outputs_paused_for_hold = false;
    g_hold_dismiss_latched = false;
    g_any_btn_prev_held = false;
    g_press_start_ms = 0;

    copy_str(g_alarm_path, sizeof(g_alarm_path), (s.alarm_mp3[0] != '\0') ? s.alarm_mp3 : "/test.mp3");

    if (skip_ramp) {
        audio_manager_play_loop(g_alarm_path);
        g_audio_started = true;
        apply_alarm_outputs(true, g_flow_end_volume, true, g_flow_end_led, false);
        g_active_start_ms = g_ramp_start_ms;
        g_state = AlarmState::ACTIVE;
    } else {
        g_state = AlarmState::RAMP_UP;
    }

    ui_alarm_active_window_set_hint(s.snooze_enabled);
    ui_alarm_active_window_set_hold_progress(0.0f);
    ui_alarm_active_window_show();
}

static void maybe_complete_once_alarm_for_today(const DateTime &now) {
    AppSettings &s = storage_manager_get();
    if (s.repeat_mode & 0x80U) {
        s.alarm_enabled = false;
        storage_manager_save_alarm();
    }
    g_last_match_stamp = epoch_minutes(now);
}

static bool compute_current_flow_levels(uint32_t now_ms,
                                        bool *out_audio_active,
                                        uint8_t *out_volume,
                                        bool *out_led_active,
                                        uint8_t *out_led_value,
                                        bool *out_is_fully_active) {
    if (!out_audio_active || !out_volume || !out_led_active || !out_led_value || !out_is_fully_active) {
        return false;
    }

    const uint32_t flow_elapsed_ms = now_ms - g_ramp_start_ms;
    const uint16_t lead_min = (g_flow_vol_ramp_min > g_flow_sun_ramp_min) ? g_flow_vol_ramp_min : g_flow_sun_ramp_min;
    const uint32_t lead_ms = (uint32_t)lead_min * 60U * 1000U;
    const uint32_t vol_ramp_ms = (uint32_t)g_flow_vol_ramp_min * 60U * 1000U;
    const uint32_t sun_ramp_ms = (uint32_t)g_flow_sun_ramp_min * 60U * 1000U;
    const uint32_t vol_start_offset_ms = lead_ms - vol_ramp_ms;
    const uint32_t sun_start_offset_ms = lead_ms - sun_ramp_ms;

    *out_audio_active = flow_elapsed_ms >= vol_start_offset_ms;
    *out_led_active = flow_elapsed_ms >= sun_start_offset_ms;
    *out_volume = g_flow_end_volume;
    *out_led_value = g_flow_end_led;

    if (*out_audio_active && vol_ramp_ms > 0U) {
        *out_volume = ramp_from_one_to_end(g_flow_end_volume, flow_elapsed_ms - vol_start_offset_ms, vol_ramp_ms);
    }
    if (*out_led_active && sun_ramp_ms > 0U) {
        *out_led_value = led_ramp_from_one_to_end(g_flow_end_led, flow_elapsed_ms - sun_start_offset_ms, sun_ramp_ms);
    }

    const bool audio_complete = *out_audio_active && ((vol_ramp_ms == 0U) || ((flow_elapsed_ms - vol_start_offset_ms) >= vol_ramp_ms));
    const bool led_complete = *out_led_active && ((sun_ramp_ms == 0U) || ((flow_elapsed_ms - sun_start_offset_ms) >= sun_ramp_ms));
    *out_is_fully_active = audio_complete && led_complete;
    return true;
}

} // namespace

void alarm_manager_init() {
    const AppSettings &s = storage_manager_get();
    g_state = s.alarm_enabled ? AlarmState::ARMED : AlarmState::IDLE;

    ui_alarm_active_window_init();
    ui_alarm_active_window_set_hold_progress(0.0f);
}

void alarm_manager_check_trigger() {
    AppSettings &s = storage_manager_get();
    DateTime now = rtc_manager_get_time();

    // Snooze re-trigger is part of an already-started alarm flow and should work
    // even when a one-time alarm has disabled future scheduling.
    if (g_state == AlarmState::SNOOZING) {
        if (now.unixtime() >= g_snooze_until_epoch) {
            start_alarm_flow(g_snooze_until_epoch, true);
        }
        return;
    }

    if (!s.alarm_enabled) {
        if (g_state == AlarmState::IDLE || g_state == AlarmState::ARMED) {
            g_state = AlarmState::IDLE;
        }
        return;
    }

    if (g_state == AlarmState::IDLE) {
        g_state = AlarmState::ARMED;
    }

    if (g_state != AlarmState::ARMED) {
        return;
    }

    uint32_t alarm_epoch = 0;
    if (!compute_next_scheduled_alarm_epoch(s, now, &alarm_epoch)) {
        return;
    }

    const uint32_t alarm_stamp = alarm_epoch / 60U;
    if (g_last_match_stamp == alarm_stamp) {
        return;
    }

    const uint16_t lead_min = clamp_u16(
        (s.alarm_vol_ramp_min > s.alarm_sun_ramp_min) ? s.alarm_vol_ramp_min : s.alarm_sun_ramp_min,
        0,
        30);
    const uint32_t flow_start_epoch = alarm_epoch - ((uint32_t)lead_min * 60U);

    if (now.unixtime() < flow_start_epoch) {
        return;
    }

    start_alarm_flow(alarm_epoch, false);
    maybe_complete_once_alarm_for_today(now);
    g_last_match_stamp = alarm_stamp;
}

void alarm_manager_update(bool enc1_held, bool enc2_held) {
    const AppSettings &s = storage_manager_get();

    if (g_state == AlarmState::IDLE || g_state == AlarmState::ARMED || g_state == AlarmState::SNOOZING) {
        return;
    }

    const uint32_t now_ms = millis();
    ui_alarm_active_window_update_clock();

    const bool any_held = enc1_held || enc2_held;

    if (any_held && !g_any_btn_prev_held) {
        g_press_start_ms = now_ms;
        g_hold_dismiss_latched = false;
        g_outputs_paused_for_hold = true;
        ui_alarm_active_window_set_hold_progress(0.0f);
    }

    if (g_state == AlarmState::RAMP_UP || g_state == AlarmState::ACTIVE) {
        bool audio_active = false;
        bool led_active = false;
        bool fully_active = false;
        uint8_t vol_now = g_flow_end_volume;
        uint8_t led_now = g_flow_end_led;
        compute_current_flow_levels(now_ms, &audio_active, &vol_now, &led_active, &led_now, &fully_active);

        if (audio_active && !g_audio_started) {
            audio_manager_play_loop(g_alarm_path);
            g_audio_started = true;
        }

        if (fully_active) {
            g_state = AlarmState::ACTIVE;
            if (g_active_start_ms == 0) {
                g_active_start_ms = now_ms;
            }
        } else {
            g_state = AlarmState::RAMP_UP;
        }

        apply_alarm_outputs(audio_active, vol_now, led_active, led_now, g_outputs_paused_for_hold);

        if (g_active_start_ms != 0) {
            if (!g_auto_loud_triggered && s.alarm_auto_loud_min > 0 &&
                (now_ms - g_active_start_ms) >= ((uint32_t)s.alarm_auto_loud_min * 60U * 1000U)) {
                g_auto_loud_triggered = true;
                g_auto_loud_start_ms = now_ms;
            }

            if (g_auto_loud_triggered) {
                set_back_output(true, 255);
                set_front_output(!g_outputs_paused_for_hold, g_outputs_paused_for_hold ? 0 : 255);
                audio_manager_set_volume(g_outputs_paused_for_hold ? 0 : 100);

                if (s.alarm_auto_dismiss_min > 0 &&
                    (now_ms - g_auto_loud_start_ms) >= ((uint32_t)s.alarm_auto_dismiss_min * 60U * 1000U)) {
                    stop_alarm(true, false);
                    return;
                }
            }

            uint32_t active_timeout_ms = 5U * 60U * 1000U;
            if (s.alarm_auto_loud_min > 0) {
                active_timeout_ms = ((uint32_t)s.alarm_auto_loud_min + 5U) * 60U * 1000U;
                if (s.alarm_auto_dismiss_min > 0) {
                    active_timeout_ms = ((uint32_t)s.alarm_auto_loud_min +
                                         (uint32_t)s.alarm_auto_dismiss_min) * 60U * 1000U;
                }
            }
            if ((now_ms - g_active_start_ms) >= active_timeout_ms) {
                stop_alarm(true, false);
                return;
            }
        }
    }

    if (any_held) {
        const uint32_t held_ms = now_ms - g_press_start_ms;
        const uint32_t displayed_held_ms = (held_ms == 0U) ? 1U : held_ms;
        const uint32_t hold_target_ms = (uint32_t)clamp_u8(s.hold_dismiss_sec, 1, 10) * 1000U;

        float progress = (displayed_held_ms >= hold_target_ms)
            ? 1.0f
            : (float)displayed_held_ms / (float)hold_target_ms;
        ui_alarm_active_window_set_hold_progress(progress);

        if (!g_hold_dismiss_latched && held_ms >= hold_target_ms) {
            g_hold_dismiss_latched = true;
            stop_alarm(true, true);
            return;
        }
    }

    if (!any_held && g_any_btn_prev_held) {
        const uint32_t held_ms = now_ms - g_press_start_ms;
        g_outputs_paused_for_hold = false;
        ui_alarm_active_window_set_hold_progress(0.0f);

        // Quick release snoozes; longer-than-tap hold releases just reset the progress ring.
        if (!g_hold_dismiss_latched && s.snooze_enabled && held_ms < TAP_SNOOZE_MS) {
            audio_manager_stop();
            restore_pre_alarm_outputs();
            g_state = AlarmState::SNOOZING;
            g_snooze_until_epoch = rtc_manager_get_time().unixtime() + ((uint32_t)clamp_u16(s.snooze_duration_min, 1, 120) * 60U);
            settings_menu_return_home();
            g_any_btn_prev_held = false;
            return;
        }
    }

    g_any_btn_prev_held = any_held;
}

AlarmState alarm_manager_get_state() {
    return g_state;
}

bool alarm_manager_is_alarm_active() {
    return g_state == AlarmState::RAMP_UP || g_state == AlarmState::ACTIVE;
}

bool alarm_manager_is_alarm_screen_visible() {
    if (g_state == AlarmState::RAMP_UP || g_state == AlarmState::ACTIVE) {
        return true;
    }

    lv_obj_t *alarm_screen = ui_alarm_active_window_get_screen();
    return alarm_screen && lv_screen_active() == alarm_screen;
}

bool alarm_manager_get_next_alarm_time(const DateTime &now, DateTime *out_time, bool *out_is_snoozed) {
    if (out_is_snoozed) {
        *out_is_snoozed = false;
    }

    if (g_state == AlarmState::SNOOZING && g_snooze_until_epoch > now.unixtime()) {
        if (out_time) {
            *out_time = DateTime(g_snooze_until_epoch);
        }
        if (out_is_snoozed) {
            *out_is_snoozed = true;
        }
        return true;
    }

    uint32_t next_epoch = 0;
    if (!compute_next_scheduled_alarm_epoch(storage_manager_get(), now, &next_epoch)) {
        return false;
    }

    if (out_time) {
        *out_time = DateTime(next_epoch);
    }
    return true;
}
