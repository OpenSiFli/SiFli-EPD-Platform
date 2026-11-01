/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOK_COMPUTE_H
#define BOOK_COMPUTE_H
#include "../formats/formats.h"
#include <sys/stat.h>
#include "book_entry.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { BOOK_COMPUTE_BUSY = -7 };
#define BOOK_COMPUTE_TEXT_MAX 2048u
#define BOOK_COMPUTE_SOURCE_TEXT_MAX 1024u

#define BOOK_COMPUTE_SELECT_CHAPTER 1u
#define BOOK_COMPUTE_CONTEXT 2u
#define BOOK_COMPUTE_FULL_SOURCE 4u
#define BOOK_COMPUTE_NOTE 8u
#define BOOK_COMPUTE_METADATA 16u
#define BOOK_COMPUTE_BODY_ONLY 32u
#define BOOK_COMPUTE_NAV_ONLY 64u
#define BOOK_COMPUTE_FIRST_BODY 128u
#define BOOK_COMPUTE_NOTE_BYTES 4096u
#define BOOK_COMPUTE_TXT_BACKSCAN 65536u
#define BOOK_COMPUTE_DEFAULT_SLICE 65536u
#define BOOK_COMPUTE_DEFAULT_CONTEXT 12288u
#define BOOK_COMPUTE_NO_SPINE UINT32_MAX
#define BOOK_COMPUTE_FORMAT_XHTML 6u
enum { BOOK_SOURCE_FILE_BYTE = 1, BOOK_SOURCE_BODY_UTF8 = 2 };
enum { BOOK_SOURCE_BEGIN = 0x100, BOOK_SOURCE_TARGET = 0x200,
       BOOK_SOURCE_END = 0x400, BOOK_SOURCE_CLAMPED = 0x800,
       BOOK_SOURCE_FRAGMENT = 0x1000, BOOK_SOURCE_TARGET_BEGIN = 0x2000 };

typedef struct book_compute_selection {
    uint32_t spine, anchor_byte, flags;
    uint32_t slice_bytes;   /* TXT forward source bytes; zero = 64KiB. */
    uint32_t context_bytes; /* Source/body bytes before target; zero = 12KiB. */
    uint32_t skip_utf8;     /* Body text bytes; NAV_ONLY uses accepted navigation events. */
    uint32_t format_hint;   /* 0 = extension detection, 6 = standalone markup. */
    const char *note_target; /* NOTE: normalized resource#fragment, borrowed. */
    const book_entry_t *entry; /* Optional bounded package result; borrowed. */
} book_compute_selection_t;

/* Strings are borrowed for the synchronous call. Chapters are zero-based in
 * reading order; id is the normalized EPUB href (split: zip!href). TOC entries
 * with no spine match use BOOK_COMPUTE_NO_SPINE. Location offsets count original
 * file bytes for TXT, normalized emitted UTF-8 text bytes within an EPUB chapter
 * (paragraph/image events do not add bytes). BEGIN/END delimit output; TARGET
 * reports the character-aligned requested location plus skip_utf8, including
 * at EOF. TXT skip counts only normalized TEXT bytes, excluding paragraphs.
 * Every selected TEXT callback has a preceding location at its true source
 * start, and contains at most BOOK_COMPUTE_SOURCE_TEXT_MAX UTF-8 bytes.
 * TXT context is bounded by a 64KiB backward scan, not by a layout page count.
 * A caller may supply the byte budget for three preceding layout pages. */
typedef struct book_compute_observer {
    void *user;
    int (*chapter)(void *user, uint32_t index, uint32_t total, book_format_t format,
                   bool selected, const char *id, const char *title);
    int (*toc)(void *user, uint32_t index, uint32_t total, const char *id,
               const char *title, unsigned level);
    int (*location)(void *user, uint32_t index, uint32_t actual, uint32_t requested,
                    unsigned flags, const char *id);
    void *entry_buffer; /* BOOK_ENTRY_MAX writable bytes, or NULL. */
    int (*entry_ready)(void *user, size_t bytes);
} book_compute_observer_t;

