#include "ui_nav_gesture.h"

enum { SIDE_WIDTH = 20, BOTTOM_HEIGHT = 24, TOP_HEIGHT = 64,
       BACK_DISTANCE = 64, HOME_DISTANCE = 72, HOLD_MS = 350, HOLD_SLOP = 12 };
static struct {
    lv_obj_t *root, *left, *right, *bottom, *handle, *hint, *screen;
    lv_indev_t *input;
    void (*dispatch)(ui_nav_action_t action);
    lv_point_t origin, anchor;
    int32_t width, height;
    unsigned scale, scope;
    uint32_t quiet_since;
    ui_nav_action_t preview;
    bool enabled, home_allowed, active, bottom_start, right_start, reached;
} nav;

static int32_t px(int32_t value) { return (value * (int32_t)nav.scale + 50) / 100; }
static void show(lv_obj_t *object, bool visible)
{
    if (visible) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

void ui_nav_gesture_cancel(void)
{
    if (nav.active && nav.input && lv_indev_get_state(nav.input) == LV_INDEV_STATE_PRESSED)
        lv_indev_wait_release(nav.input);
    nav.active = nav.reached = false;
    nav.input = NULL;
    nav.preview = UI_NAV_NONE;
    if (nav.hint) show(nav.hint, false);
}

static void preview(ui_nav_action_t action)
{
    if (nav.preview == action) return;
    nav.preview = action;
    show(nav.hint, action != UI_NAV_NONE);
    lv_obj_invalidate(nav.hint);
}

static void hint_draw(lv_event_t *event)
{
    lv_area_t area;
    lv_obj_get_coords(nav.hint, &area);
    int32_t x = (area.x1 + area.x2) / 2, y = (area.y1 + area.y2) / 2;
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_black();
    line.width = px(3);
    line.round_start = line.round_end = 1;
    lv_layer_t *layer = lv_event_get_layer(event);
#define SEGMENT(x1, y1, x2, y2) do { \
    line.p1.x = x + px(x1); line.p1.y = y + px(y1); \
    line.p2.x = x + px(x2); line.p2.y = y + px(y2); \
    lv_draw_line(layer, &line); \
} while (0)
    if (nav.preview == UI_NAV_BACK) {
        SEGMENT(7, -13, -7, 0); SEGMENT(-7, 0, 7, 13);
    } else if (nav.preview == UI_NAV_HOME) {
        SEGMENT(-16, 0, 0, -14); SEGMENT(0, -14, 16, 0);
        SEGMENT(-11, -4, -11, 14); SEGMENT(11, -4, 11, 14); SEGMENT(-11, 14, 11, 14);
    } else if (nav.preview == UI_NAV_RECENT) {
        SEGMENT(-13, -12, 13, -12); SEGMENT(-13, 0, 13, 0); SEGMENT(-13, 12, 13, 12);
    }
#undef SEGMENT
}

static void edge_event(lv_event_t *event)
{
    lv_indev_t *input = lv_indev_active();
    if (!nav.enabled || !input || lv_indev_get_type(input) != LV_INDEV_TYPE_POINTER) return;
    lv_event_code_t code = lv_event_get_code(event);
    lv_point_t point;
    lv_indev_get_point(input, &point);
    if (code == LV_EVENT_PRESSED) {
        ui_nav_gesture_cancel();
        nav.bottom_start = lv_event_get_target_obj(event) == nav.bottom;
        if (nav.bottom_start && !nav.home_allowed) return;
        nav.right_start = lv_event_get_target_obj(event) == nav.right;
        nav.origin = point;
        nav.input = input;
        nav.active = true;
        int32_t size = px(60);
        int32_t x = nav.bottom_start ? (nav.width - size) / 2 :
                    nav.right_start ? nav.width - size - px(24) : px(24);
        int32_t y = nav.bottom_start ? nav.height - size - px(48) :
                    LV_CLAMP(px(TOP_HEIGHT), point.y - size / 2, nav.height - size - px(32));
        lv_obj_set_pos(nav.hint, x, y);
        return;
    }
    if (!nav.active || input != nav.input) return;
    if (code == LV_EVENT_PRESS_LOST || nav.screen != lv_screen_active()) {
        ui_nav_gesture_cancel();
        return;
    }
    if (code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED) return;
    int32_t dx = point.x - nav.origin.x, dy = point.y - nav.origin.y;
    int32_t forward = nav.bottom_start ? -dy : nav.right_start ? -dx : dx;
    int32_t cross = nav.bottom_start ? LV_ABS(dx) : LV_ABS(dy);
    /* An initial sideways/vertical drag is never reclassified as navigation. */
    if (!nav.reached && cross > px(32) && cross > forward) {
        ui_nav_gesture_cancel();
        return;
    }
    bool armed = forward >= px(nav.bottom_start ? HOME_DISTANCE : BACK_DISTANCE) && forward * 2 > cross * 3;
    ui_nav_action_t action = UI_NAV_NONE;
    if (armed) {
        if (!nav.reached || LV_ABS(point.x - nav.anchor.x) > px(HOLD_SLOP) ||
            LV_ABS(point.y - nav.anchor.y) > px(HOLD_SLOP)) {
            nav.anchor = point;
            nav.quiet_since = lv_tick_get();
        }
        nav.reached = true;
        action = !nav.bottom_start ? UI_NAV_BACK :
                 lv_tick_elaps(nav.quiet_since) >= HOLD_MS ? UI_NAV_RECENT : UI_NAV_HOME;
    } else nav.reached = false;
    preview(action);
    if (code == LV_EVENT_RELEASED || action == UI_NAV_RECENT) {
        ui_nav_gesture_cancel();
        /* Reserved edge taps must not become clicks or play a button tone. */
        if (code == LV_EVENT_RELEASED) lv_indev_reset(input, NULL);
        if (action != UI_NAV_NONE && nav.dispatch) nav.dispatch(action);
    }
}

