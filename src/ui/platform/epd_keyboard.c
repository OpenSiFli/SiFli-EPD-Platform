#include "epd_keyboard.h"
#include "epd_keyboard_internal.h"
#include "epd_pinyin.h"
#include "epd_app.h"
#include "epd_i18n.h"
#include "ui_voice_input.h"
#include "src/core/lv_obj_private.h"
#include "src/core/lv_obj_class_private.h"
#include "src/widgets/buttonmatrix/lv_buttonmatrix_private.h"
#include <rtm.h>
#include <string.h>

#define CANDIDATES 4
#define KEY_MAP_SIZE 48
#define KEY_BACKGROUND 0xe4e5e7
#define FUNCTION_BACKGROUND 0xced0d2
typedef enum { KEYS_LETTERS, KEYS_SYMBOLS, KEYS_SYMBOLS_MORE, KEYS_NUMBERS } key_page_t;
typedef struct
{
    lv_obj_t obj;
    lv_obj_t *keys, *target, *preedit, *language, *voice, *close;
    lv_obj_t *cursor_left, *cursor_right;
    lv_obj_t *previous, *next, *candidate[CANDIDATES];
    epd_keyboard_config_t config;
    epd_pinyin_t pinyin;
    epd_pinyin_candidate_t choices[CANDIDATES + 1];
    size_t offset, count;
    uint32_t identity;
    bool layout_busy;
    bool uppercase;
    key_page_t page;
    const char *key_map[KEY_MAP_SIZE];
    lv_buttonmatrix_ctrl_t key_ctrl[KEY_MAP_SIZE];
    unsigned map_signature;
    char composition[EPD_PINYIN_MAX * 2 + 4];
    uint8_t composition_positions[EPD_PINYIN_MAX * 2 + 4];
} keyboard_t;

static const lv_obj_class_t keyboard_class = {
    .base_class = &lv_obj_class, .instance_size = sizeof(keyboard_t),
    .group_def = LV_OBJ_CLASS_GROUP_DEF_FALSE, .editable = LV_OBJ_CLASS_EDITABLE_TRUE,
    .name = "epd_keyboard"
};
static uint32_t identity;
static void render(keyboard_t *kb);
static void target_event(lv_event_t *event);

static bool language_allowed(keyboard_t *kb)
{
    return kb->config.mode == EPD_KEYBOARD_TEXT && kb->target &&
           !lv_textarea_get_password_mode(kb->target);
}

static keyboard_t *instance(lv_obj_t *obj)
{
    return obj && lv_obj_check_type(obj, &keyboard_class) ? (keyboard_t *)obj : NULL;
}

static bool alive(lv_obj_t *obj, uint32_t id)
{
    return lv_obj_is_valid(obj) && instance(obj) && ((keyboard_t *)obj)->identity == id;
}

static bool chinese(keyboard_t *kb)
{
    return kb->config.mode == EPD_KEYBOARD_TEXT && kb->config.chinese &&
        kb->target && !lv_textarea_get_password_mode(kb->target);
}

static int32_t preedit_height(keyboard_t *kb)
{
    return kb->config.mode == EPD_KEYBOARD_TEXT ?
        lv_font_get_line_height(epd_app_font_role(EPD_FONT_SMALL)) + epd_ui_px(10) : 0;
}

bool epd_keyboard_allows_voice(lv_obj_t *keys)
{
    keyboard_t *kb = keys ? instance(lv_obj_get_parent(keys)) : NULL;
    return !kb || (kb->config.mode == EPD_KEYBOARD_TEXT && kb->config.voice &&
                   !kb->pinyin.text[0] && kb->target &&
                   !lv_textarea_get_password_mode(kb->target) &&
                   !lv_textarea_get_accepted_chars(kb->target));
}

static size_t character_bytes(const char *text)
{
    size_t n = 1;
    while (n < 4 && text[n] && ((unsigned char)text[n] & 0xc0) == 0x80) ++n;
    return n;
}

static size_t character_count(const char *text)
{
    size_t count = 0;
    for (; *text; text += character_bytes(text)) ++count;
    return count;
}

static bool accepts(lv_obj_t *target, const char *text)
{
    uint32_t maximum = lv_textarea_get_max_length(target);
    if (maximum && character_count(lv_textarea_get_text(target)) + character_count(text) > maximum)
        return false;
    const char *allowed = lv_textarea_get_accepted_chars(target);
    if (!allowed) return true;
    for (; *text; text += character_bytes(text)) {
        size_t n = character_bytes(text);
        const char *p = allowed;
        for (; *p; p += character_bytes(p))
            if (character_bytes(p) == n && !memcmp(p, text, n)) break;
        if (!*p) return false;
    }
    return true;
}

/* Insert whole candidates only when they fit. Revalidate between LVGL change
 * notifications because an application may close or rebind its editor. */
static bool insert(keyboard_t *kb, const char *text)
{
    lv_obj_t *root = &kb->obj, *target = kb->target;
    uint32_t id = kb->identity;
    if (!target || !accepts(target, text)) return false;
    size_t before = strlen(lv_textarea_get_text(target)), bytes = strlen(text);
    if (!lv_textarea_get_max_length(target) && !lv_textarea_get_accepted_chars(target)) {
        lv_textarea_add_text(target, text);
        return alive(root, id) && kb->target == target &&
            strlen(lv_textarea_get_text(target)) == before + bytes;
    }
    for (const char *p = text; *p;) {
        size_t n = character_bytes(p);
        char character[5];
        memcpy(character, p, n); character[n] = 0;
        lv_textarea_add_text(target, character);
        if (!alive(root, id) || kb->target != target) return false;
        p += n;
    }
    return strlen(lv_textarea_get_text(target)) == before + bytes;
}

