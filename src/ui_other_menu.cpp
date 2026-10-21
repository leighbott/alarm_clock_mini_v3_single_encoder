#include "ui_other_menu.h"

#include "manager_led.h"
#include "manager_storage.h"

#include <lvgl.h>
#include <cstdio>

namespace {

static constexpr int DISP_W = 428;
static constexpr int DISP_H = 142;
static constexpr int HEADER_H = 34;
static constexpr int CONTENT_Y = 36;
static constexpr int CONTENT_H = DISP_H - CONTENT_Y;
static constexpr int TILE_COUNT = 4;
static constexpr int PAGE_DOT_W = 16;
static constexpr int CONTENT_PAD_X = 6;
static constexpr int CONTENT_GAP_X = 4;
static constexpr int MAIN_COL_START_X = CONTENT_PAD_X + PAGE_DOT_W;
static constexpr int MAIN_COL_AVAILABLE_W = DISP_W - MAIN_COL_START_X - CONTENT_PAD_X;
static constexpr int COL_W = (MAIN_COL_AVAILABLE_W - ((TILE_COUNT - 1) * CONTENT_GAP_X)) / TILE_COUNT;
static constexpr int VALUE_W = 90;
static constexpr int VALUE_H = 68;
static constexpr int VALUE_Y = 36;
static constexpr int SWATCH_SIZE = 28;
static constexpr uint8_t AUTO_OFF_OPTION_COUNT = 11;
static const uint16_t AUTO_OFF_OPTIONS[AUTO_OFF_OPTION_COUNT] = {
    10, 20, 30, 40, 50, 60, 120, 180, 240, 300, 360
};

static lv_obj_t *g_screen = nullptr;
static lv_obj_t *g_header_cancel_bg = nullptr;
static lv_obj_t *g_header_accept_bg = nullptr;
static lv_timer_t *g_header_flash_timer = nullptr;
static lv_timer_t *g_pending_action_timer = nullptr;
static UiOtherAction g_pending_action = UiOtherAction::NONE;
static UiOtherAction g_deferred_action = UiOtherAction::NONE;

static lv_obj_t *g_tiles[TILE_COUNT] = {nullptr, nullptr, nullptr, nullptr};
static lv_obj_t *g_swatches[2] = {nullptr, nullptr};
static lv_obj_t *g_value_labels[TILE_COUNT] = {nullptr, nullptr, nullptr, nullptr};
static uint8_t g_selected_tile = 0;

// Baseline captured when the Other menu is entered, restored on CANCEL.
static bool g_base_front_on = false;
static uint8_t g_base_front_brightness = 0;
static uint16_t g_base_front_hue = 0;
static uint8_t g_base_front_sat = 100;
static bool g_base_back_on = false;
static uint8_t g_base_back_brightness = 0;
static uint16_t g_base_back_hue = 0;
static uint8_t g_base_back_sat = 100;
static uint16_t g_base_auto_led_off_min = 0;

static void update_auto_off_value() {
    if (!g_value_labels[3]) return;
    const uint16_t minutes = storage_manager_get().auto_led_off_min;
    if (minutes < 60) {
        char value[8];
        std::snprintf(value, sizeof(value), "%um", (unsigned)minutes);
        lv_label_set_text(g_value_labels[3], value);
    } else {
        const uint16_t hours = minutes / 60;
        const uint16_t remainder = minutes % 60;
        if (remainder == 0) {
            char value[8];
            std::snprintf(value, sizeof(value), "%uhr", (unsigned)hours);
            lv_label_set_text(g_value_labels[3], value);
        } else {
            char value[16];
            std::snprintf(value, sizeof(value), "%uhr\\n%um", (unsigned)hours, (unsigned)remainder);
            lv_label_set_text(g_value_labels[3], value);
        }
    }
}

static void hide_header_flash() {
    if (g_header_cancel_bg) lv_obj_set_style_bg_opa(g_header_cancel_bg, LV_OPA_TRANSP, 0);
    if (g_header_accept_bg) lv_obj_set_style_bg_opa(g_header_accept_bg, LV_OPA_TRANSP, 0);
}

static void header_flash_timer_cb(lv_timer_t *timer) {
    (void)timer;
    hide_header_flash();
    g_header_flash_timer = nullptr;
}

static void trigger_header_flash(bool accept) {
    hide_header_flash();
    if (accept) {
        if (g_header_accept_bg) lv_obj_set_style_bg_opa(g_header_accept_bg, LV_OPA_COVER, 0);
    } else {
        if (g_header_cancel_bg) lv_obj_set_style_bg_opa(g_header_cancel_bg, LV_OPA_COVER, 0);
    }

    if (g_header_flash_timer) lv_timer_del(g_header_flash_timer);
    g_header_flash_timer = lv_timer_create(header_flash_timer_cb, 120, nullptr);
    lv_timer_set_repeat_count(g_header_flash_timer, 1);
}

static void pending_action_timer_cb(lv_timer_t *timer) {
    (void)timer;
    g_pending_action_timer = nullptr;
    g_pending_action = g_deferred_action;
    g_deferred_action = UiOtherAction::NONE;
}

static void queue_action_after_flash(UiOtherAction action) {
    g_deferred_action = action;
    if (g_pending_action_timer) lv_timer_del(g_pending_action_timer);
    g_pending_action_timer = lv_timer_create(pending_action_timer_cb, 120, nullptr);
    lv_timer_set_repeat_count(g_pending_action_timer, 1);
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
    lv_obj_set_clickable(lbl_cancel, false);
    lv_obj_set_click_focusable(lbl_cancel, false);

    lv_obj_t *lbl_title = lv_label_create(header);
    lv_label_set_text(lbl_title, title);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_white(), 0);
    lv_obj_align(lbl_title, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_clickable(lbl_title, false);
    lv_obj_set_click_focusable(lbl_title, false);

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
    lv_obj_set_clickable(lbl_accept, false);
    lv_obj_set_click_focusable(lbl_accept, false);

    g_header_cancel_bg = cancel_bg;
    g_header_accept_bg = accept_bg;
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

static lv_obj_t *create_column(lv_obj_t *parent, int x, uint8_t index, const char *title) {
    lv_obj_t *column = lv_obj_create(parent);
    lv_obj_set_size(column, COL_W, CONTENT_H);
    lv_obj_set_pos(column, x, 0);
    lv_obj_set_style_bg_opa(column, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(column, 0, 0);
    lv_obj_set_style_pad_all(column, 0, 0);
    lv_obj_set_scrollable(column, false);

    lv_obj_t *title_label = lv_label_create(column);
    lv_label_set_text(title_label, title);
    lv_obj_set_size(title_label, COL_W, 34);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title_label, lv_color_make(0xD0, 0xD0, 0xD0), 0);
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_clickable(title_label, false);
    lv_obj_set_click_focusable(title_label, false);

    g_tiles[index] = create_value_shell(column, 0);
    return g_tiles[index];
}

static void create_led_column(lv_obj_t *parent, int x, uint8_t index, const char *title,
                              lv_obj_t **out_swatch) {
    lv_obj_t *widget = create_column(parent, x, index, title);
    lv_obj_t *swatch = lv_obj_create(widget);
    lv_obj_set_size(swatch, SWATCH_SIZE, SWATCH_SIZE);
    lv_obj_set_style_radius(swatch, SWATCH_SIZE / 2, 0);
    lv_obj_set_style_border_width(swatch, 1, 0);
    lv_obj_set_style_border_color(swatch, lv_color_white(), 0);
    lv_obj_set_style_pad_all(swatch, 0, 0);
    lv_obj_set_scrollable(swatch, false);
    lv_obj_center(swatch);
    lv_obj_set_clickable(swatch, false);
    lv_obj_set_click_focusable(swatch, false);
    if (out_swatch) *out_swatch = swatch;
}

static void create_value_column(lv_obj_t *parent, int x, uint8_t index, const char *title,
                                const char *initial_value) {
    lv_obj_t *widget = create_column(parent, x, index, title);
    g_value_labels[index] = lv_label_create(widget);
    lv_label_set_text(g_value_labels[index], initial_value);
    lv_obj_set_style_text_font(g_value_labels[index], &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(g_value_labels[index], lv_color_white(), 0);
    lv_obj_set_width(g_value_labels[index], VALUE_W - 8);
    lv_obj_set_style_text_align(g_value_labels[index], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(g_value_labels[index]);
    lv_obj_set_clickable(g_value_labels[index], false);
    lv_obj_set_click_focusable(g_value_labels[index], false);
}

static void update_tile_focus() {
    for (uint8_t i = 0; i < TILE_COUNT; ++i) {
        if (!g_tiles[i]) continue;
        if (i == g_selected_tile) lv_obj_add_state(g_tiles[i], LV_STATE_FOCUSED);
        else lv_obj_clear_state(g_tiles[i], LV_STATE_FOCUSED);
    }
}

} // namespace

void ui_other_init() {
    if (g_screen) return;

    g_screen = lv_obj_create(nullptr);
    lv_obj_set_size(g_screen, DISP_W, DISP_H);
    lv_obj_set_style_pad_all(g_screen, 0, 0);
    lv_obj_set_style_border_width(g_screen, 0, 0);
    lv_obj_set_scrollable(g_screen, false);

    apply_header_base(g_screen, "Other");

    lv_obj_t *content = lv_obj_create(g_screen);
    lv_obj_set_size(content, DISP_W, CONTENT_H);
    lv_obj_set_pos(content, 0, CONTENT_Y);
    lv_obj_set_style_bg_color(content, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_scrollable(content, false);

    create_led_column(content, MAIN_COL_START_X, 0, "Front LED", &g_swatches[0]);
    create_led_column(content, MAIN_COL_START_X + COL_W + CONTENT_GAP_X, 1,
                      "Rear LED", &g_swatches[1]);
    create_value_column(content, MAIN_COL_START_X + 2 * (COL_W + CONTENT_GAP_X),
                       2, "Home Page", "Select");
    create_value_column(content, MAIN_COL_START_X + 3 * (COL_W + CONTENT_GAP_X),
                       3, "Auto LED Off", "");
    update_auto_off_value();

    g_selected_tile = 0;
    update_tile_focus();
    ui_other_refresh_previews();
}

lv_obj_t *ui_other_get_screen() {
    return g_screen;
}

void ui_other_refresh_previews() {
    if (g_swatches[0]) {
        lv_obj_set_style_bg_color(g_swatches[0],
                                  lv_color_hsv_to_rgb(led_manager_get_hue_front(), led_manager_get_sat_front(), 100), 0);
    }
    if (g_swatches[1]) {
        lv_obj_set_style_bg_color(g_swatches[1],
                                  lv_color_hsv_to_rgb(led_manager_get_hue_back(), led_manager_get_sat_back(), 100), 0);
    }
}

void ui_other_on_enter() {
    hide_header_flash();

    g_base_front_on = led_manager_is_front_on();
    g_base_front_brightness = led_manager_get_front();
    g_base_front_hue = led_manager_get_hue_front();
    g_base_front_sat = led_manager_get_sat_front();
    g_base_back_on = led_manager_is_back_on();
    g_base_back_brightness = led_manager_get_back();
    g_base_back_hue = led_manager_get_hue_back();
    g_base_back_sat = led_manager_get_sat_back();
    g_base_auto_led_off_min = storage_manager_get().auto_led_off_min;

    ui_other_refresh_previews();
    update_auto_off_value();
}

UiOtherAction ui_other_handle_inputs(int32_t enc1_delta,
                                     int32_t enc2_delta,
                                     bool enc1_pressed,
                                     bool enc2_pressed) {
    if (g_pending_action != UiOtherAction::NONE) {
        UiOtherAction out = g_pending_action;
        g_pending_action = UiOtherAction::NONE;
        return out;
    }

    if (g_pending_action_timer) {
        return UiOtherAction::NONE;
    }

    if (enc1_pressed) {
        // Undo any LED edits made while inside this menu.
        led_manager_set_hue_sat_front(g_base_front_hue, g_base_front_sat);
        led_manager_set_front(g_base_front_brightness);
        if (led_manager_is_front_on() != g_base_front_on) led_manager_toggle_front();

        led_manager_set_hue_sat_back(g_base_back_hue, g_base_back_sat);
        led_manager_set_back(g_base_back_brightness);
        if (led_manager_is_back_on() != g_base_back_on) led_manager_toggle_back();
        storage_manager_get().auto_led_off_min = g_base_auto_led_off_min;
        update_auto_off_value();

        trigger_header_flash(false);
        queue_action_after_flash(UiOtherAction::CANCEL);
        return UiOtherAction::NONE;
    }

    if (enc2_pressed) {
        AppSettings &s = storage_manager_get();
        s.led1_hue = led_manager_get_hue_front();
        s.led1_sat = led_manager_get_sat_front();
        s.led2_hue = led_manager_get_hue_back();
        s.led2_sat = led_manager_get_sat_back();
        storage_manager_save_leds();
        storage_manager_save_other();

        trigger_header_flash(true);
        queue_action_after_flash(UiOtherAction::ACCEPT);
        return UiOtherAction::NONE;
    }

    if (enc1_delta != 0) {
        const int8_t direction = (enc1_delta > 0) ? 1 : -1;
        int16_t next = (int16_t)g_selected_tile + direction;
        if (next < 0) next = TILE_COUNT - 1;
        if (next >= TILE_COUNT) next = 0;
        g_selected_tile = (uint8_t)next;
        update_tile_focus();
    }

    if (enc2_delta != 0) {
        if (g_selected_tile == 3) {
            int32_t option_index = 0;
            for (uint8_t i = 0; i < AUTO_OFF_OPTION_COUNT; ++i) {
                if (AUTO_OFF_OPTIONS[i] == storage_manager_get().auto_led_off_min) {
                    option_index = i;
                    break;
                }
            }
            option_index += enc2_delta;
            if (option_index < 0) option_index = 0;
            if (option_index >= AUTO_OFF_OPTION_COUNT) option_index = AUTO_OFF_OPTION_COUNT - 1;
            storage_manager_get().auto_led_off_min = AUTO_OFF_OPTIONS[(uint8_t)option_index];
            update_auto_off_value();
            return UiOtherAction::NONE;
        }
        switch (g_selected_tile) {
            case 0: return UiOtherAction::ENTER_LED1;
            case 1: return UiOtherAction::ENTER_LED2;
            case 2: return UiOtherAction::ENTER_HOME_PAGE;
            default: return UiOtherAction::NONE;
        }
    }

    return UiOtherAction::NONE;
}
