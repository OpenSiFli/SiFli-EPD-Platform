#include "ui_nav_gesture.h"

enum { SIDE_WIDTH = 20, BOTTOM_HEIGHT = 24, TOP_HEIGHT = 64,
       BACK_DISTANCE = 64, HOME_DISTANCE = 72, HOLD_MS = 350, HOLD_SLOP = 12 };
static struct {
    lv_obj_t *root, *handle, *hint, *screen, *contact_screen;
    lv_indev_t *input;
    lv_indev_t *pointer;
    lv_indev_read_cb_t read;
    void (*dispatch)(ui_nav_action_t action);
    lv_point_t origin, anchor;
    int32_t width, height;
    unsigned scale, scope;
    uint32_t quiet_since;
    ui_nav_action_t preview;
    bool enabled, home_allowed, active, bottom_start, right_start, reached;
    bool pointer_down, claimed;
} nav;

static int32_t px(int32_t value) { return (value * (int32_t)nav.scale + 50) / 100; }
static void show(lv_obj_t *object, bool visible)
{
    if (visible) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

static void contact_deleted(lv_event_t *event)
{
    if (lv_event_get_target(event) != nav.contact_screen) return;
    nav.contact_screen = nav.screen = NULL;
    ui_nav_gesture_cancel();
}

void ui_nav_gesture_cancel(void)
{
    lv_obj_t *screen = nav.contact_screen;
    nav.contact_screen = NULL;
    if (screen) lv_obj_remove_event_cb(screen, contact_deleted);
    if (nav.active && nav.input && nav.claimed &&
        lv_indev_get_state(nav.input) == LV_INDEV_STATE_PRESSED)
        lv_indev_wait_release(nav.input);
    nav.active = nav.reached = false;
    nav.claimed = false;
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

/* Observe edge-origin drags before widget dispatch. A tap keeps its page
 * target; only a directional drag takes ownership of the contact. */
static void pointer_read(lv_indev_t *input, lv_indev_data_t *data)
{
    nav.read(input, data);
    bool down = data->state == LV_INDEV_STATE_PRESSED;
    bool start = down && !nav.pointer_down;
    nav.pointer_down = down;
    if (!nav.enabled) return;
    lv_point_t point = data->point;
    lv_display_t *display = lv_indev_get_display(input);
    lv_display_rotation_t rotation = lv_display_get_rotation(display);
    int32_t w = lv_display_get_original_horizontal_resolution(display);
    int32_t h = lv_display_get_original_vertical_resolution(display);
    if (rotation == LV_DISPLAY_ROTATION_180 || rotation == LV_DISPLAY_ROTATION_270) {
        point.x = w - point.x - 1; point.y = h - point.y - 1;
    }
    if (rotation == LV_DISPLAY_ROTATION_90 || rotation == LV_DISPLAY_ROTATION_270) {
        int32_t y = point.y; point.y = point.x; point.x = h - y - 1;
    }
    if (start) {
        ui_nav_gesture_cancel();
        bool bottom = nav.home_allowed && point.y >= nav.height - px(BOTTOM_HEIGHT);
        bool side = point.y >= px(TOP_HEIGHT) && point.y < nav.height - px(BOTTOM_HEIGHT) &&
                    (point.x < px(SIDE_WIDTH) || point.x >= nav.width - px(SIDE_WIDTH));
        if (bottom || side) {
            /* Navigation can replace the screen after the last shell update.
             * Bind this new contact to its actual owner, and cancel on DELETE
             * before an allocator can reuse that screen address. */
            lv_obj_t *screen = lv_screen_active();
            if (!screen || !lv_obj_add_event_cb(screen, contact_deleted, LV_EVENT_DELETE, NULL)) return;
            nav.contact_screen = nav.screen = screen;
            nav.bottom_start = bottom;
            nav.right_start = point.x >= nav.width - px(SIDE_WIDTH);
            nav.active = true;
            nav.input = input; nav.origin = point;
            int32_t size = px(60);
            lv_obj_set_pos(nav.hint, bottom ? (nav.width - size) / 2 :
                nav.right_start ? nav.width - size - px(24) : px(24),
                bottom ? nav.height - px(108) :
                LV_CLAMP(px(TOP_HEIGHT), point.y - size / 2, nav.height - size - px(32)));
        }
    }
    if (!nav.active || nav.input != input) return;
    if (nav.screen != lv_screen_active()) { ui_nav_gesture_cancel(); return; }
    int32_t dx = point.x - nav.origin.x, dy = point.y - nav.origin.y;
    int32_t forward = nav.bottom_start ? -dy : nav.right_start ? -dx : dx;
    int32_t cross = nav.bottom_start ? LV_ABS(dx) : LV_ABS(dy);
    if (!nav.claimed && cross > px(24) && cross > forward) {
        ui_nav_gesture_cancel(); return;
    }
    if (!nav.claimed && forward >= px(24) && forward * 2 > cross * 3) {
        nav.claimed = true;
        lv_indev_reset(input, NULL);
        lv_indev_wait_release(input);
    }
    bool armed = forward >= px(nav.bottom_start ? HOME_DISTANCE : BACK_DISTANCE) && forward * 2 > cross * 3;
    ui_nav_action_t action = UI_NAV_NONE;
    if (armed) {
        if (!nav.reached || LV_ABS(point.x - nav.anchor.x) > px(HOLD_SLOP) ||
            LV_ABS(point.y - nav.anchor.y) > px(HOLD_SLOP)) {
            nav.anchor = point; nav.quiet_since = lv_tick_get();
        }
        nav.reached = true;
        action = !nav.bottom_start ? UI_NAV_BACK : lv_tick_elaps(nav.quiet_since) >= HOLD_MS ? UI_NAV_RECENT : UI_NAV_HOME;
    } else nav.reached = false;
    preview(action);
    if (!down || action == UI_NAV_RECENT) {
        bool claimed = nav.claimed;
        ui_nav_gesture_cancel();
        if (claimed) { lv_indev_reset(input, NULL); lv_indev_wait_release(input); }
        if (action != UI_NAV_NONE && nav.dispatch) nav.dispatch(action);
    }
}

static lv_obj_t *object(lv_obj_t *parent)
{
    lv_obj_t *out = lv_obj_create(parent);
    lv_obj_remove_style_all(out);
    lv_obj_remove_flag(out, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE |
                           LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    return out;
}

void ui_nav_gesture_init(void (*dispatch)(ui_nav_action_t action))
{
    if (nav.root) return;
    nav.dispatch = dispatch;
    nav.root = object(lv_layer_top());
    nav.handle = object(nav.root);
    lv_obj_set_style_bg_color(nav.handle, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(nav.handle, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(nav.handle, LV_RADIUS_CIRCLE, 0);
    nav.hint = object(nav.root);
    lv_obj_set_style_bg_color(nav.hint, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(nav.hint, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(nav.hint, lv_color_black(), 0);
    lv_obj_set_style_border_width(nav.hint, 1, 0);
    lv_obj_set_style_radius(nav.hint, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(nav.hint, hint_draw, LV_EVENT_DRAW_MAIN, NULL);
    show(nav.root, false);
    ui_nav_gesture_cancel();
    for (lv_indev_t *input = lv_indev_get_next(NULL); input; input = lv_indev_get_next(input)) {
        if (lv_indev_get_type(input) == LV_INDEV_TYPE_POINTER && lv_indev_get_read_cb(input)) {
            nav.pointer = input; nav.read = lv_indev_get_read_cb(input);
            lv_indev_set_read_cb(input, pointer_read);
            break;
        }
    }
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
        lv_obj_set_size(nav.root, width, height);
        lv_obj_set_pos(nav.handle, (width - px(440)) / 2, height - px(10));
        lv_obj_set_size(nav.handle, px(440), px(4));
        lv_obj_set_size(nav.hint, px(60), px(60));
    }
    show(nav.root, enabled);
    show(nav.handle, home_allowed);
    if (enabled) lv_obj_move_to_index(nav.root, -1);
}