static void select_candidate(keyboard_t *kb, size_t index)
{
    if (index >= kb->count || index >= CANDIDATES) return;
    lv_obj_t *root = &kb->obj;
    uint32_t id = kb->identity;
    epd_pinyin_candidate_t chosen = kb->choices[index];
    if (!insert(kb, chosen.text) || !alive(root, id)) return;
    epd_pinyin_consume(&kb->pinyin, chosen.consumed);
    kb->offset = 0;
    render(kb);
}

static bool insert_composition(keyboard_t *kb)
{
    if (!kb->pinyin.text[0]) return true;
    char text[EPD_PINYIN_MAX + 1];
    memcpy(text, kb->pinyin.text, sizeof(text));
    lv_obj_t *root = &kb->obj;
    uint32_t id = kb->identity;
    if (!insert(kb, text) || !alive(root, id)) return false;
    epd_pinyin_reset(&kb->pinyin);
    kb->offset = 0;
    return true;
}

static void finish(keyboard_t *kb, lv_event_code_t code)
{
    if (code == LV_EVENT_READY && kb->pinyin.text[0]) {
        select_candidate(kb, 0);
        return;
    }
    epd_pinyin_reset(&kb->pinyin);
    kb->offset = 0;
    render(kb);
    lv_obj_send_event(&kb->obj, code, NULL);
}

bool epd_keyboard_back(lv_obj_t *root)
{
    if (!root || lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) return false;
    keyboard_t *kb = instance(root);
    if (kb) {
        finish(kb, LV_EVENT_CANCEL);
        return true;
    }
    for (int32_t i = (int32_t)lv_obj_get_child_count(root) - 1; i >= 0; --i)
        if (epd_keyboard_back(lv_obj_get_child(root, i))) return true;
    return false;
}

static void map_key(keyboard_t *kb, unsigned *entry, unsigned *button_id,
                    const char *text, unsigned width, bool function)
{
    kb->key_map[(*entry)++] = text;
    unsigned flags = width | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT;
    if (function) flags |= LV_BUTTONMATRIX_CTRL_CHECKED;
    if (!strcmp(text, "\1")) flags |= LV_BUTTONMATRIX_CTRL_HIDDEN | LV_BUTTONMATRIX_CTRL_DISABLED;
    if (!strcmp(text, LV_SYMBOL_BACKSPACE)) flags &= ~(LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT);
    kb->key_ctrl[(*button_id)++] = (lv_buttonmatrix_ctrl_t)flags;
}

static void configure_map(keyboard_t *kb)
{
    bool cn = chinese(kb), language = language_allowed(kb);
    bool number_only = kb->config.mode == EPD_KEYBOARD_NUMBER;
    unsigned signature = 1 + kb->page + 4 * cn + 8 * kb->uppercase +
                         16 * language + 32 * number_only;
    if (kb->map_signature == signature) return;
    kb->map_signature = signature;
    unsigned entry = 0, id = 0;
#define KEY(text, width, function) map_key(kb, &entry, &id, text, width, function)
#define ROW() (kb->key_map[entry++] = "\n")
    if (kb->page == KEYS_NUMBERS || number_only) {
        KEY("1", 4, false); KEY("2", 4, false); KEY("3", 4, false); KEY(LV_SYMBOL_BACKSPACE, 4, true); ROW();
        KEY("4", 4, false); KEY("5", 4, false); KEY("6", 4, false); KEY(number_only ? LV_SYMBOL_LEFT : "ABC", 4, true); ROW();
        KEY("7", 4, false); KEY("8", 4, false); KEY("9", 4, false); KEY(number_only ? LV_SYMBOL_RIGHT : "+/-", 4, true); ROW();
        KEY(number_only ? "+/-" : (cn ? "符" : "Sym"), 4, true);
        KEY("0", 4, false); KEY(".", 4, false); KEY(LV_SYMBOL_NEW_LINE, 4, true);
    } else {
        if (kb->page == KEYS_LETTERS) {
            static const char * const upper[] = {"Q","W","E","R","T","Y","U","I","O","P",
                "A","S","D","F","G","H","J","K","L","Z","X","C","V","B","N","M"};
            static const char * const lower[] = {"q","w","e","r","t","y","u","i","o","p",
                "a","s","d","f","g","h","j","k","l","z","x","c","v","b","n","m"};
            const char * const *letters = cn || kb->uppercase ? upper : lower;
            for (unsigned i = 0; i < 10; ++i) KEY(letters[i], 4, false);
            ROW(); KEY("\1", 2, false);
            for (unsigned i = 10; i < 19; ++i) KEY(letters[i], 4, false);
            KEY("\1", 2, false); ROW();
            KEY(cn ? "分词" : LV_SYMBOL_UP, 5, true);
            for (unsigned i = 19; i < 26; ++i) KEY(letters[i], 4, false);
            KEY(LV_SYMBOL_BACKSPACE, 5, true); ROW();
        } else {
            static const char * const symbols[] = {"@","#","$","_","&","-","+","(",")","/",
                "=","%","\"","'",":",";","!","?","[","]","\\","|","~","*","<",">","^"};
            static const char * const more[] = {"，","。","、","！","？","：","；","“","”","…",
                "（","）","【","】","《","》","「","」","{","}","·","—","～","‘","’","`","&"};
            static const char * const more_ascii[] = {"1","2","3","4","5","6","7","8","9","0",
                "{","}","[","]","<",">","^","`","~","_","\\","|","$","@","%","&","*"};
            const char * const *symbols_page = kb->page == KEYS_SYMBOLS_MORE ? (cn ? more : more_ascii) : symbols;
            for (unsigned i = 0; i < 10; ++i) KEY(symbols_page[i], 4, false);
            ROW();
            for (unsigned i = 10; i < 20; ++i) KEY(symbols_page[i], 4, false);
            ROW(); KEY(kb->page == KEYS_SYMBOLS_MORE ? "1/2" : "2/2", 5, true);
            for (unsigned i = 20; i < 27; ++i) KEY(symbols_page[i], 4, false);
            KEY(LV_SYMBOL_BACKSPACE, 5, true); ROW();
        }
        KEY(kb->page == KEYS_LETTERS ? (cn ? "符" : "Sym") : "ABC", 5, true);
        KEY("123", 5, true); KEY(cn ? "，" : ",", 4, false);
        KEY(cn ? "空格" : "space", 11, false); KEY(cn ? "。" : ".", 4, false);
        KEY(language ? "中/EN" : "EN", 5, true);
        if (!language) kb->key_ctrl[id - 1] |= LV_BUTTONMATRIX_CTRL_DISABLED;
        KEY(LV_SYMBOL_NEW_LINE, 6, true);
    }
    kb->key_map[entry] = "";
#undef ROW
#undef KEY
    /* Keep maps per instance: LVGL's keyboard map setter changes global tables. */
    lv_buttonmatrix_set_map(kb->keys, kb->key_map);
    lv_buttonmatrix_set_ctrl_map(kb->keys, kb->key_ctrl);
    lv_buttonmatrix_set_selected_button(kb->keys, LV_BUTTONMATRIX_BUTTON_NONE);
}

