#pragma once

#include <stdint.h>
#include <stdbool.h>

// ── Home screen element customization ─────────────────────────────────────────
static constexpr uint8_t UI_HOME_ELEMENT_COUNT = 11;

struct UiElementConfig {
    uint8_t  font_size;    // pt size (16/24/32/48/64/80); 0 = not customized, use built-in default
    int16_t  x;
    int16_t  y;
    uint8_t  visible;      // 0 = hidden, 1 = shown (always concrete, default 1)
    uint16_t color_rgb565; // packed RGB565; 0xFFFF sentinel = use built-in default color
};

// ── All persistent settings ───────────────────────────────────────────────────
struct AppSettings {
    // Alarm
    bool     alarm_enabled;
    uint8_t  alarm_hour;
    uint8_t  alarm_minute;
    char     alarm_mp3[64];          // path on SD, e.g. "/alarm.mp3"
    uint8_t  alarm_volume;           // 0–100 %
    uint8_t  alarm_end_brightness;   // LED brightness at end of sunrise, 0–255
    uint16_t alarm_vol_ramp_min;     // volume ramp duration, minutes
    uint16_t alarm_sun_ramp_min;     // sunrise ramp duration, minutes
    uint16_t alarm_sun_lead_min;     // sunrise lead-in before alarm, minutes
    bool     snooze_enabled;
    uint16_t snooze_duration_min;    // snooze duration, minutes
    uint8_t  hold_dismiss_sec;       // hold-to-dismiss duration, seconds
    uint8_t  repeat_mode;            // bit7=once, bit0..6=Sun..Sat
    uint8_t  alarm_auto_loud_min;    // 0=off, otherwise minutes after alarm becomes active
    uint8_t  alarm_auto_dismiss_min; // 0=off, minutes after Auto Loud

    // Display
    bool     min_brightness_off;     // true = screen off at minimum, false = value 1
    uint8_t  manual_brightness;      // 0–255
    bool     auto_brightness;
    uint8_t  boost_brightness;       // 0–255
    float    ldr_max_raw;            // ADC scale ceiling for auto brightness mapping
    uint16_t boost_duration_ms;      // display boost hold duration, 500–5000 ms

    // LEDs (last known state — restored on boot)
    uint8_t  led_front_brightness;   // 0–255
    uint8_t  led_back_brightness;    // 0–255
    bool     led_front_enabled;
    bool     led_back_enabled;
    uint16_t led1_hue;               // 0–360 degrees
    uint8_t  led1_sat;               // 0–100 %
    uint16_t led2_hue;               // 0–360 degrees
    uint8_t  led2_sat;               // 0–100 %
    uint16_t auto_led_off_min;       // inactivity timeout in minutes, 10–360

    // Home page customization
    UiElementConfig home_elements[UI_HOME_ELEMENT_COUNT];
    uint16_t        home_background_color_rgb565;
};

// ── API ───────────────────────────────────────────────────────────────────────
void              storage_manager_init();   // load from NVS (call before UI init)

AppSettings&      storage_manager_get();   // direct mutable reference

void              storage_manager_save_all();          // write everything to NVS
void              storage_manager_save_alarm();        // alarm fields only
void              storage_manager_load_display();      // display fields only
void              storage_manager_save_display();      // display fields only
void              storage_manager_save_leds();         // LED fields only
void              storage_manager_save_other();        // Other-menu fields
void              storage_manager_save_home_element(uint8_t index); // one home-page element
void              storage_manager_save_home_background();
