#include "epd_memory.h"
#include "chat_markdown.h"
#include "markdown.h"
#include <stdio.h>
#include <string.h>

#ifndef AI_MARKDOWN_PLAIN_LABEL
#define AI_MARKDOWN_PLAIN_LABEL 1
#endif

#if defined(EPD_INPUT_TEST_ENABLED) && EPD_INPUT_TEST_ENABLED && \
    defined(AI_MARKDOWN_PLAIN_LABEL) && AI_MARKDOWN_PLAIN_LABEL && LV_LABEL_LONG_TXT_HINT && \
    !LV_USE_BIDI && !LV_USE_ARABIC_PERSIAN_CHARS
#define CHAT_PLAIN_LABEL 1
#include <rtthread.h>

typedef struct {
    uint32_t label_setup;
    uint32_t set_text;
    uint32_t text_layout;
    uint32_t box_layout;
    uint32_t labels;
} markdown_restore_timing_t;
#else
#define CHAT_PLAIN_LABEL 0
#endif

typedef struct {
    lv_obj_t *object;
    uint32_t node;
    bool reused;
#if CHAT_PLAIN_LABEL
    char *label_text;
#endif
} rendered_block_t;

struct chat_markdown {
    lv_obj_t *root;
    md_doc_t document;
    rendered_block_t *blocks;
    size_t count;
    char *source;
    int width;
    uint32_t scale;
};

static void *allocate(void *unused, size_t bytes)
{ (void)unused; return epd_app_alloc(bytes, EPD_APP_PSRAM); }
static void release(void *unused, void *memory)
{ (void)unused; epd_app_free(memory); }
static const md_allocator_t allocator = {NULL, allocate, release};
static void (*link_handler)(const char *url);
void chat_markdown_set_link_handler(void (*handler)(const char *url)) { link_handler = handler; }
typedef struct { lv_span_t *span; char url[]; } link_target_t;
static void link_event(lv_event_t *event)
{
    link_target_t *target = lv_event_get_user_data(event);
    if (lv_event_get_code(event) == LV_EVENT_DELETE) { release(NULL, target); return; }
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !link_handler || !lv_indev_active()) return;
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    if (lv_spangroup_get_span_by_point(lv_event_get_target_obj(event), &point) == target->span)
        link_handler(target->url);
}

#define RENDER_BUDGET (512u * 1024u)
#define UI_RESERVE (128u * 1024u)
static bool room_for(size_t bytes)
{
    size_t available = epd_app_ui_available();
    return available > UI_RESERVE && bytes < available - UI_RESERVE;
}

/* Conservative allowance for objects, spans, styles, repeated table captions
 * and copied text. The LVGL pool is separate from parser/application PSRAM. */
static size_t render_cost(const md_doc_t *doc)
{
    return (size_t)doc->node_count * 1536u + (size_t)doc->run_count * 384u +
           (size_t)doc->text_size * 2u + 4096u;
}

#if CHAT_PLAIN_LABEL
static void wait_label_draws(const rendered_block_t *blocks, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (blocks[i].label_text) {
            lv_draw_wait_for_finish();
            return;
        }
    }
}
#endif

static void destroyed(lv_event_t *event)
{
    chat_markdown_t *view = lv_event_get_user_data(event);
    md_document_free(&view->document);
#if CHAT_PLAIN_LABEL
    wait_label_draws(view->blocks, view->count);
    for (size_t i = 0; i < view->count; ++i) release(NULL, view->blocks[i].label_text);
#endif
    epd_app_free(view->blocks);
    epd_app_free(view->source);
    epd_app_free(view);
}

