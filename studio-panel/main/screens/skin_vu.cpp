#include "skin_vu.h"
#include "lvgl.h"
#include "theme.h"
#include "esp_log.h"
#include <cmath>
#include <algorithm>
#include <cstdio>

static const char *TAG = "SkinVU";

// ── Panel geometry ────────────────────────────────────────────────────────────

static constexpr lv_coord_t PANEL_W       = 340;
static constexpr lv_coord_t PANEL_H       = 360;
// PIVOT_X shifted right of panel center so the -20dBVU scale (60° left of vertical)
// stays within the panel bounds. At PIVOT_X=230: leftmost tick at +33px, needle at +12px.
static constexpr lv_coord_t PIVOT_X       = 230;
// Panels centered in 800px: 2×340 + 20px gap = 700px → 50px margin each side.
static constexpr lv_coord_t PANEL_LEFT_X  = 50;
static constexpr lv_coord_t PANEL_RIGHT_X = 410;
static constexpr lv_coord_t PANEL_Y       = THEME_CONTENT_Y + (THEME_CONTENT_H - PANEL_H) / 2;

// ── Needle geometry ───────────────────────────────────────────────────────────
static constexpr lv_coord_t PIVOT_Y   = 300;  // 60px below: room for corner L/R labels
static constexpr int        NEEDLE_LEN = 252;
static constexpr int        NEEDLE_W   = 2;

// ── Scale arc geometry ────────────────────────────────────────────────────────

static constexpr int SCALE_RADIUS   = 228;  // outer tick endpoint radius
static constexpr int TICK_MAJOR_LEN = 22;
static constexpr int TICK_MINOR_LEN = 12;
static constexpr int ARC_OUTER_R    = SCALE_RADIUS - 3;   // zone arc outer edge (just inside ticks)
static constexpr int ARC_WIDTH      = 11;                  // zone arc stroke thickness

// ── VU angular mapping ────────────────────────────────────────────────────────
// 0° = straight up from pivot, negative = left, positive = right.
// MeterEngine delivers vu_l/vu_r in dBFS with VU ballistics (300 ms power avg).

static constexpr float SCALE_DB_MIN = -20.0f;
static constexpr float SCALE_DB_MAX =  +3.0f;
static constexpr float ANGLE_AT_MIN = -60.0f;  // degrees from vertical at -20 dBVU
static constexpr float ANGLE_AT_MAX = +20.0f;  // degrees from vertical at +3 dBVU

// ── LVGL arc angle conversion ─────────────────────────────────────────────────
// LVGL arc: 0° = 3 o'clock (right), clockwise. Our 0° = 12 o'clock (up) = LVGL 270°.
// Conversion: lvgl_angle = 270 + our_angle_deg.
static constexpr int32_t LVGL_NORTH = 270;

// Zone arc boundaries in LVGL degrees (all constexpr, computed from the VU scale)
// db=-3 → our_angle = -60 + (17/23)*80 ≈ -0.87° → LVGL ≈ 269
// db= 0 → our_angle = -60 + (20/23)*80 ≈ +9.57° → LVGL ≈ 280
static constexpr int32_t ARC_START       = LVGL_NORTH + (int32_t)ANGLE_AT_MIN;  // 210
static constexpr int32_t ARC_CAUTION_DEG = 269;  // -3 dBVU boundary
static constexpr int32_t ARC_HOT_DEG     = 280;  //  0 dBVU boundary
static constexpr int32_t ARC_END         = LVGL_NORTH + (int32_t)ANGLE_AT_MAX;  // 290

// ── Colours ───────────────────────────────────────────────────────────────────

static constexpr uint32_t COL_BG            = 0x080808u;  // near-black panel background
static constexpr uint32_t COL_BORDER        = 0x2A3548u;  // cool dark blue-gray outline
static constexpr uint32_t COL_ARC_SAFE      = 0x1A263Au;  // subtle dark-blue safe zone
static constexpr uint32_t COL_ARC_CAUTION   = 0x3D2800u;  // dark amber caution zone
static constexpr uint32_t COL_ARC_HOT       = 0x3D0800u;  // dark red hot zone
static constexpr uint32_t COL_TICK_NORMAL   = 0x8B949Eu;  // silver ticks
static constexpr uint32_t COL_TICK_HOT      = 0xFF4040u;  // red overload ticks
static constexpr uint32_t COL_NEEDLE        = 0xF5C330u;  // gold/amber needle
static constexpr uint32_t COL_NEEDLE_SHADOW = 0x402200u;  // dark amber shadow
static constexpr uint32_t COL_PEAK_HOLD     = 0xFF8800u;  // orange peak-hold dot
static constexpr uint32_t COL_PIVOT_DOT     = 0x5A6375u;
static constexpr uint32_t COL_CHANNEL_LBL   = 0xE6EDF3u;  // near-white channel L/R
static constexpr uint32_t COL_NUM_NORMAL    = 0x8B949Eu;
static constexpr uint32_t COL_NUM_HOT       = 0xFF4040u;

