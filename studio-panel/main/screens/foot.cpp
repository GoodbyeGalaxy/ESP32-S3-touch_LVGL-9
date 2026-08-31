#include "foot.h"
#include "theme.h"
#include "screens/home.h"
#include "screens/nav_controller.h"
#include "screens/metering_hub.h"
#include "lvgl.h"

static void on_home(lv_event_t *e)
{
    (void)e;
    nav_go_home();
}

static void on_hub_back(lv_event_t *e)
{
    (void)e;
    metering_hub_exit();
}

// IN: scr. OUT: foot bar on scr + ← Home button. Returns right_zone for screen actions.
lv_obj_t *foot_create(lv_obj_t *scr)
{
    lv_obj_t *foot = lv_obj_create(scr);
    lv_obj_remove_style_all(foot);
    lv_obj_set_size(foot, LV_HOR_RES, THEME_FOOT_H);
    lv_obj_set_pos(foot, 0, THEME_FOOT_Y);
    lv_obj_set_style_bg_color(foot, THEME_FOOT_BG, 0);
    lv_obj_set_style_bg_opa(foot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(foot, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *home_btn = lv_btn_create(foot);
    lv_obj_set_size(home_btn, 90, 40);
    lv_obj_align(home_btn, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_color(home_btn, THEME_BG_CARD, 0);
    lv_obj_set_style_bg_color(home_btn, THEME_BG_CARD_HOVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(home_btn, on_home, LV_EVENT_CLICKED, nullptr);
    theme_apply_glow(home_btn);
    lv_obj_t *lbl = lv_label_create(home_btn);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT " Home");
    lv_obj_set_style_text_color(lbl, THEME_TEXT_PRIMARY, 0);
    lv_obj_center(lbl);

    // Right zone — caller adds screen-specific buttons here
    lv_obj_t *right = lv_obj_create(foot);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, 400, THEME_FOOT_H);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    return right;
}

lv_obj_t *foot_create_hub_back(lv_obj_t *scr)
{
    lv_obj_t *foot = lv_obj_create(scr);
    lv_obj_remove_style_all(foot);
    lv_obj_set_size(foot, LV_HOR_RES, THEME_FOOT_H);
    lv_obj_set_pos(foot, 0, THEME_FOOT_Y);
    lv_obj_set_style_bg_color(foot, THEME_FOOT_BG, 0);
    lv_obj_set_style_bg_opa(foot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(foot, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back_btn = lv_btn_create(foot);
    lv_obj_set_size(back_btn, 90, 40);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_color(back_btn, THEME_BG_CARD, 0);
    lv_obj_set_style_bg_color(back_btn, THEME_BG_CARD_HOVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(back_btn, on_hub_back, LV_EVENT_CLICKED, nullptr);
    theme_apply_glow(back_btn);
    lv_obj_t *lbl = lv_label_create(back_btn);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT " back");
    lv_obj_set_style_text_color(lbl, THEME_TEXT_PRIMARY, 0);
    lv_obj_center(lbl);

    lv_obj_t *right = lv_obj_create(foot);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, 400, THEME_FOOT_H);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    return right;
}
