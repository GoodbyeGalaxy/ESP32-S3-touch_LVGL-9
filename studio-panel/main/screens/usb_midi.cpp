#include "usb_midi.h"
#include "theme.h"
#include "screens/home.h"
#include "screens/touch_nav.h"
#include "lvgl.h"

// ── Layout constants ──────────────────────────────────────────────────────────

static constexpr int STRIP_COUNT   = 8;
static constexpr int STRIP_PAD_X   = 20;   // left/right margin
static constexpr int STRIP_W       = 90;   // 8×90 + 2×20 = 760 ≤ 800
static constexpr int STRIP_GAP     = 5;    // gap between strips (unused — strips are flush)
static constexpr int STRIP_TOP_Y   = 60;   // 32px statusbar + 28px breathing room
static constexpr int STRIP_H       = 380;

static constexpr int BAR_W         = 18;
static constexpr int BAR_H         = 220;
static constexpr int BAR_OFFSET_X  = (STRIP_W - BAR_W) / 2;  // centred in strip
static constexpr int BAR_OFFSET_Y  = 38;   // below value label

// ── CC parameter table ────────────────────────────────────────────────────────

struct FaderDef { uint8_t cc; const char *name; uint8_t init; };
static const FaderDef FADERS[STRIP_COUNT] = {
    { 74, "Cutoff",  64 },
    { 71, "Reso",     0 },
    {  1, "Mod",      0 },
    { 72, "Env Amt", 64 },
    { 73, "Attack",   0 },
    { 75, "Decay",   64 },
    { 78, "Release", 32 },
    {  7, "Volume", 100 },
};

// ── Navigation ────────────────────────────────────────────────────────────────

static void on_back(lv_event_t *e)
{
    lv_obj_t *home = home_screen_create();
    lv_screen_load_anim(home, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, true);
}

// IN: direction (+1=fwd, -1=back), user_data unused. OUT: loads home on direction==-1.
static void on_swipe(int direction, void *user_data)
{
    if (direction == -1) {
        lv_obj_t *home = home_screen_create();
        lv_screen_load_anim(home, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, true);
    }
}

// ── Fader strip builder ───────────────────────────────────────────────────────

// IN: parent scr, FaderDef, strip x position. OUT: creates strip with bar + labels on parent.
// Bar range 0–127, initial value def.init. All colors ≥38% luminance (IPS rule).
static void create_fader(lv_obj_t *parent, const FaderDef &def, int x)
{
    // Strip background card
    lv_obj_t *strip = lv_obj_create(parent);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, STRIP_W, STRIP_H);
    lv_obj_set_pos(strip, x, STRIP_TOP_Y);
    lv_obj_set_style_bg_color(strip, THEME_BG_CARD, 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(strip, 6, 0);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);

    // Value label at top (shows CC value 0–127)
    char val_buf[8];
    snprintf(val_buf, sizeof(val_buf), "%d", def.init);
    lv_obj_t *val_lbl = lv_label_create(strip);
    lv_label_set_text(val_lbl, val_buf);
    lv_obj_set_style_text_color(val_lbl, THEME_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(val_lbl, THEME_FONT_HINT, 0);
    lv_obj_align(val_lbl, LV_ALIGN_TOP_MID, 0, 8);

    // Vertical bar (CC value visual)
    lv_obj_t *bar = lv_bar_create(strip);
    lv_obj_set_size(bar, BAR_W, BAR_H);
    lv_obj_set_pos(bar, BAR_OFFSET_X, BAR_OFFSET_Y);
    lv_bar_set_mode(bar, LV_BAR_MODE_NORMAL);
    lv_bar_set_range(bar, 0, 127);
    lv_bar_set_value(bar, def.init, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x606060), 0);  // track
    lv_obj_set_style_bg_color(bar, THEME_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 3, 0);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);

    // CC number label (e.g. "CC 74")
    char cc_buf[8];
    snprintf(cc_buf, sizeof(cc_buf), "CC %d", def.cc);
    lv_obj_t *cc_lbl = lv_label_create(strip);
    lv_label_set_text(cc_lbl, cc_buf);
    lv_obj_set_style_text_color(cc_lbl, THEME_TEXT_HINT, 0);
    lv_obj_set_style_text_font(cc_lbl, THEME_FONT_HINT, 0);
    lv_obj_align(cc_lbl, LV_ALIGN_BOTTOM_MID, 0, -24);

    // Parameter name label (e.g. "Cutoff")
    lv_obj_t *name_lbl = lv_label_create(strip);
    lv_label_set_text(name_lbl, def.name);
    lv_obj_set_style_text_color(name_lbl, THEME_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(name_lbl, THEME_FONT_HINT, 0);
    lv_obj_align(name_lbl, LV_ALIGN_BOTTOM_MID, 0, -8);
}

// ── Screen entry point ────────────────────────────────────────────────────────

// IN: nothing. OUT: new screen lv_obj_t* — NOT loaded; caller calls lv_screen_load/lv_screen_load_anim.
// Swipe-right navigates home. Lifetime managed by LVGL.
lv_obj_t *usb_midi_screen_create()
{
    lv_obj_t *scr = theme_make_screen();

    // Title (left-aligned, sits in statusbar zone)
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "USB MIDI");
    lv_obj_set_style_text_color(title, THEME_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_LABEL, 0);
    lv_obj_set_pos(title, STRIP_PAD_X, 8);

    // Subtitle: Nord Lead 2X hint
    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "Nord Lead 2X");
    lv_obj_set_style_text_color(hint, THEME_TEXT_HINT, 0);
    lv_obj_set_style_text_font(hint, THEME_FONT_HINT, 0);
    lv_obj_set_pos(hint, STRIP_PAD_X, 28);

    // 8 CC fader strips
    for (int i = 0; i < STRIP_COUNT; ++i) {
        int x = STRIP_PAD_X + i * STRIP_W;
        create_fader(scr, FADERS[i], x);
    }

    // Back button
    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 90, 36);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, 16, -8);
    lv_obj_set_style_bg_color(btn, THEME_BG_CARD, 0);
    lv_obj_add_event_cb(btn, on_back, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, LV_SYMBOL_LEFT " Home");
    lv_obj_set_style_text_color(btn_label, THEME_TEXT_PRIMARY, 0);
    lv_obj_center(btn_label);

    // Swipe-right → Home
    touch_nav_attach(scr, on_swipe, nullptr);

    return scr;
}
