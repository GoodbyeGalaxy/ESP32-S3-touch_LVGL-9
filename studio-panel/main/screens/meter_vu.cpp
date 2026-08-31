// meter_vu.cpp — Dedicated VU Meter screen.
// Two full-size analog VU panels (L + R) with gold needle, peak hold, clip LED.
// Uses MeterEngine (VU 300ms ballistics) + SkinVU (LVGL draw-callback rendering).
// 30 Hz timer — matches audio packet rate from sender.

#include "meter_vu.h"
#include "metering_hub.h"
#include "meter_engine.h"
#include "skin_vu.h"
#include "theme.h"
#include "screens/touch_nav.h"
#include "screens/statusbar.h"
#include "screens/foot.h"
#include "screens/settings_overlay.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG __attribute__((unused)) = "meter_vu";

// ── State ─────────────────────────────────────────────────────────────────────

struct VuScreenState {
    lv_timer_t  *timer  = nullptr;
    MeterEngine *engine = nullptr;
    SkinVU      *skin   = nullptr;
    uint32_t     last_ms = 0;
    int vu_ref_sel = 0;  // 0=-18 dBFS, 1=-20 dBFS
};

// ── Timer callback ────────────────────────────────────────────────────────────

static void vu_timer_cb(lv_timer_t *t)
{
    VuScreenState *st = static_cast<VuScreenState *>(lv_timer_get_user_data(t));
    if (!st) return;

    uint32_t now = lv_tick_get();
    float dt = (st->last_ms == 0) ? (1.0f / 30.0f)
                                   : (float)(now - st->last_ms) / 1000.0f;
    st->last_ms = now;

    const MeterReadings &r = st->engine->tick(dt);
    st->skin->update(r);
}

// ── Screen lifecycle ──────────────────────────────────────────────────────────

static void on_vu_delete(lv_event_t *e)
{
    VuScreenState *st = static_cast<VuScreenState *>(lv_event_get_user_data(e));
    if (!st) return;
    if (st->timer)  lv_timer_delete(st->timer);
    if (st->skin)   { st->skin->destroy(); delete st->skin; }
    if (st->engine) delete st->engine;
    delete st;
}

lv_obj_t *meter_vu_screen_create()
{
    VuScreenState *st = new VuScreenState{};
    st->engine = new MeterEngine{};
    st->skin   = new SkinVU{};

    lv_obj_t *scr = theme_make_screen();
    lv_obj_add_event_cb(scr, on_vu_delete, LV_EVENT_DELETE, st);
    statusbar_set_screen_name("VU METER");

    // Skin creates its LVGL panels (VU needle panels) on the screen
    st->skin->create(scr);

    lv_obj_t *right_zone = foot_create_hub_back(scr);

    static const SettingOption vu_ref_opts[] = { {"-18 dBFS"}, {"-20 dBFS"} };
    auto *vu_items = new SettingItem[1];
    vu_items[0] = { "0 VU Reference", vu_ref_opts, 2, &st->vu_ref_sel };
    settings_btn_create(right_zone, scr, vu_items, 1);

    touch_nav_attach_2d(scr, [](int dir_h, int dir_v, void *) {
        if (dir_h == -1) { lv_async_call([](void *){ metering_hub_next(); }, nullptr); return; }
        if (dir_h ==  1) { lv_async_call([](void *){ metering_hub_prev(); }, nullptr); return; }
        if (dir_v ==  1) { lv_async_call([](void *){ metering_hub_exit(); }, nullptr); return; }
    }, nullptr);

    // 30 Hz — matches audio packet rate
    st->timer = lv_timer_create(vu_timer_cb, 33, st);

    return scr;
}
