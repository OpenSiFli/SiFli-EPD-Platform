#include "ui_settings_list.h"
#include "ui_separator.h"
#include "src/misc/lv_text_private.h"
#include <stdint.h>
#include <string.h>

typedef struct {
    ui_settings_list_row_t item;
    struct ui_settings_list *owner;
    char *icon_text;
} settings_row_t;

typedef struct { uint32_t start, length; } text_line_t;
typedef struct {
    text_line_t *title, *value;
    size_t titles, values;
    int32_t x, width, height;
    bool inline_value;
    int32_t value_x, value_width;
} row_layout_t;

typedef struct {
    size_t row, page, title, titles, value, values;
    int32_t y, height;
} row_fragment_t;

typedef struct {
    row_layout_t *rows;
    row_fragment_t *fragments;
    size_t count, fragments_count, pages;
    int32_t width, height, content_height, footer_height, margin, right_margin, spacing;
    size_t dot_columns;
    lv_font_t body, small_font;
} settings_layout_t;

struct ui_settings_list {
    lv_obj_t *parent, *object, *view;
    settings_row_t *rows;
    size_t count, page;
    settings_layout_t layout;
    const lv_font_t *body, *small_font;
    lv_group_t *group;
    ui_settings_list_cb_t callback;
    void *user_data;
    lv_coord_t left, top, right, bottom;
    int anchor;
    bool has_anchor, busy, deleting, pressed, cancel_click;
    lv_point_t origin;
};

enum { ROW_PAD = 16, VALUE_GAP = 8, DOT_SIZE = 10, DOT_PITCH = 22, DOT_PAD = 12 };

static void list_event(lv_event_t *event);
static void row_event(lv_event_t *event);
static void parent_event(lv_event_t *event);
static void pointer_event(ui_settings_list_t *list, lv_event_t *event);

static int32_t larger(int32_t a, int32_t b) { return a > b ? a : b; }
static int32_t smaller(int32_t a, int32_t b) { return a < b ? a : b; }

lv_coord_t ui_settings_list_content_margin(lv_coord_t width)
{
    return larger(0, smaller(42, width / 12));
}

static void *array_alloc(size_t count, size_t size)
{
    if (!count || count > SIZE_MAX / size) return NULL;
    void *memory = lv_malloc(count * size);
    if (memory) memset(memory, 0, count * size);
    return memory;
}

static char *copy_text(const char *text)
{
    if (!text) text = "";
    size_t length = strlen(text);
    if (length >= UINT32_MAX) return NULL;
    char *copy = lv_malloc(length + 1);
    if (copy) memcpy(copy, text, length + 1);
    return copy;
}

static void rows_free(settings_row_t *rows, size_t count)
{
    if (!rows) return;
    for (size_t i = 0; i < count; ++i)
    {
        lv_free((void *)rows[i].item.title);
        lv_free((void *)rows[i].item.value);
        lv_free(rows[i].icon_text);
    }
    lv_free(rows);
}

static bool text_equal(const char *a, const char *b)
{
    return strcmp(a ? a : "", b ? b : "") == 0;
}

static bool rows_equal(const ui_settings_list_t *list,
                       const ui_settings_list_row_t *rows, size_t count)
{
    if (list->count != count) return false;
    for (size_t i = 0; i < count; ++i)
    {
        const ui_settings_list_row_t *a = &list->rows[i].item, *b = &rows[i];
        if (a->key != b->key || a->kind != b->kind || a->enabled != b->enabled ||
            a->checked != b->checked || a->min_height != b->min_height || a->icon_size != b->icon_size ||
            a->no_chevron != b->no_chevron || a->bottom_gap != b->bottom_gap ||
            a->no_separator != b->no_separator ||
            a->trailing_icon != b->trailing_icon || a->control_size != b->control_size ||
            a->muted_title != b->muted_title || a->black_value != b->black_value ||
            a->joined_value != b->joined_value ||
            !text_equal(a->title, b->title) || !text_equal(a->value, b->value)) return false;
        if (list->rows[i].icon_text)
        {
            if (!b->icon || lv_image_src_get_type(b->icon) == LV_IMAGE_SRC_VARIABLE ||
                !text_equal(list->rows[i].icon_text, b->icon)) return false;
        }
        else if (a->icon != b->icon) return false;
    }
    return true;
}

static settings_row_t *rows_copy(ui_settings_list_t *list,
                                 const ui_settings_list_row_t *source, size_t count)
{
    settings_row_t *rows = array_alloc(count, sizeof(*rows));
    if (!rows) return NULL;
    for (size_t i = 0; i < count; ++i)
    {
        if (source[i].kind < UI_SETTINGS_ROW_LINK || source[i].kind > UI_SETTINGS_ROW_SECTION ||
            source[i].min_height < 0 || source[i].bottom_gap < 0 || source[i].icon_size < 0 ||
            source[i].control_size < 0 || (source[i].trailing_icon && !source[i].control_size)) goto failed;
        for (size_t j = 0; j < i; ++j) if (source[i].key == source[j].key) goto failed;
        rows[i].item = source[i];
        rows[i].item.title = NULL;
        rows[i].item.value = NULL;
        rows[i].owner = list;
        rows[i].item.title = copy_text(source[i].title);
        rows[i].item.value = copy_text(source[i].value);
        if (!rows[i].item.title || !rows[i].item.value) goto failed;
        if (source[i].icon && lv_image_src_get_type(source[i].icon) != LV_IMAGE_SRC_VARIABLE)
        {
            rows[i].icon_text = copy_text(source[i].icon);
            if (!rows[i].icon_text) goto failed;
            rows[i].item.icon = rows[i].icon_text;
        }
    }
    return rows;
failed:
    rows_free(rows, count);
    return NULL;
}

static void layout_free(settings_layout_t *layout)
{
    if (layout->rows)
        for (size_t i = 0; i < layout->count; ++i)
        {
            lv_free(layout->rows[i].title);
            lv_free(layout->rows[i].value);
        }
    lv_free(layout->rows);
    lv_free(layout->fragments);
    memset(layout, 0, sizeof(*layout));
}