static lv_obj_t *container(lv_obj_t *parent)
{
    lv_obj_t *object = lv_obj_create(parent);
    if (!object) return NULL;
    epd_app_object_init(object);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_bg_color(object, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    return object;
}

chat_markdown_t *chat_markdown_create(lv_obj_t *parent)
{
    chat_markdown_t *view = allocate(NULL, sizeof(*view));
    if (!view) return NULL;
    memset(view, 0, sizeof(*view));
    view->root = container(parent);
    if (!view->root) { release(NULL, view); return NULL; }
    lv_obj_add_event_cb(view->root, destroyed, LV_EVENT_DELETE, view);
    return view;
}

lv_obj_t *chat_markdown_root(chat_markdown_t *view) { return view ? view->root : NULL; }
const char *chat_markdown_source(const chat_markdown_t *view)
{ return view && view->source ? view->source : ""; }
size_t chat_markdown_block_count(const chat_markdown_t *view) { return view ? view->count : 0; }
lv_obj_t *chat_markdown_block(const chat_markdown_t *view, size_t index)
{ return view && index < view->count ? view->blocks[index].object : NULL; }

static bool leaf(md_node_kind_t kind)
{
    return kind == MD_NODE_P || kind == MD_NODE_HEADING || kind == MD_NODE_CODE ||
           kind == MD_NODE_HR || kind == MD_NODE_TR;
}

static unsigned children(const md_doc_t *doc, uint32_t node)
{
    unsigned count = 0;
    for (uint32_t child = doc->nodes[node].first_child; child != MD_INDEX_NONE;
         child = doc->nodes[child].next_sibling) ++count;
    return count;
}

static uint32_t table_header(const md_doc_t *doc, uint32_t row)
{
    uint32_t table = doc->nodes[row].parent;
    while (table != MD_INDEX_NONE && doc->nodes[table].kind != MD_NODE_TABLE)
        table = doc->nodes[table].parent;
    if (table == MD_INDEX_NONE) return MD_INDEX_NONE;
    for (uint32_t i = table + 1; i < doc->nodes[table].subtree_end; ++i)
        if (doc->nodes[i].kind == MD_NODE_TR) return i;
    return MD_INDEX_NONE;
}

static unsigned run_style(const md_run_t *run, bool heading)
{
    return ((heading || (run->style & MD_STYLE_BOLD)) ? EPD_FONT_STYLE_BOLD : 0) |
           ((run->style & MD_STYLE_ITALIC) ? EPD_FONT_STYLE_ITALIC : 0);
}

static bool same_presentation(const md_doc_t *doc, const md_run_t *a,
                              const md_run_t *b, bool heading)
{
    unsigned mask = MD_STYLE_CODE | MD_STYLE_LINK | MD_STYLE_STRIKE;
    if ((a->style & mask) != (b->style & mask) ||
        run_style(a, heading) != run_style(b, heading)) return false;
    return !(a->style & MD_STYLE_LINK) ||
        (a->link.length == b->link.length &&
         (!a->link.length || !memcmp(doc->text + a->link.offset,
                                    doc->text + b->link.offset, a->link.length)));
}

/* Soft/hard breaks already contain their display bytes (space/newline).
 * Keep link targets separate even when their visual style is identical. */
static uint32_t span_end(const md_doc_t *doc, const md_node_t *block,
                         uint32_t first, bool heading, size_t *length)
{
    const md_run_t *run = &doc->runs[block->first_run + first];
    uint32_t end = first + 1;
    *length = run->text.length;
    while (end < block->run_count) {
        const md_run_t *next = &doc->runs[block->first_run + end];
        if (!same_presentation(doc, run, next, heading)) break;
        *length += next->text.length;
        ++end;
    }
    return end;
}

#if CHAT_PLAIN_LABEL
static lv_obj_t *plain_paragraph_label(lv_obj_t *parent, const md_doc_t *doc,
                                       const md_node_t *block, int width, char **owned, markdown_restore_timing_t *timing)
{
    if (block->kind != MD_NODE_P || !block->run_count) return NULL;
    size_t length = 0;
    for (uint32_t i = 0; i < block->run_count; ++i) {
        const md_run_t *run = &doc->runs[block->first_run + i];
        if (run->style || run->kind == MD_RUN_HARD_BREAK) return NULL;
        length += run->text.length;
    }
    if (length < 4096u) return NULL;
    uint32_t started = timing ? lv_tick_get() : 0;
    char *text = allocate(NULL, length + 1u);
    if (!text) return NULL;
    size_t used = 0;
    for (uint32_t i = 0; i < block->run_count; ++i) {
        const md_run_t *run = &doc->runs[block->first_run + i];
        memcpy(text + used, doc->text + run->text.offset, run->text.length);
        used += run->text.length;
    }
    text[used] = 0;
    const lv_font_t *font = epd_app_font_style(EPD_FONT_BODY, 0);
    lv_obj_t *label = font ? lv_label_create(parent) : NULL;
    if (label) {
        epd_app_object_init(label);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_size(label, width, LV_SIZE_CONTENT);
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x000000), 0);
        lv_obj_set_style_text_line_space(label, epd_ui_px(8), 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        uint32_t setting = timing ? lv_tick_get() : 0;
        lv_label_set_text_static(label, text);
        if (timing) {
            timing->label_setup += setting - started;
            timing->set_text += lv_tick_get() - setting;
            ++timing->labels;
        }
        *owned = text;
        return label;
    }
    release(NULL, text);
    return NULL;
}
#endif

