/* SPDX-License-Identifier: Apache-2.0 */
#include "ui_reader.h"
#include "ui_bookshelf_data.h"
#include "document/document_view.h"
#include "../common/reader/reader.h"
#include "../common/reader/reader_geometry.h"
#include "file_version.h"
#include "document/document_cache.h"
#include "platform/epd_reader_font.h"
#include "../../components/compute_reader_render/include/epd_reader_render_image.h"
#include <rthw.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include "platform/epd_app_font.h"
#include "epd_memory.h"
#include "epd_resource_runtime.h"
#include "epd_activity_work.h"
#include "ui_settings.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

/* Entry pictures are independent of live layout/font/compute ownership. */
#define ENTRY_SLOTS 4u
#define ENTRY_BYTES_MAX (1024u * 1024u)
#define ENTRY_PIXEL_BUDGET (1024u * 1024u)

typedef struct { unsigned references; size_t bytes; uint8_t data[]; } entry_pixels_t;
/* Includes every live pixel block: resident, queued, active and ready. Shared
 * save references consume the same block and are charged only once. */
static size_t entry_pixel_bytes;
static void *entry_allocate(size_t bytes)
{
    if (epd_resource_context_reserve(EPD_MEMORY_REGION_HCPU_PSRAM, epd_app_allocation_peak(bytes)) != EPD_RESOURCE_OK)
        return NULL;
    return epd_app_alloc(bytes, EPD_APP_PSRAM);
}
static entry_pixels_t *entry_pixels_new(size_t bytes)
{
    if (bytes > ENTRY_PIXEL_BUDGET - sizeof(entry_pixels_t)) return NULL;
    size_t charge = sizeof(entry_pixels_t) + bytes;
    rt_base_t level = rt_hw_interrupt_disable();
    bool room = entry_pixel_bytes <= ENTRY_PIXEL_BUDGET - charge;
    if (room) entry_pixel_bytes += charge;
    rt_hw_interrupt_enable(level);
    if (!room) return NULL;
    entry_pixels_t *p = entry_allocate(charge);
    if (p) { p->references = 1; p->bytes = charge; }
    else { level = rt_hw_interrupt_disable(); entry_pixel_bytes -= charge; rt_hw_interrupt_enable(level); }
    return p;
}
static void entry_pixels_ref(entry_pixels_t *p)
{
    rt_base_t level = rt_hw_interrupt_disable(); ++p->references; rt_hw_interrupt_enable(level);
}
static void entry_pixels_release(entry_pixels_t *p)
{
    if (!p) return;
    rt_base_t level = rt_hw_interrupt_disable(); bool last = --p->references == 0;
    size_t bytes = p->bytes;
    rt_hw_interrupt_enable(level);
    if (last) {
        epd_app_free(p);
        level = rt_hw_interrupt_disable(); entry_pixel_bytes -= bytes; rt_hw_interrupt_enable(level);
    }
}
typedef struct {
    bookshelf_item_t file;
    book_file_version_t version;
    document_anchor_t source;
    uint32_t font_id, font_revision, font_fingerprint;
    unsigned settings[UI_SETTING_ENCODING - UI_SETTING_FONT_SIZE + 1];
    unsigned appearance;
    reader_geometry_t area;
    ui_reader_view_t view;
    document_bitmap_t bitmap;
    entry_pixels_t *pixels;
    uint32_t age;
} reader_entry_t;
static reader_entry_t entries[ENTRY_SLOTS];
static uint32_t entry_age;
static reader_entry_t *preview;
static lv_obj_t *preview_object;
static unsigned opened_book = UINT_MAX;
static book_file_version_t opened_version;
static bool opening, first_frame;
static ui_reader_view_t loading;
static char loading_title[BOOKSHELF_NAME_MAX];
static lv_obj_t *captured_root;
static bool entry_dirty;
static epd_resource_cache_t entry_resource;
static void entry_resource_publish(void);
static int entry_resource_shrink(void *, uint32_t, size_t, size_t *);

