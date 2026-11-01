#include "reader_note_ui.h"
#include "reader_i18n.h"
#include "epd_memory.h"
#include <string.h>

#define NOTE_TEXT_CAPACITY 4096u
#define NOTE_WAIT_MS 10000u

static struct {
    lv_obj_t *root, *bubble, *scroll, *label, *tail;
    lv_timer_t *timer;
    document_note_request_t *request;
    lv_area_t anchor;
    uint32_t started;
    char *text;
    int32_t tail_base, tail_tip, tail_half;
    bool above;
} note;

bool epd_reader_note_visible(void) { return note.root != NULL; }

static void note_deleted(lv_event_t *event)
{
    if (lv_event_get_target_obj(event) != note.root) return;
    if (note.timer) lv_timer_delete(note.timer);
    document_note_cancel(note.request);
    epd_app_free(note.text);
    memset(&note, 0, sizeof(note));
}

void epd_reader_note_close(void)
{
    if (note.root) lv_obj_delete(note.root);
}

static void note_close_clicked(lv_event_t *event)
{
    lv_event_stop_bubbling(event);
    epd_reader_note_close();
}

static void note_outside(lv_event_t *event)
{
    if (lv_event_get_target_obj(event) == note.root) epd_reader_note_close();
}

static void note_key(epd_app_key_t key, epd_app_key_event_t event, void *data)
{
    (void)data;
    if (event != EPD_APP_KEY_CLICKED || !note.scroll) return;
    if (key != EPD_APP_KEY2 && key != EPD_APP_KEY3) return;
    int32_t step = lv_obj_get_height(note.scroll) * 3 / 4;
    int32_t y = lv_obj_get_scroll_y(note.scroll);
    int32_t end = y + lv_obj_get_scroll_bottom(note.scroll);
    int32_t target = y + (key == EPD_APP_KEY2 ? -step : step);
    if (target > end) target = end;
    if (target < 0) target = 0;
    lv_obj_scroll_to_y(note.scroll, target, LV_ANIM_OFF);
}

static int32_t limited(int32_t value, int32_t low, int32_t high)
{
    return value < low ? low : value > high ? high : value;
}

static void note_tail(lv_event_t *event)
{
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_target_obj(event), &area);
    lv_draw_triangle_dsc_t draw;
    lv_draw_triangle_dsc_init(&draw);
    draw.color = lv_color_hex(0x333333);
    draw.opa = LV_OPA_COVER;
    int32_t base = note.above ? area.y1 : area.y2;
    int32_t tip = note.above ? area.y2 : area.y1;
    draw.p[0] = (lv_point_precise_t){area.x1 + note.tail_base - note.tail_half, base};
    draw.p[1] = (lv_point_precise_t){area.x1 + note.tail_base + note.tail_half, base};
    draw.p[2] = (lv_point_precise_t){area.x1 + note.tail_tip, tip};
    lv_draw_triangle(lv_event_get_layer(event), &draw);
}

static void note_layout(void)
{
    lv_obj_update_layout(note.root);
    int32_t width = lv_obj_get_width(note.root), height = lv_obj_get_height(note.root);
    int32_t margin = epd_ui_px(16), pad = epd_ui_px(22), tail = epd_ui_px(14);
    int32_t top = note.anchor.y1 - margin - tail, bottom = height - note.anchor.y2 - margin - tail;
    int32_t available = top > bottom ? top : bottom;
    note.above = top >= bottom;
    int32_t box_width = width * 4 / 5;
    box_width = limited(box_width, width / 2, width - margin * 2);
    lv_obj_set_width(note.label, box_width - 2 * pad);
    lv_obj_update_layout(note.label);
    int32_t header = epd_ui_touch(42);
    int32_t wanted = lv_obj_get_height(note.label) + header + pad * 2;
    int32_t maximum = height * 3 / 5;
    if (maximum > available) maximum = available;
    int32_t box_height = limited(wanted, header + pad * 2 + epd_ui_px(32), maximum);
    int32_t center = (note.anchor.x1 + note.anchor.x2) / 2;
    int32_t x = limited(center - box_width / 2, margin, width - margin - box_width);
    int32_t y = note.above ? note.anchor.y1 - tail - box_height : note.anchor.y2 + tail;
    y = limited(y, margin, height - margin - box_height);
    lv_obj_set_pos(note.bubble, x, y);
    lv_obj_set_size(note.bubble, box_width, box_height);
    lv_obj_set_pos(note.scroll, pad, header + pad / 2);
    lv_obj_set_size(note.scroll, box_width - 2 * pad, box_height - header - pad * 3 / 2);
    int32_t base = limited(center, x + pad + tail, x + box_width - pad - tail);
    int32_t left = base - tail < center ? base - tail : center;
    int32_t right = base + tail > center ? base + tail : center;
    note.tail_base = base - left; note.tail_tip = center - left; note.tail_half = tail;
    lv_obj_set_pos(note.tail, left, note.above ? y + box_height - 1 : y - tail + 1);
    lv_obj_set_size(note.tail, right - left + 1, tail);
    lv_obj_invalidate(note.tail);
}