/* Keep LVGL's UTF-8 and word-boundary decisions, including every source byte. */
static bool wrap_text(const char *text, const lv_font_t *font, int32_t width,
                      text_line_t **lines, size_t *count)
{
    lv_text_attributes_t attributes;
    lv_text_attributes_init(&attributes);
    attributes.max_width = width;
    attributes.letter_space = 0;
    attributes.text_flags = LV_TEXT_FLAG_NONE;
    uint32_t length = (uint32_t)strlen(text), offset = 0;
    size_t capacity = 0;
    *lines = NULL;
    *count = 0;
    while (offset < length)
    {
        uint32_t step = lv_text_get_next_line(text + offset, length - offset, font, NULL, &attributes);
        if (!step || step > length - offset) return false;
        if (*count == capacity)
        {
            if (capacity > SIZE_MAX / 2 / sizeof(**lines)) return false;
            size_t next_capacity = capacity ? capacity * 2 : 1;
            text_line_t *next = lv_realloc(*lines, next_capacity * sizeof(**lines));
            if (!next) return false;
            *lines = next;
            capacity = next_capacity;
        }
        (*lines)[(*count)++] = (text_line_t){offset, step};
        offset += step;
    }
    return true;
}

static int64_t text_height(size_t lines, const lv_font_t *font, int32_t spacing)
{
    if (!lines) return 0;
    return (int64_t)lines * (font->line_height + spacing) - spacing;
}

static int32_t value_gap(const ui_settings_list_row_t *item)
{
    return item->joined_value ? 0 : VALUE_GAP;
}

static int64_t fragment_height(const ui_settings_list_t *list, const ui_settings_list_row_t *item,
                               size_t titles, size_t values, int32_t spacing)
{
    return 2 * ROW_PAD + text_height(titles, list->body, spacing) +
        text_height(values, list->small_font, spacing) + (titles && values ? value_gap(item) : 0);
}

static int64_t continued_height(const ui_settings_list_t *list, const ui_settings_list_row_t *item,
                                 size_t titles, size_t values, int32_t spacing)
{
    const lv_font_t *font = item->kind == UI_SETTINGS_ROW_SECTION ? list->small_font : list->body;
    int64_t content = text_height(titles, font, spacing) +
        text_height(values, list->small_font, spacing) + (titles && values ? value_gap(item) : 0);
    int32_t icon_size = item->icon_size ? item->icon_size : 60;
    if (item->icon && content < icon_size) content = icon_size;
    return content + (item->bottom_gap ? item->bottom_gap : 2 * ROW_PAD);
}

static bool append_fragment(settings_layout_t *layout, row_fragment_t fragment, bool write)
{
    if (layout->fragments_count == SIZE_MAX) return false;
    if (write) layout->fragments[layout->fragments_count] = fragment;
    ++layout->fragments_count;
    return true;
}

static bool paginate(const ui_settings_list_t *list, const settings_row_t *rows,
                     settings_layout_t *layout, bool write)
{
    size_t page = 0;
    int32_t y = 0, capacity = layout->content_height;
    layout->fragments_count = 0;
    for (size_t i = 0; i < layout->count; ++i)
    {
        const row_layout_t *row = &layout->rows[i];
        const ui_settings_list_row_t *item = &rows[i].item;
        if (rows[i].item.kind == UI_SETTINGS_ROW_SECTION && i + 1 < layout->count && y)
        {
            int64_t group = (int64_t)row->height + layout->rows[i + 1].height;
            if (group <= capacity && group > capacity - y) { ++page; y = 0; }
        }
        if (row->height <= capacity)
        {
            if (y && row->height > capacity - y) { ++page; y = 0; }
            row_fragment_t f = {i, page, 0, row->titles, 0, row->values, y, row->height};
            if (!append_fragment(layout, f, write)) return false;
            y += row->height;
            continue;
        }
        if (y) { ++page; y = 0; }
        size_t title = 0, value = 0;
        do
        {
            size_t old_title = title, old_value = value;
            row_fragment_t f = {i, page, title, 0, value, 0, 0, 0};
            if (title == row->titles && row->titles)
            {
                /* Repeat the whole title when it fits with one value line;
                 * exceptionally long titles retain their first line as context. */
                f.title = 0;
                f.titles = continued_height(list, item, row->titles, 1, layout->spacing) <= capacity ?
                    row->titles : 1;
            }
            while (title < row->titles &&
                   continued_height(list, item, f.titles + 1, 0, layout->spacing) <= capacity)
            { ++f.titles; ++title; }
            while (title == row->titles && value < row->values &&
                   continued_height(list, item, f.titles, f.values + 1, layout->spacing) <= capacity)
            { ++f.values; ++value; }
            if (title == old_title && value == old_value) return false;
            f.height = (int32_t)continued_height(list, item, f.titles, f.values, layout->spacing);
            if (!append_fragment(layout, f, write)) return false;
            y = f.height;
            if (title < row->titles || value < row->values) { ++page; y = 0; }
            else break;
            /* A value continuation must make progress even in a very short viewport. */
            if (title == row->titles && value < row->values &&
                continued_height(list, item, row->titles ? 1 : 0, 1, layout->spacing) > capacity) return false;
        } while (true);
    }
    layout->pages = page + 1;
    return true;
}

/* Keep value snapshots: a caller-owned font can be resized at the same address. */
static bool font_equal(const lv_font_t *a, const lv_font_t *b)
{
    return a->line_height == b->line_height && a->base_line == b->base_line &&
        a->get_glyph_dsc == b->get_glyph_dsc && a->get_glyph_bitmap == b->get_glyph_bitmap &&
        a->release_glyph == b->release_glyph && a->dsc == b->dsc && a->fallback == b->fallback &&
        a->user_data == b->user_data && a->kerning == b->kerning && a->subpx == b->subpx &&
        a->static_bitmap == b->static_bitmap && a->underline_position == b->underline_position &&
        a->underline_thickness == b->underline_thickness;
}

static bool layout_fonts_equal(const settings_layout_t *layout,
                               const lv_font_t *body, const lv_font_t *small_font)
{
    return font_equal(&layout->body, body) && font_equal(&layout->small_font, small_font);
}

/* These inputs affect row measurements; selection, enabled state and colors
 * only affect drawing and group membership. */
