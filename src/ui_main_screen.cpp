#include "ui_main_screen.h"
#include "manager_alarm.h"
#include "manager_brightness.h"
#include "manager_rtc.h"
#include "manager_sensor.h"
#include "manager_storage.h"
#include <lvgl.h>
#include <Arduino.h>
#include <math.h>

// Colours — not constexpr (LVGL colour functions are runtime)
#define COL_BG      lv_color_black()
#define COL_PRIMARY lv_color_white()
#define COL_DIM     lv_color_make(0x88, 0x88, 0x88)
#define COL_ACCENT  lv_color_make(0xFF, 0xB0, 0x00)

// ── Widget handles ────────────────────────────────────────────────────────────
static lv_obj_t *lbl_time     = nullptr;   // "12:34"
static lv_obj_t *lbl_ampm     = nullptr;   // "AM" / "PM"
static lv_obj_t *lbl_secs     = nullptr;   // "56"
static lv_obj_t *lbl_date     = nullptr;   // "14th Jul"
static lv_obj_t *lbl_dow      = nullptr;   // "Monday,"
static lv_obj_t *lbl_alarm    = nullptr;   // "Alarm  07:00" / "Alarm  OFF"
static lv_obj_t *lbl_until    = nullptr;   // "in 14h 26m"
static lv_obj_t *lbl_temp     = nullptr;   // "23.4°C"
static lv_obj_t *lbl_hum      = nullptr;   // "48%"
static lv_obj_t *lbl_ldr_raw  = nullptr;   // "LDR: 4095"
static lv_obj_t *lbl_brightness = nullptr; // "BR: 255"
static lv_obj_t *main_screen  = nullptr;

static bool colon_visible = true;

// Weekday label (index 10) is anchored by its bottom-right corner instead of
// top-left, since its text length varies ("Mon," vs "Wednesday,") and should
// grow leftward from a stable right edge. g_dow_anchor_x/y hold that corner;
// the label's actual top-left lv_obj position is recomputed from it whenever
// its text, font size, or position changes.
static int16_t g_dow_anchor_x = 0;
static int16_t g_dow_anchor_y = 0;

// ── Customizable element registry (Home Page menu) ───────────────────────────
static lv_obj_t   *g_elements[UI_HOME_ELEMENT_COUNT_MAIN]   = {nullptr};
static const char  *g_element_names[UI_HOME_ELEMENT_COUNT_MAIN] = {
    "Time", "AM/PM", "Seconds", "Day/Month", "Alarm",
    "Time Until", "Temperature", "Humidity", "Brightness", "LDR Raw",
    "Weekday",
};
static uint8_t g_element_font_size[UI_HOME_ELEMENT_COUNT_MAIN] = {0};
static bool     g_element_visible[UI_HOME_ELEMENT_COUNT_MAIN] = {true};
static uint16_t g_element_color[UI_HOME_ELEMENT_COUNT_MAIN] = {0};
static bool     g_element_color_customized[UI_HOME_ELEMENT_COUNT_MAIN] = {false};

static const uint8_t g_factory_font_size[UI_HOME_ELEMENT_COUNT_MAIN] = {
    32, 20, 20, 20, 16, 16, 16, 16, 16, 16,
    20,
};
static const int16_t g_factory_x[UI_HOME_ELEMENT_COUNT_MAIN] = {
    0, 150, 105, 130, 0, 100, 190, 270, 0, 90,
    0,
};
static const int16_t g_factory_y[UI_HOME_ELEMENT_COUNT_MAIN] = {
    0, 8, 8, 38, 70, 70, 70, 70, 100, 100,
    38,
};

// Factory defaults, captured once at init before any NVS customization is applied.
static uint8_t  g_element_default_font_size[UI_HOME_ELEMENT_COUNT_MAIN] = {0};
static int16_t  g_element_default_x[UI_HOME_ELEMENT_COUNT_MAIN] = {0};
static int16_t  g_element_default_y[UI_HOME_ELEMENT_COUNT_MAIN] = {0};
static uint16_t g_element_default_color[UI_HOME_ELEMENT_COUNT_MAIN] = {0};
static uint16_t g_background_color = 0x0000;