static void entry_release(reader_entry_t *entry)
{
    if (entry->bitmap.memory) lv_image_cache_drop(&entry->bitmap.image);
    entry_pixels_release(entry->pixels);
    memset(entry, 0, sizeof(*entry));
}
static entry_pixels_t *entry_take_pixels(reader_entry_t *entry, size_t bytes)
{
    rt_base_t level = rt_hw_interrupt_disable();
    entry_pixels_t *pixels = entry->pixels;
    bool reusable = pixels && pixels->references == 1 &&
        pixels->bytes == sizeof(*pixels) + bytes;
    rt_hw_interrupt_enable(level);
    if (!reusable) { entry_release(entry); return NULL; }
    if (entry->bitmap.memory) lv_image_cache_drop(&entry->bitmap.image);
    memset(entry, 0, sizeof(*entry));
    return pixels;
}
static void entry_make_room(size_t bytes)
{
    for (unsigned i = 0; i < ENTRY_SLOTS; ++i) {
        rt_base_t level = rt_hw_interrupt_disable(); size_t used = entry_pixel_bytes; rt_hw_interrupt_enable(level);
        if (used <= ENTRY_PIXEL_BUDGET - bytes) break;
        if (&entries[i] != preview) entry_release(&entries[i]);
    }
}
static bool entry_source_valid(const bookshelf_item_t *file, const book_file_version_t *version)
{
    if (!book_file_version_valid(file->path, version)) return false;
    /* A split EPUB's child files can change without mutating the directory token. */
    return file->format != DOCUMENT_SPLIT_EPUB ||
        book_file_version(file->path).storage == version->storage;
}
static bool entry_matches(const reader_entry_t *entry, const ui_bookshelf_book_t *book)
{
    if (!entry->bitmap.memory || strcmp(entry->file.path, book->file.path) ||
        entry->file.size != book->file.size || entry->file.modified != book->file.modified ||
        entry->file.format != book->file.format ||
        book->position.document_version != DOCUMENT_CONVERSION_VERSION ||
        book->position.document_kind != book->file.format ||
        memcmp(&entry->source, &book->position.source, sizeof(entry->source)) ||
        !entry_source_valid(&book->file, &entry->version) ||
        entry->font_id != epd_app_reading_font_id() ||
        entry->font_revision != epd_app_reading_font_revision()) return false;
    uint32_t fingerprint;
    if (!epd_reader_font_fingerprint(entry->font_id, &fingerprint) || fingerprint != entry->font_fingerprint) return false;
    reader_geometry_t area = reader_geometry();
    if (area.width != entry->area.width || area.top != entry->area.top || area.bottom != entry->area.bottom)
        return false;
    for (unsigned i = UI_SETTING_FONT_SIZE; i <= UI_SETTING_ENCODING; ++i)
        if (entry->settings[i - UI_SETTING_FONT_SIZE] != ui_settings_index(i)) return false;
    if (entry->appearance != ui_settings_index(UI_SETTING_READER_APPEARANCE)) return false;
    return true;
}
static void entry_resource_publish(void)
{
    epd_resource_cache_open(&entry_resource, EPD_RESOURCE_UI, entry_resource_shrink, NULL);
    rt_base_t level = rt_hw_interrupt_disable();
    size_t total = entry_pixel_bytes, reclaimable = 0;
    for (unsigned i = 0; i < ENTRY_SLOTS; ++i)
        if (&entries[i] != preview && entries[i].pixels && entries[i].pixels->references == 1)
            reclaimable += entries[i].pixels->bytes;
    rt_hw_interrupt_enable(level);
    epd_resource_cache_update_region(&entry_resource, EPD_MEMORY_REGION_HCPU_PSRAM, total, reclaimable, 3, 0);
}
static int entry_resource_shrink(void *context, uint32_t backend, size_t requested, size_t *released)
{
    (void)context; (void)backend;
    lv_draw_wait_for_finish();
    *released = 0;
    for (unsigned i = 0; i < ENTRY_SLOTS && *released < requested; ++i) {
        rt_base_t level = rt_hw_interrupt_disable();
        size_t bytes = &entries[i] != preview && entries[i].pixels && entries[i].pixels->references == 1 ?
            entries[i].pixels->bytes : 0;
        rt_hw_interrupt_enable(level);
        if (bytes) { entry_release(&entries[i]); *released += bytes; }
    }
    entry_resource_publish(); return EPD_RESOURCE_OK;
}
#include "reader_entry_store.inc"