static void switch_language(keyboard_t *kb)
{
    if (!language_allowed(kb) || !insert_composition(kb)) return;
    kb->config.chinese = !kb->config.chinese;
    kb->page = KEYS_LETTERS;
    kb->uppercase = false;
    render(kb);
}

static void keys_event(lv_event_t *event)
{
    keyboard_t *kb = lv_event_get_user_data(event);
    uint32_t index = lv_buttonmatrix_get_selected_button(kb->keys);
    if (index == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *text = lv_buttonmatrix_get_button_text(kb->keys, index);
    if (!text) return;
    if (!strcmp(text, LV_SYMBOL_OK)) { finish(kb, LV_EVENT_READY); return; }
    if (!strcmp(text, LV_SYMBOL_CLOSE) || !strcmp(text, LV_SYMBOL_KEYBOARD)) {
        finish(kb, LV_EVENT_CANCEL); return;
    }
    if (!kb->target) return;
    if (lv_buttonmatrix_has_button_ctrl(kb->keys, index, LV_BUTTONMATRIX_CTRL_DISABLED)) return;
    if (!strcmp(text, "中/EN")) { switch_language(kb); return; }
    if (!strcmp(text, "符") || !strcmp(text, "Sym")) { kb->page = KEYS_SYMBOLS; render(kb); return; }
    if (!strcmp(text, "123")) { kb->page = KEYS_NUMBERS; render(kb); return; }
    if (!strcmp(text, "ABC")) { kb->page = KEYS_LETTERS; render(kb); return; }
    if (!strcmp(text, "1/2") || !strcmp(text, "2/2")) {
        kb->page = kb->page == KEYS_SYMBOLS ? KEYS_SYMBOLS_MORE : KEYS_SYMBOLS;
        render(kb); return;
    }
    if (!strcmp(text, LV_SYMBOL_UP)) { kb->uppercase = !kb->uppercase; render(kb); return; }
    if (!strcmp(text, "分词")) text = "'";
    if (!strcmp(text, "空格") || !strcmp(text, "space")) text = " ";
    if (chinese(kb)) {
        char key = text[0];
        if (!text[1] && ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') || key == '\'')) {
            if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
            epd_pinyin_insert(&kb->pinyin, key);
            kb->offset = 0;
            render(kb);
            return;
        }
        if (kb->pinyin.text[0]) {
            if (!strcmp(text, LV_SYMBOL_BACKSPACE)) epd_pinyin_backspace(&kb->pinyin);
            else if (!strcmp(text, LV_SYMBOL_LEFT)) epd_pinyin_move(&kb->pinyin, -1);
            else if (!strcmp(text, LV_SYMBOL_RIGHT)) epd_pinyin_move(&kb->pinyin, 1);
            else if (!strcmp(text, " ") || !strcmp(text, "Enter") || !strcmp(text, LV_SYMBOL_NEW_LINE)) {
                select_candidate(kb, 0); return;
            } else {
                if (kb->count && kb->choices[0].consumed == strlen(kb->pinyin.text)) {
                    lv_obj_t *root = &kb->obj, *target = kb->target;
                    uint32_t id = kb->identity;
                    select_candidate(kb, 0);
                    if (alive(root, id) && kb->target == target && !kb->pinyin.text[0]) insert(kb, text);
                }
                return;
            }
            kb->offset = 0;
            render(kb);
            return;
        }
    }
    if ((!strcmp(text, "Enter") || !strcmp(text, LV_SYMBOL_NEW_LINE)) &&
        lv_textarea_get_one_line(kb->target)) {
        finish(kb, LV_EVENT_READY); return;
    }
    lv_obj_t *root = &kb->obj, *target = kb->target;
    uint32_t id = kb->identity;
    if (!strcmp(text, LV_SYMBOL_LEFT)) lv_textarea_cursor_left(target);
    else if (!strcmp(text, LV_SYMBOL_RIGHT)) lv_textarea_cursor_right(target);
    else if (!strcmp(text, LV_SYMBOL_BACKSPACE)) lv_textarea_delete_char(target);
    else if (!strcmp(text, "Enter") || !strcmp(text, LV_SYMBOL_NEW_LINE)) insert(kb, "\n");
    else if (!strcmp(text, "+/-")) {
        uint32_t cursor = lv_textarea_get_cursor_pos(target);
        char first = lv_textarea_get_text(target)[0];
        bool sign = first == '+' || first == '-';
        lv_textarea_set_cursor_pos(target, sign ? 1 : 0);
        if (sign) lv_textarea_delete_char(target);
        if (!alive(root, id) || kb->target != target) return;
        insert(kb, first == '-' ? "+" : "-");
        if (!alive(root, id) || kb->target != target) return;
        lv_textarea_set_cursor_pos(target, cursor + (sign ? 0 : 1));
    } else insert(kb, text);
}