static bool row_geometry_equal(const ui_settings_list_row_t *a, const ui_settings_list_row_t *b)
{
    return a->kind == b->kind && a->min_height == b->min_height &&
        (a->icon != NULL) == (b->icon != NULL) && a->icon_size == b->icon_size &&
        a->control_size == b->control_size && a->no_chevron == b->no_chevron &&
        a->bottom_gap == b->bottom_gap && a->joined_value == b->joined_value &&
        text_equal(a->title, b->title) && text_equal(a->value, b->value);
}

static bool row_layout_copy(row_layout_t *row, const row_layout_t *source)
{
    *row = *source;
    row->title = row->value = NULL;
    if (source->titles)
    {
        row->title = array_alloc(source->titles, sizeof(*row->title));
        if (!row->title) return false;
        memcpy(row->title, source->title, source->titles * sizeof(*row->title));
    }
    if (source->values)
    {
        row->value = array_alloc(source->values, sizeof(*row->value));
        if (!row->value) return false;
        memcpy(row->value, source->value, source->values * sizeof(*row->value));
    }
    return true;
}

static bool layout_build(const ui_settings_list_t *list, settings_row_t *rows,
                          size_t count, settings_layout_t *layout, bool reuse_rows)
{
    memset(layout, 0, sizeof(*layout));
    reuse_rows = reuse_rows && layout_fonts_equal(&list->layout, list->body, list->small_font);
    layout->body = *list->body;
    layout->small_font = *list->small_font;
    layout->width = lv_obj_get_content_width(list->parent) - list->left - list->right;
    layout->height = lv_obj_get_content_height(list->parent) - list->top - list->bottom;
    layout->spacing = larger(4, list->body->line_height / 6);
    layout->margin = ui_settings_list_content_margin(layout->width);
    layout->right_margin = layout->margin;
    if (layout->width <= 0 || layout->height <= 0) return false;
    layout->count = count;
    if (count)
    {
        layout->rows = array_alloc(count, sizeof(*layout->rows));
        if (!layout->rows) return false;
    }
    for (size_t i = 0; i < count; ++i)
    {
        row_layout_t *row = &layout->rows[i];
        const ui_settings_list_row_t *item = &rows[i].item;
        if (reuse_rows && i < list->layout.count && layout->width == list->layout.width &&
            layout->spacing == list->layout.spacing && row_geometry_equal(item, &list->rows[i].item))
        {
            /* Own the copied line tables so a failed update can discard the
             * entire candidate layout without changing the displayed rows. */
            if (!row_layout_copy(row, &list->layout.rows[i])) goto failed;
            continue;
        }
        bool information = item->kind == UI_SETTINGS_ROW_INFO || item->kind == UI_SETTINGS_ROW_INLINE_INFO ||
                           item->kind == UI_SETTINGS_ROW_SECTION;
        int32_t control = information || (item->kind == UI_SETTINGS_ROW_LINK && item->no_chevron) ? 0 :
            item->kind == UI_SETTINGS_ROW_SWITCH ? 80 : 48;
        if (item->control_size) control = item->control_size * 11 / 6;
        int32_t icon_size = smaller(item->icon_size ? item->icon_size : 60, layout->width / 8);
        int32_t icon = item->icon ? icon_size + icon_size / 2 : 0;
        row->x = layout->margin + icon;
        row->width = layout->width - row->x - layout->right_margin - control;
        const lv_font_t *title_font = item->kind == UI_SETTINGS_ROW_SECTION ? list->small_font : list->body;
        row->value_width = row->width;
        row->value_x = row->x;
        if (item->kind == UI_SETTINGS_ROW_INLINE_INFO)
        {
            lv_point_t title_size, value_size;
            lv_text_get_size(&title_size, item->title, title_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            lv_text_get_size(&value_size, item->value, list->small_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            if (!strchr(item->title, '\n') && !strchr(item->value, '\n') &&
                (int64_t)title_size.x + value_size.x + 24 <= row->width)
            {
                row->inline_value = true;
                row->value_width = larger(1, value_size.x);
                row->value_x = row->x + row->width - row->value_width;
                row->width = larger(1, title_size.x);
            }
        }
        if (row->width <= 0 ||
            !wrap_text(item->title, title_font, row->width, &row->title, &row->titles) ||
            !wrap_text(item->value, list->small_font, row->value_width, &row->value, &row->values)) goto failed;
        int64_t height = fragment_height(list, item, row->titles, row->values, layout->spacing);
        if (row->inline_value) height = 2 * ROW_PAD + larger(title_font->line_height, list->small_font->line_height);
        if (item->kind == UI_SETTINGS_ROW_SECTION)
            height = 2 * ROW_PAD + text_height(row->titles, title_font, layout->spacing);
        if (item->bottom_gap || item->icon) height -= 2 * ROW_PAD;
        if (!information && !item->bottom_gap && !item->icon) height = height > 88 ? height : 88;
        int32_t icon_height = icon_size;
        if (item->icon && height < icon_height) height = icon_height;
        height += item->bottom_gap;
        if (height < item->min_height) height = item->min_height;
        if (height > INT32_MAX) goto failed;
        row->height = (int32_t)height;
    }
    layout->dot_columns = (size_t)larger(1, (layout->width - 2 * layout->margin) / DOT_PITCH);
    layout->content_height = layout->height;
    for (;;)
    {
        if (!paginate(list, rows, layout, false)) goto failed;
        size_t dot_rows = layout->pages > 1 ? (layout->pages - 1) / layout->dot_columns + 1 : 0;
        if (dot_rows > (size_t)layout->height / DOT_PITCH) goto failed;
        int32_t footer = dot_rows ? (int32_t)dot_rows * DOT_PITCH + 2 * DOT_PAD : 0;
        if (footer <= layout->footer_height) break;
        if (footer >= layout->height) goto failed;
        layout->footer_height = footer;
        layout->content_height = layout->height - footer;
    }
    if (layout->fragments_count)
    {
        layout->fragments = array_alloc(layout->fragments_count, sizeof(*layout->fragments));
        if (!layout->fragments || !paginate(list, rows, layout, true)) goto failed;
    }
    return true;
failed:
    layout_free(layout);
    return false;
}

static lv_obj_t *flat(lv_obj_t *parent, int32_t x, int32_t y, int32_t width, int32_t height)
{
    lv_obj_t *object = lv_obj_create(parent);
    if (!object) return NULL;
    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE |
                       LV_OBJ_FLAG_SCROLL_ON_FOCUS | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    return object;
}

static void text_deleted(lv_event_t *event)
{
    lv_free(lv_obj_get_user_data(lv_event_get_target_obj(event)));
}

static lv_color_t title_color(const ui_settings_list_row_t *item)
{
    if (item->muted_title) return lv_color_hex(0x767474);
    return lv_color_hex(item->enabled || item->kind == UI_SETTINGS_ROW_INFO ||
        item->kind == UI_SETTINGS_ROW_INLINE_INFO || item->kind == UI_SETTINGS_ROW_SECTION ? 0x000000 : 0x888888);
}

static bool draw_text_lines(lv_obj_t *parent, const char *text, const text_line_t *lines,
                            size_t first, size_t count, const lv_font_t *font,
                            int32_t x, int32_t *y, int32_t width, int32_t spacing, lv_color_t color)
{
    for (size_t i = first; i < first + count; ++i)
    {
        uint32_t length = lines[i].length;
        while (length && (text[lines[i].start + length - 1] == '\n' ||
                          text[lines[i].start + length - 1] == '\r')) --length;
        char *part = lv_malloc((size_t)length + 1);
        if (!part) return false;
        memcpy(part, text + lines[i].start, length);
        part[length] = 0;
        lv_obj_t *label = lv_label_create(parent);
        if (!label) { lv_free(part); return false; }
        if (!lv_obj_add_event_cb(label, text_deleted, LV_EVENT_DELETE, NULL))
        { lv_free(part); return false; }
        lv_obj_set_user_data(label, part);
        lv_obj_remove_style_all(label);
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, color, 0);
        lv_obj_set_style_text_letter_space(label, 0, 0);
        lv_obj_set_style_text_line_space(label, 0, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_label_set_text_static(label, part);
        lv_obj_set_pos(label, x, *y);
        lv_obj_set_size(label, width, font->line_height);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
        *y += font->line_height + spacing;
    }
    if (count) *y -= spacing;
    return true;
}

static void row_draw(lv_event_t *event, const settings_row_t *row)
{
    lv_obj_t *object = lv_event_get_current_target_obj(event);
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    int32_t margin = ui_settings_list_content_margin(lv_area_get_width(&area));
    int32_t right_margin = margin;
    lv_layer_t *layer = lv_event_get_layer(event);
    if (row->item.kind == UI_SETTINGS_ROW_SECTION) return;
    int32_t content_end = area.y2 - row->item.bottom_gap;
    if (!row->item.no_separator)
        ui_separator_draw(layer, area.x1 + margin, area.x2 - right_margin, content_end);
    if (row->item.trailing_icon)
    {
        int32_t size = row->item.control_size;
        int32_t cx = area.x2 + 1 - right_margin - size * 3 / 4;
        int32_t cy = (area.y1 + content_end) / 2;
        lv_draw_line_dsc_t divider;
        lv_draw_line_dsc_init(&divider);
        divider.color = lv_color_black();
        divider.width = UI_SEPARATOR_THICKNESS;
        divider.p1 = (lv_point_precise_t){cx - size * 5 / 6, cy - size / 2};
        divider.p2 = (lv_point_precise_t){cx - size * 5 / 6, cy + size / 2 - 1};
        lv_draw_line(layer, &divider);
        return;
    }
    if (row->item.kind == UI_SETTINGS_ROW_INFO || row->item.kind == UI_SETTINGS_ROW_INLINE_INFO ||
        (row->item.kind == UI_SETTINGS_ROW_LINK && row->item.no_chevron)) return;
    int32_t right = area.x2 - right_margin, cy = (area.y1 + content_end) / 2;
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_hex(row->item.enabled ? 0x111111 : 0x999999);
    line.width = 2;
    if (row->item.kind == UI_SETTINGS_ROW_LINK)
    {
        line.p1 = (lv_point_precise_t){right - 12, cy - 10};
        line.p2 = (lv_point_precise_t){right - 2, cy};
        lv_draw_line(layer, &line);
        line.p1 = line.p2;
        line.p2 = (lv_point_precise_t){right - 12, cy + 10};
        lv_draw_line(layer, &line);
    }
    else
    {
        lv_draw_rect_dsc_t rect;
        lv_draw_rect_dsc_init(&rect);
        rect.radius = LV_RADIUS_CIRCLE;
        rect.border_width = 2;
        rect.border_color = line.color;
        rect.bg_color = lv_color_white();
        rect.bg_opa = LV_OPA_COVER;
        bool toggle = row->item.kind == UI_SETTINGS_ROW_SWITCH;
        lv_area_t control = {right - (toggle ? 60 : 28), cy - 14, right, cy + 14};
        int32_t slot = row->item.control_size;
        int32_t cx = right + 1 - slot * 3 / 4;
        int32_t track_width = (slot * 55 + 30) / 60, track_height = slot / 2;
        if (toggle && slot)
            control = (lv_area_t){cx - track_width / 2, cy - track_height / 2,
                cx - track_width / 2 + track_width - 1, cy - track_height / 2 + track_height - 1};
        if (toggle && row->item.checked) rect.bg_color = line.color;
        lv_draw_rect(layer, &rect, &control);
        if (toggle || row->item.checked)
        {
            int32_t mark_x = toggle ? (row->item.checked ? right - 15 : right - 45) : right - 14;
            lv_area_t mark = {mark_x - 8, cy - 8, mark_x + 8, cy + 8};
            if (toggle && slot)
            {
                int32_t diameter = slot / 3, inset = slot / 12;
                int32_t left = row->item.checked ? control.x2 + 1 - inset - diameter : control.x1 + inset;
                mark = (lv_area_t){left, cy - diameter / 2, left + diameter - 1,
                    cy - diameter / 2 + diameter - 1};
            }
            rect.border_width = 0;
            rect.bg_color = toggle && row->item.checked ? lv_color_white() : line.color;
            lv_draw_rect(layer, &rect, &mark);
        }
    }
}

static lv_obj_t *view_build(ui_settings_list_t *list, settings_row_t *rows,
                            const settings_layout_t *layout, size_t page)
{
    lv_obj_t *view = flat(list->object, 0, 0, layout->width, layout->height);
    if (!view) return NULL;
    lv_obj_add_flag(view, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_EVENT_BUBBLE);
    for (size_t i = 0; i < layout->fragments_count; ++i)
    {
        const row_fragment_t *f = &layout->fragments[i];
        if (f->page != page) continue;
        settings_row_t *row = &rows[f->row];
        const row_layout_t *r = &layout->rows[f->row];
        lv_obj_t *object = flat(view, 0, f->y, layout->width, f->height);
        if (!object) goto failed;
        lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_user_data(object, row);
        lv_obj_set_style_outline_width(object, 1, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_color(object, lv_color_hex(0x777777), LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_pad(object, -2, LV_STATE_FOCUS_KEY);
        if (!lv_obj_add_event_cb(object, row_event, LV_EVENT_ALL, row)) goto failed;
        const lv_font_t *title_font = row->item.kind == UI_SETTINGS_ROW_SECTION ? list->small_font : list->body;
        int32_t text_h = r->inline_value ? larger(title_font->line_height, list->small_font->line_height) :
            (int32_t)(text_height(f->titles, title_font, layout->spacing) +
            text_height(f->values, list->small_font, layout->spacing)) + (f->titles && f->values ? value_gap(&row->item) : 0);
        int32_t content_height = f->height - row->item.bottom_gap;
        int32_t y = larger(0, (content_height - text_h) / 2);
        int32_t value_y = y + (text_h - list->small_font->line_height) / 2;
        if (!draw_text_lines(object, row->item.title, r->title, f->title, f->titles,
                             title_font, r->x, &y, r->width, layout->spacing, title_color(&row->item))) goto failed;
        if (f->titles && f->values) y += value_gap(&row->item);
        if (r->inline_value) y = value_y;
        if (!draw_text_lines(object, row->item.value, r->value, f->value, f->values,
                             list->small_font, r->value_x, &y, r->value_width, layout->spacing,
                             lv_color_hex(row->item.black_value ? 0x000000 : 0x595959))) goto failed;
        if (row->item.trailing_icon)
        {
            int32_t size = row->item.control_size;
            lv_obj_t *icon = lv_image_create(object);
            if (!icon) goto failed;
            lv_image_set_src(icon, row->item.trailing_icon);
            lv_obj_set_size(icon, size, size);
            lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
            lv_obj_set_pos(icon, layout->width - layout->right_margin - size * 5 / 4,
                (content_height - size) / 2);
            lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        }
        if (row->item.icon)
        {
            lv_obj_t *icon = lv_image_create(object);
            if (!icon) goto failed;
            int32_t size = smaller(row->item.icon_size ? row->item.icon_size : 60, layout->width / 8);
            lv_image_set_src(icon, row->item.icon);
            lv_obj_set_size(icon, size, smaller(size, content_height));
            lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
            lv_obj_set_pos(icon, layout->margin, (content_height - size) / 2);
            lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    if (layout->pages > 1)
    {
        lv_obj_t *footer = flat(view, 0, layout->content_height, layout->width, layout->footer_height);
        if (!footer) goto failed;
        for (size_t i = 0; i < layout->pages; ++i)
        {
            size_t start = i / layout->dot_columns * layout->dot_columns;
            size_t columns = layout->pages - start;
            if (columns > layout->dot_columns) columns = layout->dot_columns;
            int32_t x = (layout->width - (int32_t)columns * DOT_PITCH + DOT_PITCH - DOT_SIZE) / 2 +
                (int32_t)(i % layout->dot_columns) * DOT_PITCH;
            int32_t y = DOT_PAD + (int32_t)(i / layout->dot_columns) * DOT_PITCH;
            lv_obj_t *dot = flat(footer, x, y, DOT_SIZE, DOT_SIZE);
            if (!dot) goto failed;
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(dot, lv_color_hex(i == page ? 0x111111 : 0xaaaaaa), 0);
        }
    }
    return view;
failed:
    lv_obj_delete(view);
    return NULL;
}

static bool interactive(const settings_row_t *row)
{
    return row && row->item.enabled && row->item.kind != UI_SETTINGS_ROW_INFO &&
        row->item.kind != UI_SETTINGS_ROW_INLINE_INFO && row->item.kind != UI_SETTINGS_ROW_SECTION;
}

static lv_obj_t *visible_key(ui_settings_list_t *list, int key)
{
    if (!list->view) return NULL;
    for (uint32_t i = 0; i < lv_obj_get_child_count(list->view); ++i)
    {
        lv_obj_t *object = lv_obj_get_child(list->view, (int32_t)i);
        settings_row_t *row = lv_obj_get_user_data(object);
        if (row && row->item.key == key) return object;
    }
    return NULL;
}

static void group_attach(ui_settings_list_t *list)
{
    if (!list->view || !list->group) return;
    for (uint32_t i = 0; i < lv_obj_get_child_count(list->view); ++i)
    {
        lv_obj_t *object = lv_obj_get_child(list->view, (int32_t)i);
        if (interactive(lv_obj_get_user_data(object)) && lv_obj_get_group(object) != list->group)
            lv_group_add_obj(list->group, object);
    }
    /* Re-enabled rows are appended by LVGL. Restore visible order without
     * detaching existing members or moving unrelated controls' group slots. */
    uint32_t slot = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(list->view); ++i)
    {
        lv_obj_t *object = lv_obj_get_child(list->view, (int32_t)i);
        if (!interactive(lv_obj_get_user_data(object))) continue;
        lv_obj_t *member;
        do { member = lv_group_get_obj_by_index(list->group, slot++); }
        while (member && lv_obj_get_parent(member) != list->view);
        if (!member) break;
        if (member != object) lv_group_swap_obj(member, object);
    }
}

static size_t page_for_key(const settings_layout_t *layout, const settings_row_t *rows,
                           int key, size_t fallback)
{
    for (size_t i = 0; i < layout->fragments_count; ++i)
        if (rows[layout->fragments[i].row].item.key == key) return layout->fragments[i].page;
    return fallback < layout->pages ? fallback : layout->pages - 1;
}

static bool same_icon(const settings_row_t *a, const settings_row_t *b)
{
    return a->icon_text && b->icon_text ? text_equal(a->icon_text, b->icon_text) :
        a->item.icon == b->item.icon;
}

/* Only current-page geometry constrains reuse; other pages use the new layout
 * when opened. Keep row records stable because events retain their addresses. */
static bool view_reusable(const ui_settings_list_t *list, const settings_row_t *rows,
                          const settings_layout_t *next)
{
    const settings_layout_t *old = &list->layout;
    if (!list->view || old->count != next->count || old->width != next->width ||
        old->height != next->height || old->pages != next->pages ||
        old->content_height != next->content_height || old->footer_height != next->footer_height ||
        old->spacing != next->spacing || old->margin != next->margin ||
        old->right_margin != next->right_margin || old->dot_columns != next->dot_columns ||
        !layout_fonts_equal(old, &next->body, &next->small_font)) return false;
    for (size_t i = 0; i < next->count; ++i)
        if (list->rows[i].item.key != rows[i].item.key) return false;
    size_t j = 0;
    for (size_t i = 0; i < old->fragments_count; ++i)
    {
        const row_fragment_t *a = &old->fragments[i];
        if (a->page != list->page) continue;
        while (j < next->fragments_count && next->fragments[j].page != list->page) ++j;
        if (j == next->fragments_count) return false;
        const row_fragment_t *b = &next->fragments[j++];
        if (a->row != b->row || a->y != b->y || a->height != b->height ||
            a->titles != b->titles || a->values != b->values) return false;
        const row_layout_t *ar = &old->rows[a->row], *br = &next->rows[b->row];
        if (ar->x != br->x || ar->width != br->width || ar->inline_value != br->inline_value ||
            ar->value_x != br->value_x || ar->value_width != br->value_width) return false;
        const ui_settings_list_row_t *ai = &list->rows[a->row].item, *bi = &rows[b->row].item;
        if (ai->kind != bi->kind || ai->bottom_gap != bi->bottom_gap ||
            value_gap(ai) != value_gap(bi) || ai->icon_size != bi->icon_size ||
            ai->control_size != bi->control_size || ai->trailing_icon != bi->trailing_icon ||
            ai->no_chevron != bi->no_chevron || !same_icon(&list->rows[a->row], &rows[b->row])) return false;
    }
    while (j < next->fragments_count)
        if (next->fragments[j++].page == list->page) return false;
    return true;
}

typedef struct text_update {
    struct text_update *next;
    lv_obj_t *label;
    char *text;
} text_update_t;

static bool prepare_text(text_update_t **updates, lv_obj_t *label,
                         const char *text, text_line_t line)
{
    while (line.length && (text[line.start + line.length - 1] == '\n' ||
                          text[line.start + line.length - 1] == '\r')) --line.length;
    const char *old = lv_label_get_text(label);
    if (strlen(old) == line.length && !memcmp(old, text + line.start, line.length)) return true;
    text_update_t *update = lv_malloc(sizeof(*update));
    if (!update) return false;
    update->text = lv_malloc((size_t)line.length + 1);
    if (!update->text) { lv_free(update); return false; }
    memcpy(update->text, text + line.start, line.length);
    update->text[line.length] = 0;
    update->label = label;
    update->next = *updates;
    *updates = update;
    return true;
}

static bool view_update(ui_settings_list_t *list, settings_row_t *rows, settings_layout_t *layout)
{
    text_update_t *updates = NULL;
    bool ready = true;
    int32_t child = 0;
    for (size_t i = 0; i < layout->fragments_count && ready; ++i)
    {
        const row_fragment_t *f = &layout->fragments[i];
        if (f->page != list->page) continue;
        lv_obj_t *object = lv_obj_get_child(list->view, child++);
        const row_layout_t *r = &layout->rows[f->row];
        const ui_settings_list_row_t *item = &rows[f->row].item;
        for (size_t n = 0; n < f->titles + f->values && ready; ++n)
            ready = prepare_text(&updates, lv_obj_get_child(object, (int32_t)n),
                n < f->titles ? item->title : item->value,
                n < f->titles ? r->title[f->title + n] : r->value[f->value + n - f->titles]);
    }
    /* Allocate every replacement before changing any live label. */
    list->busy = true;
    while (updates)
    {
        text_update_t *next = updates->next;
        if (ready)
        {
            void *old = lv_obj_get_user_data(updates->label);
            lv_label_set_text_static(updates->label, updates->text);
            lv_obj_set_user_data(updates->label, updates->text);
            lv_free(old);
        }
        else lv_free(updates->text);
        lv_free(updates);
        updates = next;
    }
    if (!ready) { list->busy = false; return false; }
    child = 0;
    for (size_t i = 0; i < layout->fragments_count; ++i)
    {
        const row_fragment_t *f = &layout->fragments[i];
        if (f->page != list->page) continue;
        lv_obj_t *object = lv_obj_get_child(list->view, child++);
        const ui_settings_list_row_t *old = &list->rows[f->row].item, *item = &rows[f->row].item;
        for (size_t n = 0; n < f->titles + f->values; ++n)
        {
            lv_obj_t *label = lv_obj_get_child(object, (int32_t)n);
            lv_color_t color = n < f->titles ? title_color(item) :
                lv_color_hex(item->black_value ? 0x000000 : 0x595959);
            if (!lv_color_eq(lv_obj_get_style_text_color(label, 0), color))
                lv_obj_set_style_text_color(label, color, 0);
        }
        if (old->checked != item->checked || old->enabled != item->enabled ||
            old->no_separator != item->no_separator) lv_obj_invalidate(object);
        if (!interactive(&rows[f->row]) && lv_obj_get_group(object)) lv_group_remove_obj(object);
    }
    for (size_t i = 0; i < list->count; ++i)
    {
        /* Keep any image source referenced by a surviving image object alive. */
        if (list->rows[i].icon_text && same_icon(&list->rows[i], &rows[i]))
        {
            char *icon = list->rows[i].icon_text;
            list->rows[i].item.icon = list->rows[i].icon_text = rows[i].icon_text;
            rows[i].item.icon = rows[i].icon_text = icon;
        }
        settings_row_t old = list->rows[i];
        list->rows[i] = rows[i];
        rows[i] = old;
    }
    layout_free(&list->layout);
    list->layout = *layout;
    group_attach(list);
    list->busy = false;
    return true;
}

static void view_commit(ui_settings_list_t *list, lv_obj_t *view, size_t page, bool keep_anchor)
{
    lv_obj_t *old = list->view;
    lv_obj_t *focused = list->group ? lv_group_get_focused(list->group) : NULL;
    bool had_focus = focused && old && lv_obj_get_parent(focused) == old;
    int focus_key = had_focus ? ((settings_row_t *)lv_obj_get_user_data(focused))->item.key : 0;
    list->view = view;
    list->page = page;
    list->busy = true;
    if (old) lv_obj_delete(old);
    if (!keep_anchor || !visible_key(list, list->anchor))
    {
        list->has_anchor = false;
        for (size_t i = 0; i < list->layout.fragments_count; ++i)
            if (list->layout.fragments[i].page == page)
            {
                list->anchor = list->rows[list->layout.fragments[i].row].item.key;
                list->has_anchor = true;
                break;
            }
    }
    lv_obj_remove_flag(view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(view);
    group_attach(list);
    if (had_focus)
    {
        lv_obj_t *target = keep_anchor ? visible_key(list, focus_key) : NULL;
        if (!target || !interactive(lv_obj_get_user_data(target)))
        {
            target = NULL;
            for (uint32_t i = 0; i < lv_obj_get_child_count(view); ++i)
            {
                lv_obj_t *candidate = lv_obj_get_child(view, (int32_t)i);
                if (interactive(lv_obj_get_user_data(candidate))) { target = candidate; break; }
            }
        }
        if (target) lv_group_focus_obj(target);
    }
    list->busy = false;
}

static bool select_page(ui_settings_list_t *list, size_t page, bool keep_anchor)
{
    if (!list || page >= list->layout.pages) return false;
    if (page == list->page) return true;
    lv_obj_t *view = view_build(list, list->rows, &list->layout, page);
    if (!view) return false;
    view_commit(list, view, page, keep_anchor);
    return true;
}

static bool relayout_commit(ui_settings_list_t *list, settings_row_t *rows, size_t count,
                            settings_layout_t *layout)
{
    size_t page = list->has_anchor ? page_for_key(layout, rows, list->anchor, list->page) : 0;
    lv_obj_t *view = view_build(list, rows, layout, page);
    if (!view) { layout_free(layout); return false; }
    settings_layout_t old_layout = list->layout;
    list->layout = *layout;
    list->rows = rows;
    list->count = count;
    list->busy = true;
    lv_obj_set_pos(list->object, list->left, list->top);
    lv_obj_set_size(list->object, layout->width, layout->height);
    view_commit(list, view, page, true);
    layout_free(&old_layout);
    return true;
}

static bool relayout(ui_settings_list_t *list, settings_row_t *rows, size_t count)
{
    settings_layout_t layout;
    return layout_build(list, rows, count, &layout, false) && relayout_commit(list, rows, count, &layout);
}

static void row_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_DRAW_MAIN && code != LV_EVENT_FOCUSED &&
        code != LV_EVENT_CLICKED && code != LV_EVENT_KEY &&
        code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING &&
        code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    settings_row_t *row = lv_event_get_user_data(event);
    ui_settings_list_t *list = row->owner;
    if (code == LV_EVENT_DRAW_MAIN) { row_draw(event, row); return; }
    if (list->busy || list->deleting) return;
    if (code == LV_EVENT_FOCUSED)
    { list->anchor = row->item.key; list->has_anchor = true; }
    else if (code == LV_EVENT_KEY)
    {
        uint32_t key = lv_event_get_key(event);
        if (key == LV_KEY_RIGHT) ui_settings_list_next_page(list);
        else if (key == LV_KEY_LEFT) ui_settings_list_prev_page(list);
    }
    else if (code == LV_EVENT_CLICKED && !list->cancel_click && interactive(row) && list->callback)
    {
        list->anchor = row->item.key;
        list->has_anchor = true;
        list->callback(row->item.key, list->user_data);
    }
    else if (lv_event_get_target_obj(event) == lv_event_get_current_target_obj(event))
        pointer_event(list, event);
}

static void list_event(lv_event_t *event)
{
    ui_settings_list_t *list = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE && lv_event_get_target_obj(event) == list->object)
    {
        list->deleting = true;
        lv_obj_remove_event_cb_with_user_data(list->parent, parent_event, list);
        if (list->view) lv_obj_delete(list->view);
        layout_free(&list->layout);
        rows_free(list->rows, list->count);
        lv_free(list);
        return;
    }
    if (list->busy || list->deleting) return;
    if (lv_event_get_target_obj(event) == list->object) pointer_event(list, event);
}

static void pointer_event(ui_settings_list_t *list, lv_event_t *event)
{
    /* A page change deletes the pressed row. Handle it on that row before
     * bubbling so the surviving parent's event array is not cleaned as deleted. */
    lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING &&
        code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    lv_indev_t *indev = lv_event_get_indev(event);
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER)
    {
        if (code == LV_EVENT_PRESSED) list->cancel_click = false;
        return;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    if (code == LV_EVENT_PRESSED)
    {
        list->origin = point;
        list->pressed = true;
        list->cancel_click = false;
        return;
    }
    if (code == LV_EVENT_PRESS_LOST)
    { list->pressed = false; list->cancel_click = true; return; }
    if (!list->pressed) return;
    int32_t dx = point.x - list->origin.x, dy = point.y - list->origin.y;
    int32_t ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    if (ax > 12 || ay > 12) list->cancel_click = true;
    if (code == LV_EVENT_RELEASED)
    {
        list->pressed = false;
        if (ax >= larger(40, list->layout.width / 10) && ax > ay * 2)
        {
            list->cancel_click = true;
            if (dx < 0) ui_settings_list_next_page(list);
            else ui_settings_list_prev_page(list);
        }
    }
}

static void parent_event(lv_event_t *event)
{
    ui_settings_list_t *list = lv_event_get_user_data(event);
    if (lv_event_get_target_obj(event) == list->parent && !list->busy && !list->deleting)
        ui_settings_list_update_layout(list);
}

ui_settings_list_t *ui_settings_list_create(lv_obj_t *parent, const lv_font_t *body,
    const lv_font_t *small_font, ui_settings_list_cb_t callback, void *user_data)
{
    if (!parent || !body || !small_font || !body->line_height || !small_font->line_height) return NULL;
    ui_settings_list_t *list = array_alloc(1, sizeof(*list));
    if (!list) return NULL;
    list->parent = parent;
    list->body = body;
    list->small_font = small_font;
    list->callback = callback;
    list->user_data = user_data;
    lv_obj_update_layout(parent);
    list->object = flat(parent, 0, 0, lv_obj_get_content_width(parent), lv_obj_get_content_height(parent));
    if (!list->object) { lv_free(list); return NULL; }
    lv_obj_add_flag(list->object, LV_OBJ_FLAG_CLICKABLE);
    if (!lv_obj_add_event_cb(list->object, list_event, LV_EVENT_ALL, list))
    { lv_obj_delete(list->object); lv_free(list); return NULL; }
    if (!lv_obj_add_event_cb(parent, parent_event, LV_EVENT_SIZE_CHANGED, list) ||
        !ui_settings_list_update_layout(list))
    { lv_obj_delete(list->object); return NULL; }
    return list;
}

bool ui_settings_list_set_rows(ui_settings_list_t *list,
    const ui_settings_list_row_t *rows, size_t count)
{
    if (!list || (count && !rows) || count > SIZE_MAX / sizeof(settings_row_t)) return false;
    if (rows_equal(list, rows, count)) return ui_settings_list_update_layout(list);
    settings_row_t *copy = count ? rows_copy(list, rows, count) : NULL;
    if (count && !copy) return false;
    settings_layout_t layout;
    if (!layout_build(list, copy, count, &layout, true)) { rows_free(copy, count); return false; }
    if (view_reusable(list, copy, &layout))
    {
        bool updated = view_update(list, copy, &layout);
        if (!updated) layout_free(&layout);
        rows_free(copy, count);
        return updated;
    }
    settings_row_t *old = list->rows;
    size_t old_count = list->count;
    if (!relayout_commit(list, copy, count, &layout)) { rows_free(copy, count); return false; }
    rows_free(old, old_count);
    return true;
}

bool ui_settings_list_update_layout(ui_settings_list_t *list)
{
    if (!list || list->deleting || list->busy) return false;
    list->busy = true;
    lv_obj_update_layout(list->parent);
    list->busy = false;
    int32_t width = lv_obj_get_content_width(list->parent) - list->left - list->right;
    int32_t height = lv_obj_get_content_height(list->parent) - list->top - list->bottom;
    if (list->view && list->layout.width == width && list->layout.height == height &&
        layout_fonts_equal(&list->layout, list->body, list->small_font)) return true;
    return relayout(list, list->rows, list->count);
}

bool ui_settings_list_refresh_fonts(ui_settings_list_t *list,
    const lv_font_t *body, const lv_font_t *small_font)
{
    if (!list || !body || !small_font || !body->line_height || !small_font->line_height) return false;
    const lv_font_t *old_body = list->body, *old_small = list->small_font;
    if (old_body == body && old_small == small_font) return ui_settings_list_update_layout(list);
    list->body = body;
    list->small_font = small_font;
    if (relayout(list, list->rows, list->count)) return true;
    list->body = old_body;
    list->small_font = old_small;
    return false;
}

void ui_settings_list_set_group(ui_settings_list_t *list, lv_group_t *group)
{
    if (!list) return;
    list->busy = true;
    if (list->view && list->group != group)
        for (uint32_t i = 0; i < lv_obj_get_child_count(list->view); ++i)
        {
            lv_obj_t *object = lv_obj_get_child(list->view, (int32_t)i);
            if (lv_obj_get_group(object)) lv_group_remove_obj(object);
        }
    list->group = group;
    group_attach(list);
    list->busy = false;
}

bool ui_settings_list_show_key(ui_settings_list_t *list, int key)
{
    if (!list) return false;
    for (size_t i = 0; i < list->count; ++i)
        if (list->rows[i].item.key == key)
        {
            size_t page = page_for_key(&list->layout, list->rows, key, 0);
            if (!select_page(list, page, true)) return false;
            list->anchor = key;
            list->has_anchor = true;
            return true;
        }
    return false;
}

bool ui_settings_list_focus_key(ui_settings_list_t *list, int key)
{
    if (!list || !list->group) return false;
    for (size_t i = 0; i < list->count; ++i)
        if (list->rows[i].item.key == key && interactive(&list->rows[i]))
        {
            if (!ui_settings_list_show_key(list, key)) return false;
            lv_obj_t *object = visible_key(list, key);
            if (!object) return false;
            lv_group_focus_obj(object);
            return true;
        }
    return false;
}

bool ui_settings_list_set_insets(ui_settings_list_t *list,
    lv_coord_t left, lv_coord_t top, lv_coord_t right, lv_coord_t bottom)
{
    if (!list || left < 0 || top < 0 || right < 0 || bottom < 0 ||
        (int64_t)left + right >= lv_obj_get_content_width(list->parent) ||
        (int64_t)top + bottom >= lv_obj_get_content_height(list->parent)) return false;
    if (list->left == left && list->top == top && list->right == right && list->bottom == bottom) return true;
    lv_coord_t old_left = list->left, old_top = list->top, old_right = list->right, old_bottom = list->bottom;
    list->left = left; list->top = top; list->right = right; list->bottom = bottom;
    if (relayout(list, list->rows, list->count)) return true;
    list->left = old_left; list->top = old_top; list->right = old_right; list->bottom = old_bottom;
    return false;
}

bool ui_settings_list_next_page(ui_settings_list_t *list)
{
    return list && list->page + 1 < list->layout.pages && select_page(list, list->page + 1, false);
}

bool ui_settings_list_prev_page(ui_settings_list_t *list)
{
    return list && list->page && select_page(list, list->page - 1, false);
}

size_t ui_settings_list_get_page(const ui_settings_list_t *list) { return list ? list->page : 0; }
size_t ui_settings_list_get_page_count(const ui_settings_list_t *list) { return list ? list->layout.pages : 0; }
lv_obj_t *ui_settings_list_get_obj(ui_settings_list_t *list) { return list ? list->object : NULL; }
void ui_settings_list_destroy(ui_settings_list_t *list) { if (list) lv_obj_delete(list->object); }
