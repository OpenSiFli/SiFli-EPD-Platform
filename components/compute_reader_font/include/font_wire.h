#ifndef EPD_READER_FONT_WIRE_H
#define EPD_READER_FONT_WIRE_H
#include "compute_reader_font.h"

#define EPD_FONT_COMPONENT "epd/reader-font"
#define EPD_FONT_VERSION 3u
#define EPD_FONT_PACKET_BYTES (16u * 1024u)
#define EPD_FONT_BATCH_MAX 64u
#define EPD_FONT_MEMORY_DEFAULT (704u * 1024u)
#define EPD_FONT_MEMORY_MAX EPD_FONT_MEMORY_DEFAULT
#define EPD_FONT_STACK_BYTES (16u * 1024u)

#define EPD_FONT_SOURCE_FTZ1 1u
/* Optional READ hints. OPEN message.values[7] advertises support; old peers
 * leave/ignore it and continue sending zero in READ values[7]. No payload or
 * structure changes. Hints affect cache replacement only, never read validity. */
#define EPD_FONT_READ_HINTS_MAGIC 0x4d455441u
#define EPD_FONT_READ_METADATA 1u
/* Optional batch timing: OPEN values[6] opts in. FINAL OUTPUT values[1]
 * acknowledges this magic; path[0..31] contains the fixed-width snapshot.
 * Existing payload layouts and protocol version remain unchanged. */
#define EPD_FONT_STEP_PROFILE_MAGIC 0x53545031u
typedef struct {
    uint32_t tick_hz, reads, idles, outputs, run_ticks;
    uint32_t read_wait_ticks, idle_wait_ticks, output_wait_ticks;
} epd_font_step_wire_t;
typedef char epd_font_step_layout[sizeof(epd_font_step_wire_t) == 32 ? 1 : -1];
/* OPEN payload. The HCPU owns handle; remote never opens or closes it.
 * Zero memory/cache/operation_limit selects defaults. For raw sources flags=0
 * and source_bytes=0 (or font.file_size). FTZ1 uses SOURCE_FTZ1 and the packed
 * source_bytes; font.file_size remains the first face's logical byte extent.
 * One session pins one immutable font file/face; every batch selects its style. */
typedef struct {
    uint32_t version, bytes;
    crf_font_identity font;
    uint32_t handle, memory_bytes, cache_bytes, operation_limit, flags;
    uint32_t source_bytes;
    /* Immutable built-in FTZ bytes in the HCPU resource partition; zero uses READ. */
    uint32_t source_address;
} epd_font_open_t;

enum { EPD_FONT_MEASURE = 1, EPD_FONT_RENDER = 2, EPD_FONT_END = 3 };
/* COMMAND payload = this header followed by count crf_codepoint records.
 * request_id must be nonzero. flags=0. END has count=0. */
typedef struct {
    uint32_t version, bytes, request_id, operation;
    crf_font_identity font;
    crf_style style;
    uint32_t count, flags;
} epd_font_command_t;

#define EPD_FONT_OUTPUT_FINAL 1u
#define EPD_FONT_OUTPUT_OPEN 2u
/* OUTPUT payload = header + completed crf_glyph records + bitmap_bytes A8.
 * crf_glyph.bitmap_offset is relative to the start of the A8 area (NOT packet).
 * first indexes the original command; count<=64 may produce multiple packets.
 * status is a crf_status value. FINAL ends this command, not the font session.
 * OPEN|FINAL (request_id=0) acknowledges initialization, with no glyph records.
 * A single glyph that cannot fit reports OUTPUT_FULL and required_bytes (A8
 * bytes for that glyph); no glyph fragmentation or HCPU raster fallback. */
typedef struct {
    uint32_t version, bytes, request_id, status, flags;
    uint32_t first, completed, total, bitmap_bytes, required_bytes;
    crf_font_identity font;
    crf_style style;
    crf_metrics metrics;
} epd_font_output_t;

/* All structs are little-endian fixed-width; no pointers, enums or size_t on wire.
 * Pipe message.version is EPD_COMPUTE_PIPE_VERSION, independently of this version.
 * READ request: handle from OPEN; offset absolute; length exact requested bytes;
 * offset addresses the provider's raw or compressed bytes; size=source_bytes
 * (font.file_size for raw defaults); values[0]=EPD_FONT_VERSION, [1..5]=source_id/signature/
 * generation/file_size/face_index, [6]=CRF_OK, [7]=0 or READ_METADATA after OPEN
 * advertises READ_HINTS_MAGIC in message.values[7]. FTZ1 never sends hints.
 * HCPU preserves these fields,
 * sets values[6] to its crf_status, fills data and length, and returns transport
 * status. A successful READ must echo version/identity/handle/offset/size and
 * exactly length bytes. Only the common pipe manages response_sequence.
 * COMMAND and OUTPUT also carry values[0]=EPD_FONT_VERSION; an OUTPUT response
 * must preserve the payload length and values[0]. END finishes with PIPE_DONE.
 * On cancellation, close the pipe; this releases the fiber and engine before
 * HCPU releases the handle. An unacknowledged OUTPUT is not committed on HCPU. */
typedef char epd_font_open_layout[sizeof(epd_font_open_t) == 56 ? 1 : -1];
typedef char epd_font_command_layout[sizeof(epd_font_command_t) == 52 ? 1 : -1];
typedef char epd_font_output_layout[sizeof(epd_font_output_t) == 88 ? 1 : -1];
typedef char epd_font_glyph_layout[sizeof(crf_glyph) == 44 ? 1 : -1];
typedef char epd_font_pair_layout[sizeof(crf_codepoint) == 8 ? 1 : -1];
#endif