/* All operations are synchronous and may suspend the caller's cooperative
 * thread. A temporary lack of data must be handled inside the callback.
 * open/stat/close/dir_open/dir_close return BOOK_OK or a negative BOOK_* code.
 * Successful opens return a non-NULL opaque handle, owned until close.
 * Failed opens leave *handle NULL. close releases the handle even on error.
 * read_at returns bytes (short reads allowed), 0 only at EOF, or BOOK_* error.
 * dir_next returns 1 for a NUL-terminated name, 0 at end, or BOOK_* error;
 * names that do not fit capacity must fail, never silently truncate.
 * stat uses the target C library's struct stat; no binary IPC ABI is implied. */
typedef struct book_compute_resource_ops {
    int (*stat)(void *user, const char *path, struct stat *result);
    int (*open)(void *user, const char *path, void **handle, uint64_t *size);
    int (*read_at)(void *user, void *handle, uint64_t offset, void *buffer, size_t size);
    int (*close)(void *user, void *handle);
    int (*dir_open)(void *user, const char *path, void **handle);
    int (*dir_next)(void *user, void *handle, char *name, size_t capacity);
    int (*dir_close)(void *user, void *handle);
} book_compute_resource_ops_t;

/* Supports EPUB, split EPUB, Markdown, TXT, MOBI/AZW/AZW3 and standalone
 * XHTML/HTML/HTM/XHT. Calls the original structured/codec parsing algorithms.
 * TXT detects UTF-8/UTF-16 BOMs, BOM-less UTF-16 zero patterns and otherwise
 * samples UTF-8 validity before falling back to GBK. Invalid encoded units
 * become U+FFFD; CR/LF/CRLF become paragraph events. TXT anchors retain source
 * byte offsets as txt-byte-N. Text is UTF-8 in every sink callback.
 * Directory callbacks may all be NULL for regular files. Arguments, allocator
 * and all borrowed sink data must remain valid across callback suspension.
 * Cancellation is polled at resource and parser/sink boundaries; a suspended
 * callback must be awakened by its owner to observe cancellation and unwind.
 * Cleanup always closes resources, including after cancellation.
 * These public conversion entries reject reentry with BOOK_COMPUTE_BUSY.
 * The remote scheduler separately permits one bounded note child while its
 * parent is suspended; the parent resumes only after that child unwinds.
 * Entry calls must be serialized on one cooperative scheduler, or externally
 * locked across native threads. Other libxml2 users must use a separate copy.
 * All parser allocations use allocator and are released before return. */
int book_compute_convert(const char *path, const book_sink_t *sink,
                         const book_allocator_t *allocator,
                         const book_compute_resource_ops_t *resources, void *resource_user,
                         char *error, size_t error_capacity);

/* SELECT_CHAPTER converts only the requested EPUB spine item, or a bounded TXT
 * slice around anchor_byte. It never converts preceding EPUB chapter bodies.
 * EPUB reconstructs styles from the selected chapter's beginning, then emits
 * from the aligned body anchor (or bounded context) to that chapter's end.
 * UINT32_MAX with CONTEXT requests chapter-end context; only that chapter is
 * measured and reopened. MOBI does not support SELECT_CHAPTER.
 * Each call is independent: adjacent chapters/slices can be opened in any order.
 * METADATA reads only the EPUB package and declared cover. It emits metadata
 * before attempting the cover, and never reads navigation or chapter bodies.
 * It is exclusive with all other flags. BODY_ONLY emits the selected chapter
 * declaration and body; NAV_ONLY emits all other declarations and the TOC.
 * FIRST_BODY selects the reading entry and places TARGET before its first text.
 * A NULL selection retains whole-document conversion. */
int book_compute_convert_at(const char *path, const book_sink_t *sink,
                            const book_allocator_t *allocator,
                            const book_compute_resource_ops_t *resources, void *resource_user,
                            const book_compute_selection_t *selection,
                            const book_compute_observer_t *observer,
                            char *error, size_t error_capacity);

#ifdef __cplusplus
}
#endif
#endif
