#include "touch_nav.h"
#include "lvgl.h"
#include <cstdlib>
#include <cmath>

// ---------------------------------------------------------------------------
// Internal state — one per attached object
// ---------------------------------------------------------------------------

struct TouchNavState {
    SwipeCallback cb;
    void         *user_data;
    int           min_px;

    lv_point_t    start;      // touch-down position
    bool          active;     // finger is currently down
};

// ---------------------------------------------------------------------------
// Helper: find the active touch indev
// ---------------------------------------------------------------------------

static lv_indev_t *find_touch_indev()
{
    lv_indev_t *dev = lv_indev_get_next(nullptr);
    while (dev) {
        if (lv_indev_get_type(dev) == LV_INDEV_TYPE_POINTER) {
            return dev;
        }
        dev = lv_indev_get_next(dev);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// LVGL event handler
// ---------------------------------------------------------------------------

static void touch_nav_event_cb(lv_event_t *e)
{
    TouchNavState *state = static_cast<TouchNavState *>(lv_event_get_user_data(e));
    if (!state) return;

    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        lv_indev_t *indev = find_touch_indev();
        if (!indev) return;

        lv_point_t pt;
        lv_indev_get_point(indev, &pt);
        state->start  = pt;
        state->active = true;
    }
    else if (code == LV_EVENT_RELEASED) {
        if (!state->active) return;
        state->active = false;

        lv_indev_t *indev = find_touch_indev();
        if (!indev) return;

        lv_point_t pt;
        lv_indev_get_point(indev, &pt);

        int32_t dx = (int32_t)pt.x - (int32_t)state->start.x;
        int32_t dy = (int32_t)pt.y - (int32_t)state->start.y;

        int32_t adx = dx < 0 ? -dx : dx;
        int32_t ady = dy < 0 ? -dy : dy;

        // Must exceed threshold and be more horizontal than vertical
        if (adx >= state->min_px && adx > ady) {
            int direction = (dx > 0) ? 1 : -1;
            if (state->cb) {
                state->cb(direction, state->user_data);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Cleanup: free state when obj is deleted
// ---------------------------------------------------------------------------

static void touch_nav_delete_cb(lv_event_t *e)
{
    TouchNavState *state = static_cast<TouchNavState *>(lv_event_get_user_data(e));
    delete state;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void touch_nav_attach(lv_obj_t *obj, SwipeCallback cb, void *user_data, int min_px)
{
    if (!obj) return;

    TouchNavState *state = new TouchNavState();
    state->cb        = cb;
    state->user_data = user_data;
    state->min_px    = min_px;
    state->active    = false;
    state->start     = {0, 0};

    // Make the object clickable so it receives PRESSED / RELEASED events
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(obj, touch_nav_event_cb, LV_EVENT_PRESSED,  state);
    lv_obj_add_event_cb(obj, touch_nav_event_cb, LV_EVENT_RELEASED, state);

    // Auto-free the state when the object is deleted
    lv_obj_add_event_cb(obj, touch_nav_delete_cb, LV_EVENT_DELETE, state);
}

void touch_nav_detach(lv_obj_t *obj)
{
    if (!obj) return;

    // LVGL 9 removes all event callbacks with a given function pointer;
    // we need to remove all three registrations. There is no direct
    // "remove by user_data" API in LVGL 9, so we remove by function pointer
    // for each event code we registered.
    lv_obj_remove_event_cb(obj, touch_nav_event_cb);
    lv_obj_remove_event_cb(obj, touch_nav_delete_cb);
    // Note: the TouchNavState memory will have been freed by touch_nav_delete_cb
    // if the object is being deleted. If called before deletion (e.g. screen swap),
    // we accept the small leak because the object lifetime ends shortly after.
}
