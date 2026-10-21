#include <Arduino.h>
#include <SPI.h>
#include <lvgl.h>
#include "pins_config.h"
#include "manager_input.h"
#include "manager_rtc.h"
#include "manager_sensor.h"
#include "manager_storage.h"
#include "manager_led.h"
#include "manager_audio.h"
#include "manager_brightness.h"
#include "manager_alarm.h"
#include "ui_main_screen.h"
#include "ui_settings_menu.h"

#define SPI_CLK_HZ  40000000UL
#define TFT_BL_LEDC_CH 0

// NV3007 bring-up toggles for quick field testing
#define NV3007_GAP_X        0
#define NV3007_GAP_Y        12
#define NV3007_USE_INVERT   0
#define NV3007_COLOR_SWAPPED 1

// ── Backlight (LEDC) ──────────────────────────────────────────────────────────
static void display_backlight_init(void) {
    ledcSetup(TFT_BL_LEDC_CH, 44100, 8);  // channel 0, frequency 44100 Hz, 8-bit resolution
    ledcAttachPin(PIN_TFT_BLK, TFT_BL_LEDC_CH);  // attach pin to configured channel
}

void display_set_brightness(uint8_t brightness) {
    // ledcWrite first argument is channel, not GPIO number.
    ledcWrite(TFT_BL_LEDC_CH, brightness);
}

// ── NV3007 SPI callbacks ──────────────────────────────────────────────────────
// SPI bus already initialised by audio_manager_init() — reuse it here.

