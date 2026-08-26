#pragma once
#include "meter_skin.h"

class SkinVU final : public MeterSkin {
public:
    void create(lv_obj_t *parent) override;
    void update(const MeterReadings &r) override;
    void destroy() override;
    const char *name() const override { return "STUDIO"; }

private:
    // State passed as user_data into the LVGL draw callback.
    // Two separate instances — one per channel — so each panel is self-contained.
    struct PanelData {
        float       db               = -20.0f;
        const char *label            = nullptr;   // "L" or "R"
        bool        peak             = false;     // true when db >= 0 dBVU (drives clip LED)
        float       peak_hold_db     = -20.0f;   // position of the hold marker
        int         peak_hold_frames = 0;         // countdown at 30 Hz; 0 = marker hidden
    };

    PanelData   left_data_;
    PanelData   right_data_;
    lv_obj_t   *left_panel_  = nullptr;
    lv_obj_t   *right_panel_ = nullptr;

    // Helper — builds one lv_obj_t panel with the draw callback attached.
    lv_obj_t *create_panel(lv_obj_t *parent, lv_coord_t x, PanelData *data);

    // Single static draw callback shared by both panels.
    static void panel_draw_cb(lv_event_t *e);
};