/* Native LVGL spans handle mixed fonts, UTF-8 wrapping and baseline alignment. */
static lv_obj_t *rich_text(lv_obj_t *parent, const md_doc_t *doc, uint32_t node,
                          int width, epd_font_role_t role, bool bold)
{
    const md_node_t *block = &doc->nodes[node];
    lv_obj_t *object = lv_spangroup_create(parent);
    if (!object) return NULL;
    epd_app_object_init(object);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_size(object, LV_MAX(width, 1), LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(object, epd_app_font_role(role), 0);
    lv_obj_set_style_text_line_space(object, epd_ui_px(8), 0);
    if (block->cell_align == MD_ALIGN_MIDDLE) lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_CENTER, 0);
    else if (block->cell_align == MD_ALIGN_END) lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_RIGHT, 0);
    size_t maximum = 1;
    for (uint32_t i = 0; i < block->run_count;) {
        size_t length;
        i = span_end(doc, block, i, bold, &length);
        if (length + 1u > maximum) maximum = length + 1u;
    }
    char *text = allocate(NULL, maximum);
    if (!text) { lv_obj_delete(object); return NULL; }
    bool ok = true;
    for (uint32_t i = 0; i < block->run_count;) {
        const md_run_t *run = &doc->runs[block->first_run + i];
        size_t length, used = 0;
        uint32_t end = span_end(doc, block, i, bold, &length);
        for (; i < end; ++i) {
            const md_run_t *part = &doc->runs[block->first_run + i];
            memcpy(text + used, doc->text + part->text.offset, part->text.length);
            used += part->text.length;
        }
        text[used] = 0;
        if (!length) continue;
        epd_font_role_t current_role = run->style & MD_STYLE_CODE ? EPD_FONT_SMALL : role;
        const lv_font_t *font = epd_app_font_style(current_role, run_style(run, bold));
        if (!font) { ok = false; break; }
        lv_span_t *span = lv_spangroup_add_span(object);
        if (!span) { ok = false; break; }
        lv_span_set_text(span, text);
        if (!lv_span_get_text(span) || strcmp(lv_span_get_text(span), text)) { ok = false; break; }
        lv_style_t *style = lv_span_get_style(span);
        lv_color_t color = lv_color_hex(run->style & MD_STYLE_CODE ? 0x333333 : 0x000000);
        lv_text_decor_t decor = (run->style & MD_STYLE_LINK ? LV_TEXT_DECOR_UNDERLINE : 0) |
                               (run->style & MD_STYLE_STRIKE ? LV_TEXT_DECOR_STRIKETHROUGH : 0);
        lv_style_set_text_font(style, font);
        lv_style_set_text_color(style, color);
        lv_style_set_text_decor(style, decor);
        /* Setters leave the old property set intact when growth fails. */
        lv_style_value_t value;
        if (lv_style_get_prop(style, LV_STYLE_TEXT_FONT, &value) != LV_STYLE_RES_FOUND || value.ptr != font ||
            lv_style_get_prop(style, LV_STYLE_TEXT_COLOR, &value) != LV_STYLE_RES_FOUND || !lv_color_eq(value.color, color) ||
            lv_style_get_prop(style, LV_STYLE_TEXT_DECOR, &value) != LV_STYLE_RES_FOUND || value.num != decor) {
            ok = false; break;
        }
        if ((run->style & MD_STYLE_LINK) && run->link.length && run->link.length <= 2048) {
            const char *url = doc->text + run->link.offset;
            bool http = run->link.length >= 7 && !memcmp(url, "http://", 7);
            bool https = run->link.length >= 8 && !memcmp(url, "https://", 8);
            if (http || https) {
                link_target_t *target = allocate(NULL, sizeof(*target) + run->link.length + 1);
                if (!target) { ok = false; break; }
                target->span = span;
                memcpy(target->url, url, run->link.length); target->url[run->link.length] = 0;
                lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(object, link_event, LV_EVENT_ALL, target);
            }
        }
    }
    release(NULL, text);
    if (!ok) { lv_obj_delete(object); return NULL; }
    lv_spangroup_refresh(object);
    lv_obj_update_layout(object);
    return object;
}

