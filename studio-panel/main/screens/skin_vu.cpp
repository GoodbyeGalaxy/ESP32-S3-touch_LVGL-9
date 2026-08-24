#include "skin_vu.h"
#include "lvgl.h"
#include "esp_log.h"
#include <cmath>
#include <algorithm>

static const char *TAG = "SkinVU";

// ── Panel geometry ────────────────────────────────────────────────────────────

static constexpr lv_coord_t PANEL_W        = 340;
static constexpr lv_coord_t PANEL_H        = 340;
static constexpr lv_coord_t PANEL_GAP      = 10;
// Total width = 2*PANEL_W + PANEL_GAP = 690 → centred on 800px screen.
static constexpr lv_coord_t PANEL_LEFT_X   = (800 - 2 * PANEL_W - PANEL_GAP) / 2;
static constexpr lv_coord_t PANEL_RIGHT_X  = PANEL_LEFT_X + PANEL_W + PANEL_GAP;
static constexpr lv_coord_t PANEL_Y        = (480 - PANEL_H) / 2;

// ── Needle geometry ───────────────────────────────────────────────────────────

// Pivot sits near the bottom-centre of the panel.
static constexpr lv_coord_t PIVOT_X        = PANEL_W / 2;
static constexpr lv_coord_t PIVOT_Y        = PANEL_H - 30;
static constexpr int        NEEDLE_LEN     = 250;  // tip-to-pivot distance in pixels
static constexpr int        NEEDLE_W       = 3;    // line stroke width

// ── Scale arc geometry ────────────────────────────────────────────────────────

static constexpr int  SCALE_RADIUS        = 230;  // arc radius from pivot
static constexpr int  TICK_MAJOR_LEN      = 22;
static constexpr int  TICK_MINOR_LEN      = 12;

// ── VU angular mapping ────────────────────────────────────────────────────────
// Angle convention: 0° = straight up, negative = left, positive = right.
// MeterEngine delivers vu_l/vu_r in dBFS with VU ballistics (300ms power avg).
// The visual scale is labelled in relative VU dB, spanning -20 to +3.

static constexpr float SCALE_DB_MIN  = -20.0f;
static constexpr float SCALE_DB_MAX  =  +3.0f;
static constexpr float ANGLE_AT_MIN  = -60.0f;  // degrees from vertical at -20 dBVU
static constexpr float ANGLE_AT_MAX  = +20.0f;  // degrees from vertical at +3 dBVU

// ── Colours (all ≥ 38% luminance — IPS black-point rule) ─────────────────────

static constexpr uint32_t COL_BG            = 0x686868u;
static constexpr uint32_t COL_TICK_NORMAL   = 0xC8C8C8u;  // -20 to 0 dBVU
static constexpr uint32_t COL_TICK_HOT      = 0xFF5555u;  // 0 to +3 dBVU
static constexpr uint32_t COL_NEEDLE        = 0xF0C020u;  // amber
static constexpr uint32_t COL_PIVOT_DOT     = 0xA0A0A0u;
static constexpr uint32_t COL_LABEL         = 0xE0E0E0u;

// ── Helpers ───────────────────────────────────────────────────────────────────

// db is clamped to the physical stop positions so the needle never leaves the face.
static float db_to_angle(float db)
{
    float c = std::max(SCALE_DB_MIN, std::min(SCALE_DB_MAX, db));
    float t = (c - SCALE_DB_MIN) / (SCALE_DB_MAX - SCALE_DB_MIN);
    return ANGLE_AT_MIN + t * (ANGLE_AT_MAX - ANGLE_AT_MIN);
}

// Converts a polar offset (radius, angle from vertical) at the pivot to canvas XY.
// "From vertical" means 0° points straight up → subtract from 90° for trig.
static void polar_xy(float r, float angle_deg,
                     lv_value_precise_t &out_x, lv_value_precise_t &out_y,
                     lv_coord_t origin_x, lv_coord_t origin_y)
{
    float rad = (90.0f - angle_deg) * (float)(M_PI / 180.0);
    out_x = (lv_value_precise_t)(origin_x + r * cosf(rad));
    out_y = (lv_value_precise_t)(origin_y - r * sinf(rad));
}

// ── Draw callback ─────────────────────────────────────────────────────────────

