#include "manager_led.h"
#include "manager_storage.h"
#include "pins_config.h"
#include <Arduino.h>
#include <LiteLED.h>
#include <math.h>

// Two independent LiteLED (RMT driver) instances, each pinned to its own
// explicit RMT channel — avoids the shared/second-channel init bug seen with
// Adafruit_NeoPixel where the second strip's RMT channel silently failed to
// transmit (both strips defaulted to auto-assigned channels there).
// NOTE: RMT_CHANNEL_0 was confirmed dead on this board (front output never
// transmitted regardless of which GPIO/physical strip was wired to the
// "front" role) — swapped to RMT_CHANNEL_2 to avoid it. Keep off channel 0.
static LiteLED strip_front(LED_STRIP_WS2812, false, RMT_CHANNEL_2);
static LiteLED strip_back(LED_STRIP_WS2812, false, RMT_CHANNEL_1);

static uint8_t  front_brightness = 0;
static uint8_t  back_brightness  = 0;
static bool     front_on         = false;
static bool     back_on          = false;
static uint16_t front_hue        = 0;
static uint8_t  front_sat        = 100;
static uint16_t back_hue         = 0;
static uint8_t  back_sat         = 100;
static uint32_t last_user_input_ms = 0;
static uint32_t fade_start_ms = 0;
static uint8_t fade_front_start = 0;
static uint8_t fade_back_start = 0;
static bool fade_active = false;
static bool auto_off_expired = false;

static constexpr uint32_t LED_FADE_DURATION_MS = 1000U;

static uint8_t gamma_correct(uint8_t brightness) {
    if (brightness == 0) return 0;

    // Clamping the gamma curve to a floor left a wide dead band (several
    // indents all rounding to the same floor value) before it ever exceeded
    // it. Instead, rescale the curve's output range to [MIN_VISIBLE, 255] so
    // every nonzero input maps to a distinct, monotonically increasing value.
    static constexpr float MIN_VISIBLE = 0.0f;

    const float normalized = (float)brightness / 255.0f;
    float corrected = MIN_VISIBLE + powf(normalized, 2.2f) * (255.0f - MIN_VISIBLE);
    if (corrected > 255.0f) corrected = 255.0f;
    return (uint8_t)lroundf(corrected);
}

// Converts hue (0-360 deg) / sat (0-100 %) / gamma-corrected value (0-255)
// into a packed 0xRRGGBB colour, replacing Adafruit_NeoPixel::ColorHSV+gamma32.
static crgb_t hsv_color(uint16_t hue_deg, uint8_t sat_pct, uint8_t value) {
    const uint8_t v = gamma_correct(value);
    if (sat_pct == 0) return ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;

    const float h = fmodf((float)(hue_deg % 360), 360.0f) / 60.0f;
    const float s = sat_pct / 100.0f;
    const float vf = v / 255.0f;
    const int   i  = (int)h;
    const float f  = h - i;
    const float p  = vf * (1.0f - s);
    const float q  = vf * (1.0f - s * f);
    const float t  = vf * (1.0f - s * (1.0f - f));
    float r, g, b;
    switch (i % 6) {
        case 0:  r = vf; g = t;  b = p;  break;
        case 1:  r = q;  g = vf; b = p;  break;
        case 2:  r = p;  g = vf; b = t;  break;
        case 3:  r = p;  g = q;  b = vf; break;
        case 4:  r = t;  g = p;  b = vf; break;
        default: r = vf; g = p;  b = q;  break;
    }
    return ((uint32_t)lroundf(r * 255.0f) << 16) |
           ((uint32_t)lroundf(g * 255.0f) << 8) |
           (uint32_t)lroundf(b * 255.0f);
}

static void render(LiteLED &strip, bool on, uint8_t brightness, uint16_t hue, uint8_t sat) {
    if (on) {
        strip.fill(hsv_color(hue, sat, brightness), true);
    } else {
        strip.clear(true);
    }
}

static void apply_front() {
    render(strip_front, front_on, front_brightness, front_hue, front_sat);
}

static void apply_back() {
    render(strip_back, back_on, back_brightness, back_hue, back_sat);
}

