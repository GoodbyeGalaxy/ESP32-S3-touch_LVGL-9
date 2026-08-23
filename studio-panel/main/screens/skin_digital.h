#pragma once
#include "meter_skin.h"

class SkinDigital final : public MeterSkin {
public:
    void create(lv_obj_t *parent) override;
    void update(const MeterReadings &r) override;
    void destroy() override;
    const char *name() const override { return "DIGITAL"; }

private:
    // Goniometer phosphor model
    void      *gonio_buf_    = nullptr;   // RGB565 canvas pixels (PSRAM, 250×250×2)
    uint8_t   *brightness_   = nullptr;   // per-pixel brightness 0..255 (DRAM preferred)
    int        gonio_frame_  = 0;         // frame counter for 15Hz decay throttle

    // Bar widget data (passed as user_data to draw_cb)
    struct BarData { float rms_db; float peak_hold_db; };
    BarData bar_l_data_{};
    BarData bar_r_data_{};

    // LVGL widget handles
    lv_obj_t *bar_l_     = nullptr;
    lv_obj_t *bar_r_     = nullptr;
    lv_obj_t *gonio_     = nullptr;
    lv_obj_t *history_   = nullptr;
    lv_obj_t *num_i_     = nullptr;
    lv_obj_t *num_s_     = nullptr;
    lv_obj_t *num_m_     = nullptr;
    lv_obj_t *num_peak_  = nullptr;
    lv_obj_t *scale_col_ = nullptr;
    lv_obj_t *spec_strip_= nullptr;

    // Spectral band magnitudes (copied from MeterReadings each tick)
    float spec_bands_[6]{};

    // Cached history snapshot for draw_cb (history ring buffer lives in MeterEngine)
    float hist_snap_[60]{};
    int   hist_head_snap_ = 0;

    // Private helpers
    void create_bars(lv_obj_t *parent);
    void create_gonio(lv_obj_t *parent);
    void create_history(lv_obj_t *parent);
    void create_numerics(lv_obj_t *parent);
    void create_scale(lv_obj_t *parent);
    void create_spec_strip(lv_obj_t *parent);

    void update_gonio(const MeterReadings &r);
    void render_phosphor();

    // Static LVGL draw callbacks (take user_data pointer)
    static void bar_draw_cb(lv_event_t *e);
    static void history_draw_cb(lv_event_t *e);
    static void spec_strip_draw_cb(lv_event_t *e);
};