static bool g_alarm_color_customized = false;

static uint16_t pack_rgb565(lv_color_t c) {
    return (uint16_t)(((c.red & 0xF8) << 8) | ((c.green & 0xFC) << 3) | (c.blue >> 3));
}

static lv_color_t unpack_rgb565(uint16_t v) {
    uint8_t r = (uint8_t)((v >> 8) & 0xF8); r |= (uint8_t)(r >> 5);
    uint8_t g = (uint8_t)((v >> 3) & 0xFC); g |= (uint8_t)(g >> 6);
    uint8_t b = (uint8_t)((v << 3) & 0xF8); b |= (uint8_t)(b >> 5);
    return lv_color_make(r, g, b);
}

// Real fonts stop at 48pt; 64/80 fake a larger size by uniformly scaling the
// 48pt font glyphs via the transform-scale style property (256 == 100%).
static const lv_font_t *font_for_size(uint8_t size) {
    switch (size) {
        case 24: return &lv_font_montserrat_24;
        case 32: return &lv_font_montserrat_32;
        case 48: return &lv_font_montserrat_48;
        case 64: return &lv_font_montserrat_48;
        case 80: return &lv_font_montserrat_48;
        default: return &lv_font_montserrat_16;
    }
}

static int32_t transform_scale_for_size(uint8_t size) {
    if (size == 64) return (int32_t)(256 * 64 / 48);
    if (size == 80) return (int32_t)(256 * 80 / 48);
    return 256; // LV_SCALE_NONE
}


// ── Helpers ───────────────────────────────────────────────────────────────────
static const char *ordinal(uint8_t d) {
    if (d >= 11 && d <= 13) return "th";
    switch (d % 10) {
        case 1: return "st";
        case 2: return "nd";
        case 3: return "rd";
        default: return "th";
    }
}

static const char *day_name(uint8_t dow) {
    // RTClib: 0=Sunday
    static const char *days[] = {
        "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"
    };
    return (dow < 7) ? days[dow] : "---";
}

