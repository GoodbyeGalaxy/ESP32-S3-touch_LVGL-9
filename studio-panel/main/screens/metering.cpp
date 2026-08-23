#include "metering.h"
#include "meter_engine.h"
#include "meter_skin.h"
#include "skin_digital.h"
#include "theme.h"
#include "screens/home.h"
#include "lvgl.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <memory>

static const char *TAG = "metering";

struct MeteringScreenData {
    MeterEngine              engine;
    std::unique_ptr<MeterSkin> skin;

    lv_obj_t   *mode_label = nullptr;
    lv_timer_t *timer      = nullptr;

    // BOOT button
    volatile int64_t btn_press_us = 0;
    volatile bool    btn_event    = false;
    volatile bool    btn_long     = false;
};

// BOOT ISR (IRAM_ATTR)
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

static void metering_timer_cb(lv_timer_t *timer)
{
    auto *d = static_cast<MeteringScreenData*>(lv_timer_get_user_data(timer));

    if (d->btn_event) {
        d->btn_event = false;
        if (!d->btn_long) {
            auto *sd = static_cast<SkinDigital*>(d->skin.get());
            auto next = static_cast<SkinDigital::DigitalMode>(
                (static_cast<uint8_t>(sd->mode()) + 1) %
                static_cast<uint8_t>(SkinDigital::DigitalMode::COUNT));
            sd->setMode(next);
            d->engine.reset();  // clear ballistic history on mode change
        }
        d->btn_long = false;
    }

    constexpr float DT = 0.033f;
    const MeterReadings &r = d->engine.tick(DT);
    d->skin->update(r);
}

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
    auto *d = new MeteringScreenData{};
    d->skin = std::make_unique<SkinDigital>();

    lv_obj_t *scr = theme_make_screen();
    lv_obj_add_event_cb(scr, on_screen_delete, LV_EVENT_DELETE, d);

    d->skin->create(scr);

    // Back button
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