static lv_obj_t *object(lv_obj_t *parent, bool touch)
{
    lv_obj_t *out = lv_obj_create(parent);
    lv_obj_remove_style_all(out);
    lv_obj_remove_flag(out, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE |
                           LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    if (touch) {
        lv_obj_add_flag(out, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_add_event_cb(out, edge_event, LV_EVENT_ALL, NULL);
    }
    return out;
}

void ui_nav_gesture_init(void (*dispatch)(ui_nav_action_t action))
{
    if (nav.root) return;
    nav.dispatch = dispatch;
    nav.root = object(lv_layer_top(), false);
    nav.left = object(nav.root, true);
    nav.right = object(nav.root, true);
    nav.bottom = object(nav.root, true);
    nav.handle = object(nav.root, false);
    lv_obj_set_style_bg_color(nav.handle, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(nav.handle, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(nav.handle, LV_RADIUS_CIRCLE, 0);
    nav.hint = object(nav.root, false);
    lv_obj_set_style_bg_color(nav.hint, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(nav.hint, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(nav.hint, lv_color_black(), 0);
    lv_obj_set_style_border_width(nav.hint, 1, 0);
    lv_obj_set_style_radius(nav.hint, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(nav.hint, hint_draw, LV_EVENT_DRAW_MAIN, NULL);
    show(nav.root, false);
    ui_nav_gesture_cancel();
}

void ui_nav_gesture_update(bool enabled, bool home_allowed, unsigned scale, unsigned scope)
{
    if (!nav.root) return;
    lv_obj_t *screen = lv_screen_active();
    int32_t width = lv_display_get_horizontal_resolution(NULL);
    int32_t height = lv_display_get_vertical_resolution(NULL);
    if (!scale) scale = 100;
    bool layout = nav.scale != scale || nav.width != width || nav.height != height;
    if (layout || nav.screen != screen || nav.scope != scope || nav.enabled != enabled || nav.home_allowed != home_allowed)
        ui_nav_gesture_cancel();
    nav.screen = screen;
    nav.scope = scope;
    nav.enabled = enabled;
    nav.home_allowed = home_allowed;
    nav.scale = scale;
    nav.width = width;
    nav.height = height;
    if (layout) {
        int32_t edge = px(SIDE_WIDTH), bottom = px(BOTTOM_HEIGHT), top = px(TOP_HEIGHT);
        lv_obj_set_size(nav.root, width, height);
        lv_obj_set_pos(nav.left, 0, top);
        lv_obj_set_size(nav.left, edge, height - top - bottom);
        lv_obj_set_pos(nav.right, width - edge, top);
        lv_obj_set_size(nav.right, edge, height - top - bottom);
        lv_obj_set_pos(nav.bottom, 0, height - bottom);
        lv_obj_set_size(nav.bottom, width, bottom);
        lv_obj_set_pos(nav.handle, (width - px(440)) / 2, height - px(10));
        lv_obj_set_size(nav.handle, px(440), px(4));
        lv_obj_set_size(nav.hint, px(60), px(60));
    }
    show(nav.root, enabled);
    show(nav.bottom, home_allowed);
    show(nav.handle, home_allowed);
    if (enabled) lv_obj_move_to_index(nav.root, -1);
}
