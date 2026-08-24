#include "metering.h"
#include "meter_engine.h"
#include "meter_skin.h"
#include "skin_digital.h"
#include "skin_vu.h"
#include "theme.h"
#include "screens/home.h"
#include "screens/touch_nav.h"
#include "screens/spectrum.h"
#include "lvgl.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <memory>

static const char *TAG __attribute__((unused)) = "metering";

// Registered skins in cycling order.
static constexpr int SKIN_COUNT = 2;

struct MeteringScreenData {
    MeterEngine              engine;
    std::unique_ptr<MeterSkin> skin;
    int                      skin_idx       = 0;
    lv_obj_t                *skin_container = nullptr;
    lv_obj_t                *skin_btn_lbl   = nullptr;  // label on skin-switch button

    lv_timer_t *timer = nullptr;

    // BOOT button (mode cycling within SkinDigital only)
    volatile int64_t btn_press_us = 0;
    volatile bool    btn_event    = false;
    volatile bool    btn_long     = false;
};

// ── Skin factory ──────────────────────────────────────────────────────────────

static std::unique_ptr<MeterSkin> make_skin(int idx)
{
    switch (idx) {
        case 1:  return std::make_unique<SkinVU>();
        default: return std::make_unique<SkinDigital>();
    }
}

// ── BOOT ISR ─────────────────────────────────────────────────────────────────

static void IRAM_ATTR metering_boot_isr(void *arg)
{
    auto *d = static_cast<MeteringScreenData*>(arg);
    int level = gpio_get_level(GPIO_NUM_0);
    int64_t now = esp_timer_get_time();
    if (level == 0) { d->btn_press_us = now; }
    else { d->btn_long = (now - d->btn_press_us) >= 1000000; d->btn_event = true; }
}

static void metering_boot_init(MeteringScreenData *d)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&cfg);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(GPIO_NUM_0, metering_boot_isr, d);
}

// ── Timer callback ────────────────────────────────────────────────────────────

static void metering_timer_cb(lv_timer_t *timer)
{
    auto *d = static_cast<MeteringScreenData*>(lv_timer_get_user_data(timer));

    if (d->btn_event) {
        d->btn_event = false;
        if (!d->btn_long) {
            // Short press → cycle mode within SkinDigital only
            if (d->skin_idx == 0) {
                auto *sd = static_cast<SkinDigital*>(d->skin.get());
                auto next = static_cast<SkinDigital::DigitalMode>(
                    (static_cast<uint8_t>(sd->mode()) + 1) %
                    static_cast<uint8_t>(SkinDigital::DigitalMode::COUNT));
                sd->setMode(next);
                d->engine.reset();
            }
        }
        d->btn_long = false;
    }

    constexpr float DT = 0.033f;
    const MeterReadings &r = d->engine.tick(DT);
    d->skin->update(r);
}

// ── Screen lifecycle ──────────────────────────────────────────────────────────

static void on_screen_delete(lv_event_t *e)
{
    auto *d = static_cast<MeteringScreenData*>(lv_event_get_user_data(e));
    gpio_isr_handler_remove(GPIO_NUM_0);
    if (d->timer) lv_timer_delete(d->timer);
    d->skin->destroy();
    delete d;
}

static void on_back(lv_event_t *e)
{
    lv_obj_t *home = home_screen_create();
    lv_screen_load_anim(home, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, true);
}

lv_obj_t *metering_screen_create()
{
    auto *d  = new MeteringScreenData{};
    d->skin  = make_skin(0);

    lv_obj_t *scr = theme_make_screen();
    lv_obj_add_event_cb(scr, on_screen_delete, LV_EVENT_DELETE, d);

    // Transparent full-screen container — lets lv_obj_clean() swap skins safely
    d->skin_container = lv_obj_create(scr);
    lv_obj_remove_style_all(d->skin_container);
    lv_obj_set_size(d->skin_container, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(d->skin_container, 0, 0);
    lv_obj_set_style_bg_opa(d->skin_container, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(d->skin_container, LV_OBJ_FLAG_SCROLLABLE);

    d->skin->create(d->skin_container);

    touch_nav_attach(d->skin_container, [](int dir, void *) {
        if (dir > 0) {
            lv_screen_load_anim(home_screen_create(), LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, true);
        } else {
            lv_screen_load_anim(spectrum_screen_create(), LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, true);
        }
    }, nullptr);

    // Skin-Switch Button (unten rechts) — Touch-Aktion, Sibling des skin_container
    {
        lv_obj_t *sbtn = lv_btn_create(scr);
        lv_obj_set_size(sbtn, 80, 44);
        lv_obj_align(sbtn, LV_ALIGN_BOTTOM_RIGHT, -16, -16);
        lv_obj_set_style_bg_color(sbtn, THEME_BG_CARD, 0);
        lv_obj_add_event_cb(sbtn, [](lv_event_t *e) {
            auto *d = static_cast<MeteringScreenData*>(lv_event_get_user_data(e));
            d->skin->destroy();
            lv_obj_clean(d->skin_container);
            d->skin_idx = (d->skin_idx + 1) % SKIN_COUNT;
            d->skin = make_skin(d->skin_idx);
            d->skin->create(d->skin_container);
            d->engine.reset();
            if (d->skin_btn_lbl) lv_label_set_text(d->skin_btn_lbl, d->skin->name());
        }, LV_EVENT_CLICKED, d);
        d->skin_btn_lbl = lv_label_create(sbtn);
        lv_label_set_text(d->skin_btn_lbl, d->skin->name());
        lv_obj_set_style_text_color(d->skin_btn_lbl, THEME_TEXT_PRIMARY, 0);
        lv_obj_center(d->skin_btn_lbl);
    }

    // Back button — sibling of skin_container, not affected by lv_obj_clean
    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 100, 44);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, 16, -16);
    lv_obj_set_style_bg_color(btn, THEME_BG_CARD, 0);
    lv_obj_add_event_cb(btn, on_back, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT " Home");
    lv_obj_set_style_text_color(lbl, THEME_TEXT_PRIMARY, 0);
    lv_obj_center(lbl);

    metering_boot_init(d);
    d->timer = lv_timer_create(metering_timer_cb, 33, d);

    return scr;
}