static const char *month_name(uint8_t m) {
    static const char *months[] = {
        "","Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    return (m >= 1 && m <= 12) ? months[m] : "---";
}

static bool is_same_calendar_day(const DateTime &a, const DateTime &b) {
    return a.year() == b.year() && a.month() == b.month() && a.day() == b.day();
}

static void reposition_dow_anchor() {
    if (!lbl_dow) return;
    lv_obj_update_layout(lbl_dow);
    int32_t w = lv_obj_get_width(lbl_dow);
    int32_t h = lv_obj_get_height(lbl_dow);
    lv_obj_set_align(lbl_dow, LV_ALIGN_DEFAULT);
    lv_obj_set_pos(lbl_dow, (int16_t)(g_dow_anchor_x - w), (int16_t)(g_dow_anchor_y - h));
}

static void rgb565_to_hex(uint16_t v, char *out /* buffer of at least 7 bytes */) {
    uint8_t r = (uint8_t)((v >> 8) & 0xF8); r |= (uint8_t)(r >> 5);
    uint8_t g = (uint8_t)((v >> 3) & 0xFC); g |= (uint8_t)(g >> 6);
    uint8_t b = (uint8_t)((v << 3) & 0xF8); b |= (uint8_t)(b >> 5);
    snprintf(out, 7, "%02X%02X%02X", r, g, b);
}

// ── Init ──────────────────────────────────────────────────────────────────────
void ui_main_screen_init() {
    lv_obj_t *scr = lv_screen_active();
    main_screen = scr;
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF); // elements may be positioned off-screen; never show scrollbars

    // Left cluster: time/date/alarm
    lbl_time = lv_label_create(scr);
    lv_label_set_text(lbl_time, "00:00");
    lv_label_set_recolor(lbl_time, true);
    lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_time, COL_PRIMARY, 0);
    lv_obj_align(lbl_time, LV_ALIGN_TOP_LEFT, 8, -2);

    lbl_ampm = lv_label_create(scr);
    lv_label_set_text(lbl_ampm, "AM");
    lv_obj_set_style_text_font(lbl_ampm, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_ampm, COL_PRIMARY, 0);
    lv_obj_align_to(lbl_ampm, lbl_time, LV_ALIGN_OUT_RIGHT_TOP, 6, 10);

    lbl_secs = lv_label_create(scr);
    lv_label_set_text(lbl_secs, "00");
    lv_obj_set_style_text_font(lbl_secs, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_secs, COL_DIM, 0);
    lv_obj_align_to(lbl_secs, lbl_ampm, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);

    lbl_dow = lv_label_create(scr);
    lv_label_set_text(lbl_dow, "---");
    lv_obj_set_style_text_font(lbl_dow, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_dow, COL_DIM, 0);
    lv_obj_set_style_text_align(lbl_dow, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(lbl_dow, LV_ALIGN_TOP_LEFT, 10, 54);

    lbl_date = lv_label_create(scr);
    lv_label_set_text(lbl_date, "---");
    lv_obj_set_style_text_font(lbl_date, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_date, COL_DIM, 0);
    lv_obj_set_style_text_align(lbl_date, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(lbl_date, LV_ALIGN_TOP_LEFT, 130, 54);

    lbl_alarm = lv_label_create(scr);
    lv_label_set_text(lbl_alarm, "Alarm  OFF");
    lv_obj_set_style_text_font(lbl_alarm, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_alarm, COL_ACCENT, 0);
    lv_obj_set_width(lbl_alarm, 276);
    lv_obj_set_style_text_align(lbl_alarm, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(lbl_alarm, LV_ALIGN_TOP_LEFT, 10, 76);

    lbl_until = lv_label_create(scr);
    lv_label_set_text(lbl_until, "");
    lv_obj_set_style_text_font(lbl_until, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_until, COL_DIM, 0);
    lv_obj_set_width(lbl_until, 276);
    lv_obj_set_style_text_align(lbl_until, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(lbl_until, LV_ALIGN_TOP_LEFT, 10, 96);

    lbl_temp = lv_label_create(scr);
    lv_label_set_text(lbl_temp, "--.-\xc2\xb0\x43");  // "--.-°C"
    lv_obj_set_style_text_font(lbl_temp, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_temp, COL_PRIMARY, 0);
    lv_obj_align(lbl_temp, LV_ALIGN_TOP_RIGHT, -10, 10);

    lbl_hum = lv_label_create(scr);
    lv_label_set_text(lbl_hum, "--%");
    lv_obj_set_style_text_font(lbl_hum, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_hum, COL_PRIMARY, 0);
    lv_obj_align(lbl_hum, LV_ALIGN_TOP_RIGHT, -10, 42);

    lbl_brightness = lv_label_create(scr);
    lv_label_set_text(lbl_brightness, "BR: 000");
    lv_obj_set_style_text_font(lbl_brightness, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_brightness, COL_DIM, 0);
    lv_obj_align(lbl_brightness, LV_ALIGN_BOTTOM_LEFT, 10, -4);

    lbl_ldr_raw = lv_label_create(scr);
    lv_label_set_text(lbl_ldr_raw, "LDR: 0000");
    lv_obj_set_style_text_font(lbl_ldr_raw, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_ldr_raw, COL_DIM, 0);
    lv_obj_align(lbl_ldr_raw, LV_ALIGN_BOTTOM_RIGHT, -10, -4);

    // Register elements and snapshot their resolved layout as hardcoded x/y — all
    // future moves use lv_obj_set_pos() directly instead of relative alignment.
    g_elements[0] = lbl_time;
    g_elements[1] = lbl_ampm;
    g_elements[2] = lbl_secs;
    g_elements[3] = lbl_date;
    g_elements[4] = lbl_alarm;
    g_elements[5] = lbl_until;
    g_elements[6] = lbl_temp;
    g_elements[7] = lbl_hum;
    g_elements[8] = lbl_brightness;
    g_elements[9] = lbl_ldr_raw;
    g_elements[10] = lbl_dow;

    for (uint8_t i = 0; i < UI_HOME_ELEMENT_COUNT_MAIN; ++i) {
        lv_obj_t *obj = g_elements[i];
        if (!obj) continue;
        g_element_font_size[i] = g_factory_font_size[i];
        lv_obj_set_style_text_font(obj, font_for_size(g_factory_font_size[i]), 0);
        lv_obj_set_style_transform_scale(obj, transform_scale_for_size(g_factory_font_size[i]), 0);
        lv_obj_set_style_text_color(obj, COL_PRIMARY, 0);

        if (i == 10) {
            g_dow_anchor_x = g_factory_x[i];
            g_dow_anchor_y = g_factory_y[i];
            reposition_dow_anchor();
        } else {
            lv_obj_set_pos(obj, g_factory_x[i], g_factory_y[i]);
        }

        g_element_default_font_size[i] = g_factory_font_size[i];
        g_element_default_x[i] = (i == 10) ? g_dow_anchor_x : g_factory_x[i];
        g_element_default_y[i] = (i == 10) ? g_dow_anchor_y : g_factory_y[i];
        g_element_default_color[i] = pack_rgb565(lv_obj_get_style_text_color(obj, LV_PART_MAIN));
        g_element_color[i] = g_element_default_color[i];
        g_element_visible[i] = true;
    }
    ui_main_screen_set_background_color(storage_manager_get().home_background_color_rgb565);
    ui_main_screen_apply_customization();
}

// ── Update (call every second) ────────────────────────────────────────────────
void ui_main_screen_update() {
    char buf[64];

    // ── Time with flashing colon ──────────────────────────────────────────────
    colon_visible = !colon_visible;

    if (rtc_manager_is_ok()) {
        DateTime now = rtc_manager_get_time();

        // 12-hour conversion
        uint8_t h12 = now.hour() % 12;
        if (h12 == 0) h12 = 12;
        bool is_pm = now.hour() >= 12;

        char colon_hex[7];
        rgb565_to_hex(colon_visible ? g_element_color[0] : g_background_color, colon_hex);
        snprintf(buf, sizeof(buf), "%u#%s :#%02u", (unsigned)h12, colon_hex, (unsigned)now.minute());
        lv_label_set_text(lbl_time, buf);
        lv_label_set_text(lbl_ampm, is_pm ? "PM" : "AM");

        snprintf(buf, sizeof(buf), "%02d", now.second());
        lv_label_set_text(lbl_secs, buf);

        // ── Date ──────────────────────────────────────────────────────────────
        snprintf(buf, sizeof(buf), "%s,", day_name(now.dayOfTheWeek()));
        lv_label_set_text(lbl_dow, buf);
        reposition_dow_anchor();

        snprintf(buf, sizeof(buf), "%d%s %s",
                 now.day(), ordinal(now.day()),
                 month_name(now.month()));
        lv_label_set_text(lbl_date, buf);

        // ── Alarm & time-until ────────────────────────────────────────────────
        DateTime next_alarm;
        bool is_snoozed = false;
        if (alarm_manager_get_next_alarm_time(now, &next_alarm, &is_snoozed)) {
            snprintf(buf, sizeof(buf), "Alarm %02d:%02d", next_alarm.hour(), next_alarm.minute());
            lv_label_set_text(lbl_alarm, buf);
            if (!g_alarm_color_customized) lv_obj_set_style_text_color(lbl_alarm, COL_ACCENT, 0);

            uint32_t diff_seconds = 0;
            if (next_alarm.unixtime() > now.unixtime()) {
                diff_seconds = next_alarm.unixtime() - now.unixtime();
            }
            if (diff_seconds < (60U * 60U)) {
                uint32_t minutes = (diff_seconds + 59U) / 60U;
                if (minutes == 0) minutes = 1;
                if (minutes == 1U) snprintf(buf, sizeof(buf), "in 1 minute");
                else snprintf(buf, sizeof(buf), "in %lu minutes", (unsigned long)minutes);
            } else if (diff_seconds < (12U * 60U * 60U)) {
                uint32_t hours = diff_seconds / (60U * 60U);
                if (hours == 1U) snprintf(buf, sizeof(buf), "1 hour");
                else snprintf(buf, sizeof(buf), "%lu hours", (unsigned long)hours);
            } else if (diff_seconds <= (24U * 60U * 60U)) {
                snprintf(buf, sizeof(buf), "Tomorrow");
            } else if (!is_snoozed && !is_same_calendar_day(now, next_alarm)) {
                snprintf(buf, sizeof(buf), "on %s", day_name(next_alarm.dayOfTheWeek()));
            } else {
                const uint32_t diff_minutes = (diff_seconds + 59U) / 60U;
                const uint32_t h = diff_minutes / 60U;
                const uint32_t m = diff_minutes % 60U;
                if (h > 0U) snprintf(buf, sizeof(buf), "in %luh %lum", (unsigned long)h, (unsigned long)m);
                else        snprintf(buf, sizeof(buf), "in %lum",       (unsigned long)m);
            }
            lv_label_set_text(lbl_until, buf);
        } else {
            lv_label_set_text(lbl_alarm, "Alarm  OFF");
            if (!g_alarm_color_customized) lv_obj_set_style_text_color(lbl_alarm, COL_DIM, 0);
            lv_label_set_text(lbl_until, "");
        }
    } else {
        lv_label_set_text(lbl_time, "RTC ERR");
        lv_label_set_text(lbl_secs, "--");
        lv_label_set_text(lbl_dow, "RTC");
        lv_label_set_text(lbl_date, "ERROR");
    }

    // ── Sensors ───────────────────────────────────────────────────────────────
    sensor_manager_update();
    const SensorData &sd = sensor_manager_get();

    if (!isnan(sd.temperature))
        snprintf(buf, sizeof(buf), "%.1f\xc2\xb0\x43", sd.temperature);
    else
        snprintf(buf, sizeof(buf), "--.-\xc2\xb0\x43");
    lv_label_set_text(lbl_temp, buf);

    if (!isnan(sd.humidity))
        snprintf(buf, sizeof(buf), "%.0f%%", sd.humidity);
    else
        snprintf(buf, sizeof(buf), "--%%" );
    lv_label_set_text(lbl_hum, buf);

    snprintf(buf, sizeof(buf), "BR: %03u", brightness_manager_get_current_brightness());
    lv_label_set_text(lbl_brightness, buf);

    snprintf(buf, sizeof(buf), "LDR: %04u", brightness_manager_get_last_ldr_raw());
    lv_label_set_text(lbl_ldr_raw, buf);
}

lv_obj_t *ui_main_screen_get_screen() {
    return main_screen;
}

lv_obj_t *ui_main_screen_get_element(uint8_t index) {
    if (index >= UI_HOME_ELEMENT_COUNT_MAIN) return nullptr;
    return g_elements[index];
}

const char *ui_main_screen_get_element_name(uint8_t index) {
    if (index >= UI_HOME_ELEMENT_COUNT_MAIN) return "";
    return g_element_names[index];
}

uint8_t ui_main_screen_get_element_font_size(uint8_t index) {
    if (index >= UI_HOME_ELEMENT_COUNT_MAIN) return 0;
    return g_element_font_size[index];
}

void ui_main_screen_set_element_font_size(uint8_t index, uint8_t size) {
    if (index >= UI_HOME_ELEMENT_COUNT_MAIN || !g_elements[index]) return;
    if (size < 16) size = 16;
    g_element_font_size[index] = size;
    lv_obj_set_style_text_font(g_elements[index], font_for_size(size), 0);
    lv_obj_set_style_transform_scale(g_elements[index], transform_scale_for_size(size), 0);
    if (index == 10) reposition_dow_anchor(); // font size change alters width, re-anchor to bottom-right
}

void ui_main_screen_get_element_pos(uint8_t index, int16_t *x, int16_t *y) {
    if (index >= UI_HOME_ELEMENT_COUNT_MAIN || !g_elements[index]) {
        if (x) *x = 0;
        if (y) *y = 0;
        return;
    }
    if (index == 10) { // weekday: report the bottom-right anchor, not the top-left
        if (x) *x = g_dow_anchor_x;
        if (y) *y = g_dow_anchor_y;
        return;
    }
    if (x) *x = (int16_t)lv_obj_get_x(g_elements[index]);
    if (y) *y = (int16_t)lv_obj_get_y(g_elements[index]);
}

void ui_main_screen_set_element_pos(uint8_t index, int16_t x, int16_t y) {
    if (index >= UI_HOME_ELEMENT_COUNT_MAIN || !g_elements[index]) return;
    if (index == 10) { // weekday: x,y define the bottom-right corner; text grows leftward from it
        g_dow_anchor_x = x;
        g_dow_anchor_y = y;
        reposition_dow_anchor();
        return;
    }
    lv_obj_set_align(g_elements[index], LV_ALIGN_DEFAULT);
    lv_obj_set_pos(g_elements[index], x, y);
}

bool ui_main_screen_get_element_visible(uint8_t index) {
    if (index >= UI_HOME_ELEMENT_COUNT_MAIN) return true;
    return g_element_visible[index];
}

void ui_main_screen_set_element_visible(uint8_t index, bool visible) {
    if (index >= UI_HOME_ELEMENT_COUNT_MAIN || !g_elements[index]) return;
    g_element_visible[index] = visible;
    lv_obj_set_hidden(g_elements[index], !visible);
}

uint16_t ui_main_screen_get_element_color(uint8_t index) {
    if (index >= UI_HOME_ELEMENT_COUNT_MAIN) return 0xFFFF;
    return g_element_color[index];
}

void ui_main_screen_set_element_color(uint8_t index, uint16_t color_rgb565) {
    if (index >= UI_HOME_ELEMENT_COUNT_MAIN || !g_elements[index]) return;
    if (color_rgb565 == 0xFFFF) {
        g_element_color_customized[index] = false;
        g_element_color[index] = g_element_default_color[index];
        lv_obj_set_style_text_color(g_elements[index], unpack_rgb565(g_element_default_color[index]), 0);
    } else {
        g_element_color_customized[index] = true;
        g_element_color[index] = color_rgb565;
        lv_obj_set_style_text_color(g_elements[index], unpack_rgb565(color_rgb565), 0);
    }
    if (index == 4) g_alarm_color_customized = true;
}

uint8_t ui_main_screen_get_element_default_font_size(uint8_t index) {
    if (index >= UI_HOME_ELEMENT_COUNT_MAIN) return 0;
    return g_element_default_font_size[index];
}

void ui_main_screen_get_element_default_pos(uint8_t index, int16_t *x, int16_t *y) {
    if (index >= UI_HOME_ELEMENT_COUNT_MAIN) {
        if (x) *x = 0;
        if (y) *y = 0;
        return;
    }
    if (x) *x = g_element_default_x[index];
    if (y) *y = g_element_default_y[index];
}

uint16_t ui_main_screen_get_element_default_color(uint8_t index) {
    if (index >= UI_HOME_ELEMENT_COUNT_MAIN) return 0xFFFF;
    return g_element_default_color[index];
}

void ui_main_screen_reset_element(uint8_t index) {
    if (index >= UI_HOME_ELEMENT_COUNT_MAIN || !g_elements[index]) return;
    ui_main_screen_set_element_font_size(index, g_element_default_font_size[index]);
    ui_main_screen_set_element_pos(index, g_element_default_x[index], g_element_default_y[index]);
    ui_main_screen_set_element_color(index, 0xFFFF);
    ui_main_screen_set_element_visible(index, true);
}

uint16_t ui_main_screen_get_background_color() {
    return g_background_color;
}

void ui_main_screen_set_background_color(uint16_t color_rgb565) {
    g_background_color = color_rgb565;
    if (main_screen) lv_obj_set_style_bg_color(main_screen, unpack_rgb565(color_rgb565), 0);
}

void ui_main_screen_apply_customization() {
    AppSettings &s = storage_manager_get();
    for (uint8_t i = 0; i < UI_HOME_ELEMENT_COUNT_MAIN; ++i) {
        if (!g_elements[i]) continue;
        const UiElementConfig &e = s.home_elements[i];
        if (e.font_size != 0 && e.x >= 0 && e.y >= 0) {
            ui_main_screen_set_element_font_size(i, e.font_size);
            ui_main_screen_set_element_pos(i, e.x, e.y);
        }
        ui_main_screen_set_element_visible(i, e.visible != 0);
        ui_main_screen_set_element_color(i, e.color_rgb565);
    }
}