static void note_poll(lv_timer_t *timer)
{
    (void)timer;
    bool truncated = false;
    int result = note.request ? document_note_take(note.request, note.text, NOTE_TEXT_CAPACITY + 1, &truncated) : -1;
    if (!result && lv_tick_elaps(note.started) < NOTE_WAIT_MS) return;
    document_note_cancel(note.request); note.request = NULL;
    lv_timer_delete(note.timer); note.timer = NULL;
    if (result == 1 && note.text[0]) {
        lv_label_set_text(note.label, note.text);
        if (truncated) {
            size_t size = strlen(note.text);
            if (size + 4 < NOTE_TEXT_CAPACITY) { memcpy(note.text + size, "\n…", 5); lv_label_set_text(note.label, note.text); }
        }
    } else lv_label_set_text(note.label, epd_get_str(reader_note_unavailable));
    epd_app_free(note.text); note.text = NULL;
    note_layout();
}

void reader_note_ui_open(lv_obj_t *screen, document_t *document,
                         uint32_t link_record, const lv_area_t *anchor)
{
    epd_reader_note_close();
    if (!screen || !document || !anchor) return;
    note.root = lv_obj_create(screen);
    if (!note.root) return;
    lv_obj_remove_style_all(note.root);
    lv_obj_set_size(note.root, lv_obj_get_width(screen), lv_obj_get_height(screen));
    lv_obj_remove_flag(note.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(note.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(note.root, note_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(note.root, note_outside, LV_EVENT_CLICKED, NULL);
    epd_app_input_group(note.root);
    epd_app_input_listen(note.root, note_key, NULL);
    lv_area_t screen_area;
    lv_obj_get_coords(screen, &screen_area);
    note.anchor = *anchor;
    note.anchor.x1 -= screen_area.x1; note.anchor.x2 -= screen_area.x1;
    note.anchor.y1 -= screen_area.y1; note.anchor.y2 -= screen_area.y1;
    note.bubble = lv_obj_create(note.root);
    if (!note.bubble) { epd_reader_note_close(); return; }
    lv_obj_remove_style_all(note.bubble);
    lv_obj_set_style_bg_color(note.bubble, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(note.bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(note.bubble, epd_ui_px(22), 0);
    lv_obj_remove_flag(note.bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(note.bubble, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *close = epd_app_button(note.bubble, 0, epd_ui_px(4), epd_ui_touch(64), epd_ui_touch(42),
                                    epd_get_str(reader_close), note_close_clicked, NULL);
    if (!close) { epd_reader_note_close(); return; }
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -epd_ui_px(10), epd_ui_px(4));
    lv_obj_set_style_bg_opa(close, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(close, 0, LV_PART_MAIN);
    lv_obj_set_style_text_color(close, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(close, epd_app_font_role(EPD_FONT_SMALL), LV_PART_MAIN);
    lv_obj_set_style_pad_all(close, 0, LV_PART_MAIN);
    lv_obj_t *close_label = lv_obj_get_child(close, 0);
    if (close_label) {
        lv_obj_set_style_text_color(close_label, lv_color_white(), 0);
        lv_obj_set_style_text_font(close_label, epd_app_font_role(EPD_FONT_SMALL), 0);
        lv_obj_set_width(close_label, LV_SIZE_CONTENT);
        lv_obj_center(close_label);
    }
    epd_app_input_back(note.root, close);
    note.scroll = lv_obj_create(note.bubble);
    if (!note.scroll) { epd_reader_note_close(); return; }
    lv_obj_remove_style_all(note.scroll);
    lv_obj_set_scroll_dir(note.scroll, LV_DIR_VER);
    lv_obj_remove_flag(note.scroll, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_set_scrollbar_mode(note.scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(note.scroll, lv_color_white(), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(note.scroll, LV_OPA_COVER, LV_PART_SCROLLBAR);
    note.label = lv_label_create(note.scroll);
    if (!note.label) { epd_reader_note_close(); return; }
    lv_obj_set_style_text_font(note.label, epd_app_font_role(EPD_FONT_BODY), 0);
    lv_obj_set_style_text_color(note.label, lv_color_white(), 0);
    lv_obj_set_style_text_line_space(note.label, epd_ui_px(8), 0);
    lv_label_set_text(note.label, epd_get_str(reader_note_loading));
    note.tail = lv_obj_create(note.root);
    if (!note.tail) { epd_reader_note_close(); return; }
    lv_obj_remove_style_all(note.tail);
    lv_obj_remove_flag(note.tail, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(note.tail, note_tail, LV_EVENT_DRAW_MAIN, NULL);
    note_layout();
    note.text = epd_app_alloc(NOTE_TEXT_CAPACITY + 1, EPD_APP_PSRAM);
    note.request = note.text ? document_note_request(document, link_record) : NULL;
    note.started = lv_tick_get();
    note.timer = lv_timer_create(note_poll, 60, NULL);
    if (!note.timer) {
        document_note_cancel(note.request); note.request = NULL;
        epd_app_free(note.text); note.text = NULL;
        lv_label_set_text(note.label, epd_get_str(reader_note_unavailable));
        note_layout();
    }
}