static void button_event(lv_event_t *event)
{
    keyboard_t *kb = lv_event_get_user_data(event);
    lv_obj_t *button = lv_event_get_current_target(event);
    if (button == kb->close) { finish(kb, LV_EVENT_CANCEL); return; }
    if (button == kb->cursor_left || button == kb->cursor_right) {
        if (kb->target) {
            if (button == kb->cursor_left) lv_textarea_cursor_left(kb->target);
            else lv_textarea_cursor_right(kb->target);
        }
        return;
    }
    if (button == kb->voice) {
        epd_keyboard_open_voice(&kb->obj);
        return;
    }
    if (button == kb->previous) kb->offset = kb->offset >= CANDIDATES ? kb->offset - CANDIDATES : 0;
    else if (button == kb->next && kb->count > CANDIDATES) kb->offset += CANDIDATES;
    else {
        for (unsigned i = 0; i < CANDIDATES; ++i)
            if (button == kb->candidate[i]) { select_candidate(kb, i); return; }
    }
    render(kb);
}

static void flat(lv_obj_t *obj, lv_style_selector_t part)
{
    const lv_state_t states[] = {LV_STATE_DEFAULT, LV_STATE_FOCUSED,
                                LV_STATE_CHECKED, LV_STATE_DISABLED,
                                LV_STATE_CHECKED | LV_STATE_FOCUSED};
    for (unsigned i = 0; i < sizeof(states) / sizeof(states[0]); ++i) {
        lv_style_selector_t selector = part | states[i];
        lv_obj_set_style_bg_color(obj,
            lv_color_hex(states[i] & LV_STATE_CHECKED ? FUNCTION_BACKGROUND : 0xffffff), selector);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, selector);
        lv_obj_set_style_text_color(obj, lv_color_black(), selector);
        lv_obj_set_style_border_width(obj, 0, selector);
        lv_obj_set_style_shadow_width(obj, 0, selector);
        lv_obj_set_style_outline_width(obj, 0, selector);
    }
    lv_obj_set_style_radius(obj, epd_ui_px(10), part);
    lv_obj_set_style_opa(obj, LV_OPA_40, part | LV_STATE_DISABLED);
}

static void icon_draw(lv_event_t *event)
{
    keyboard_t *kb = lv_event_get_user_data(event);
    lv_obj_t *obj = lv_event_get_current_target_obj(event);
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    int32_t x = (area.x1 + area.x2) / 2, y = (area.y1 + area.y2) / 2;
    int32_t s = epd_ui_px(12);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_black();
    line.width = LV_MAX(2, epd_ui_px(2));
    line.round_start = line.round_end = true;
    if (obj == kb->voice) {
        lv_draw_rect_dsc_t rect;
        lv_draw_rect_dsc_init(&rect);
        rect.bg_opa = LV_OPA_TRANSP;
        rect.border_color = line.color;
        rect.border_width = line.width;
        rect.radius = s / 2;
        lv_area_t capsule = {x - s / 2, y - s, x + s / 2, y + s / 3};
        lv_draw_rect(layer, &rect, &capsule);
        lv_draw_arc_dsc_t arc;
        lv_draw_arc_dsc_init(&arc);
        arc.center = (lv_point_t){x, y}; arc.radius = s;
        arc.start_angle = 0; arc.end_angle = 180;
        arc.width = line.width; arc.color = line.color; arc.rounded = true;
        lv_draw_arc(layer, &arc);
        line.p1 = (lv_point_precise_t){x, y + s};
        line.p2 = (lv_point_precise_t){x, y + s + s / 2};
        lv_draw_line(layer, &line);
        line.p1 = (lv_point_precise_t){x - s / 2, y + s + s / 2};
        line.p2 = (lv_point_precise_t){x + s / 2, y + s + s / 2};
    } else if (obj == kb->close) {
        line.p1 = (lv_point_precise_t){x - s, y - s / 3};
        line.p2 = (lv_point_precise_t){x, y + s / 2};
        lv_draw_line(layer, &line);
        line.p1 = line.p2;
        line.p2 = (lv_point_precise_t){x + s, y - s / 3};
    } else {
        int32_t direction = obj == kb->cursor_left || obj == kb->previous ? -1 : 1;
        line.p1 = (lv_point_precise_t){x - direction * s / 3, y - s};
        line.p2 = (lv_point_precise_t){x + direction * s / 3, y};
        lv_draw_line(layer, &line);
        line.p1 = line.p2;
        line.p2 = (lv_point_precise_t){x - direction * s / 3, y + s};
    }
    lv_draw_line(layer, &line);
}