void SkinVU::panel_draw_cb(lv_event_t *e)
{
    auto *data     = static_cast<PanelData*>(lv_event_get_user_data(e));
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t   *obj   = lv_event_get_target_obj(e);

    lv_area_t a;
    lv_obj_get_coords(obj, &a);

    // ── Background ──────────────────────────────────────────────────────────
    {
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = lv_color_hex(COL_BG);
        dsc.radius   = 8;
        lv_draw_rect(layer, &dsc, &a);
    }

    // Absolute pivot position in screen coords
    lv_coord_t px = a.x1 + PIVOT_X;
    lv_coord_t py = a.y1 + PIVOT_Y;

    // ── Scale ticks ─────────────────────────────────────────────────────────
    // Major marks on the classic VU face.  Minor subdivisions fill the gaps.
    static const float MAJOR_MARKS[] = {
        -20.0f, -10.0f, -7.0f, -5.0f, -3.0f, -2.0f, -1.0f, 0.0f, +1.0f, +2.0f, +3.0f
    };
    static constexpr int MAJOR_COUNT = (int)(sizeof(MAJOR_MARKS) / sizeof(MAJOR_MARKS[0]));
    static constexpr int MINOR_SUBDIVISIONS = 4;  // ticks between each pair of major marks

    lv_draw_line_dsc_t ldsc;
    lv_draw_line_dsc_init(&ldsc);
    ldsc.opa = LV_OPA_COVER;

    for (int i = 0; i < MAJOR_COUNT; ++i) {
        float db    = MAJOR_MARKS[i];
        float angle = db_to_angle(db);
        bool  hot   = (db >= 0.0f);

        ldsc.color = lv_color_hex(hot ? COL_TICK_HOT : COL_TICK_NORMAL);
        ldsc.width = 2;

        polar_xy((float)SCALE_RADIUS,                angle, ldsc.p1.x, ldsc.p1.y, px, py);
        polar_xy((float)(SCALE_RADIUS - TICK_MAJOR_LEN), angle, ldsc.p2.x, ldsc.p2.y, px, py);
        lv_draw_line(layer, &ldsc);

        // Minor ticks between this major and the next
        if (i < MAJOR_COUNT - 1) {
            float next_db = MAJOR_MARKS[i + 1];
            ldsc.width = 1;
            for (int m = 1; m <= MINOR_SUBDIVISIONS; ++m) {
                float frac  = (float)m / (float)(MINOR_SUBDIVISIONS + 1);
                float mdb   = db + frac * (next_db - db);
                float mang  = db_to_angle(mdb);
                bool  mhot  = (mdb >= 0.0f);

                ldsc.color = lv_color_hex(mhot ? COL_TICK_HOT : COL_TICK_NORMAL);
                polar_xy((float)SCALE_RADIUS,                    mang, ldsc.p1.x, ldsc.p1.y, px, py);
                polar_xy((float)(SCALE_RADIUS - TICK_MINOR_LEN), mang, ldsc.p2.x, ldsc.p2.y, px, py);
                lv_draw_line(layer, &ldsc);
            }
        }
    }

    // ── Scale dB labels ─────────────────────────────────────────────────────
    {
        static const float LABEL_MARKS[]  = { -20.0f, -10.0f, -7.0f, -3.0f, 0.0f, +3.0f };
        static const char *LABEL_STRS[]   = { "-20",  "-10",  "-7",  "-3",  "0",  "+3"  };
        static constexpr int LABEL_COUNT  = (int)(sizeof(LABEL_MARKS) / sizeof(LABEL_MARKS[0]));
        static constexpr int LABEL_R      = SCALE_RADIUS - TICK_MAJOR_LEN - 18;
        static constexpr int LABEL_W      = 40;
        static constexpr int LABEL_H      = 20;

        lv_draw_label_dsc_t lbldsc;
        lv_draw_label_dsc_init(&lbldsc);
        lbldsc.align = LV_TEXT_ALIGN_CENTER;

        for (int i = 0; i < LABEL_COUNT; ++i) {
            float db    = LABEL_MARKS[i];
            float angle = db_to_angle(db);
            bool  hot   = (db >= 0.0f);
            lbldsc.color = lv_color_hex(hot ? COL_TICK_HOT : COL_TICK_NORMAL);

            lv_value_precise_t cx, cy;
            polar_xy((float)LABEL_R, angle, cx, cy, px, py);

            lv_area_t lbl_area = {
                (lv_coord_t)((int)cx - LABEL_W / 2),
                (lv_coord_t)((int)cy - LABEL_H / 2),
                (lv_coord_t)((int)cx + LABEL_W / 2),
                (lv_coord_t)((int)cy + LABEL_H / 2)
            };
            lbldsc.text = LABEL_STRS[i];
            lv_draw_label(layer, &lbldsc, &lbl_area);
        }
    }

    // ── Needle ──────────────────────────────────────────────────────────────
    {
        float angle = db_to_angle(data->db);

        lv_draw_line_dsc_t ndsc;
        lv_draw_line_dsc_init(&ndsc);
        ndsc.color = lv_color_hex(COL_NEEDLE);
        ndsc.width = (lv_coord_t)NEEDLE_W;
        ndsc.opa   = LV_OPA_COVER;

        ndsc.p1.x = (lv_value_precise_t)px;
        ndsc.p1.y = (lv_value_precise_t)py;
        polar_xy((float)NEEDLE_LEN, angle, ndsc.p2.x, ndsc.p2.y, px, py);
        lv_draw_line(layer, &ndsc);
    }

    // ── Pivot dot ────────────────────────────────────────────────────────────
    {
        lv_area_t dot = {
            (lv_coord_t)(px - 5), (lv_coord_t)(py - 5),
            (lv_coord_t)(px + 5), (lv_coord_t)(py + 5)
        };
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = lv_color_hex(COL_PIVOT_DOT);
        dsc.radius   = LV_RADIUS_CIRCLE;
        lv_draw_rect(layer, &dsc, &dot);
    }

    // ── Channel label (L / R) ────────────────────────────────────────────────
    if (data->label) {
        lv_draw_label_dsc_t lbldsc;
        lv_draw_label_dsc_init(&lbldsc);
        lbldsc.color = lv_color_hex(COL_LABEL);
        // Place label at bottom-centre below the pivot
        lv_area_t lbl_area = {
            (lv_coord_t)(a.x1 + PIVOT_X - 20), (lv_coord_t)(py + 12),
            (lv_coord_t)(a.x1 + PIVOT_X + 20), (lv_coord_t)(py + 30)
        };
        lbldsc.text = data->label;
        lv_draw_label(layer, &lbldsc, &lbl_area);
    }
}