// Peak hold: frames at 30 Hz before the marker decays
static constexpr int PEAK_HOLD_FRAMES = 90;  // 3 s

// ── Helpers ───────────────────────────────────────────────────────────────────

static float db_to_angle(float db)
{
    float c = std::max(SCALE_DB_MIN, std::min(SCALE_DB_MAX, db));
    float t = (c - SCALE_DB_MIN) / (SCALE_DB_MAX - SCALE_DB_MIN);
    return ANGLE_AT_MIN + t * (ANGLE_AT_MAX - ANGLE_AT_MIN);
}

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

    lv_coord_t px = a.x1 + PIVOT_X;   // absolute pivot X
    lv_coord_t py = a.y1 + PIVOT_Y;   // absolute pivot Y

    // ── Background ────────────────────────────────────────────────────────────
    {
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.radius   = 10;
        dsc.bg_color = lv_color_hex(COL_BG);
        lv_draw_rect(layer, &dsc, &a);
    }

    // ── Zone arcs (behind ticks) — safe / caution / hot ──────────────────────
    {
        lv_draw_arc_dsc_t arc;
        lv_draw_arc_dsc_init(&arc);
        arc.center.x = px;
        arc.center.y = py;
        arc.radius   = (uint16_t)ARC_OUTER_R;
        arc.width    = (int32_t)ARC_WIDTH;
        arc.rounded  = 0;
        arc.opa      = LV_OPA_COVER;

        // Safe zone: 210°–269°
        arc.color       = lv_color_hex(COL_ARC_SAFE);
        arc.start_angle = (lv_value_precise_t)ARC_START;
        arc.end_angle   = (lv_value_precise_t)ARC_CAUTION_DEG;
        lv_draw_arc(layer, &arc);

        // Caution zone: 269°–280°
        arc.color       = lv_color_hex(COL_ARC_CAUTION);
        arc.start_angle = (lv_value_precise_t)ARC_CAUTION_DEG;
        arc.end_angle   = (lv_value_precise_t)ARC_HOT_DEG;
        lv_draw_arc(layer, &arc);

        // Hot zone: 280°–290°
        arc.color       = lv_color_hex(COL_ARC_HOT);
        arc.start_angle = (lv_value_precise_t)ARC_HOT_DEG;
        arc.end_angle   = (lv_value_precise_t)ARC_END;
        lv_draw_arc(layer, &arc);
    }

    // ── Scale ticks ───────────────────────────────────────────────────────────
    static const float MAJOR_MARKS[] = {
        -20.0f, -10.0f, -7.0f, -5.0f, -3.0f, -2.0f, -1.0f, 0.0f, +1.0f, +2.0f, +3.0f
    };
    static constexpr int MAJOR_COUNT        = (int)(sizeof(MAJOR_MARKS) / sizeof(MAJOR_MARKS[0]));
    static constexpr int MINOR_SUBDIVISIONS = 4;

    lv_draw_line_dsc_t ldsc;
    lv_draw_line_dsc_init(&ldsc);
    ldsc.opa       = LV_OPA_COVER;
    ldsc.dash_width = 0;
    ldsc.dash_gap   = 0;

    for (int i = 0; i < MAJOR_COUNT; ++i) {
        float db    = MAJOR_MARKS[i];
        float angle = db_to_angle(db);
        bool  hot   = (db >= 0.0f);

        ldsc.color = lv_color_hex(hot ? COL_TICK_HOT : COL_TICK_NORMAL);
        ldsc.width = 3;
        polar_xy((float)SCALE_RADIUS,                angle, ldsc.p1.x, ldsc.p1.y, px, py);
        polar_xy((float)(SCALE_RADIUS - TICK_MAJOR_LEN), angle, ldsc.p2.x, ldsc.p2.y, px, py);
        lv_draw_line(layer, &ldsc);

        if (i < MAJOR_COUNT - 1) {
            float next_db = MAJOR_MARKS[i + 1];
            ldsc.width = 2;
            for (int m = 1; m <= MINOR_SUBDIVISIONS; ++m) {
                float frac = (float)m / (float)(MINOR_SUBDIVISIONS + 1);
                float mdb  = db + frac * (next_db - db);
                float mang = db_to_angle(mdb);
                bool  mhot = (mdb >= 0.0f);
                ldsc.color = lv_color_hex(mhot ? COL_TICK_HOT : COL_TICK_NORMAL);
                polar_xy((float)SCALE_RADIUS,                    mang, ldsc.p1.x, ldsc.p1.y, px, py);
                polar_xy((float)(SCALE_RADIUS - TICK_MINOR_LEN), mang, ldsc.p2.x, ldsc.p2.y, px, py);
                lv_draw_line(layer, &ldsc);
            }
        }
    }

    // ── Scale dB labels ───────────────────────────────────────────────────────
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
        lbldsc.font  = &lv_font_montserrat_14;

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

    // ── "VU" face label ───────────────────────────────────────────────────────
    {
        lv_draw_label_dsc_t lbldsc;
        lv_draw_label_dsc_init(&lbldsc);
        lbldsc.color = lv_color_hex(COL_TICK_NORMAL);
        lbldsc.font  = &lv_font_montserrat_14;
        lbldsc.align = LV_TEXT_ALIGN_CENTER;
        lbldsc.text  = "VU";
        lv_area_t lbl_area = {
            (lv_coord_t)(a.x1 + PIVOT_X - 20), (lv_coord_t)(a.y1 + PIVOT_Y - 130),
            (lv_coord_t)(a.x1 + PIVOT_X + 20), (lv_coord_t)(a.y1 + PIVOT_Y - 112)
        };
        lv_draw_label(layer, &lbldsc, &lbl_area);
    }

    // ── Numeric dBVU readout ──────────────────────────────────────────────────
    {
        char num_buf[12];
        snprintf(num_buf, sizeof(num_buf), "%+.1f", data->db);
        lv_draw_label_dsc_t lbldsc;
        lv_draw_label_dsc_init(&lbldsc);
        lbldsc.color = lv_color_hex(data->db >= 0.0f ? COL_NUM_HOT : COL_NUM_NORMAL);
        lbldsc.font  = &lv_font_unscii_16;
        lbldsc.align = LV_TEXT_ALIGN_CENTER;
        lbldsc.text  = num_buf;
        lv_area_t lbl_area = {
            (lv_coord_t)(a.x1 + PIVOT_X - 32), (lv_coord_t)(a.y1 + PIVOT_Y - 108),
            (lv_coord_t)(a.x1 + PIVOT_X + 32), (lv_coord_t)(a.y1 + PIVOT_Y - 90)
        };
        lv_draw_label(layer, &lbldsc, &lbl_area);
    }

    // ── Peak-hold ghost dot on arc ────────────────────────────────────────────
    if (data->peak_hold_frames > 0) {
        float ph_angle = db_to_angle(data->peak_hold_db);
        lv_value_precise_t phx, phy;
        polar_xy((float)(SCALE_RADIUS - TICK_MAJOR_LEN / 2), ph_angle, phx, phy, px, py);
        lv_area_t dot = {
            (lv_coord_t)((int)phx - 4), (lv_coord_t)((int)phy - 4),
            (lv_coord_t)((int)phx + 4), (lv_coord_t)((int)phy + 4)
        };
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = lv_color_hex(COL_PEAK_HOLD);
        dsc.radius   = LV_RADIUS_CIRCLE;
        // Fade: full bright for first half of hold, then dim
        dsc.bg_opa = (data->peak_hold_frames > PEAK_HOLD_FRAMES / 2)
                     ? (lv_opa_t)LV_OPA_COVER
                     : (lv_opa_t)(255 * data->peak_hold_frames / (PEAK_HOLD_FRAMES / 2));
        lv_draw_rect(layer, &dsc, &dot);
    }

    // ── Needle shadow (wider, semi-transparent amber behind the needle) ────────
    {
        float angle = db_to_angle(data->db);
        lv_draw_line_dsc_t ndsc;
        lv_draw_line_dsc_init(&ndsc);
        ndsc.color  = lv_color_hex(COL_NEEDLE_SHADOW);
        ndsc.width  = NEEDLE_W + 4;
        ndsc.opa    = LV_OPA_50;
        ndsc.p1.x   = (lv_value_precise_t)px;
        ndsc.p1.y   = (lv_value_precise_t)py;
        polar_xy((float)NEEDLE_LEN, angle, ndsc.p2.x, ndsc.p2.y, px, py);
        lv_draw_line(layer, &ndsc);
    }

    // ── Needle ────────────────────────────────────────────────────────────────
    {
        float angle = db_to_angle(data->db);
        lv_draw_line_dsc_t ndsc;
        lv_draw_line_dsc_init(&ndsc);
        ndsc.color  = lv_color_hex(COL_NEEDLE);
        ndsc.width  = (lv_coord_t)NEEDLE_W;
        ndsc.opa    = LV_OPA_COVER;
        ndsc.p1.x   = (lv_value_precise_t)px;
        ndsc.p1.y   = (lv_value_precise_t)py;
        polar_xy((float)NEEDLE_LEN, angle, ndsc.p2.x, ndsc.p2.y, px, py);
        lv_draw_line(layer, &ndsc);
    }

    // ── Pivot dot ─────────────────────────────────────────────────────────────
    {
        lv_area_t dot = {
            (lv_coord_t)(px - 5), (lv_coord_t)(py - 5),
            (lv_coord_t)(px + 5), (lv_coord_t)(py + 5)
        };
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = lv_color_hex(COL_NEEDLE);
        dsc.radius   = LV_RADIUS_CIRCLE;
        lv_draw_rect(layer, &dsc, &dot);
    }

    // ── Peak LED (clip light — rectangular, top-centre) ───────────────────────
    {
        lv_coord_t led_x = a.x1 + PIVOT_X;
        lv_area_t led = {
            (lv_coord_t)(led_x - 10), (lv_coord_t)(a.y1 + 12),
            (lv_coord_t)(led_x + 10), (lv_coord_t)(a.y1 + 24)
        };
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.radius   = 2;

        dsc.bg_color = data->peak ? lv_color_hex(0xFF2222u) : lv_color_hex(0x2A1A1Au);
        lv_draw_rect(layer, &dsc, &led);
    }

    // ── Channel label: L bottom-left, R bottom-right ─────────────────────────
    if (data->label) {
        lv_draw_label_dsc_t lbldsc;
        lv_draw_label_dsc_init(&lbldsc);
        lbldsc.color = lv_color_hex(COL_CHANNEL_LBL);
        lbldsc.font  = &lv_font_montserrat_14;
        lv_area_t lbl_area;
        if (data->label[0] == 'L') {
            lbldsc.align = LV_TEXT_ALIGN_LEFT;
            lbl_area = { (lv_coord_t)(a.x1 + 14), (lv_coord_t)(py + 38),
                         (lv_coord_t)(a.x1 + 54), (lv_coord_t)(py + 58) };
        } else {
            lbldsc.align = LV_TEXT_ALIGN_RIGHT;
            lbl_area = { (lv_coord_t)(a.x2 - 54), (lv_coord_t)(py + 38),
                         (lv_coord_t)(a.x2 - 14), (lv_coord_t)(py + 58) };
        }
        lbldsc.text = data->label;
        lv_draw_label(layer, &lbldsc, &lbl_area);
    }

    // ── Panel border (drawn last so it sits on top) ───────────────────────────
    {
        lv_draw_rect_dsc_t bdsc;
        lv_draw_rect_dsc_init(&bdsc);
        bdsc.bg_opa       = LV_OPA_TRANSP;
        bdsc.border_color = lv_color_hex(COL_BORDER);
        bdsc.border_width = 1;
        bdsc.border_opa   = LV_OPA_COVER;
        bdsc.radius       = 10;
        lv_draw_rect(layer, &bdsc, &a);
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
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(panel, panel_draw_cb, LV_EVENT_DRAW_MAIN, data);
    return panel;
}

