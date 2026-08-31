// meter_osc.cpp — Oscilloscope meter screen.
// Shows L (cyan) and R (orange) waveforms in time domain.
// Ring buffer of gonio_l/gonio_r samples, trigger on zero-crossing (L channel).
// Canvas 800×480, PSRAM, ARGB8888. Updates at 20 Hz via lv_timer.

#include <initializer_list>
#include "meter_osc.h"
#include "metering_hub.h"
#include "audio_data.h"
#include "theme.h"
#include "screens/touch_nav.h"
#include "screens/statusbar.h"
#include "screens/foot.h"
#include "screens/settings_overlay.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <cstring>
#include <cmath>

static const char *TAG __attribute__((unused)) = "meter_osc";

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr int CW = 800;
static constexpr int CH = 480;

// Content area (between statusbar and foot bar)
static constexpr int CONTENT_Y = THEME_CONTENT_Y;       // 32
static constexpr int CONTENT_H = THEME_CONTENT_H;       // 392
static constexpr int CONTENT_BOT = CONTENT_Y + CONTENT_H; // 424

// Oscilloscope drawing area — inset 12px from content edges
static constexpr int OSC_X  = 12;
static constexpr int OSC_Y  = CONTENT_Y + 12;
static constexpr int OSC_W  = CW - 24;
static constexpr int OSC_H  = CONTENT_H - 24;

// Ring buffer — holds enough samples to fill the screen width at 30 Hz UDP
// With ~30 packets/s and 1 sample/packet we need a large buffer.
// We accumulate samples per packet tick; the buffer stores the latest OSC_W samples.
static constexpr int RING_SIZE = 1024;

// ── Screen state ──────────────────────────────────────────────────────────────

struct OscState {
    lv_timer_t    *timer;
    lv_obj_t      *canvas;
    lv_color32_t  *buf;          // PSRAM canvas buffer

    float          ring_l[RING_SIZE];
    float          ring_r[RING_SIZE];
    int            ring_head;    // next write index

    uint32_t       last_seq;     // detect new packets
    int trig_sel  = 0;  // 0=L, 1=R
    int scale_sel = 1;  // 0=10 ms, 1=25 ms, 2=50 ms
};

// ── Direct pixel helpers ───────────────────────────────────────────────────────

static inline void put_px(lv_color32_t *buf, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if ((unsigned)x >= CW || (unsigned)y >= CH) return;
    lv_color32_t *p = buf + y * CW + x;
    p->red   = r;
    p->green = g;
    p->blue  = b;
    p->alpha = 0xFF;
}

