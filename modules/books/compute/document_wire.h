/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOK_DOCUMENT_WIRE_H
#define BOOK_DOCUMENT_WIRE_H
#include <stdint.h>
#include "book_entry.h"

#define BOOK_DOCUMENT_COMPONENT "epd/reader-document"
#define BOOK_DOCUMENT_VERSION 3u
#define BOOK_DOCUMENT_WIRE_VERSION 3u
#define BOOK_DOCUMENT_MAX_BUDGET (1024u * 1024u)
#define BOOK_DOCUMENT_DEFAULT_BUDGET BOOK_DOCUMENT_MAX_BUDGET
#define BOOK_DOCUMENT_STACK_BYTES (48u * 1024u)
#define BOOK_DOCUMENT_NOTE_BUDGET (512u * 1024u)
#define BOOK_DOCUMENT_FIRST_FLUSH 2048u
#define BOOK_DOCUMENT_BATCH_BYTES 16384u
#define BOOK_DOCUMENT_TEXT_BYTES 1024u

/* DONE-only additive diagnostics in message.path; no additional pipe call.
 * values[2]=magic, values[3]=sizeof(book_document_timing_t). Copy with memcpy
 * rather than casting path. The error string in data/length is unchanged. */
#define BOOK_DOCUMENT_TIMING_MAGIC 0x314d5442u /* BTM1 */
enum book_document_stage {
    BOOK_DOCUMENT_STAGE_PARSE, BOOK_DOCUMENT_STAGE_INFLATE,
    BOOK_DOCUMENT_STAGE_IMAGE, BOOK_DOCUMENT_STAGE_SVG,
    BOOK_DOCUMENT_STAGE_WAIT, BOOK_DOCUMENT_STAGE_COUNT
};
typedef struct book_document_timing {
    uint32_t tick_hz, saturated;
    uint32_t ticks[BOOK_DOCUMENT_STAGE_COUNT], calls[BOOK_DOCUMENT_STAGE_COUNT];
} book_document_timing_t;
typedef char book_document_timing_layout[(sizeof(book_document_timing_t) == 48) ? 1 : -1];

/* OPEN: pipe.path is a NUL-terminated source path. values[] are:
 * [0] wire version; [1] private heap budget (0 = default, includes reserved
 * fiber stack/control allowance); [2] max image width; [3] max image height
 * (0 defaults to 684/1216); [4] target spine (zero based); [5] anchor byte;
 * [6] BOOK_DOCUMENT_SELECT_CHAPTER | BOOK_DOCUMENT_CONTEXT | BOOK_DOCUMENT_FULL_SOURCE;
 * FULL_SOURCE keeps TXT source anchors but continues to EOF instead of ending
 * at the forward slice boundary. The caller uses a zero source anchor.
 * [7] TXT forward slice source bytes (0=64KiB). Initial message.offset is
 * context bytes (0=12KiB when CONTEXT is set, maximum 64KiB). HCPU can supply
 * its byte estimate for three preceding pages; conversion does not do layout.
 * Initial message.size is skip_utf8 (0..UINT32_MAX): normalized TEXT bytes
 * after anchor byte, excluding PARAGRAPH/image events. TXT decodes forward
 * from its original encoded source anchor; EPUB adds it to body UTF-8 offset.
 * A partial UTF-8 code point rounds down. TARGET reports the resulting true
 * source offset. Persist {spine, source_offset, unit, skip_utf8}; unit comes
 * from SOURCE_ANCHOR.d & 0xff, and skip counts TEXT bytes after that anchor.
 * Initial message.flags = 0 for detection, or BOOK_DOCUMENT_FORMAT_XHTML (6)
 * for a standalone markup file. XHTML/HTML/HTM/XHT extensions also select it.
 * Standalone markup uses one chapter (index 0), BODY_UTF8 source coordinates,
 * and resources resolved relative to its filename using the same resource API.
 * EPUB anchor UINT32_MAX plus CONTEXT selects the chapter's trailing context;
 * only that chapter is measured and reopened before emitting its tail.
 * DONE: status is BOOK_*; values[0] wire version; values[1] peak budget bytes;
 * values[4] selected spine (UINT32_MAX for TXT); values[5] actual target;
 * values[6] source unit; values[7] next source offset (END).
 * length bytes in data are an optional UTF-8 error string, without NUL.
 * Transport return codes are EPD_COMPUTE_*; service status is BOOK_*.
 * STAT response: size = byte size; flags = BOOK_DOCUMENT_REGULAR/DIRECTORY.
 * OPEN response: handle != 0 and size = byte size. READ request: handle,
 * offset, length (capacity); response: length bytes, zero only at EOF.
 * DIR_NEXT response: status BOOK_OK, length=0 at end; otherwise length
 * includes a terminating NUL in data. CLOSE/DIR_CLOSE release the handle.
 * All provider handles belong to this pipe session. HCPU must also close all
 * remaining handles on cancel/close, because a cancelled fiber cannot RPC.
 */
#define BOOK_DOCUMENT_REGULAR 1u
#define BOOK_DOCUMENT_DIRECTORY 2u
#define BOOK_DOCUMENT_FULL_SOURCE 4u
#define BOOK_DOCUMENT_NOTE 8u
/* METADATA reads the EPUB package and declared cover only, without body/TOC. */
#define BOOK_DOCUMENT_METADATA_ONLY 16u
#define BOOK_DOCUMENT_BODY_ONLY 32u
#define BOOK_DOCUMENT_NAV_ONLY 64u
#define BOOK_DOCUMENT_FIRST_BODY 128u
/* Optional ordinary-EPUB BODY_ONLY entry reuse. OPEN data holds a book_entry_t;
 * absent/invalid entries fall back. OUTPUT.flags=ENTRY_OUTPUT carries a new
 * entry, separate from unchanged document records. Both peers opt in via OPEN. */
