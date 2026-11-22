void epd_obj_init(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_add_style(obj, &epd_style, LV_PART_MAIN);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC |
                      LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}
lv_obj_t *label_create(lv_obj_t *parent, const char *text, int x, int y,
                              int width, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    /* Configure geometry with no text so font and size changes do not repeatedly
     * measure the default label or the business string. */
    lv_label_set_text(label, "");
    epd_obj_init(label);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x222222), 0);
    lv_obj_set_pos(label, x, y);
    if (width > 0)
    {
        lv_obj_set_width(label, width);
        lv_obj_set_height(label, font->line_height);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    }
    text_update(label, text);
    return label;
}
lv_obj_t *panel_create(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *panel = lv_obj_create(parent);
    epd_obj_init(panel);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, width, height);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x999999), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    return panel;
}
lv_obj_t *centered_label(lv_obj_t *parent, const char *text, int x, int y,
                                int width, const lv_font_t *font)
{
    lv_obj_t *label = label_create(parent, text, x, y, width, font);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}
lv_obj_t *epd_app_button(lv_obj_t *parent, int x, int y, int width, int height,
                         const char *text, lv_event_cb_t callback, void *data)
{
    lv_obj_t *button = panel_create(parent, x, y, width, height);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_border_color(button, lv_color_black(), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(button, lv_color_black(), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(button, 3, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(button, -4, LV_STATE_FOCUS_KEY);
    if (callback) lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, data);
    epd_input_add_object(button);
    if (text)
    {
        lv_obj_t *label = centered_label(button, text, 0, 0, width - 24, ui_font_body());
        lv_obj_center(label);
    }
    return button;
}