static void preview_deleted(lv_event_t *event)
{
    if (lv_event_get_target_obj(event) == preview_object) preview_object = NULL;
}
static void preview_detach(void)
{
    if (preview_object) lv_obj_delete(preview_object);
    preview_object = NULL;
    preview = NULL;
    entry_resource_publish();
}
static void captured_deleted(lv_event_t *event)
{
    if (lv_event_get_target_obj(event) == captured_root) { captured_root = NULL; entry_dirty = false; }
}
void ui_reader_cache_entry(lv_obj_t *body)
{
    entry_resource_publish();
    const ui_bookshelf_book_t *book = ui_bookshelf_book(opened_book);
    const ui_reader_view_t *view = document_view_get();
    if (!body || !book || !entry_source_valid(&book->file, &opened_version) ||
        !view->ready || view->error_code || document_view_waiting() || document_view_settings_changed() || !book->position.source.unit) return;
    lv_obj_t *root = lv_obj_get_child(body, 0);
    if (!root || root != captured_root || !entry_dirty) return;
    const lv_image_dsc_t *source = epd_reader_bound_image(body);
    if (!source || !source->data || source->header.cf != LV_COLOR_FORMAT_L8 ||
        !source->data_size || source->data_size > ENTRY_BYTES_MAX) return;
    reader_entry_t *entry = &entries[0];
    for (unsigned i = 0; i < ENTRY_SLOTS; ++i) {
        if (!strcmp(entries[i].file.path, book->file.path)) { entry = &entries[i]; break; }
        if (!entries[i].bitmap.memory || entries[i].age < entry->age) entry = &entries[i];
    }
    if (entry == preview) return;
    /* One optional capture attempt for this bound page, also on exit. */
    entry_dirty = false;
    entry_pixels_t *pixels = entry_take_pixels(entry, source->data_size);
    if (!pixels) {
        if (source->data_size <= ENTRY_PIXEL_BUDGET - sizeof(entry_pixels_t))
            entry_make_room(sizeof(entry_pixels_t) + source->data_size);
        pixels = entry_pixels_new(source->data_size);
    }
    if (!pixels) return;
    memcpy(pixels->data, source->data, source->data_size);
    entry->pixels = pixels;
    entry->bitmap.image = *source;
    entry->bitmap.image.data = pixels->data;
    entry->bitmap.memory = pixels->data;
    entry->file = book->file;
    entry->version = opened_version;
    entry->source = book->position.source;
    entry->font_id = epd_app_reading_font_id();
    entry->font_revision = epd_app_reading_font_revision();
    if (!epd_reader_font_fingerprint(entry->font_id, &entry->font_fingerprint)) { entry_release(entry); return; }
    entry->area = reader_geometry();
    for (unsigned i = UI_SETTING_FONT_SIZE; i <= UI_SETTING_ENCODING; ++i)
        entry->settings[i - UI_SETTING_FONT_SIZE] = ui_settings_index(i);
    entry->appearance = ui_settings_index(UI_SETTING_READER_APPEARANCE);
    entry->view = *view;
    entry->view.text = ""; entry->view.title = entry->file.name;
    entry->view.font = epd_app_font_role(EPD_FONT_BODY);
    entry->age = ++entry_age;
    entry_store_save(entry);
}

void ui_reader_cache_idle(lv_obj_t *body, bool idle)
{
    /* The last committed page is captured by reader_pause(), at the session boundary. */
    entry_store_allow_write(idle && !body && !opening);
}

