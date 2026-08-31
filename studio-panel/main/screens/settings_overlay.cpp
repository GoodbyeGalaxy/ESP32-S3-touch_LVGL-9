#include "settings_overlay.h"
#include "theme.h"
#include "lvgl.h"

// ── Layout ────────────────────────────────────────────────────────────────────

static constexpr int CARD_W    = 560;
static constexpr int CARD_PAD  = 16;
static constexpr int HEADER_H  = 52;
static constexpr int ROW_H     = 52;
static constexpr int PILL_H    = 30;
static constexpr int PILL_W    = 88;
static constexpr int PILL_GAP  = 6;
static constexpr int BOT_PAD   = 16;
static constexpr int DESC_H    = 72;   // height of the description box
static constexpr int DESC_GAP  = 8;    // vertical gap between row and desc box

// Content area: Y=32..424
static constexpr int CONTENT_Y = 32;
static constexpr int CONTENT_H = 392;

// ── Overlay state ─────────────────────────────────────────────────────────────

static constexpr int MAX_ITEMS = 12;
static constexpr int MAX_OPTS  = 6;

struct OverlayData {
    lv_obj_t *dimmer   = nullptr;
    bool      closing  = false;
    bool      alive    = true;
    int       item_count = 0;
    int      *selected[MAX_ITEMS]       = {};
    int       opt_count[MAX_ITEMS]      = {};
    lv_obj_t *pills[MAX_ITEMS][MAX_OPTS] = {};

    struct PillCtx { OverlayData *ov; int8_t item; int8_t opt; };
    PillCtx ctx[MAX_ITEMS][MAX_OPTS] = {};

    const char *descs[MAX_ITEMS][MAX_OPTS] = {};  // desc string per option
    lv_obj_t   *desc_lbl[MAX_ITEMS]        = {};  // label to update on selection change
};

// ── Close mechanism ───────────────────────────────────────────────────────────

static void on_dimmer_delete(lv_event_t *e);
static void on_overlay_close(lv_event_t *e);

static void close_overlay_cb(void *arg)
{
    auto *ov = static_cast<OverlayData *>(arg);
    if (!ov->alive) {
        // Dimmer already deleted by screen cascade — just free ov
        delete ov;
        return;
    }
    // Remove dimmer handlers so cascade-delete doesn't double-free
    lv_obj_remove_event_cb(ov->dimmer, on_dimmer_delete);
    lv_obj_remove_event_cb(ov->dimmer, on_overlay_close);
    lv_obj_t *dim = ov->dimmer;
    delete ov;
    lv_obj_delete(dim);
}

static void on_dimmer_delete(lv_event_t *e)
{
    auto *ov = static_cast<OverlayData *>(lv_event_get_user_data(e));
    ov->alive = false;
    if (!ov->closing) delete ov;
    // if closing==true, close_overlay_cb will delete ov
}

static void on_overlay_close(lv_event_t *e)
{
    auto *ov = static_cast<OverlayData *>(lv_event_get_user_data(e));
    if (ov->closing) return;
    ov->closing = true;
    lv_async_call(close_overlay_cb, ov);
}

// ── Pill click ────────────────────────────────────────────────────────────────

static void on_pill_click(lv_event_t *e)
{
    auto *ctx = static_cast<OverlayData::PillCtx *>(lv_event_get_user_data(e));
    OverlayData *ov = ctx->ov;
    if (ov->closing) return;

    int item = ctx->item;
    int opt  = ctx->opt;
    *ov->selected[item] = opt;

    for (int j = 0; j < ov->opt_count[item]; j++) {
        bool active = (j == opt);
        lv_obj_t *pill = ov->pills[item][j];
        lv_obj_set_style_bg_color(pill,
            active ? THEME_ACCENT : lv_color_hex(0x2A2A2A), 0);
        lv_obj_t *lbl = lv_obj_get_child(pill, 0);
        if (lbl) lv_obj_set_style_text_color(lbl,
            active ? THEME_TEXT_PRIMARY : THEME_TEXT_HINT, 0);
    }

    if (ov->desc_lbl[item] != nullptr) {
        const char *d = ov->descs[item][opt];
        lv_label_set_text(ov->desc_lbl[item], d ? d : "");
    }
}

// ── Pill helper ───────────────────────────────────────────────────────────────

static lv_obj_t *make_pill(lv_obj_t *parent, const char *text,
                            bool active, OverlayData::PillCtx *ctx)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, PILL_W, PILL_H);
    lv_obj_set_style_radius(btn, PILL_H / 2, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, active ? THEME_ACCENT : lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_bg_color(btn, active ? THEME_ACCENT : lv_color_hex(0x383838), LV_STATE_PRESSED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, on_pill_click, LV_EVENT_CLICKED, ctx);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_remove_style_all(lbl);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, active ? THEME_TEXT_PRIMARY : THEME_TEXT_HINT, 0);
    lv_obj_set_style_text_font(lbl, THEME_FONT_LABEL, 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    return btn;
}

