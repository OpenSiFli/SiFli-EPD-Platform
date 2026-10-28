#include "markdown.h"
#include "markdown_internal.h"
#define entity_lookup md_entity_lookup
#include "vendor/md4c/entity.h"
#undef entity_lookup
#include <string.h>
#include <limits.h>

typedef union md_allocation md_allocation;
union md_allocation {
    long double align_float;
    void *align_pointer;
    uint64_t align_integer;
    struct { md_allocation *previous, *next; size_t size; bool scratch; } data;
};

typedef struct { uint32_t node, last_child, items; } block_frame;
typedef struct { uint32_t style; md_string_t link; } span_frame;
typedef struct {
    md_allocator_t allocator;
    md_limits_t limits;
    md_doc_t *doc;
    md_allocation *allocations;
    size_t live, peak;
    md_status_t status;
    uint32_t nodes_capacity, runs_capacity, text_capacity;
    uint32_t depth, span_depth, style;
    md_string_t link;
    block_frame blocks[MD_DEPTH_MAX];
    span_frame spans[MD_DEPTH_MAX];
} parse_state;

static int fail(parse_state *s, md_status_t code)
{
    if(s->status == MD_OK) s->status = code;
    return 1;
}

static void *allocate(parse_state *s, size_t size, bool scratch)
{
    md_allocation *block;
    size_t charge;
    if(s->status != MD_OK) return NULL;
    if(size > SIZE_MAX - sizeof(*block)) { fail(s, MD_LIMIT); return NULL; }
    charge = sizeof(*block) + size;
    if(charge > s->limits.memory_bytes - s->live) { fail(s, MD_LIMIT); return NULL; }
    block = s->allocator.allocate(s->allocator.context, charge);
    if(!block) { fail(s, MD_OUT_OF_MEMORY); return NULL; }
    block->data.size = size;
    block->data.scratch = scratch;
    block->data.previous = NULL;
    block->data.next = s->allocations;
    if(s->allocations) s->allocations->data.previous = block;
    s->allocations = block;
    s->live += charge;
    if(s->live > s->peak) s->peak = s->live;
    return block + 1;
}

static void release(parse_state *s, void *ptr)
{
    md_allocation *block;
    if(!ptr) return;
    block = (md_allocation *)ptr - 1;
    if(block->data.previous) block->data.previous->data.next = block->data.next;
    else s->allocations = block->data.next;
    if(block->data.next) block->data.next->data.previous = block->data.previous;
    s->live -= sizeof(*block) + block->data.size;
    s->allocator.release(s->allocator.context, block);
}

static void *resize(parse_state *s, void *ptr, size_t size, bool scratch)
{
    void *next;
    size_t old_size = ptr ? ((md_allocation *)ptr - 1)->data.size : 0;
    if(size == 0) { release(s, ptr); return NULL; }
    if(ptr && size <= old_size) return ptr;
    next = allocate(s, size, scratch);
    if(!next) return NULL;
    if(ptr) { memcpy(next, ptr, old_size); release(s, ptr); }
    return next;
}

void *md_engine_alloc(void *context, size_t size) { return allocate(context, size, true); }
void *md_engine_resize(void *context, void *ptr, size_t size) { return resize(context, ptr, size, true); }
void md_engine_free(void *context, void *ptr) { release(context, ptr); }
int md_engine_depth(void *context, unsigned depth)
{
    parse_state *s = context;
    return depth >= s->limits.depth ? fail(s, MD_LIMIT) : 0;
}

static int reserve(parse_state *s, void **ptr, uint32_t *capacity,
                    uint32_t need, uint32_t maximum, size_t width)
{
    uint32_t next;
    void *memory;
    if(need > maximum) return fail(s, MD_LIMIT);
    if(need <= *capacity) return 0;
    next = *capacity ? *capacity : 16;
    while(next < need) next *= 2;
    if(next > maximum) next = maximum;
    memory = resize(s, *ptr, (size_t)next * width, false);
    if(!memory) return 1;
    *ptr = memory; *capacity = next;
    return 0;
}

static int append_bytes(parse_state *s, const char *text, uint32_t length)
{
    md_doc_t *d = s->doc;
    void *memory = d->text;
    if(length > s->limits.text_bytes - d->text_size) return fail(s, MD_LIMIT);
    if(reserve(s, &memory, &s->text_capacity, d->text_size + length,
               s->limits.text_bytes, 1)) return 1;
    d->text = memory;
    if(length) memcpy(d->text + d->text_size, text, length);
    d->text_size += length;
    return 0;
}