bool ui_reader_open(unsigned book)
{
    ui_reader_close();
    const ui_bookshelf_book_t *item = ui_bookshelf_book(book);
    if (!item) return false;
    opened_book = book;
    opened_version = book_file_version(item->file.path);
    entry_store_allow_write(false);
    opening = true;
    first_frame = false;
    reader_geometry_t area = reader_geometry();
    snprintf(loading_title, sizeof(loading_title), "%s", item->file.name);
    loading = (ui_reader_view_t){0};
    loading.title = loading_title; loading.text = "";
    loading.font = epd_app_font_role(EPD_FONT_BODY);
    loading.margin = ui_reading_margin_px(ui_settings_index(UI_SETTING_MARGIN));
    loading.content_width = area.width - 2 * (int)loading.margin;
    loading.top = area.top; loading.bottom = area.bottom;
    loading.indexing = true; loading.revision = 1;
    for (unsigned i = 0; i < ENTRY_SLOTS; ++i) {
        if (entry_matches(&entries[i], item)) {
            preview = &entries[i]; preview->age = ++entry_age;
            loading = preview->view;
            loading.ready = false; loading.indexing = true;
            loading.title = loading_title;
            loading.font = epd_app_font_role(EPD_FONT_BODY);
            break;
        }
    }
    ui_reader_cache_preload(book, 1); /* Resident matches cancel redundant disk work. */
    return true;
}
const bookshelf_item_t *ui_reader_file(void)
{
    const ui_bookshelf_book_t *book = ui_bookshelf_book(opened_book);
    return book ? &book->file : NULL;
}
const char *ui_reader_identity(void)
{
    const ui_bookshelf_book_t *book = ui_bookshelf_book(opened_book);
    return book ? book->file.path : "";
}
void ui_reader_close(void)
{
    opening = first_frame = false;
    preview_detach();
    document_view_close(); opened_book = UINT_MAX; captured_root = NULL; entry_dirty = false;
}
void ui_reader_shutdown(void)
{
    ui_reader_close();
    entry_store_reset();
    for (unsigned i = 0; i < ENTRY_SLOTS; ++i) entry_release(&entries[i]);
    entry_resource_publish(); epd_resource_cache_close(&entry_resource);
}
void ui_reader_first_frame(void) { first_frame = true; }
bool ui_reader_open_pending(void) { return opening; }
bool ui_reader_preview_active(void) { return preview != NULL; }
void ui_reader_cache_diagnostic(void)
{
    const ui_bookshelf_book_t *book = ui_bookshelf_book(opened_book);
    unsigned resident = 0;
    bool match = false;
    for (unsigned i = 0; i < ENTRY_SLOTS; ++i) {
        if (entries[i].pixels) ++resident;
        if (book && entry_matches(&entries[i], book)) match = true;
    }
    rt_base_t level = rt_hw_interrupt_disable();
    unsigned bytes = (unsigned)entry_pixel_bytes, next = entry_store.next, count = entry_store.count;
    bool active = entry_store.active, pending = entry_store.pending != NULL, ready = entry_store.ready != NULL;
    bool allowed = entry_store.write_allowed;
    rt_hw_interrupt_enable(level);
    rt_kprintf("reader entry: preview=%u preview_bound=%u entrymatch=%u resident=%u pixel_bytes=%u "
               "dirty=%u write_allowed=%u active=%u pending=%u ready=%u preload=%u/%u pressure=%u\n",
        preview != NULL, preview && preview_object, match, resident, bytes,
        entry_dirty, allowed, active, pending, ready, next, count, 0);
}
bool ui_reader_bind(lv_obj_t *body)
{
    if (!opening && document_view_bind(body)) {
        preview_detach();
        lv_obj_t *root = lv_obj_get_child(body, 0);
        if (root && root != captured_root) {
            captured_root = root; entry_dirty = true;
            lv_obj_add_event_cb(root, captured_deleted, LV_EVENT_DELETE, NULL);
            entry_store_allow_write(false);
        }
        return true;
    }
    if (preview) {
        const ui_bookshelf_book_t *book = ui_bookshelf_book(opened_book);
        if (!book || !entry_matches(preview, book)) preview_detach();
    }
    if (!preview) return false;
    if (!preview_object) {
        if (!epd_reader_render_image_init()) return false;
        preview_object = lv_image_create(body);
        if (!preview_object) return false;
        lv_image_set_src(preview_object, &preview->bitmap.image);
        lv_obj_add_event_cb(preview_object, preview_deleted, LV_EVENT_DELETE, NULL);
        lv_obj_set_pos(body, preview->view.margin, preview->view.top);
        lv_obj_set_size(body, preview->bitmap.image.header.w, preview->bitmap.image.header.h);
    }
    return true;
}
bool ui_reader_save(void) { return !opening && document_view_save(); }
bool ui_reader_reflow(void) { entry_store_allow_write(false); return !opening && document_view_reflow(); }
bool ui_reader_settings_changed(void) { return !opening && document_view_settings_changed(); }
ui_reader_work_t ui_reader_step(uint32_t budget, bool foreground, bool commit)
{
    if (opening) {
        /* Once the first frame is submitted, prepare the document while the
         * panel refreshes. Publication still follows the display guard. */
        if (!first_frame || !foreground) return UI_READER_WORK_TARGET;
        opening = false;
        if (!document_view_open(opened_book)) return UI_READER_WORK_IDLE;
    }
    return document_view_step(budget, foreground, commit);
}
void ui_reader_background_pause(bool paused) { epd_reader_background_pause(paused); }
void ui_reader_cancel_pending(void) { if (!opening) document_view_cancel_pending(); }
void ui_reader_seek(unsigned page) { entry_store_allow_write(false); if (!opening) document_view_seek(page); }
void ui_reader_turn(int direction) { entry_store_allow_write(false); if (!opening) epd_reader_turn(direction); }
const ui_reader_view_t *ui_reader_view(void) { return opening ? &loading : document_view_get(); }
bool ui_reader_waiting(void) { return opening || document_view_waiting(); }