static void draw_line(lv_color32_t *buf, int x0, int y0, int x1, int y1,
                      uint8_t r, uint8_t g, uint8_t b)
{
    int dx  = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy  = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        put_px(buf, x0, y0, r, g, b);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// ── Render logic ──────────────────────────────────────────────────────────────

// Convert normalized sample [-1..1] to canvas Y within OSC area.
static inline int sample_to_y(float s)
{
    // clamp
    if (s >  1.0f) s =  1.0f;
    if (s < -1.0f) s = -1.0f;
    // map +1 → OSC_Y, -1 → OSC_Y + OSC_H - 1
    return OSC_Y + (int)((1.0f - s) * 0.5f * (float)(OSC_H - 1));
}

// Find trigger offset: first zero-crossing from negative→positive in L channel.
// Searches the visible window in the ring buffer.
// Returns 0..OSC_W-1 offset into the ring if found, else 0.
static int find_trigger(const OscState *st)
{
    // Walk backward from ring_head to find a suitable trigger point
    // We need OSC_W samples visible after the trigger.
    // Search in the last RING_SIZE/2 samples for a rising zero-crossing.
    int search_limit = RING_SIZE / 2 - OSC_W;
    if (search_limit < 1) search_limit = 1;

    for (int offset = OSC_W; offset < OSC_W + search_limit; offset++) {
        int i_prev = (st->ring_head - offset - 1 + RING_SIZE) % RING_SIZE;
        int i_curr = (st->ring_head - offset     + RING_SIZE) % RING_SIZE;
        float prev = st->ring_l[i_prev];
        float curr = st->ring_l[i_curr];
        if (prev < 0.0f && curr >= 0.0f) {
            return offset;  // offset from ring_head to start of display window
        }
    }
    return OSC_W;  // fallback: just show the last OSC_W samples
}

static void render_osc(OscState *st)
{
    lv_color32_t *buf = st->buf;

    // Clear canvas to background
    lv_color32_t bg = { .blue=0x0A, .green=0x0A, .red=0x0A, .alpha=0xFF };
    for (int i = 0; i < CW * CH; i++) buf[i] = bg;

    // Draw OSC frame (dim border)
    lv_color32_t frame_col = { .blue=0x30, .green=0x30, .red=0x30, .alpha=0xFF };
    for (int x = OSC_X; x < OSC_X + OSC_W; x++) {
        buf[OSC_Y * CW + x] = frame_col;
        buf[(OSC_Y + OSC_H - 1) * CW + x] = frame_col;
    }
    for (int y = OSC_Y; y < OSC_Y + OSC_H; y++) {
        buf[y * CW + OSC_X] = frame_col;
        buf[y * CW + (OSC_X + OSC_W - 1)] = frame_col;
    }

    // Center line (zero reference)
    int cy = sample_to_y(0.0f);
    lv_color32_t grid_col = { .blue=0x28, .green=0x28, .red=0x28, .alpha=0xFF };
    for (int x = OSC_X + 1; x < OSC_X + OSC_W - 1; x++) {
        if ((unsigned)cy < (unsigned)CH) buf[cy * CW + x] = grid_col;
    }
    // +/-0.5 grid lines
    for (int frac : {-75, -50, 50, 75}) {
        int gy = sample_to_y((float)frac / 100.0f);
        lv_color32_t minor = { .blue=0x1E, .green=0x1E, .red=0x1E, .alpha=0xFF };
        for (int x = OSC_X + 1; x < OSC_X + OSC_W - 1; x++) {
            if ((unsigned)gy < (unsigned)CH) buf[gy * CW + x] = minor;
        }
    }

    // Find trigger
    int start_offset = find_trigger(st);

    // Draw L channel (cyan: 0x00E5FF)
    {
        int prev_y = -1;
        for (int px = 0; px < OSC_W; px++) {
            int ring_idx = (st->ring_head - start_offset + px + RING_SIZE) % RING_SIZE;
            float s = st->ring_l[ring_idx];
            int y = sample_to_y(s);
            // clamp to OSC area
            if (y < OSC_Y) y = OSC_Y;
            if (y >= OSC_Y + OSC_H) y = OSC_Y + OSC_H - 1;
            int cx = OSC_X + px;
            if (prev_y < 0) {
                put_px(buf, cx, y, 0x00, 0xE5, 0xFF);
            } else {
                draw_line(buf, cx - 1, prev_y, cx, y, 0x00, 0xE5, 0xFF);
            }
            prev_y = y;
        }
    }

    // Draw R channel (orange: 0xFF6D00) — slightly dimmer alpha via color
    {
        int prev_y = -1;
        for (int px = 0; px < OSC_W; px++) {
            int ring_idx = (st->ring_head - start_offset + px + RING_SIZE) % RING_SIZE;
            float s = st->ring_r[ring_idx];
            int y = sample_to_y(s);
            if (y < OSC_Y) y = OSC_Y;
            if (y >= OSC_Y + OSC_H) y = OSC_Y + OSC_H - 1;
            int cx = OSC_X + px;
            if (prev_y < 0) {
                put_px(buf, cx, y, 0xFF, 0x6D, 0x00);
            } else {
                draw_line(buf, cx - 1, prev_y, cx, y, 0xFF, 0x6D, 0x00);
            }
            prev_y = y;
        }
    }

    // Legend labels (drawn as simple rectangles for L and R indicator dots)
    // L label — top-left of OSC area
    for (int dy = 0; dy < 6; dy++)
        for (int dx = 0; dx < 6; dx++)
            put_px(buf, OSC_X + 6 + dx, OSC_Y + 6 + dy, 0x00, 0xE5, 0xFF);
    // R label
    for (int dy = 0; dy < 6; dy++)
        for (int dx = 0; dx < 6; dx++)
            put_px(buf, OSC_X + 20 + dx, OSC_Y + 6 + dy, 0xFF, 0x6D, 0x00);

    lv_obj_invalidate(st->canvas);
}

// ── Timer callback ────────────────────────────────────────────────────────────

static void osc_timer_cb(lv_timer_t *t)
{
    OscState *st = static_cast<OscState *>(lv_timer_get_user_data(t));
    if (!st) return;

    AudioPacket pkt{};
    if (xQueuePeek(g_audio_queue, &pkt, 0) == pdTRUE && pkt.seq != st->last_seq) {
        st->last_seq = pkt.seq;
        // Push new sample into ring buffer
        st->ring_l[st->ring_head] = pkt.gonio_l;
        st->ring_r[st->ring_head] = pkt.gonio_r;
        st->ring_head = (st->ring_head + 1) % RING_SIZE;
    }

    render_osc(st);
}

// ── Screen lifecycle ──────────────────────────────────────────────────────────

static void on_osc_delete(lv_event_t *e)
{
    OscState *st = static_cast<OscState *>(lv_event_get_user_data(e));
    if (!st) return;
    if (st->timer) lv_timer_delete(st->timer);
    // buf is PSRAM — keep alive between sessions? For simplicity, free it.
    if (st->buf) heap_caps_free(st->buf);
    delete st;
}

lv_obj_t *meter_osc_screen_create()
{
    OscState *st = new OscState{};
    memset(st->ring_l, 0, sizeof(st->ring_l));
    memset(st->ring_r, 0, sizeof(st->ring_r));
    st->ring_head = 0;
    st->last_seq  = 0xFFFFFFFF;

    // Allocate PSRAM canvas buffer
    st->buf = static_cast<lv_color32_t *>(
        heap_caps_malloc(CW * CH * sizeof(lv_color32_t),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!st->buf) {
        ESP_LOGE(TAG, "PSRAM alloc failed");
        delete st;
        return theme_make_screen();
    }
    // Init to background
    lv_color32_t bg = { .blue=0x0A, .green=0x0A, .red=0x0A, .alpha=0xFF };
    for (int i = 0; i < CW * CH; i++) st->buf[i] = bg;

    lv_obj_t *scr = theme_make_screen();
    lv_obj_add_event_cb(scr, on_osc_delete, LV_EVENT_DELETE, st);
    statusbar_set_screen_name("OSCILLOSCOPE");

    // Canvas
    lv_obj_t *canvas = lv_canvas_create(scr);
    lv_obj_set_pos(canvas, 0, 0);
    lv_obj_set_size(canvas, CW, CH);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_canvas_set_buffer(canvas, st->buf, CW, CH, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_invalidate(canvas);
    st->canvas = canvas;

    // LVGL legend labels (on top of canvas, as LVGL objects)
    // "L" label
    lv_obj_t *lbl_l = lv_label_create(scr);
    lv_obj_remove_style_all(lbl_l);
    lv_label_set_text(lbl_l, "L");
    lv_obj_set_style_text_color(lbl_l, lv_color_hex(0x00E5FFu), 0);
    lv_obj_set_style_text_font(lbl_l, THEME_FONT_HINT, 0);
    lv_obj_set_pos(lbl_l, OSC_X + 4, OSC_Y + 4);

    lv_obj_t *lbl_r = lv_label_create(scr);
    lv_obj_remove_style_all(lbl_r);
    lv_label_set_text(lbl_r, "R");
    lv_obj_set_style_text_color(lbl_r, lv_color_hex(0xFF6D00u), 0);
    lv_obj_set_style_text_font(lbl_r, THEME_FONT_HINT, 0);
    lv_obj_set_pos(lbl_r, OSC_X + 22, OSC_Y + 4);

    // Foot bar + Settings
    lv_obj_t *osc_rz = foot_create_hub_back(scr);

    static const SettingOption trig_opts[]  = { {"L"}, {"R"} };
    static const SettingOption scale_opts[] = { {"10 ms"}, {"25 ms"}, {"50 ms"} };
    auto *osc_items = new SettingItem[2];
    osc_items[0] = { "Trigger",    trig_opts,  2, &st->trig_sel  };
    osc_items[1] = { "Time Scale", scale_opts, 3, &st->scale_sel };
    settings_btn_create(osc_rz, scr, osc_items, 2);

    // 2D swipe navigation
    touch_nav_attach_2d(scr, [](int dir_h, int dir_v, void *) {
        if (dir_h == -1) { lv_async_call([](void *){ metering_hub_next(); }, nullptr); return; }
        if (dir_h ==  1) { lv_async_call([](void *){ metering_hub_prev(); }, nullptr); return; }
        if (dir_v ==  1) { lv_async_call([](void *){ metering_hub_exit(); }, nullptr); return; }
    }, nullptr);

    // 20 Hz render timer
    st->timer = lv_timer_create(osc_timer_cb, 50, st);

    return scr;
}