static int append_codepoint(parse_state *s, uint32_t c)
{
    char buf[4]; uint32_t n;
    if(c == 0 || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) c = 0xfffd;
    if(c < 0x80) { buf[0] = (char)c; n = 1; }
    else if(c < 0x800) { buf[0] = (char)(0xc0 | (c >> 6)); buf[1] = (char)(0x80 | (c & 63)); n = 2; }
    else if(c < 0x10000) {
        buf[0] = (char)(0xe0 | (c >> 12)); buf[1] = (char)(0x80 | ((c >> 6) & 63));
        buf[2] = (char)(0x80 | (c & 63)); n = 3;
    } else {
        buf[0] = (char)(0xf0 | (c >> 18)); buf[1] = (char)(0x80 | ((c >> 12) & 63));
        buf[2] = (char)(0x80 | ((c >> 6) & 63)); buf[3] = (char)(0x80 | (c & 63)); n = 4;
    }
    return append_bytes(s, buf, n);
}

static int append_entity(parse_state *s, const char *text, uint32_t length)
{
    const ENTITY *entity;
    if(length >= 4 && text[0] == '&' && text[1] == '#') {
        uint32_t value = 0, base = 10, i = 2;
        if(text[i] == 'x' || text[i] == 'X') { base = 16; ++i; }
        for(; i + 1 < length; ++i) {
            uint32_t c = (unsigned char)text[i], digit;
            if(c >= '0' && c <= '9') digit = c - '0';
            else if(c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if(c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return append_bytes(s, text, length);
            if(digit >= base) return append_bytes(s, text, length);
            value = value > 0x10ffff ? 0x110000 : value * base + digit;
        }
        return append_codepoint(s, value);
    }
    entity = md_entity_lookup(text, length);
    if(!entity) return append_bytes(s, text, length);
    if(append_codepoint(s, entity->codepoints[0])) return 1;
    return entity->codepoints[1] ? append_codepoint(s, entity->codepoints[1]) : 0;
}

static int append_decoded(parse_state *s, MD_TEXTTYPE type, const char *text, uint32_t length)
{
    if(type == MD_TEXT_ENTITY) return append_entity(s, text, length);
    if(type == MD_TEXT_NULLCHAR) return append_codepoint(s, 0xfffd);
    return append_bytes(s, text, length);
}

static int copy_attribute(parse_state *s, const MD_ATTRIBUTE *a, md_string_t *result)
{
    uint32_t i = 0, offset = 0;
    result->offset = s->doc->text_size; result->length = 0;
    while(offset < a->size) {
        uint32_t next = a->substr_offsets[i + 1];
        if(next < offset || next > a->size) return fail(s, MD_PARSER_ERROR);
        if(next > offset && append_decoded(s, a->substr_types[i], a->text + offset, next - offset)) return 1;
        offset = next; ++i;
    }
    result->length = s->doc->text_size - result->offset;
    return 0;
}

static int block_enter(MD_BLOCKTYPE type, void *detail, void *context)
{
    static const md_node_kind_t kinds[] = {
        MD_NODE_DOC, MD_NODE_QUOTE, MD_NODE_UL, MD_NODE_OL, MD_NODE_LI,
        MD_NODE_HR, MD_NODE_HEADING, MD_NODE_CODE, MD_NODE_P, MD_NODE_P,
        MD_NODE_TABLE, MD_NODE_THEAD, MD_NODE_TBODY, MD_NODE_TR, MD_NODE_TH, MD_NODE_TD
    };
    parse_state *s = context;
    md_doc_t *d = s->doc;
    md_node_t *n;
    uint32_t index = d->node_count;
    void *memory = d->nodes;
    if((unsigned)type >= sizeof(kinds) / sizeof(kinds[0])) return fail(s, MD_PARSER_ERROR);
    if(s->depth >= s->limits.depth) return fail(s, MD_LIMIT);
    if(reserve(s, &memory, &s->nodes_capacity, index + 1, s->limits.nodes, sizeof(*d->nodes))) return 1;
    d->nodes = memory; ++d->node_count;
    n = &d->nodes[index]; memset(n, 0, sizeof(*n));
    n->kind = kinds[type];
    n->parent = n->first_child = n->next_sibling = MD_INDEX_NONE;
    n->first_run = d->run_count;
    if(s->depth) {
        block_frame *parent = &s->blocks[s->depth - 1];
        n->parent = parent->node;
        if(parent->last_child == MD_INDEX_NONE) d->nodes[parent->node].first_child = index;
        else d->nodes[parent->last_child].next_sibling = index;
        parent->last_child = index;
        if(type == MD_BLOCK_LI) {
            md_node_t *list = &d->nodes[parent->node];
            n->list_start = (list->kind == MD_NODE_OL ? list->list_start : 1) + parent->items++;
            n->list_marker = list->list_marker; n->list_tight = list->list_tight;
        }
    }
    s->blocks[s->depth].node = index;
    s->blocks[s->depth].last_child = MD_INDEX_NONE;
    s->blocks[s->depth].items = 0; ++s->depth;
    switch(type) {
    case MD_BLOCK_H: n->heading_level = ((MD_BLOCK_H_DETAIL *)detail)->level; break;
    case MD_BLOCK_UL: {
        MD_BLOCK_UL_DETAIL *v = detail;
        n->list_marker = (uint8_t)v->mark; n->list_tight = v->is_tight != 0; break;
    }
    case MD_BLOCK_OL: {
        MD_BLOCK_OL_DETAIL *v = detail;
        n->list_start = v->start; n->list_marker = (uint8_t)v->mark_delimiter;
        n->list_tight = v->is_tight != 0; break;
    }
    case MD_BLOCK_CODE: {
        MD_BLOCK_CODE_DETAIL *v = detail;
        n->fence_char = (uint8_t)v->fence_char;
        if(copy_attribute(s, &v->info, &n->code_info)) return 1;
        break;
    }
    case MD_BLOCK_TABLE: n->table_columns = ((MD_BLOCK_TABLE_DETAIL *)detail)->col_count; break;
    case MD_BLOCK_TH: case MD_BLOCK_TD: n->cell_align = (md_align_t)((MD_BLOCK_TD_DETAIL *)detail)->align; break;
    default: break;
    }
    return 0;
}

static int block_leave(MD_BLOCKTYPE type, void *detail, void *context)
{
    parse_state *s = context;
    (void)type; (void)detail;
    if(!s->depth || s->span_depth) return fail(s, MD_PARSER_ERROR);
    s->doc->nodes[s->blocks[--s->depth].node].subtree_end = s->doc->node_count;
    return 0;
}

static int span_enter(MD_SPANTYPE type, void *detail, void *context)
{
    parse_state *s = context;
    if(s->span_depth >= s->limits.depth) return fail(s, MD_LIMIT);
    s->spans[s->span_depth].style = s->style;
    s->spans[s->span_depth++].link = s->link;
    switch(type) {
    case MD_SPAN_EM: s->style |= MD_STYLE_ITALIC; break;
    case MD_SPAN_STRONG: s->style |= MD_STYLE_BOLD; break;
    case MD_SPAN_DEL: s->style |= MD_STYLE_STRIKE; break;
    case MD_SPAN_CODE: s->style |= MD_STYLE_CODE; break;
    case MD_SPAN_A:
        s->style |= MD_STYLE_LINK;
        return copy_attribute(s, &((MD_SPAN_A_DETAIL *)detail)->href, &s->link);
    case MD_SPAN_IMG: break; /* ALT text only; no resource is requested. */
    default: return fail(s, MD_PARSER_ERROR);
    }
    return 0;
}

static int span_leave(MD_SPANTYPE type, void *detail, void *context)
{
    parse_state *s = context;
    (void)type; (void)detail;
    if(!s->span_depth) return fail(s, MD_PARSER_ERROR);
    s->style = s->spans[--s->span_depth].style;
    s->link = s->spans[s->span_depth].link;
    return 0;
}

static int text_callback(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE length, void *context)
{
    parse_state *s = context;
    md_doc_t *d = s->doc;
    md_node_t *node;
    md_run_t run, *last;
    void *memory = d->runs;
    if(!s->depth) return fail(s, MD_PARSER_ERROR);
    node = &d->nodes[s->blocks[s->depth - 1].node];
    memset(&run, 0, sizeof(run));
    run.text.offset = d->text_size; run.link = s->link; run.style = s->style;
    if(type == MD_TEXT_CODE) run.style |= MD_STYLE_CODE;
    if(type == MD_TEXT_SOFTBR) {
        run.kind = MD_RUN_SOFT_BREAK;
        if(append_bytes(s, " ", 1)) return 1;
    } else if(type == MD_TEXT_BR) {
        run.kind = MD_RUN_HARD_BREAK;
        if(append_bytes(s, "\n", 1)) return 1;
    } else if(append_decoded(s, type, text, length)) return 1;
    run.text.length = d->text_size - run.text.offset;
    if(!run.text.length) return 0;
    if(node->run_count) {
        last = &d->runs[node->first_run + node->run_count - 1];
        if(run.kind == MD_RUN_TEXT && last->kind == run.kind && last->style == run.style &&
           last->text.offset + last->text.length == run.text.offset &&
           last->link.offset == run.link.offset && last->link.length == run.link.length) {
            last->text.length += run.text.length;
            return 0;
        }
    }
    if(reserve(s, &memory, &s->runs_capacity, d->run_count + 1, s->limits.runs, sizeof(run))) return 1;
    d->runs = memory; d->runs[d->run_count++] = run; ++node->run_count;
    return 0;
}

static uint64_t hash_bytes(uint64_t h, const void *bytes, size_t length)
{
    const unsigned char *p = bytes;
    size_t i;
    for(i = 0; i < length; ++i) { h ^= p[i]; h *= UINT64_C(1099511628211); }
    return h;
}

static uint64_t hash_number(uint64_t h, uint64_t value)
{
    unsigned i;
    for(i = 0; i < 8; ++i) { unsigned char b = (unsigned char)value; h = hash_bytes(h, &b, 1); value >>= 8; }
    return h;
}

static uint64_t hash_string(uint64_t h, const md_doc_t *d, md_string_t str)
{
    h = hash_number(h, str.length);
    return str.length ? hash_bytes(h, d->text + str.offset, str.length) : h;
}

static void compute_hashes(md_doc_t *d)
{
    uint32_t i, j;
    for(i = d->node_count; i-- > 0;) {
        md_node_t *n = &d->nodes[i];
        uint64_t h = UINT64_C(14695981039346656037);
        h = hash_number(h, n->kind); h = hash_number(h, n->heading_level);
        h = hash_number(h, n->list_start); h = hash_number(h, n->table_columns);
        h = hash_number(h, n->list_marker); h = hash_number(h, n->list_tight);
        h = hash_number(h, n->fence_char); h = hash_number(h, n->cell_align);
        h = hash_string(h, d, n->code_info); h = hash_number(h, n->run_count);
        for(j = 0; j < n->run_count; ++j) {
            md_run_t *r = &d->runs[n->first_run + j];
            h = hash_number(h, r->kind); h = hash_number(h, r->style);
            h = hash_string(h, d, r->text); h = hash_string(h, d, r->link);
        }
        for(j = n->first_child; j != MD_INDEX_NONE; j = d->nodes[j].next_sibling)
            h = hash_number(h, d->nodes[j].hash);
        n->hash = h;
    }
}

static bool string_equal(const md_doc_t *a, md_string_t x, const md_doc_t *b, md_string_t y)
{
    return x.length == y.length && (!x.length || memcmp(a->text + x.offset, b->text + y.offset, x.length) == 0);
}

static bool metadata_equal(const md_doc_t *a, const md_node_t *x, const md_doc_t *b, const md_node_t *y)
{
    return x->kind == y->kind && x->heading_level == y->heading_level &&
        x->list_start == y->list_start && x->list_marker == y->list_marker &&
        x->list_tight == y->list_tight && x->table_columns == y->table_columns &&
        x->fence_char == y->fence_char && x->cell_align == y->cell_align &&
        string_equal(a, x->code_info, b, y->code_info);
}

bool md_node_equal(const md_doc_t *a, uint32_t ai, const md_doc_t *b, uint32_t bi)
{
    uint32_t count, i, j;
    if(!a || !b || ai >= a->node_count || bi >= b->node_count) return false;
    if(a->nodes[ai].hash != b->nodes[bi].hash) return false;
    count = a->nodes[ai].subtree_end - ai;
    if(count != b->nodes[bi].subtree_end - bi) return false;
    for(i = 0; i < count; ++i) {
        const md_node_t *x = &a->nodes[ai + i], *y = &b->nodes[bi + i];
        if(!metadata_equal(a, x, b, y) || x->run_count != y->run_count ||
           x->subtree_end - ai != y->subtree_end - bi ||
           (i && x->parent - ai != y->parent - bi)) return false;
        for(j = 0; j < x->run_count; ++j) {
            const md_run_t *rx = &a->runs[x->first_run + j], *ry = &b->runs[y->first_run + j];
            if(rx->kind != ry->kind || rx->style != ry->style ||
               !string_equal(a, rx->text, b, ry->text) || !string_equal(a, rx->link, b, ry->link)) return false;
        }
    }
    return true;
}

bool md_node_context_equal(const md_doc_t *a, uint32_t ai, const md_doc_t *b, uint32_t bi)
{
    if(!a || !b || ai >= a->node_count || bi >= b->node_count) return false;
    ai = a->nodes[ai].parent; bi = b->nodes[bi].parent;
    while(ai != MD_INDEX_NONE && bi != MD_INDEX_NONE) {
        if(!metadata_equal(a, &a->nodes[ai], b, &b->nodes[bi])) return false;
        ai = a->nodes[ai].parent; bi = b->nodes[bi].parent;
    }
    return ai == bi;
}

void md_limits_default(md_limits_t *l)
{
    if(!l) return;
    l->memory_bytes = MD_MEMORY_MAX; l->nodes = MD_NODES_MAX;
    l->runs = MD_RUNS_MAX; l->text_bytes = MD_TEXT_MAX; l->depth = MD_DEPTH_MAX;
}

static md_status_t validate_utf8(const char *source, size_t length, uint32_t *consumed)
{
    size_t i = 0;
    while(i < length) {
        unsigned char c = (unsigned char)source[i];
        size_t width, j;
        if(c < 0x80) { ++i; continue; }
        if(c >= 0xc2 && c <= 0xdf) width = 2;
        else if(c >= 0xe0 && c <= 0xef) width = 3;
        else if(c >= 0xf0 && c <= 0xf4) width = 4;
        else return MD_INVALID_UTF8;
        for(j = 1; j < width && i + j < length; ++j) {
            unsigned char t = (unsigned char)source[i + j];
            if(t < 0x80 || t > 0xbf) return MD_INVALID_UTF8;
            if(j == 1 && ((c == 0xe0 && t < 0xa0) || (c == 0xed && t > 0x9f) ||
                          (c == 0xf0 && t < 0x90) || (c == 0xf4 && t > 0x8f))) return MD_INVALID_UTF8;
        }
        if(length - i < width) break;
        i += width;
    }
    *consumed = (uint32_t)i;
    return MD_OK;
}

static void clear_scratch(parse_state *s)
{
    md_allocation *block = s->allocations;
    while(block) {
        md_allocation *next = block->data.next;
        if(block->data.scratch) release(s, block + 1);
        block = next;
    }
}

void md_document_free(md_doc_t *doc)
{
    parse_state *s;
    md_allocator_t allocator;
    if(!doc) return;
    s = doc->private_state;
    if(s) {
        allocator = s->allocator;
        while(s->allocations) release(s, s->allocations + 1);
        allocator.release(allocator.context, s);
    }
    memset(doc, 0, sizeof(*doc));
}

md_status_t md_document_parse(const char *source, size_t length,
                              const md_allocator_t *allocator,
                              const md_limits_t *limits, md_doc_t *out)
{
    md_limits_t selected;
    parse_state *s;
    uint32_t consumed;
    md_status_t status;
    MD_PARSER parser;
    int result;
    if(!out || out->private_state || out->nodes || out->runs || out->text) return MD_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if(!allocator || !allocator->allocate || !allocator->release || (!source && length)) return MD_INVALID_ARGUMENT;
    if(length > MD_INPUT_MAX) return MD_LIMIT;
    md_limits_default(&selected);
    if(limits) selected = *limits;
    if(!selected.memory_bytes || selected.memory_bytes > MD_MEMORY_MAX ||
       !selected.nodes || selected.nodes > MD_NODES_MAX || !selected.runs || selected.runs > MD_RUNS_MAX ||
       !selected.text_bytes || selected.text_bytes > MD_TEXT_MAX ||
       !selected.depth || selected.depth > MD_DEPTH_MAX) return MD_INVALID_ARGUMENT;
    status = validate_utf8(source, length, &consumed);
    if(status != MD_OK) return status;
    if(selected.memory_bytes < sizeof(*s)) return MD_LIMIT;
    s = allocator->allocate(allocator->context, sizeof(*s));
    if(!s) return MD_OUT_OF_MEMORY;
    memset(s, 0, sizeof(*s));
    s->allocator = *allocator; s->limits = selected; s->doc = out;
    s->live = s->peak = sizeof(*s);
    out->private_state = s; out->input_consumed = consumed;
    memset(&parser, 0, sizeof(parser));
    parser.flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_NOHTML;
    parser.enter_block = block_enter; parser.leave_block = block_leave;
    parser.enter_span = span_enter; parser.leave_span = span_leave; parser.text = text_callback;
    result = md_engine_parse(source ? source : "", consumed, &parser, s);
    clear_scratch(s);
    status = s->status;
    if(status == MD_OK && (result != 0 || s->depth || s->span_depth || !out->node_count)) status = MD_PARSER_ERROR;
    if(status != MD_OK) { md_document_free(out); return status; }
    compute_hashes(out);
    out->memory_bytes = s->live; out->peak_memory_bytes = s->peak;
    return MD_OK;
}