static void nv3007_send_cmd(lv_display_t *disp, const uint8_t *cmd, size_t cmd_size,
                             const uint8_t *param, size_t param_size)
{
    SPI.beginTransaction(SPISettings(SPI_CLK_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_TFT_DC, LOW);
    digitalWrite(PIN_TFT_CS, LOW);
    SPI.transferBytes(cmd, nullptr, cmd_size);
    if (param && param_size > 0) {
        digitalWrite(PIN_TFT_DC, HIGH);
        SPI.transferBytes(param, nullptr, param_size);
    }
    digitalWrite(PIN_TFT_CS, HIGH);
    SPI.endTransaction();
}

static void nv3007_send_color(lv_display_t *disp, const uint8_t *cmd, size_t cmd_size,
                               uint8_t *param, size_t param_size)
{
    SPI.beginTransaction(SPISettings(SPI_CLK_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_TFT_CS, LOW);
    digitalWrite(PIN_TFT_DC, LOW);
    SPI.transferBytes(cmd, nullptr, cmd_size);
    if (param && param_size > 0) {
        digitalWrite(PIN_TFT_DC, HIGH);
        SPI.transferBytes(param, nullptr, param_size);
    }
    digitalWrite(PIN_TFT_CS, HIGH);
    SPI.endTransaction();
    lv_display_flush_ready(disp);
}

// ── LVGL draw buffers ─────────────────────────────────────────────────────────
// After LV_DISPLAY_ROTATION_270 the logical display is 428 wide × 142 tall.
static const uint32_t LV_BUF_ROWS  = 40;
static const uint32_t LV_BUF_BYTES = 428 * LV_BUF_ROWS * sizeof(uint16_t);
static uint8_t *lv_buf1 = nullptr;
static uint8_t *lv_buf2 = nullptr;

static uint32_t lv_tick_cb() { return (uint32_t)millis(); }

static void adjust_home_led(bool front_led, int32_t delta) {
    if (delta == 0) {
        return;
    }

    // Home-screen adjustments are transient (RAM/live LED state only) — never
    // persisted to NVS, and never seeded from it. Base level comes from the
    // live LED manager state (which always boots at 0/off).
    const uint8_t current_brightness = front_led ? led_manager_get_front() : led_manager_get_back();
    const bool was_enabled = front_led ? led_manager_is_front_on() : led_manager_is_back_on();

    static constexpr int32_t LEVEL_COUNT = 12;
    static constexpr int32_t LEVEL_MAX = LEVEL_COUNT - 1;

    const int32_t current_level = (int32_t)(((uint32_t)current_brightness * LEVEL_MAX + 127U) / 255U);
    int32_t next_level = current_level + delta;
    if (next_level < 0) next_level = 0;
    if (next_level > LEVEL_MAX) next_level = LEVEL_MAX;

    const int32_t next = (int32_t)((next_level * 255 + (LEVEL_MAX / 2)) / LEVEL_MAX);

    if (front_led) {
        led_manager_set_front((uint8_t)next);
        if (next > 0 && !was_enabled) {
            led_manager_toggle_front();
        } else if (next == 0 && was_enabled) {
            led_manager_toggle_front();
        }
    } else {
        led_manager_set_back((uint8_t)next);
        if (next > 0 && !was_enabled) {
            led_manager_toggle_back();
        } else if (next == 0 && was_enabled) {
            led_manager_toggle_back();
        }
    }
}


void setup() {
    Serial.begin(115200);
    delay(500);

    // 1. NVS — before anything reads settings
    storage_manager_init();

    // 2. Audio + SD — initialises the shared SPI bus (calls SPI.begin internally)
    audio_manager_set_volume(storage_manager_get().alarm_volume);
    audio_manager_init();

    // 3. Display GPIO + reset
    pinMode(PIN_TFT_DC,  OUTPUT);
    pinMode(PIN_TFT_CS,  OUTPUT);
    pinMode(PIN_TFT_RES, OUTPUT);
    pinMode(PIN_SD_CS,   OUTPUT);

    // Keep SD deselected while talking to the display on shared SPI.
    digitalWrite(PIN_SD_CS, HIGH);
    digitalWrite(PIN_TFT_CS, HIGH);

    digitalWrite(PIN_TFT_RES, HIGH);
    delay(100);
    digitalWrite(PIN_TFT_RES, LOW);
    delay(120);
    digitalWrite(PIN_TFT_RES, HIGH);
    delay(120);

    display_backlight_init();
    brightness_manager_init();

    // 4. LVGL
    lv_init();
    lv_tick_set_cb(lv_tick_cb);

    lv_buf1 = (uint8_t *)heap_caps_malloc(LV_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_buf2 = (uint8_t *)heap_caps_malloc(LV_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!lv_buf1 || !lv_buf2) {
        heap_caps_free(lv_buf1); heap_caps_free(lv_buf2);
        lv_buf1 = (uint8_t *)malloc(LV_BUF_BYTES);
        lv_buf2 = (uint8_t *)malloc(LV_BUF_BYTES);
    }

    // NV3007: native portrait 142×428, 14px y-gap, rotated 270° → landscape 428×142
    lv_display_t *lv_disp = lv_nv3007_create(142, 428, LV_LCD_FLAG_NONE,
                                               nv3007_send_cmd, nv3007_send_color);
    lv_nv3007_set_gap(lv_disp, NV3007_GAP_X, NV3007_GAP_Y);
    lv_display_set_rotation(lv_disp, LV_DISPLAY_ROTATION_90);
#if NV3007_COLOR_SWAPPED
    lv_display_set_color_format(lv_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
#else
    lv_display_set_color_format(lv_disp, LV_COLOR_FORMAT_RGB565);
#endif
#if NV3007_USE_INVERT
    lv_nv3007_set_invert(lv_disp, true);
#endif
    lv_display_set_buffers(lv_disp, lv_buf1, lv_buf2, LV_BUF_BYTES,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    Serial.printf("Display: gap=(%u,%u), invert=%d, swapped=%d\n",
                  (unsigned)NV3007_GAP_X, (unsigned)NV3007_GAP_Y,
                  (int)NV3007_USE_INVERT, (int)NV3007_COLOR_SWAPPED);

    // 5. I2C peripherals
    rtc_manager_init();
    sensor_manager_init();

    // 6. LEDs — always boot off; only hue/sat colours are restored from NVS
    {
        AppSettings &s = storage_manager_get();
        led_manager_init(0, 0,
                         false, false,
                         s.led1_hue, s.led1_sat,
                         s.led2_hue, s.led2_sat);
    }

    // 7. Input
    input_manager_init();

    // 8. Main screen
    ui_main_screen_init();
    settings_menu_init(ui_main_screen_get_screen());
    alarm_manager_init();
    lv_screen_load(ui_main_screen_get_screen());
    ui_main_screen_update();

    Serial.println("Boot OK");
}

#if AUDIO_DEBUG_TIMING
static uint32_t dbg_last_loop_start_ms = 0;
static void debug_log_lv_task_handler() {
    const uint32_t start_us = micros();
    lv_task_handler();
    const uint32_t duration_us = micros() - start_us;
    if (duration_us > 8000) {
        Serial.printf("[lv_task_handler] took %luus\n", (unsigned long)duration_us);
    }
}
#endif

void loop() {
#if AUDIO_DEBUG_TIMING
    {
        const uint32_t now_ms = millis();
        const uint32_t gap_ms = now_ms - dbg_last_loop_start_ms;
        if (gap_ms > 15) {
            Serial.printf("[loop] gap=%lums\n", (unsigned long)gap_ms);
        }
        dbg_last_loop_start_ms = now_ms;
    }
#endif
    input_manager_update();
    audio_manager_loop();

    static int32_t last_enc1 = 0;
    static int32_t last_enc2 = 0;

    const int32_t enc1_now = input_manager_get_count(ENC1);
    const int32_t enc2_now = input_manager_get_count(ENC2);
    const int32_t enc1_delta = enc1_now - last_enc1;
    const int32_t enc2_delta = enc2_now - last_enc2;
    last_enc1 = enc1_now;
    last_enc2 = enc2_now;

    static bool hold_open_latched = false;
    static bool hold_open_armed = false;
    const bool enc1_held = input_manager_button_held(ENC1);
    const bool enc2_held = input_manager_button_held(ENC2);
    const bool enc1_pressed = input_manager_button_pressed(ENC1);
    const bool enc2_pressed = input_manager_button_pressed(ENC2);
    const bool user_input = (enc1_delta != 0) || (enc2_delta != 0) || enc1_pressed || enc2_pressed;
    if (user_input) {
        led_manager_note_user_input();
    }

    static uint32_t last_alarm_check_ms = 0;
    if (millis() - last_alarm_check_ms >= 1000) {
        last_alarm_check_ms = millis();
        alarm_manager_check_trigger();
    }

    if (alarm_manager_is_alarm_active() || alarm_manager_is_alarm_screen_visible()) {
        // Disarm home-screen hold-to-open so dismissing the alarm (which may
        // involve holding an encoder) can't immediately trigger a menu open
        // once the alarm window closes. Requires releasing both encoders
        // before hold-to-open can be armed again.
        hold_open_armed = false;
        hold_open_latched = true;
        alarm_manager_update(enc1_held, enc2_held);
        brightness_manager_update();
#if AUDIO_DEBUG_TIMING
        debug_log_lv_task_handler();
#else
        lv_task_handler();
#endif
        delay(5);
        return;
    }

    if (settings_menu_is_home()) {
        if (!hold_open_armed && !enc1_held && !enc2_held) {
            hold_open_armed = true;
        }

        if (user_input) {
            brightness_manager_note_home_input();
        }

        if (enc1_delta != 0) {
            adjust_home_led(true, enc1_delta);
        }

        if (enc2_delta != 0) {
            adjust_home_led(false, enc2_delta);
        }

        const bool enc1_hold_ready = enc1_held && input_manager_button_hold_ms(ENC1) >= 500;
        const bool enc2_hold_ready = enc2_held && input_manager_button_hold_ms(ENC2) >= 500;

        if (hold_open_armed && !hold_open_latched && (enc1_hold_ready || enc2_hold_ready)) {
            // Re-check alarm state at the action point to avoid opening menus
            // during alarm state transitions where screen visibility can lag.
            if (!alarm_manager_is_alarm_active() && !alarm_manager_is_alarm_screen_visible()) {
                if (enc1_hold_ready) {
                    settings_menu_open_main();
                } else {
                    settings_menu_open_quick_alarm();
                }
            }
            hold_open_latched = true;

        }
    } else {
        settings_menu_handle_inputs(
            enc1_delta,
            enc2_delta,
            enc1_pressed,
            enc2_pressed);
    }

    brightness_manager_update();
    alarm_manager_update(enc1_held, enc2_held);
    led_manager_update();

    if (hold_open_latched && !enc1_held && !enc2_held) {
        hold_open_latched = false;
    }

    static uint32_t last_sec = 0;
    if (settings_menu_is_home() && (millis() - last_sec >= 1000)) {
        last_sec = millis();
        ui_main_screen_update();
    }

#if AUDIO_DEBUG_TIMING
    debug_log_lv_task_handler();
#else
    lv_task_handler();
#endif
    delay(5);
}