static bool first_in_item(const md_doc_t *doc, uint32_t item, uint32_t node)
{
    for (uint32_t i = item + 1; i < doc->nodes[item].subtree_end; ++i)
        if (leaf(doc->nodes[i].kind)) return i == node;
    return false;
}

static int indentation(const md_doc_t *doc, uint32_t node, int width,
                       unsigned *quotes, char *marker, size_t capacity)
{
    unsigned lists = 0;
    *quotes = 0; marker[0] = 0;
    for (uint32_t i = doc->nodes[node].parent; i != MD_INDEX_NONE; i = doc->nodes[i].parent) {
        const md_node_t *ancestor = &doc->nodes[i];
        if (ancestor->kind == MD_NODE_QUOTE) ++*quotes;
        else if (ancestor->kind == MD_NODE_UL || ancestor->kind == MD_NODE_OL) ++lists;
        else if (ancestor->kind == MD_NODE_LI && !marker[0] && first_in_item(doc, i, node)) {
            const md_node_t *list = &doc->nodes[ancestor->parent];
            if (list->kind == MD_NODE_OL) snprintf(marker, capacity, "%lu.", (unsigned long)ancestor->list_start);
            else snprintf(marker, capacity, "•");
        }
    }
    int indent = epd_ui_px((int)lists * 38 + (int)*quotes * 20);
    return LV_MIN(indent, width / 2);
}

static lv_obj_t *table_row(lv_obj_t *parent, const md_doc_t *doc, uint32_t node, int width)
{
    unsigned columns = children(doc, node);
    if (!columns) return container(parent);
    bool stacked = width / (int)columns < epd_ui_px(112);
    uint32_t header = table_header(doc, node);
    lv_obj_t *row = container(parent);
    if (!row) return NULL;
    lv_obj_set_size(row, width, 1);
    int pad = epd_ui_px(10), height = 0, x = 0;
    unsigned column = 0;
    uint32_t heading = header != MD_INDEX_NONE ? doc->nodes[header].first_child : MD_INDEX_NONE;
    for (uint32_t cell = doc->nodes[node].first_child; cell != MD_INDEX_NONE;
         cell = doc->nodes[cell].next_sibling, ++column) {
        int cell_width = stacked ? width : width * (int)(column + 1) / (int)columns - x;
        lv_obj_t *box = container(row);
        if (!box) goto failed;
        lv_obj_set_size(box, cell_width, 1);
        int y = pad;
        if (stacked && header != node && heading != MD_INDEX_NONE) {
            lv_obj_t *caption = rich_text(box, doc, heading, cell_width - pad * 2, EPD_FONT_SMALL, true);
            if (!caption) goto failed;
            lv_obj_set_pos(caption, pad, y);
            y += lv_obj_get_height(caption) + epd_ui_px(4);
        }
        lv_obj_t *text = rich_text(box, doc, cell, cell_width - pad * 2,
                                  EPD_FONT_BODY, doc->nodes[cell].kind == MD_NODE_TH);
        if (!text) goto failed;
        lv_obj_set_pos(text, pad, y);
        y += lv_obj_get_height(text) + pad;
        lv_obj_set_height(box, y);
        lv_obj_set_pos(box, stacked ? 0 : x, stacked ? height : 0);
        if (stacked) height += y;
        else { height = LV_MAX(height, y); x += cell_width; }
        if (heading != MD_INDEX_NONE) heading = doc->nodes[heading].next_sibling;
    }
    if (!stacked) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(row); ++i) {
            lv_obj_t *box = lv_obj_get_child(row, (int32_t)i);
            lv_obj_set_height(box, height);
            lv_obj_set_style_border_width(box, 1, 0);
            lv_obj_set_style_border_side(box, LV_BORDER_SIDE_RIGHT, 0);
            lv_obj_set_style_border_color(box, lv_color_hex(0x666666), 0);
        }
    }
    lv_obj_set_height(row, LV_MAX(height, 1));
    lv_obj_set_style_border_width(row, header == node ? 2 : 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(header == node ? 0x333333 : 0x666666), 0);
    return row;
