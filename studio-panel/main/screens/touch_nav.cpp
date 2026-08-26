#include "touch_nav.h"
#include "lvgl.h"
#include <vector>
#include <algorithm>

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
//
// Two independent states can be attached to the same obj: one for swipe/gesture,
// one for long-press. Both are tracked in a static registry so touch_nav_detach()
// can free them explicitly (LVGL 9's lv_obj_remove_event_cb removes by function
// pointer only — there is no per-user_data removal API, so a registry is needed
// to avoid leaks on detach).

struct SwipeState {
    SwipeCallback cb;
    void         *user_data;
    int           min_px;

    // Fallback tracking (only used if the object itself receives PRESSED/RELEASED,
    // e.g. when no clickable child intercepts). Gesture events are the primary path.
    lv_point_t    start;
    bool          active;
};

struct LongPressState {
    LongPressCallback cb;
    void             *user_data;
    uint32_t          threshold_ms;
    lv_timer_t       *timer;      // one-shot, running only while pressed
    lv_point_t        press_pos;
    lv_obj_t         *obj;        // needed inside timer callback
    bool              fired;      // long-press already fired for this press
};

// Registry entries — either a swipe or a long-press state is stored per (obj, kind).
enum class Kind : uint8_t { Swipe, LongPress };
struct Entry {
    lv_obj_t *obj;
    Kind      kind;
    void     *state;   // SwipeState* or LongPressState*
};

static std::vector<Entry> s_registry;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void free_entry(const Entry &e)
{
    if (e.kind == Kind::Swipe) {
        delete static_cast<SwipeState *>(e.state);
    } else {
        auto *lp = static_cast<LongPressState *>(e.state);
        if (lp->timer) { lv_timer_delete(lp->timer); lp->timer = nullptr; }
        delete lp;
    }
}

// Forward declarations
static void swipe_event_cb(lv_event_t *e);
static void swipe_delete_cb(lv_event_t *e);
static void long_press_event_cb(lv_event_t *e);
static void long_press_delete_cb(lv_event_t *e);
static void long_press_timer_cb(lv_timer_t *t);

// ---------------------------------------------------------------------------
// Swipe: gesture-based (primary) + PRESSED/RELEASED fallback
// ---------------------------------------------------------------------------

// IN: LVGL event on the attached obj (PRESSED/RELEASED/GESTURE). OUT: fires swipe cb
// when horizontal displacement > min_px and horizontal > vertical.
static void swipe_event_cb(lv_event_t *e)
{
    auto *state = static_cast<SwipeState *>(lv_event_get_user_data(e));
    if (!state) return;

    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev    = lv_indev_active();
    if (!indev) return;

    if (code == LV_EVENT_GESTURE) {
        // LVGL detected a gesture on this obj (or a bubbled child). Read direction.
        lv_dir_t dir = lv_indev_get_gesture_dir(indev);

        // We only care about horizontal swipes. Optional min_px filter uses the
        // actual displacement, which LVGL exposes via lv_indev_get_vect() during
        // scroll; after gesture it's not directly available, so approximate via
        // the point delta since press. lv_indev_get_point gives current, we don't
        // have press-start here — trust LVGL's threshold (LV_INDEV_DEF_GESTURE_LIMIT)
        // and just check direction. min_px acts as a soft minimum: LVGL fires
        // gesture at ~50px default, so any value <= 50 always passes.
        int direction = 0;
        if (dir == LV_DIR_LEFT)  direction = -1;
        else if (dir == LV_DIR_RIGHT) direction = 1;

        if (direction != 0 && state->cb) {
            state->cb(direction, state->user_data);
        }
        // Reset fallback tracking so a subsequent RELEASED doesn't double-fire.
        state->active = false;
        return;
    }

    // Fallback path — only runs if obj itself is CLICKABLE and receives raw events
    // (e.g. legacy overlay usage). No-op for non-clickable roots since PRESSED/RELEASED
    // won't reach them.
    if (code == LV_EVENT_PRESSED) {
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);
        state->start  = pt;
        state->active = true;
    }
    else if (code == LV_EVENT_RELEASED) {
        if (!state->active) return;
        state->active = false;

        lv_point_t pt;
        lv_indev_get_point(indev, &pt);

        int32_t dx  = (int32_t)pt.x - (int32_t)state->start.x;
        int32_t dy  = (int32_t)pt.y - (int32_t)state->start.y;
        int32_t adx = dx < 0 ? -dx : dx;
        int32_t ady = dy < 0 ? -dy : dy;

        if (adx >= state->min_px && adx > ady) {
            int direction = (dx > 0) ? 1 : -1;
            if (state->cb) state->cb(direction, state->user_data);
        }
    }
}

// IN: LV_EVENT_DELETE on obj. OUT: removes swipe entry from registry, frees state.
static void swipe_delete_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    auto it = std::find_if(s_registry.begin(), s_registry.end(),
                           [&](const Entry &x) { return x.obj == obj && x.kind == Kind::Swipe; });
    if (it != s_registry.end()) {
        free_entry(*it);
        s_registry.erase(it);
    }
}

// ---------------------------------------------------------------------------
// Long-press: PRESSED starts timer, RELEASED cancels it, timer fires cb once
// ---------------------------------------------------------------------------

// IN: one-shot lv_timer_t created on PRESSED. OUT: fires long-press cb, marks fired.
static void long_press_timer_cb(lv_timer_t *t)
{
    auto *lp = static_cast<LongPressState *>(lv_timer_get_user_data(t));
    if (!lp) return;
    // Timer is auto-deleted after this call (repeat_count = 1). Clear our ref.
    lp->timer = nullptr;
    lp->fired = true;
    if (lp->cb) lp->cb(lp->press_pos, lp->user_data);
}