#define BOOK_DOCUMENT_ENTRY_CACHE 256u
#define BOOK_DOCUMENT_ENTRY_OUTPUT 0x31454e54u
/* BODY_ONLY and NAV_ONLY are exclusive EPUB modes with SELECT_CHAPTER.
 * FIRST_BODY requires BODY_ONLY and selects the package reading entry.
 * NAV_ONLY uses OPEN.size as the number of already accepted CHAPTER/TOC
 * events. It emits no body and does not repeat the selected declaration. */
/* NOTE: OPEN data is the NUL-terminated normalized resource#fragment.
 * One note may interrupt a retained body session at a fiber checkpoint. The
 * body is preserved, not stepped concurrently. NOTE budget is capped at 256KiB.
 * Output is bounded TEXT, then NOTE_END.a reports truncation; no cache/position. */
#define BOOK_DOCUMENT_FORMAT_XHTML 6u
#define BOOK_DOCUMENT_SELECT_CHAPTER 1u
#define BOOK_DOCUMENT_CONTEXT 2u
#define BOOK_DOCUMENT_META_BOOK 0u
#define BOOK_DOCUMENT_META_TOC 1u
#define BOOK_DOCUMENT_SOURCE_FILE_BYTE 1u
#define BOOK_DOCUMENT_SOURCE_BODY_UTF8 2u
#define BOOK_DOCUMENT_SOURCE_BEGIN 0x100u
#define BOOK_DOCUMENT_SOURCE_TARGET 0x200u
#define BOOK_DOCUMENT_SOURCE_END 0x400u
#define BOOK_DOCUMENT_SOURCE_CLAMPED 0x800u
/* Image-only BODY target: use the already emitted SOURCE_BEGIN cursor. */
#define BOOK_DOCUMENT_SOURCE_TARGET_BEGIN 0x2000u
#define BOOK_DOCUMENT_SOURCE_FRAGMENT 0x1000u

enum book_document_event_type {
    BOOK_DOCUMENT_TEXT = 1, BOOK_DOCUMENT_PARAGRAPH, BOOK_DOCUMENT_ANCHOR,
    BOOK_DOCUMENT_LINK, BOOK_DOCUMENT_METADATA, BOOK_DOCUMENT_IMAGE_BEGIN,
    BOOK_DOCUMENT_IMAGE_DATA, BOOK_DOCUMENT_IMAGE_END,
    BOOK_DOCUMENT_CHAPTER, BOOK_DOCUMENT_SOURCE_ANCHOR, BOOK_DOCUMENT_NOTE_END
};

/* OUTPUT consists of complete records, each aligned to 4 bytes. Integers are
 * little endian. bytes includes this 32-byte header, payload and zero padding.
 * Payload is length1 bytes followed by length2 bytes; strings have no NUL.
 * The HCPU consumes/copies a whole batch before acknowledging BOOK_OK.
 * TEXT: a=BOOK_STYLE_*, payload1=UTF-8 (at most 1024 bytes, complete code
 *       points), payload2 empty.
 * PARAGRAPH: no payload/scalars.
 * ANCHOR: a=level, b=title-present (0 defines location, 1 declares TOC),
 *         payload1=id, payload2=title (possibly empty).
 * LINK: a=begin (0/1), payload1=target.
 * METADATA: a=META_BOOK: payload1=title, payload2=author.
 *           a=META_TOC: b=spine index (UINT32_MAX if unresolved), c=level,
 *           d=chapter total, payload1=stable target id, payload2=TOC title.
 * CHAPTER: a=spine index, b=total, c=selected (0/1), d=book_format_t,
 *          payload1=stable chapter id, payload2=chapter title (href fallback).
 * SOURCE_ANCHOR: a=spine index (UINT32_MAX for TXT), b=actual source offset,
 *                c=requested anchor, d=source unit | BEGIN/TARGET/END/CLAMPED/
 *                FRAGMENT bits, payload1=stable chapter id (TXT: txt-byte-N).
 *                Every selected TEXT record is preceded by an unflagged
 *                anchor at that TEXT's true encoded source/body byte start.
 *                c remains the original OPEN anchor; TARGET.b includes skip.
 *                TXT FRAGMENT means no paragraph boundary in the bounded scan.
 * IMAGE_BEGIN: a=image sequence (starts at 1), b=width, c=height,
 *              d=stride=(width+1)/2, payload1=source id.
 * IMAGE_DATA: a=image sequence, b=first row, c=row count, d=stride,
 *             payload1=gray4 bytes, high nibble is left pixel, 0 black/15 white;
 *             unused low nibble on an odd-width row is white.
 * IMAGE_END: a=image sequence, no payload. Each image's rows are contiguous
 * and exhaustive before END. Failed/cancelled sessions may have incomplete
 * images/output; only successful DONE makes the document complete.
 */
typedef struct book_document_event {
    uint32_t type, bytes, a, b, c, d, length1, length2;
} book_document_event_t;

typedef char book_document_event_layout[(sizeof(book_document_event_t) == 32) ? 1 : -1];
#endif
