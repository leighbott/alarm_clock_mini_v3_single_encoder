#include "manager_brightness.h"

#include "manager_alarm.h"
#include "manager_storage.h"
#include "pins_config.h"
#include "ui_settings_menu.h"

#include <Arduino.h>
#include <math.h>

void display_set_brightness(uint8_t brightness);

namespace {

static constexpr uint32_t LDR_AVERAGE_WINDOW_MS = 2000;

static uint16_t g_last_ldr_raw = 0;
static float g_smoothed_ldr = 0.0f;
static uint32_t g_last_home_input_ms = 0;
static uint8_t g_current_brightness = 0;
static bool g_has_sample = false;
static bool g_preview_override_active = false;
static uint8_t g_preview_override_value = 0;

static uint32_t g_ldr_accum_sum = 0;
static uint32_t g_ldr_accum_count = 0;
static uint32_t g_ldr_window_start_ms = 0;

static uint8_t minimum_raw_brightness() {
    const AppSettings &settings = storage_manager_get();
    return settings.min_brightness_off ? 0 : 1;
}

static uint8_t clamp_with_minimum(uint8_t value) {
    const uint8_t minimum = minimum_raw_brightness();
    return (value < minimum) ? minimum : value;
}

static uint8_t map_ldr_to_brightness(float ldr_value) {
    float ldr_max_raw = storage_manager_get().ldr_max_raw;
    if (ldr_max_raw < 0.0f) ldr_max_raw = 0.0f;
    if (ldr_max_raw > 4095.0f) ldr_max_raw = 4095.0f;

    float normalized = 0.0f;
    if (ldr_max_raw <= 0.0f) {
        normalized = (ldr_value > 0.0f) ? 1.0f : 0.0f;
    } else {
        normalized = ldr_value / ldr_max_raw;
    }
    if (normalized < 0.0f) normalized = 0.0f;
    if (normalized > 1.0f) normalized = 1.0f;

    const float curved = powf(normalized, 2.4f);
    int brightness = (int)lroundf(curved * 255.0f);
    if (brightness < 0) brightness = 0;
    if (brightness > 255) brightness = 255;
    return clamp_with_minimum((uint8_t)brightness);
}

static bool boost_is_active() {
    if (g_last_home_input_ms == 0) return false;
    return (uint32_t)(millis() - g_last_home_input_ms) <= storage_manager_get().boost_duration_ms;
}

static uint8_t boosted_brightness(const AppSettings &settings) {
    const uint8_t boost = clamp_with_minimum(settings.boost_brightness);
    if (!settings.auto_brightness) return boost;

    const uint8_t auto_brightness = map_ldr_to_brightness(g_smoothed_ldr);
    return (auto_brightness > boost) ? auto_brightness : boost;
}

static uint8_t select_target_brightness() {
    const AppSettings &settings = storage_manager_get();

    if (alarm_manager_is_alarm_active()) {
        return 255;
    }

    if (g_preview_override_active) {
        return g_preview_override_value;
    }

    if (!settings_menu_is_home()) {
        return boosted_brightness(settings);
    }

    if (boost_is_active()) {
        return boosted_brightness(settings);
    }

    if (settings.auto_brightness) {
        return map_ldr_to_brightness(g_smoothed_ldr);
    }

    return clamp_with_minimum(settings.manual_brightness);
}

} // namespace

void brightness_manager_init() {
    pinMode(PIN_LDR, INPUT);
    analogReadResolution(12);

    g_last_ldr_raw = (uint16_t)analogRead(PIN_LDR);
    g_smoothed_ldr = (float)g_last_ldr_raw;
    g_has_sample = true;
    g_ldr_window_start_ms = millis();

    g_current_brightness = select_target_brightness();
    display_set_brightness(g_current_brightness);
}

void brightness_manager_update() {
    const uint16_t sample = (uint16_t)analogRead(PIN_LDR);
    g_last_ldr_raw = sample;

    if (!g_has_sample) {
        g_smoothed_ldr = (float)sample;
        g_has_sample = true;
        g_ldr_window_start_ms = millis();
    }

    // Accumulate raw samples; only fold into the value used for auto-brightness
    // once per averaging window so the display doesn't flicker in real time.
    g_ldr_accum_sum += sample;
    g_ldr_accum_count++;

    const uint32_t now = millis();
    if ((uint32_t)(now - g_ldr_window_start_ms) >= LDR_AVERAGE_WINDOW_MS) {
        g_smoothed_ldr = (float)g_ldr_accum_sum / (float)g_ldr_accum_count;
        g_ldr_accum_sum = 0;
        g_ldr_accum_count = 0;
        g_ldr_window_start_ms = now;
    }

    g_current_brightness = select_target_brightness();
    display_set_brightness(g_current_brightness);
}

void brightness_manager_note_home_input() {
    g_last_home_input_ms = millis();
}

void brightness_manager_set_preview_override(uint8_t raw_brightness) {
    g_preview_override_active = true;
    g_preview_override_value = raw_brightness;
}

void brightness_manager_clear_preview_override() {
    g_preview_override_active = false;
}

uint16_t brightness_manager_get_last_ldr_raw() {
    return g_last_ldr_raw;
}

uint8_t brightness_manager_get_current_brightness() {
    return g_current_brightness;
}