// IN: PRESSED/RELEASED/PRESS_LOST on the attached obj.
// OUT: starts one-shot timer on press; cancels it on release/lost if still pending.
static void long_press_event_cb(lv_event_t *e)
{
    auto *lp = static_cast<LongPressState *>(lv_event_get_user_data(e));
    if (!lp) return;

    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        // Cancel any leftover timer from a previous press (shouldn't happen, defensive).
        if (lp->timer) { lv_timer_delete(lp->timer); lp->timer = nullptr; }
        lp->fired = false;

        lv_indev_t *indev = lv_indev_active();
        if (indev) lv_indev_get_point(indev, &lp->press_pos);

        lp->timer = lv_timer_create(long_press_timer_cb, lp->threshold_ms, lp);
        if (lp->timer) lv_timer_set_repeat_count(lp->timer, 1);
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (lp->timer) { lv_timer_delete(lp->timer); lp->timer = nullptr; }
    }
}

// IN: LV_EVENT_DELETE on obj. OUT: removes long-press entry from registry, frees state.
static void long_press_delete_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    auto it = std::find_if(s_registry.begin(), s_registry.end(),
                           [&](const Entry &x) { return x.obj == obj && x.kind == Kind::LongPress; });
    if (it != s_registry.end()) {
        free_entry(*it);
        s_registry.erase(it);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void touch_nav_attach(lv_obj_t *obj, SwipeCallback cb, void *user_data, int min_px)
{
    if (!obj) return;

    // If a swipe was already attached, detach first to avoid duplicate callbacks/leaks.
    for (const auto &e : s_registry) {
        if (e.obj == obj && e.kind == Kind::Swipe) { touch_nav_detach(obj); break; }
    }

    auto *state = new SwipeState();
    state->cb        = cb;
    state->user_data = user_data;
    state->min_px    = min_px;
    state->active    = false;
    state->start     = {0, 0};

    // Make obj receive PRESSED/RELEASED even on areas with no clickable child.
    // Gesture path (LV_EVENT_GESTURE) is secondary — requires LV_USE_GESTURE_RECOGNITION
    // which is currently disabled. PRESSED/RELEASED on deep clickable children (tiles,
    // buttons) still go to those children first; scr only receives events on empty areas.
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(obj, swipe_event_cb, LV_EVENT_GESTURE,  state);

    // Fallback for callsites where obj is itself clickable (legacy overlay usage).
    lv_obj_add_event_cb(obj, swipe_event_cb, LV_EVENT_PRESSED,  state);
    lv_obj_add_event_cb(obj, swipe_event_cb, LV_EVENT_RELEASED, state);

    // Auto-free when obj is deleted.
    lv_obj_add_event_cb(obj, swipe_delete_cb, LV_EVENT_DELETE, state);

    s_registry.push_back({obj, Kind::Swipe, state});
}

void touch_nav_attach_long_press(lv_obj_t *obj, LongPressCallback long_cb,
                                 void *user_data, uint32_t threshold_ms)
{
    if (!obj || !long_cb) return;

    // Detach any prior long-press to prevent duplicate registrations.
    for (const auto &e : s_registry) {
        if (e.obj == obj && e.kind == Kind::LongPress) {
            // Remove only long-press part (touch_nav_detach removes both kinds;
            // here we want targeted cleanup so a paired swipe stays).
            lv_obj_remove_event_cb(obj, long_press_event_cb);
            lv_obj_remove_event_cb(obj, long_press_delete_cb);
            free_entry(e);
            s_registry.erase(std::remove_if(s_registry.begin(), s_registry.end(),
                [&](const Entry &x){ return x.obj == obj && x.kind == Kind::LongPress; }),
                s_registry.end());
            break;
        }
    }

    auto *lp = new LongPressState();
    lp->cb           = long_cb;
    lp->user_data    = user_data;
    lp->threshold_ms = threshold_ms;
    lp->timer        = nullptr;
    lp->press_pos    = {0, 0};
    lp->obj          = obj;
    lp->fired        = false;

    // obj must be clickable to receive PRESSED/RELEASED. Ensure it.
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(obj, long_press_event_cb, LV_EVENT_PRESSED,    lp);
    lv_obj_add_event_cb(obj, long_press_event_cb, LV_EVENT_RELEASED,   lp);
    lv_obj_add_event_cb(obj, long_press_event_cb, LV_EVENT_PRESS_LOST, lp);
    lv_obj_add_event_cb(obj, long_press_delete_cb, LV_EVENT_DELETE, lp);

    s_registry.push_back({obj, Kind::LongPress, lp});
}

void touch_nav_detach(lv_obj_t *obj)
{
    if (!obj) return;

    // Remove all our event callbacks by function pointer. LVGL 9 removes ALL
    // registrations that share the given function, which is what we want here
    // (we've registered each fn potentially multiple times per obj across kinds).
    lv_obj_remove_event_cb(obj, swipe_event_cb);
    lv_obj_remove_event_cb(obj, swipe_delete_cb);
    lv_obj_remove_event_cb(obj, long_press_event_cb);
    lv_obj_remove_event_cb(obj, long_press_delete_cb);

    // Free all registry entries for this obj and drop them.
    for (auto it = s_registry.begin(); it != s_registry.end(); ) {
        if (it->obj == obj) {
            free_entry(*it);
            it = s_registry.erase(it);
        } else {
            ++it;
        }
    }
}