failed:
    lv_obj_delete(row); return NULL;
}

static lv_obj_t *build_block(lv_obj_t *parent, const md_doc_t *doc, uint32_t node, int width
#if CHAT_PLAIN_LABEL
                            , char **label_text, markdown_restore_timing_t *timing
#endif
                            )
{
    const md_node_t *block = &doc->nodes[node];
    lv_obj_t *box = container(parent);
    if (!box) return NULL;
    lv_obj_set_size(box, width, 1);
    unsigned quotes;
    char marker[24];
    int indent = indentation(doc, node, width, &quotes, marker, sizeof(marker));
    int inset = block->kind == MD_NODE_CODE ? epd_ui_px(14) : 0;
    lv_obj_t *text = NULL;
    if (block->kind == MD_NODE_HR) {
        text = container(box);
        if (text) {
            lv_obj_set_size(text, width - indent, epd_ui_px(1));
            lv_obj_set_style_bg_color(text, lv_color_hex(0x666666), 0);
        }
    } else if (block->kind == MD_NODE_TR) text = table_row(box, doc, node, width - indent);
    else {
        epd_font_role_t role = block->kind == MD_NODE_CODE ? EPD_FONT_SMALL :
                              block->kind == MD_NODE_HEADING && block->heading_level <= 2 ? EPD_FONT_TITLE : EPD_FONT_BODY;
#if CHAT_PLAIN_LABEL
        if (role == EPD_FONT_BODY)
            text = plain_paragraph_label(box, doc, block, LV_MAX(width - indent - inset * 2, 1), label_text, timing);
#endif
        if (!text)
            text = rich_text(box, doc, node, width - indent - inset * 2, role, block->kind == MD_NODE_HEADING);
    }
    if (!text) { lv_obj_delete(box); return NULL; }
    lv_obj_set_pos(text, indent + inset, inset);
#if CHAT_PLAIN_LABEL
    bool measure_label = timing && *label_text;
    uint32_t layout_at = measure_label ? lv_tick_get() : 0;
#endif
    lv_obj_update_layout(text);
#if CHAT_PLAIN_LABEL
    if (measure_label) timing->text_layout += lv_tick_get() - layout_at;
#endif
    int height = LV_MAX(epd_ui_px(12), lv_obj_get_height(text)) + inset * 2;
    if (block->kind == MD_NODE_CODE) {
        lv_obj_set_style_border_width(box, 1, 0);
        lv_obj_set_style_border_color(box, lv_color_hex(0x666666), 0);
    }
    if (marker[0]) {
        lv_point_t size;
        const lv_font_t *font = epd_app_font_role(EPD_FONT_BODY);
        lv_text_get_size(&size, marker, font, 0, 0, width, LV_TEXT_FLAG_NONE);
        int marker_width = size.x + epd_ui_px(8);
        lv_obj_t *label = epd_app_label(box, marker, LV_MAX(0, indent - marker_width), inset,
                                       marker_width, font);
        if (!label) { lv_obj_delete(box); return NULL; }
        height = LV_MAX(height, lv_font_get_line_height(font));
    }
    for (unsigned i = 0; i < quotes && i < 8; ++i) {
        lv_obj_t *bar = container(box);
        if (!bar) { lv_obj_delete(box); return NULL; }
        lv_obj_set_pos(bar, epd_ui_px((int)i * 12), 0);
        lv_obj_set_size(bar, epd_ui_px(2), height);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x555555), 0);
    }
    lv_obj_set_height(box, height);
#if CHAT_PLAIN_LABEL
    if (measure_label) layout_at = lv_tick_get();
#endif
    lv_obj_update_layout(box);
#if CHAT_PLAIN_LABEL
    if (measure_label) timing->box_layout += lv_tick_get() - layout_at;
#endif
    return box;
}

