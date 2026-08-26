#include "theme.h"

// ── Dark Glow Theme ───────────────────────────────────────────────────────────
// Elegant dark with blue accent glow. Deep backgrounds, crisp near-white text.
// Note: bg_primary (0x0D1117) is below the old IPS 38%-luminance minimum.
// With corrected LCD pins (GPIO8/9) the actual minimum may be lower — verify
// via gradient test screen if green tint appears.
const ThemeColors THEME_DARK_GLOW = {
    /* bg_primary     */ lv_color_hex(0x0A0A0Au),   // deep black (IPS minimum confirmed safe)
    /* bg_card        */ lv_color_hex(0x161B22u),   // card surface
    /* bg_card_hover  */ lv_color_hex(0x1F4070u),   // pressed card — dim accent blue
    /* accent         */ lv_color_hex(0x58A6FFu),   // bright blue
    /* accent_dim     */ lv_color_hex(0x1F4070u),   // dim blue
    /* text_primary   */ lv_color_hex(0xE6EDF3u),   // near-white
    /* text_title     */ lv_color_hex(0xFFFFFFu),   // pure white
    /* text_secondary */ lv_color_hex(0x8B949Eu),   // medium gray
    /* text_hint      */ lv_color_hex(0x6E7681u),   // subtle
    /* separator      */ lv_color_hex(0x30363Du),   // dark divider
    /* statusbar_bg   */ lv_color_hex(0x161B22u),   // head bar
    /* foot_bg        */ lv_color_hex(0x0A0A0Au),   // foot bar
    /* glow_color     */ lv_color_hex(0x58A6FFu),   // blue glow
    /* glow_opa       */ LV_OPA_30,
    /* glow_width     */ 12,
    /* glow_spread    */ 2,
};

const ThemeColors *g_theme = &THEME_DARK_GLOW;

// IN: non-null ThemeColors pointer with static lifetime. OUT: active theme switched.
void theme_set(const ThemeColors *t)
{
    if (t) g_theme = t;
}
