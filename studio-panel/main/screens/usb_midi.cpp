#include "usb_midi.h"
#include "theme.h"
#include "screens/home.h"
#include "screens/touch_nav.h"
#include "screens/nav_controller.h"
#include "screens/foot.h"
#include "screens/statusbar.h"
#include "usb_midi_driver.h"
#include "lvgl.h"
#include <cstdio>

// ── Layout constants ──────────────────────────────────────────────────────────

static constexpr int STRIP_COUNT   = 8;
static constexpr int STRIP_PAD_X   = 34;   // 2×34 + 8×88 + 7×4 = 800px, symmetric
static constexpr int STRIP_W       = 88;
static constexpr int STRIP_GAP     = 4;
static constexpr int STRIP_TOP_Y   = 60;   // 32px statusbar + 28px breathing room
static constexpr int STRIP_H       = 340;  // 60+340=400 < 424 (foot at Y=424)

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

static constexpr uint8_t MIDI_CH = 0;  // MIDI channel 0 = channel 1

// ── Module-level handles (populated by create_fader) ─────────────────────────

static lv_obj_t *s_sliders[STRIP_COUNT] = {};
static lv_obj_t *s_val_lbls[STRIP_COUNT] = {};

// ── Fader value-changed callback — live CC send ───────────────────────────────

static void on_fader_change(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    int32_t val = lv_slider_get_value(lv_event_get_target_obj(e));

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", (int)val);
    lv_label_set_text(s_val_lbls[idx], buf);

    usb_midi_send_cc(MIDI_CH, FADERS[idx].cc, (uint8_t)(val & 0x7F));
}

// ── SEND CC button — resend all current values (snapshot) ────────────────────

static void on_send_cc(lv_event_t *e)
{
    (void)e;
    for (int i = 0; i < STRIP_COUNT; ++i) {
        if (s_sliders[i] == nullptr) continue;
        const int32_t val = lv_slider_get_value(s_sliders[i]);
        usb_midi_send_cc(MIDI_CH, FADERS[i].cc, (uint8_t)(val & 0x7F));
    }
}

// ── Navigation ────────────────────────────────────────────────────────────────

// IN: dir_h, dir_v from 2D swipe. OUT: delegates to nav_controller.
static void on_swipe(int dir_h, int dir_v, void * /*user_data*/)
{
    // Vertical swipe disabled — would conflict with slider drag.
    if (dir_h != 0) nav_swipe(dir_h, 0);
}

// ── Fader strip builder ───────────────────────────────────────────────────────

// IN: parent scr, FaderDef, strip x position, index into s_bars[].
// OUT: creates strip with bar + labels on parent; stores bar handle in s_bars[idx].
// Bar range 0–127, initial value def.init. All colors ≥38% luminance (IPS rule).
static void create_fader(lv_obj_t *parent, const FaderDef &def, int x, int idx)
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
    theme_apply_glow(strip);

    // Value label at top (shows CC value 0–127, updated live by on_fader_change)
    char val_buf[8];
    snprintf(val_buf, sizeof(val_buf), "%d", def.init);
    lv_obj_t *val_lbl = lv_label_create(strip);
    lv_label_set_text(val_lbl, val_buf);
    lv_obj_set_style_text_color(val_lbl, THEME_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(val_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(val_lbl, LV_ALIGN_TOP_MID, 0, 8);
    s_val_lbls[idx] = val_lbl;

    // Vertical slider (height > width → LVGL renders vertically; 0=bottom, 127=top)
    lv_obj_t *slider = lv_slider_create(strip);
    lv_obj_set_size(slider, BAR_W, BAR_H);
    lv_obj_set_pos(slider, BAR_OFFSET_X, BAR_OFFSET_Y);
    lv_slider_set_range(slider, 0, 127);
    lv_slider_set_value(slider, def.init, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x606060), 0);
    lv_obj_set_style_bg_color(slider, THEME_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, THEME_ACCENT, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, 3, 0);
    lv_obj_set_style_radius(slider, 3, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, 3, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 4, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, on_fader_change, LV_EVENT_VALUE_CHANGED,
                        (void *)(uintptr_t)idx);

    s_sliders[idx] = slider;

    // CC number label (e.g. "CC 74")
    char cc_buf[8];
    snprintf(cc_buf, sizeof(cc_buf), "CC %d", def.cc);
    lv_obj_t *cc_lbl = lv_label_create(strip);
    lv_label_set_text(cc_lbl, cc_buf);
    lv_obj_set_style_text_color(cc_lbl, THEME_TEXT_HINT, 0);
    lv_obj_set_style_text_font(cc_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(cc_lbl, LV_ALIGN_BOTTOM_MID, 0, -24);

    // Parameter name label (e.g. "Cutoff")
    lv_obj_t *name_lbl = lv_label_create(strip);
    lv_label_set_text(name_lbl, def.name);
    lv_obj_set_style_text_color(name_lbl, THEME_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(name_lbl, LV_ALIGN_BOTTOM_MID, 0, -8);
}

// ── Screen entry point ────────────────────────────────────────────────────────

// IN: nothing. OUT: new screen lv_obj_t* — NOT loaded; caller calls lv_screen_load/lv_screen_load_anim.
// Swipe-right navigates home. Lifetime managed by LVGL.
lv_obj_t *usb_midi_screen_create()
{
    for (int i = 0; i < STRIP_COUNT; ++i) { s_sliders[i] = nullptr; s_val_lbls[i] = nullptr; }

    lv_obj_t *scr = theme_make_screen();

    statusbar_set_screen_name("USB MIDI");

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
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(hint, STRIP_PAD_X, 36);

    // 8 CC fader strips — pass index so s_bars[] gets populated
    for (int i = 0; i < STRIP_COUNT; ++i) {
        const int x = STRIP_PAD_X + i * (STRIP_W + STRIP_GAP);
        create_fader(scr, FADERS[i], x, i);
    }

    // Foot bar with Home button; SEND CC button in right_zone
    lv_obj_t *right_zone = foot_create(scr);

    lv_obj_t *send_btn = lv_btn_create(right_zone);
    lv_obj_set_size(send_btn, 120, 40);
    lv_obj_align(send_btn, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(send_btn, THEME_ACCENT, 0);
    lv_obj_set_style_bg_color(send_btn, lv_color_hex(0x909090), LV_STATE_PRESSED);

    lv_obj_t *send_lbl = lv_label_create(send_btn);
    lv_label_set_text(send_lbl, "SEND CC");
    lv_obj_set_style_text_color(send_lbl, THEME_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(send_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(send_lbl);

    // Wire up click → MIDI CC send
    lv_obj_add_event_cb(send_btn, on_send_cc, LV_EVENT_CLICKED, nullptr);

    // 2D swipe → nav_controller
    touch_nav_attach_2d(scr, on_swipe, nullptr);

    return scr;
}