static bool same_block(const md_doc_t *old, uint32_t a, const md_doc_t *next, uint32_t b, int width)
{
    if (a == MD_INDEX_NONE || !md_node_equal(old, a, next, b) ||
        !md_node_context_equal(old, a, next, b)) return false;
    /* A preceding paragraph can change which block owns a list marker. */
    uint32_t pa = old->nodes[a].parent, pb = next->nodes[b].parent;
    while (pa != MD_INDEX_NONE && pb != MD_INDEX_NONE) {
        if (next->nodes[pb].kind == MD_NODE_LI &&
            first_in_item(old, pa, a) != first_in_item(next, pb, b)) return false;
        pa = old->nodes[pa].parent;
        pb = next->nodes[pb].parent;
    }
    if (next->nodes[b].kind == MD_NODE_TR) {
        unsigned quotes, columns = children(next, b);
        char marker[24];
        int indent = indentation(next, b, width, &quotes, marker, sizeof(marker));
        /* Only stacked cells repeat the header inside each data row. */
        if (columns && (width - indent) / (int)columns < epd_ui_px(112)) {
            uint32_t ah = table_header(old, a), bh = table_header(next, b);
            if ((ah == MD_INDEX_NONE) != (bh == MD_INDEX_NONE)) return false;
            if (ah != MD_INDEX_NONE && !md_node_equal(old, ah, next, bh)) return false;
        }
    }
    return true;
}

/* Syntax limits affect presentation only; always retain and show the text. */
static bool plain_fallback(chat_markdown_t *view, const char *text, size_t length, int width)
{
    if (!room_for(length * 2u + 8192u)) return false;
    char *source = allocate(NULL, length + 1);
    rendered_block_t *block = allocate(NULL, sizeof(*block));
    if (!source || !block) { release(NULL, source); release(NULL, block); return false; }
    memcpy(source, text, length + 1);
    lv_obj_t *staging = container(NULL);
    if (!staging) { release(NULL, source); release(NULL, block); return false; }
    lv_obj_t *label = epd_app_label(staging, text, 0, 0, width, epd_app_font_role(EPD_FONT_BODY));
    if (!label) { lv_obj_delete(staging); release(NULL, source); release(NULL, block); return false; }
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(label, epd_ui_px(8), 0);
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    lv_obj_update_layout(label);
#if CHAT_PLAIN_LABEL
    wait_label_draws(view->blocks, view->count);
#endif
    for (size_t i = 0; i < view->count; ++i) {
        lv_obj_delete(view->blocks[i].object);
#if CHAT_PLAIN_LABEL
        release(NULL, view->blocks[i].label_text);
#endif
    }
    lv_obj_set_parent(label, view->root);
    lv_obj_delete(staging);
    md_document_free(&view->document);
    release(NULL, view->blocks); release(NULL, view->source);
    block->object = label; block->node = MD_INDEX_NONE; block->reused = false;
#if CHAT_PLAIN_LABEL
    block->label_text = NULL;
#endif
    view->blocks = block; view->count = 1; view->source = source;
    view->width = width; view->scale = epd_ui_scale_revision();
    lv_obj_set_size(view->root, width, LV_MAX(lv_obj_get_height(label), epd_ui_px(32)));
    return true;
}