// ── SkinVU::create_panel ──────────────────────────────────────────────────────

lv_obj_t *SkinVU::create_panel(lv_obj_t *parent, lv_coord_t x, PanelData *data)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(panel, PANEL_W, PANEL_H);
    lv_obj_set_pos(panel, x, PANEL_Y);

    // Transparent base — the draw callback paints everything including the background
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);

    lv_obj_add_event_cb(panel, panel_draw_cb, LV_EVENT_DRAW_MAIN, data);
    return panel;
}

// ── MeterSkin interface ───────────────────────────────────────────────────────

void SkinVU::create(lv_obj_t *parent)
{
    left_data_  = { SCALE_DB_MIN, "L" };
    right_data_ = { SCALE_DB_MIN, "R" };

    left_panel_  = create_panel(parent, PANEL_LEFT_X,  &left_data_);
    right_panel_ = create_panel(parent, PANEL_RIGHT_X, &right_data_);

    if (!left_panel_ || !right_panel_) {
        ESP_LOGE(TAG, "Panel creation failed");
    }
}

void SkinVU::update(const MeterReadings &r)
{
    // Ballistics are applied by MeterEngine; this just pushes the current value
    // into each panel's state and schedules a redraw.  LVGL will call the draw
    // callback at the next frame — no manual erase/redraw loop needed.
    left_data_.db  = r.vu_l;
    right_data_.db = r.vu_r;

    lv_obj_invalidate(left_panel_);
    lv_obj_invalidate(right_panel_);
}

void SkinVU::destroy()
{
    // Panels are children of parent — LVGL owns and deletes them.
    // PanelData members are stored by value in the skin struct (no heap allocs).
    left_panel_  = nullptr;
    right_panel_ = nullptr;
}