static bool key_icon(const char *text)
{
    return text && (!strcmp(text, LV_SYMBOL_BACKSPACE) || !strcmp(text, LV_SYMBOL_NEW_LINE) ||
                    !strcmp(text, LV_SYMBOL_UP));
}

static void key_icons_draw(lv_event_t *event)
{
    keyboard_t *kb = lv_event_get_user_data(event);
    if (lv_event_get_code(event) == LV_EVENT_DRAW_TASK_ADDED) {
        lv_draw_label_dsc_t *label = lv_draw_task_get_label_dsc(lv_event_get_draw_task(event));
        if (label && label->base.part == LV_PART_ITEMS && key_icon(label->text)) label->opa = LV_OPA_TRANSP;
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_buttonmatrix_t *matrix = (lv_buttonmatrix_t *)kb->keys;
    lv_area_t origin;
    lv_obj_get_coords(kb->keys, &origin);
    for (unsigned i = 0; i < matrix->btn_cnt; ++i) {
        const char *text = lv_buttonmatrix_get_button_text(kb->keys, i);
        if (!key_icon(text)) continue;
        const lv_area_t *area = &matrix->button_areas[i];
        int32_t x = origin.x1 + (area->x1 + area->x2) / 2;
        int32_t y = origin.y1 + (area->y1 + area->y2) / 2;
        int32_t s = LV_MIN(epd_ui_px(16), LV_MIN(lv_area_get_width(area), lv_area_get_height(area)) / 3);
        lv_draw_line_dsc_t line;
        lv_draw_line_dsc_init(&line);
        line.width = LV_MAX(2, epd_ui_px(2));
        line.round_start = line.round_end = true;
        line.color = lv_color_black();
#define SEGMENT(ax, ay, bx, by) do { \
        line.p1 = (lv_point_precise_t){x + (ax), y + (ay)}; \
        line.p2 = (lv_point_precise_t){x + (bx), y + (by)}; \
        lv_draw_line(layer, &line); \
    } while (0)
        if (!strcmp(text, LV_SYMBOL_BACKSPACE)) {
            SEGMENT(-s, 0, -s / 3, -s * 2 / 3);
            SEGMENT(-s / 3, -s * 2 / 3, s, -s * 2 / 3);
            SEGMENT(s, -s * 2 / 3, s, s * 2 / 3);
            SEGMENT(s, s * 2 / 3, -s / 3, s * 2 / 3);
            SEGMENT(-s / 3, s * 2 / 3, -s, 0);
            SEGMENT(-s / 6, -s / 3, s / 2, s / 3);
            SEGMENT(-s / 6, s / 3, s / 2, -s / 3);
        } else if (!strcmp(text, LV_SYMBOL_NEW_LINE)) {
            SEGMENT(s, -s / 2, s, s / 3);
            SEGMENT(s, s / 3, -s, s / 3);
            SEGMENT(-s, s / 3, -s / 2, -s / 6);
            SEGMENT(-s, s / 3, -s / 2, s * 5 / 6);
        } else {
            SEGMENT(0, -s, -s, 0); SEGMENT(-s, 0, -s / 2, 0);
            SEGMENT(-s / 2, 0, -s / 2, s * 2 / 3);
            SEGMENT(-s / 2, s * 2 / 3, s / 2, s * 2 / 3);
            SEGMENT(s / 2, s * 2 / 3, s / 2, 0); SEGMENT(s / 2, 0, s, 0);
            SEGMENT(s, 0, 0, -s);
            if (kb->uppercase) SEGMENT(-s / 2, s, s / 2, s);
        }
#undef SEGMENT
    }
}

static lv_obj_t *button(keyboard_t *kb, const char *text)
{
    lv_obj_t *obj = lv_button_create(&kb->obj);
    epd_app_object_init(obj);
    flat(obj, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(KEY_BACKGROUND), 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(obj, button_event, LV_EVENT_CLICKED, kb);
    lv_obj_t *label = lv_label_create(obj);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_center(label);
    return obj;
}

static lv_obj_t *icon_button(keyboard_t *kb)
{
    lv_obj_t *obj = button(kb, "");
    lv_obj_add_event_cb(obj, icon_draw, LV_EVENT_DRAW_MAIN, kb);
    return obj;
}

static void enable(lv_obj_t *obj, bool enabled)
{
    if (enabled) lv_obj_remove_state(obj, LV_STATE_DISABLED);
    else lv_obj_add_state(obj, LV_STATE_DISABLED);
}

static void visible(lv_obj_t *obj, bool shown)
{
    if (shown) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void set_label(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text)) lv_label_set_text(label, text);
}