bool chat_markdown_update(chat_markdown_t *view, const char *text, size_t length, int width)
{
    if (!view || !text || width <= 0 || length > MD_INPUT_MAX || strlen(text) != length) return false;
    uint32_t scale = epd_ui_scale_revision();
    if (view->source && view->width == width && view->scale == scale && !strcmp(view->source, text)) return true;
#if CHAT_PLAIN_LABEL
    markdown_restore_timing_t timing = {0};
    bool measure_restore = !view->source && length >= 4096u;
    uint32_t parse_at = measure_restore ? lv_tick_get() : 0;
#endif
    md_doc_t next = {0};
    md_status_t parsed = md_document_parse(text, length, &allocator, NULL, &next);
    if (parsed == MD_LIMIT) return plain_fallback(view, text, length, width);
    if (parsed != MD_OK) return false;
#if CHAT_PLAIN_LABEL
    uint32_t prep_at = measure_restore ? lv_tick_get() : 0;
#endif
    size_t cost = render_cost(&next);
    if (cost > RENDER_BUDGET) {
        md_document_free(&next);
        return plain_fallback(view, text, length, width);
    }
    if (!room_for(cost)) { md_document_free(&next); return false; }
    char *source = allocate(NULL, length + 1);
    rendered_block_t *blocks = allocate(NULL, (next.node_count ? next.node_count : 1) * sizeof(*blocks));
    if (!source || !blocks) {
        release(NULL, source); release(NULL, blocks); md_document_free(&next); return false;
    }
    memcpy(source, text, length + 1);
    /* An inactive screen allows measurement without invalidating the visible
     * message. New blocks enter the active tree only at their final positions. */
    lv_obj_t *staging = container(NULL);
    if (!staging) {
        release(NULL, source); release(NULL, blocks); md_document_free(&next); return false;
    }
#if CHAT_PLAIN_LABEL
    uint32_t build_at = measure_restore ? lv_tick_get() : 0;
#endif
    size_t count = 0;
    for (uint32_t i = 1; i < next.node_count; ++i) {
        if (!leaf(next.nodes[i].kind)) continue;
        rendered_block_t *entry = &blocks[count];
        memset(entry, 0, sizeof(*entry)); entry->node = i;
        if (view->width == width && view->scale == scale && count < view->count &&
            same_block(&view->document, view->blocks[count].node, &next, i, width)) {
            entry->object = view->blocks[count].object; entry->reused = true;
#if CHAT_PLAIN_LABEL
            entry->label_text = view->blocks[count].label_text;
#endif
        } else {
            entry->object = build_block(staging, &next, i, width
#if CHAT_PLAIN_LABEL
                                        , &entry->label_text, measure_restore ? &timing : NULL
#endif
                                        );
            if (!entry->object) {
                lv_obj_delete(staging);
#if CHAT_PLAIN_LABEL
                for (size_t j = 0; j <= count; ++j)
                    if (!blocks[j].reused) release(NULL, blocks[j].label_text);
#endif
                release(NULL, source); release(NULL, blocks); md_document_free(&next); return false;
            }
            lv_obj_add_flag(entry->object, LV_OBJ_FLAG_HIDDEN);
        }
        ++count;
        /* A row owns its cells; no second pass through their content. */
        i = next.nodes[i].subtree_end - 1;
    }
#if CHAT_PLAIN_LABEL
    uint32_t install_at = measure_restore ? lv_tick_get() : 0;
    for (size_t i = 0; i < view->count; ++i) {
        if ((i >= count || !blocks[i].reused) && view->blocks[i].label_text) {
            lv_draw_wait_for_finish();
            break;
        }
    }
#endif
    for (size_t i = 0; i < view->count; ++i) {
        if (i < count && blocks[i].reused) continue;
        lv_obj_delete(view->blocks[i].object);
#if CHAT_PLAIN_LABEL
        release(NULL, view->blocks[i].label_text);
#endif
    }
    int y = 0;
    for (size_t i = 0; i < count; ++i) {
        const md_node_t *node = &next.nodes[blocks[i].node];
        lv_obj_set_pos(blocks[i].object, 0, y);
        if (!blocks[i].reused) lv_obj_set_parent(blocks[i].object, view->root);
        lv_obj_update_layout(blocks[i].object);
        lv_obj_remove_flag(blocks[i].object, LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(blocks[i].object);
        y += lv_obj_get_height(blocks[i].object);
        if (i + 1 < count) y += epd_ui_px(node->kind == MD_NODE_TR ? 0 : 16);
    }
    lv_obj_delete(staging);
    lv_obj_set_size(view->root, width, LV_MAX(y, epd_ui_px(32)));
    md_document_free(&view->document);
    release(NULL, view->blocks); release(NULL, view->source);
    view->document = next; view->source = source; view->blocks = blocks;
    view->count = count; view->width = width; view->scale = scale;
#if CHAT_PLAIN_LABEL
    if (measure_restore) {
        uint32_t finished = lv_tick_get();
        rt_kprintf("ai_md_restore bytes=%lu blocks=%lu labels=%lu total_ms=%lu\n",
                   (unsigned long)length, (unsigned long)count, (unsigned long)timing.labels,
                   (unsigned long)(finished - parse_at));
        rt_kprintf("ai_md_restore stages parse_ms=%lu prep_ms=%lu build_ms=%lu install_ms=%lu\n",
                   (unsigned long)(prep_at - parse_at), (unsigned long)(build_at - prep_at),
                   (unsigned long)(install_at - build_at), (unsigned long)(finished - install_at));
        rt_kprintf("ai_md_restore label setup_ms=%lu set_text_ms=%lu text_layout_ms=%lu box_layout_ms=%lu\n",
                   (unsigned long)timing.label_setup, (unsigned long)timing.set_text,
                   (unsigned long)timing.text_layout, (unsigned long)timing.box_layout);
    }
#endif
    return true;
}