void led_manager_init(uint8_t init_front_brightness, uint8_t init_back_brightness,
                      bool init_front_on, bool init_back_on,
                      uint16_t init_front_hue, uint8_t init_front_sat,
                      uint16_t init_back_hue, uint8_t init_back_sat) {
    front_brightness = init_front_brightness;
    back_brightness  = init_back_brightness;
    front_on         = init_front_on;
    back_on          = init_back_on;
    front_hue        = init_front_hue;
    front_sat        = init_front_sat;
    back_hue         = init_back_hue;
    back_sat         = init_back_sat;

    esp_err_t err_front = strip_front.begin(PIN_LED_FRONT, LED_STRIP_COUNT);
    esp_err_t err_back  = strip_back.begin(PIN_LED_BACK, LED_STRIP_COUNT);
    if (err_front != ESP_OK) {
        Serial.printf("LED: front strip init failed: %s\n", esp_err_to_name(err_front));
    }
    if (err_back != ESP_OK) {
        Serial.printf("LED: back strip init failed: %s\n", esp_err_to_name(err_back));
    }

    apply_front();
    apply_back();

    Serial.println("LED: OK");
}

void led_manager_note_user_input() {
    last_user_input_ms = millis();
    fade_active = false;
    auto_off_expired = false;
}

void led_manager_update() {
    const uint16_t timeout_min = storage_manager_get().auto_led_off_min;
    if (timeout_min == 0 || auto_off_expired) return;

    const uint32_t now = millis();
    if (!fade_active) {
        const uint32_t timeout_ms = (uint32_t)timeout_min * 60U * 1000U;
        if ((uint32_t)(now - last_user_input_ms) < timeout_ms) return;
        if (!front_on && !back_on) {
            auto_off_expired = true;
            return;
        }
        fade_start_ms = now;
        fade_front_start = front_brightness;
        fade_back_start = back_brightness;
        fade_active = true;
    }

    const uint32_t elapsed = now - fade_start_ms;
    if (elapsed >= LED_FADE_DURATION_MS) {
        front_brightness = 0;
        back_brightness = 0;
        front_on = false;
        back_on = false;
        apply_front();
        apply_back();
        fade_active = false;
        auto_off_expired = true;
        return;
    }

    const uint32_t remaining = LED_FADE_DURATION_MS - elapsed;
    if (front_on) {
        front_brightness = (uint8_t)(((uint32_t)fade_front_start * remaining) / LED_FADE_DURATION_MS);
        apply_front();
    }
    if (back_on) {
        back_brightness = (uint8_t)(((uint32_t)fade_back_start * remaining) / LED_FADE_DURATION_MS);
        apply_back();
    }
}


void led_manager_set_front(uint8_t brightness) {
    front_brightness = brightness;
    if (front_on) apply_front();
}

void led_manager_set_back(uint8_t brightness) {
    back_brightness = brightness;
    if (back_on) apply_back();
}

uint8_t led_manager_get_front() { return front_brightness; }
uint8_t led_manager_get_back()  { return back_brightness;  }

void led_manager_toggle_front() {
    front_on = !front_on;
    apply_front();
}

void led_manager_toggle_back() {
    back_on = !back_on;
    apply_back();
}

bool led_manager_is_front_on() { return front_on; }
bool led_manager_is_back_on()  { return back_on;  }

void led_manager_set_hue_sat_front(uint16_t hue, uint8_t sat) {
    front_hue = hue;
    front_sat = sat;
    apply_front();
}

void led_manager_set_hue_sat_back(uint16_t hue, uint8_t sat) {
    back_hue = hue;
    back_sat = sat;
    apply_back();
}

uint16_t led_manager_get_hue_front() { return front_hue; }
uint8_t  led_manager_get_sat_front() { return front_sat; }
uint16_t led_manager_get_hue_back()  { return back_hue;  }
uint8_t  led_manager_get_sat_back()  { return back_sat;  }

void led_manager_set_hsv(LedStrip strip, uint16_t hue, uint8_t sat, uint8_t brightness) {
    if (strip == LED_STRIP_FRONT) {
        front_hue = hue;
        front_sat = sat;
        front_brightness = brightness;
        front_on = brightness > 0;
        apply_front();
    } else {
        back_hue = hue;
        back_sat = sat;
        back_brightness = brightness;
        back_on = brightness > 0;
        apply_back();
    }
}