// ── Overlay open ──────────────────────────────────────────────────────────────

static void settings_overlay_open(lv_obj_t *scr,
                                   const SettingItem *items, int count)
{
    if (count <= 0 || count > MAX_ITEMS) return;

    auto *ov = new OverlayData{};
    ov->item_count = count;

    bool has_desc[MAX_ITEMS] = {};
    int card_h = HEADER_H + BOT_PAD;
    for (int i = 0; i < count; i++) {
        card_h += ROW_H;
        int ni = (items[i].option_count < MAX_OPTS) ? items[i].option_count : MAX_OPTS;
        for (int j = 0; j < ni; j++) {
            if (items[i].options[j].desc) { has_desc[i] = true; break; }
        }
        if (has_desc[i]) card_h += DESC_H + DESC_GAP;
    }

    int card_x = (800 - CARD_W) / 2;
    int card_y = CONTENT_Y + (CONTENT_H - card_h) / 2;
    if (card_y < CONTENT_Y + 4) card_y = CONTENT_Y + 4;

    // ── Dimmer ────────────────────────────────────────────────────────────────
    lv_obj_t *dimmer = lv_obj_create(scr);
    lv_obj_remove_style_all(dimmer);
    lv_obj_set_size(dimmer, 800, 480);
    lv_obj_set_pos(dimmer, 0, 0);
    lv_obj_set_style_bg_color(dimmer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dimmer, LV_OPA_70, 0);
    lv_obj_clear_flag(dimmer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(dimmer, on_overlay_close, LV_EVENT_CLICKED, ov);
    lv_obj_add_event_cb(dimmer, on_dimmer_delete, LV_EVENT_DELETE, ov);
    ov->dimmer = dimmer;

    // ── Card ──────────────────────────────────────────────────────────────────
    lv_obj_t *card = lv_obj_create(dimmer);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, CARD_W, card_h);
    lv_obj_set_pos(card, card_x, card_y);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1C1C1C), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, THEME_RADIUS * 2, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    // Shadow
    lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
    lv_obj_set_style_shadow_width(card, 24, 0);
    lv_obj_set_style_shadow_spread(card, 4, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_60, 0);

    // ── Header ────────────────────────────────────────────────────────────────
    lv_obj_t *hdr_lbl = lv_label_create(card);
    lv_obj_remove_style_all(hdr_lbl);
    lv_label_set_text(hdr_lbl, LV_SYMBOL_SETTINGS " SETTINGS");
    lv_obj_set_style_text_color(hdr_lbl, THEME_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(hdr_lbl, THEME_FONT_LABEL, 0);
    lv_obj_set_pos(hdr_lbl, CARD_PAD, (HEADER_H - 14) / 2);

    // Close button
    lv_obj_t *x_btn = lv_btn_create(card);
    lv_obj_remove_style_all(x_btn);
    lv_obj_set_size(x_btn, 32, 32);
    lv_obj_set_pos(x_btn, CARD_W - CARD_PAD - 32, (HEADER_H - 32) / 2);
    lv_obj_set_style_bg_color(x_btn, lv_color_hex(0x2E2E2E), 0);
    lv_obj_set_style_bg_color(x_btn, lv_color_hex(0x404040), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(x_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(x_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(x_btn, on_overlay_close, LV_EVENT_CLICKED, ov);
    lv_obj_t *x_lbl = lv_label_create(x_btn);
    lv_obj_remove_style_all(x_lbl);
    lv_label_set_text(x_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(x_lbl, THEME_TEXT_HINT, 0);
    lv_obj_set_style_text_font(x_lbl, THEME_FONT_LABEL, 0);
    lv_obj_center(x_lbl);

    // Header separator
    lv_obj_t *sep = lv_obj_create(card);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, CARD_W - 2 * CARD_PAD, 1);
    lv_obj_set_pos(sep, CARD_PAD, HEADER_H - 1);
    lv_obj_set_style_bg_color(sep, THEME_SEPARATOR, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

    // ── Item rows ─────────────────────────────────────────────────────────────
    int cur_y = HEADER_H;
    for (int i = 0; i < count; i++) {
        const SettingItem &item = items[i];
        int n = (item.option_count < MAX_OPTS) ? item.option_count : MAX_OPTS;

        ov->selected[i]  = item.selected;
        ov->opt_count[i] = n;

        int row_y = cur_y;

        // Row separator (between rows)
        if (i > 0) {
            lv_obj_t *rs = lv_obj_create(card);
            lv_obj_remove_style_all(rs);
            lv_obj_set_size(rs, CARD_W - 2 * CARD_PAD, 1);
            lv_obj_set_pos(rs, CARD_PAD, row_y);
            lv_obj_set_style_bg_color(rs, lv_color_hex(0x252525), 0);
            lv_obj_set_style_bg_opa(rs, LV_OPA_COVER, 0);
        }

        // Row label
        lv_obj_t *row_lbl = lv_label_create(card);
        lv_obj_remove_style_all(row_lbl);
        lv_label_set_text(row_lbl, item.label);
        lv_obj_set_style_text_color(row_lbl, THEME_TEXT_SECONDARY, 0);
        lv_obj_set_style_text_font(row_lbl, THEME_FONT_LABEL, 0);
        lv_obj_set_pos(row_lbl, CARD_PAD, row_y + (ROW_H - 14) / 2);

        // Pills: right-aligned
        int pills_total_w = n * PILL_W + (n - 1) * PILL_GAP;
        int pill_x0 = CARD_W - CARD_PAD - pills_total_w;
        int pill_y  = row_y + (ROW_H - PILL_H) / 2;

        int cur_sel = (item.selected && *item.selected >= 0 && *item.selected < n)
                      ? *item.selected : 0;

        for (int j = 0; j < n; j++) {
            ov->ctx[i][j]   = { ov, (int8_t)i, (int8_t)j };
            ov->descs[i][j] = item.options[j].desc;
            int px = pill_x0 + j * (PILL_W + PILL_GAP);
            lv_obj_t *pill = make_pill(card, item.options[j].label,
                                       j == cur_sel, &ov->ctx[i][j]);
            lv_obj_set_pos(pill, px, pill_y);
            ov->pills[i][j] = pill;
        }

        cur_y += ROW_H;

        // Description box — shown when the selected option has a desc string
        if (has_desc[i]) {
            lv_obj_t *dbox = lv_obj_create(card);
            lv_obj_remove_style_all(dbox);
            lv_obj_set_size(dbox, CARD_W - 2 * CARD_PAD, DESC_H);
            lv_obj_set_pos(dbox, CARD_PAD, cur_y + DESC_GAP / 2);
            lv_obj_set_style_bg_color(dbox, lv_color_hex(0x0E0E0E), 0);
            lv_obj_set_style_bg_opa(dbox, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(dbox, 6, 0);
            lv_obj_set_style_border_color(dbox, THEME_ACCENT, 0);
            lv_obj_set_style_border_width(dbox, 3, 0);
            lv_obj_set_style_border_side(dbox, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_opa(dbox, LV_OPA_COVER, 0);
            lv_obj_clear_flag(dbox, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *dlbl = lv_label_create(dbox);
            lv_obj_remove_style_all(dlbl);
            lv_obj_set_size(dlbl, CARD_W - 2 * CARD_PAD - 22, DESC_H - 12);
            lv_obj_set_pos(dlbl, 12, 6);
            lv_label_set_long_mode(dlbl, LV_LABEL_LONG_WRAP);
            const char *init_d = (item.options[cur_sel].desc) ? item.options[cur_sel].desc : "";
            lv_label_set_text(dlbl, init_d);
            lv_obj_set_style_text_color(dlbl, THEME_TEXT_SECONDARY, 0);
            lv_obj_set_style_text_font(dlbl, THEME_FONT_HINT, 0);

            ov->desc_lbl[i] = dlbl;
            cur_y += DESC_H + DESC_GAP;
        }
    }
}

// ── Gear button ───────────────────────────────────────────────────────────────

struct BtnData {
    lv_obj_t    *scr;
    SettingItem *items; // owned
    int          count;
};

static void on_gear_click(lv_event_t *e)
{
    auto *d = static_cast<BtnData *>(lv_event_get_user_data(e));
    settings_overlay_open(d->scr, d->items, d->count);
}

static void on_gear_delete(lv_event_t *e)
{
    auto *d = static_cast<BtnData *>(lv_event_get_user_data(e));
    delete[] d->items;
    delete d;
}

void settings_btn_create(lv_obj_t *right_zone, lv_obj_t *scr,
                         SettingItem *items, int count)
{
    auto *d = new BtnData{ scr, items, count };

    lv_obj_t *btn = lv_btn_create(right_zone);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 44, 44);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(btn, THEME_BG_CARD, 0);
    lv_obj_set_style_bg_color(btn, THEME_ACCENT_DIM, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    theme_apply_glow(btn);
    lv_obj_add_event_cb(btn, on_gear_click, LV_EVENT_CLICKED, d);
    lv_obj_add_event_cb(btn, on_gear_delete, LV_EVENT_DELETE, d);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_remove_style_all(lbl);
    lv_label_set_text(lbl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(lbl, THEME_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(lbl, THEME_FONT_HINT, 0);
    lv_obj_center(lbl);
}