// ── MeterSkin interface ───────────────────────────────────────────────────────

void SkinVU::create(lv_obj_t *parent)
{
    left_data_  = { SCALE_DB_MIN, "L", false, SCALE_DB_MIN, 0 };
    right_data_ = { SCALE_DB_MIN, "R", false, SCALE_DB_MIN, 0 };

    left_panel_  = create_panel(parent, PANEL_LEFT_X,  &left_data_);
    right_panel_ = create_panel(parent, PANEL_RIGHT_X, &right_data_);

    if (!left_panel_ || !right_panel_) {
        ESP_LOGE(TAG, "Panel creation failed");
    }
}

void SkinVU::update(const MeterReadings &r)
{
    auto update_channel = [](PanelData &d, float val) {
        d.db   = val;
        d.peak = (val >= 0.0f);

        // Peak hold: reset timer on new max, decay otherwise
        if (val >= d.peak_hold_db) {
            d.peak_hold_db     = val;
            d.peak_hold_frames = PEAK_HOLD_FRAMES;
        } else if (d.peak_hold_frames > 0) {
            --d.peak_hold_frames;
            // Once hold expires, snap marker to current value so it doesn't linger
            if (d.peak_hold_frames == 0) d.peak_hold_db = val;
        }
    };

    update_channel(left_data_,  r.vu_l);
    update_channel(right_data_, r.vu_r);

    lv_obj_invalidate(left_panel_);
    lv_obj_invalidate(right_panel_);
}

void SkinVU::destroy()
{
    left_panel_  = nullptr;
    right_panel_ = nullptr;
}