static void composition_render(keyboard_t *kb, int32_t width, const lv_font_t *font)
{
    char full[sizeof(kb->composition)];
    uint8_t positions[sizeof(kb->composition_positions)];
    size_t length = strlen(kb->pinyin.text), used = 0, letters = 0, cursor_at = 0;
    uint32_t boundaries = kb->count ? kb->choices[0].boundaries : 0;
    if (kb->offset) {
        epd_pinyin_candidate_t primary;
        if (epd_pinyin_candidates(&kb->pinyin, 0, &primary, 1)) boundaries = primary.boundaries;
    }
    for (size_t i = 0; i <= length; ++i) {
        if (i == kb->pinyin.cursor) {
            cursor_at = used;
            if (i < length) { positions[used] = (uint8_t)i; full[used++] = '|'; }
        }
        if (i == length) break;
        if (i && kb->pinyin.text[i] != '\'' && kb->pinyin.text[i - 1] != '\'' &&
            letters < 32 && (boundaries & ((uint32_t)1 << letters))) {
            positions[used] = (uint8_t)i;
            full[used++] = '\'';
        }
        positions[used] = (uint8_t)i;
        full[used++] = kb->pinyin.text[i];
        if (kb->pinyin.text[i] != '\'') ++letters;
    }
    positions[used] = (uint8_t)length;
    full[used] = 0;
    int32_t padding = epd_ui_px(12), maximum = LV_MAX(1, width * 2 / 3 - padding * 2);
    size_t first = 0, last = used;
    lv_point_t size;
    for (;;) {
        full[last] = 0;
        lv_text_get_size(&size, full + first, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x <= maximum || last - first <= 1) break;
        if (cursor_at > (first + last) / 2) ++first;
        else --last;
    }
    memcpy(kb->composition, full + first, last - first);
    kb->composition[last - first] = 0;
    memcpy(kb->composition_positions, positions + first, last - first + 1);
    set_label(kb->preedit, kb->composition);
    lv_obj_set_style_text_font(kb->preedit, font, 0);
    lv_obj_set_style_pad_hor(kb->preedit, padding, 0);
    lv_obj_set_style_pad_ver(kb->preedit, epd_ui_px(5), 0);
    int32_t height = preedit_height(kb);
    lv_obj_set_pos(kb->preedit, 0, 0);
    lv_obj_set_size(kb->preedit, size.x + padding * 2, height);
}

