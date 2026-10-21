#pragma once

#include <stdint.h>
#include <stdbool.h>

enum LedStrip : uint8_t {
    LED_STRIP_FRONT = 0,
    LED_STRIP_BACK  = 1,
};

void    led_manager_init(uint8_t front_brightness, uint8_t back_brightness,
                         bool front_on, bool back_on,
                         uint16_t front_hue, uint8_t front_sat,
                         uint16_t back_hue, uint8_t back_sat);

// Call for every detected user input and from the main loop to service Auto LED Off.
void    led_manager_note_user_input();
void    led_manager_update();

// Brightness: 0–255
void    led_manager_set_front(uint8_t brightness);
void    led_manager_set_back(uint8_t brightness);

uint8_t led_manager_get_front();
uint8_t led_manager_get_back();

void    led_manager_toggle_front();
void    led_manager_toggle_back();

bool    led_manager_is_front_on();
bool    led_manager_is_back_on();

// Hue: 0–360 degrees, Saturation: 0–100 %. Re-renders immediately.
void    led_manager_set_hue_sat_front(uint16_t hue, uint8_t sat);
void    led_manager_set_hue_sat_back(uint16_t hue, uint8_t sat);

uint16_t led_manager_get_hue_front();
uint8_t  led_manager_get_sat_front();
uint16_t led_manager_get_hue_back();
uint8_t  led_manager_get_sat_back();

// Direct HSV + brightness render for one strip (e.g. alarm sunrise ramp).
void    led_manager_set_hsv(LedStrip strip, uint16_t hue, uint8_t sat, uint8_t brightness);
