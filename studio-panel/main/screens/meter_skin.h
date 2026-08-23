#pragma once
#include "lvgl.h"
#include "meter_engine.h"

struct MeterSkin {
    virtual ~MeterSkin() = default;
    // Called once after screen object exists. Skin creates its LVGL children on parent.
    virtual void create(lv_obj_t *parent) = 0;
    // Called at 30Hz from LVGL timer callback (already locked). Update widgets from r.
    virtual void update(const MeterReadings &r) = 0;
    // Called before screen is deleted. Skin must NOT delete parent — only its own allocs.
    virtual void destroy() = 0;
    // Short human-readable name shown in mode label.
    virtual const char *name() const = 0;
};