static void preedit_event(lv_event_t *event)
{
    keyboard_t *kb = lv_event_get_user_data(event);
    lv_indev_t *indev = lv_event_get_indev(event);
    if (!indev || !kb->pinyin.text[0]) return;
    lv_point_t point, size;
    lv_indev_get_point(indev, &point);
    lv_area_t area;
    lv_obj_get_coords(kb->preedit, &area);
    int32_t x = point.x - area.x1 - lv_obj_get_style_pad_left(kb->preedit, 0);
    const lv_font_t *font = lv_obj_get_style_text_font(kb->preedit, 0);
    char prefix[sizeof(kb->composition)];
    size_t length = strlen(kb->composition), selected = length;
    int32_t before = 0;
    for (size_t i = 0; i < length; ++i) {
        memcpy(prefix, kb->composition, i + 1); prefix[i + 1] = 0;
        lv_text_get_size(&size, prefix, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (x < (before + size.x) / 2) { selected = i; break; }
        before = size.x;
    }
    kb->pinyin.cursor = kb->composition_positions[selected];
    render(kb);
}

static void render(keyboard_t *kb)
{
    if (!kb->keys || kb->layout_busy) return;
    kb->layout_busy = true;
    bool use_pinyin = chinese(kb), composing = use_pinyin && kb->pinyin.text[0];
    int32_t w = lv_obj_get_content_width(&kb->obj), h = lv_obj_get_content_height(&kb->obj);
    int32_t headroom = preedit_height(kb);
    int32_t bar = LV_MIN(epd_ui_px(68), LV_MAX(1, (h - headroom) / 5));
    int32_t inset = epd_ui_px(8), gap = epd_ui_px(8);
    const lv_font_t *font = epd_app_font_role(EPD_FONT_BODY);
    const lv_font_t *small = epd_app_font_role(EPD_FONT_SMALL);
    lv_obj_set_style_text_font(&kb->obj, font, 0);
    lv_obj_set_style_text_font(kb->keys, epd_app_font_role(EPD_FONT_TITLE), LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb->keys, small, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_pad_row(kb->keys, gap, 0);
    lv_obj_set_style_pad_column(kb->keys, gap, 0);
    lv_obj_set_style_radius(kb->keys, epd_ui_px(10), LV_PART_ITEMS);
    lv_obj_set_pos(kb->keys, inset, headroom + bar);
    lv_obj_set_size(kb->keys, LV_MAX(1, w - inset * 2), LV_MAX(1, h - headroom - bar - inset));
    configure_map(kb);
    int32_t control_w = LV_MIN(epd_ui_touch(72), LV_MAX(1, w / 7));
    bool voice_allowed = kb->config.mode == EPD_KEYBOARD_TEXT && kb->config.voice && kb->target &&
                         !lv_textarea_get_password_mode(kb->target) && !lv_textarea_get_accepted_chars(kb->target);
    lv_obj_t *controls[] = {kb->close, kb->voice, kb->cursor_right, kb->cursor_left};
    int32_t x = w - inset;
    for (unsigned i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i) {
        bool allowed = controls[i] != kb->voice || voice_allowed;
        visible(controls[i], !composing && allowed);
        if (allowed) {
            x -= control_w;
            lv_obj_set_pos(controls[i], x, headroom);
            lv_obj_set_size(controls[i], control_w, bar);
        }
    }
    enable(kb->voice, epd_keyboard_allows_voice(kb->keys));
    enable(kb->cursor_left, kb->target != NULL); enable(kb->cursor_right, kb->target != NULL);
    set_label(kb->language, kb->page == KEYS_NUMBERS || kb->config.mode == EPD_KEYBOARD_NUMBER ?
              epd_get_str(keyboard_numbers) : use_pinyin ? epd_get_str(keyboard_chinese) :
              kb->uppercase ? "ABC" : "English");
    lv_obj_set_style_text_font(kb->language, small, 0);
    lv_obj_set_pos(kb->language, epd_ui_px(24), headroom + (bar - lv_font_get_line_height(small)) / 2);
    lv_obj_set_size(kb->language, LV_MAX(1, x - epd_ui_px(28)), lv_font_get_line_height(small));
    visible(kb->language, !composing);
    kb->count = epd_pinyin_candidates(&kb->pinyin, kb->offset, kb->choices, CANDIDATES + 1);
    visible(kb->preedit, composing);
    if (composing) composition_render(kb, w, small);
    int32_t arrow = LV_MIN(epd_ui_touch(56), LV_MAX(1, w / 10));
    int32_t start = kb->offset ? arrow : inset;
    int32_t choices_w = LV_MAX(1, w - start - arrow);
    for (unsigned i = 0; i < CANDIDATES; ++i) {
        visible(kb->candidate[i], composing && i < kb->count);
        enable(kb->candidate[i], i < kb->count);
        lv_obj_t *label = lv_obj_get_child(kb->candidate[i], 0);
        set_label(label, i < kb->count ? kb->choices[i].text : "");
        lv_obj_set_style_text_font(label, i == 0 ? epd_app_font_style(EPD_FONT_BODY, EPD_FONT_STYLE_BOLD) : font, 0);
        int32_t left = start + choices_w * i / CANDIDATES;
        int32_t width = choices_w * (i + 1) / CANDIDATES - choices_w * i / CANDIDATES;
        lv_obj_set_pos(kb->candidate[i], left, headroom);
        lv_obj_set_size(kb->candidate[i], width, bar);
        lv_obj_set_width(label, LV_MAX(1, width - epd_ui_px(12)));
        lv_obj_center(label);
    }
    visible(kb->previous, composing && kb->offset != 0);
    visible(kb->next, composing);
    enable(kb->previous, kb->offset != 0); enable(kb->next, kb->count > CANDIDATES);
    lv_obj_set_pos(kb->previous, 0, headroom); lv_obj_set_size(kb->previous, arrow, bar);
    lv_obj_set_pos(kb->next, w - arrow, headroom); lv_obj_set_size(kb->next, arrow, bar);
    kb->layout_busy = false;
}

static void target_event(lv_event_t *event)
{
    keyboard_t *kb = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE) {
        kb->target = NULL;
        lv_keyboard_set_textarea(kb->keys, NULL);
        epd_pinyin_reset(&kb->pinyin);
        kb->offset = 0;
        render(kb);
    } else if ((code == LV_EVENT_CLICKED || code == LV_EVENT_FOCUSED) &&
               lv_obj_has_flag(&kb->obj, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(&kb->obj, LV_OBJ_FLAG_HIDDEN);
        render(kb);
    }
}

static void root_event(lv_event_t *event)
{
    keyboard_t *kb = (keyboard_t *)lv_event_get_current_target(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DRAW_MAIN) {
        lv_area_t area;
        lv_obj_get_coords(&kb->obj, &area);
        area.y1 += preedit_height(kb);
        lv_draw_rect_dsc_t background;
        lv_draw_rect_dsc_init(&background);
        background.bg_color = lv_color_hex(KEY_BACKGROUND);
        background.bg_opa = LV_OPA_COVER;
        lv_draw_rect(lv_event_get_layer(event), &background, &area);
    } else if (code == LV_EVENT_DELETE) {
        if (kb->target) lv_obj_remove_event_cb_with_user_data(kb->target, target_event, kb);
        kb->target = NULL;
    } else if (code == LV_EVENT_CANCEL) {
        epd_pinyin_reset(&kb->pinyin);
        kb->offset = 0;
        render(kb);
        lv_obj_add_flag(&kb->obj, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_SIZE_CHANGED) render(kb);
    else if (code == LV_EVENT_FOCUSED) {
        lv_obj_add_state(kb->keys, LV_STATE_FOCUSED);
        if (lv_buttonmatrix_get_selected_button(kb->keys) == LV_BUTTONMATRIX_BUTTON_NONE)
            lv_buttonmatrix_set_selected_button(kb->keys, 0);
    } else if (code == LV_EVENT_DEFOCUSED) lv_obj_remove_state(kb->keys, LV_STATE_FOCUSED);
    else if (code == LV_EVENT_KEY) {
        uint32_t key = lv_event_get_key(event);
        if (key == LV_KEY_ESC) {
            /* Keypad/encoder dispatch CANCEL after KEY; do not emit it twice. */
            lv_indev_t *indev = lv_indev_active();
            if (!indev || (lv_indev_get_type(indev) != LV_INDEV_TYPE_KEYPAD &&
                           lv_indev_get_type(indev) != LV_INDEV_TYPE_ENCODER))
                finish(kb, LV_EVENT_CANCEL);
            return;
        }
        if (key == LV_KEY_ENTER) lv_obj_send_event(kb->keys, LV_EVENT_VALUE_CHANGED, NULL);
        else lv_obj_send_event(kb->keys, LV_EVENT_KEY, lv_event_get_param(event));
    }
}

lv_obj_t *epd_keyboard_create(lv_obj_t *parent, lv_obj_t *textarea, const epd_keyboard_config_t *config)
{
    lv_obj_t *root = lv_obj_class_create_obj(&keyboard_class, parent);
    if (!root) return NULL;
    lv_obj_class_init_obj(root);
    keyboard_t *kb = (keyboard_t *)root;
    kb->identity = ++identity;
    epd_app_object_init(root);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(KEY_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    kb->keys = lv_keyboard_create(root);
    epd_app_object_init(kb->keys);
    lv_obj_set_style_pad_all(kb->keys, 0, 0);
    lv_obj_set_style_pad_row(kb->keys, 0, 0);
    lv_obj_set_style_pad_column(kb->keys, 0, 0);
    lv_obj_set_style_border_width(kb->keys, 0, 0);
    lv_obj_set_style_bg_opa(kb->keys, LV_OPA_TRANSP, 0);
    flat(kb->keys, LV_PART_ITEMS);
    lv_keyboard_set_popovers(kb->keys, false);
    lv_obj_remove_event_cb(kb->keys, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(kb->keys, keys_event, LV_EVENT_VALUE_CHANGED, kb);
    lv_obj_add_flag(kb->keys, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(kb->keys, key_icons_draw, LV_EVENT_DRAW_TASK_ADDED, kb);
    lv_obj_add_event_cb(kb->keys, key_icons_draw, LV_EVENT_DRAW_MAIN_END, kb);
    kb->preedit = lv_label_create(root);
    lv_label_set_long_mode(kb->preedit, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_bg_color(kb->preedit, lv_color_hex(KEY_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(kb->preedit, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(kb->preedit, epd_ui_px(6), 0);
    lv_obj_add_flag(kb->preedit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(kb->preedit, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(kb->preedit, preedit_event, LV_EVENT_CLICKED, kb);
    kb->language = lv_label_create(root);
    lv_label_set_long_mode(kb->language, LV_LABEL_LONG_DOT);
    kb->voice = icon_button(kb); kb->close = icon_button(kb);
    kb->cursor_left = icon_button(kb); kb->cursor_right = icon_button(kb);
    kb->previous = icon_button(kb); kb->next = icon_button(kb);
    for (unsigned i = 0; i < CANDIDATES; ++i) kb->candidate[i] = button(kb, "");
    lv_obj_add_event_cb(root, root_event, LV_EVENT_ALL, NULL);
    epd_keyboard_set_mode(root, config);
    epd_keyboard_set_textarea(root, textarea);
    lv_obj_set_size(root, lv_pct(100), epd_keyboard_preferred_height(root));
    return root;
}

void epd_keyboard_set_textarea(lv_obj_t *root, lv_obj_t *textarea)
{
    keyboard_t *kb = instance(root);
    if (!kb || kb->target == textarea) return;
    if (textarea && !lv_obj_check_type(textarea, &lv_textarea_class)) return;
    if (kb->target) lv_obj_remove_event_cb_with_user_data(kb->target, target_event, kb);
    kb->target = textarea;
    if (textarea && !lv_obj_add_event_cb(textarea, target_event, LV_EVENT_ALL, kb)) kb->target = NULL;
    lv_keyboard_set_textarea(kb->keys, kb->target);
    epd_pinyin_reset(&kb->pinyin); kb->offset = 0;
    render(kb);
}

lv_obj_t *epd_keyboard_get_textarea(lv_obj_t *root)
{
    keyboard_t *kb = instance(root);
    return kb ? kb->target : NULL;
}

void epd_keyboard_open_voice(lv_obj_t *root)
{
    keyboard_t *kb = instance(root);
    if (kb && epd_keyboard_allows_voice(kb->keys)) ui_voice_input_open_for(kb->target);
}

void epd_keyboard_set_mode(lv_obj_t *root, const epd_keyboard_config_t *config)
{
    keyboard_t *kb = instance(root);
    if (!kb) return;
    kb->config = config ? *config : (epd_keyboard_config_t){EPD_KEYBOARD_TEXT, true, true};
    epd_pinyin_reset(&kb->pinyin); kb->offset = 0;
    kb->page = kb->config.mode == EPD_KEYBOARD_NUMBER ? KEYS_NUMBERS : KEYS_LETTERS;
    kb->uppercase = false;
    render(kb);
    lv_obj_invalidate(root);
}

void epd_keyboard_refresh(lv_obj_t *root)
{
    keyboard_t *kb = instance(root);
    if (kb) render(kb);
}

int32_t epd_keyboard_preferred_height(lv_obj_t *root)
{
    keyboard_t *kb = instance(root);
    return epd_ui_px(kb && kb->config.mode == EPD_KEYBOARD_NUMBER ? 324 : 420) +
           (kb ? preedit_height(kb) : 0);
}

RTM_EXPORT(epd_keyboard_create);
RTM_EXPORT(epd_keyboard_set_textarea);
RTM_EXPORT(epd_keyboard_get_textarea);
RTM_EXPORT(epd_keyboard_open_voice);
RTM_EXPORT(epd_keyboard_set_mode);
RTM_EXPORT(epd_keyboard_refresh);
RTM_EXPORT(epd_keyboard_preferred_height);
