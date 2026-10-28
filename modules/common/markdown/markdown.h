#ifndef EPD_COMMON_MARKDOWN_H
#define EPD_COMMON_MARKDOWN_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MD_INDEX_NONE UINT32_MAX
#define MD_INPUT_MAX (64u * 1024u)
#define MD_MEMORY_MAX (2u * 1024u * 1024u)
#define MD_DEPTH_MAX 64u
#define MD_NODES_MAX 4096u
#define MD_RUNS_MAX 16384u
#define MD_TEXT_MAX (512u * 1024u)

typedef enum {
    MD_OK = 0, MD_INVALID_ARGUMENT, MD_INVALID_UTF8, MD_LIMIT,
    MD_OUT_OF_MEMORY, MD_PARSER_ERROR
} md_status_t;

typedef enum {
    MD_NODE_DOC, MD_NODE_QUOTE, MD_NODE_UL, MD_NODE_OL, MD_NODE_LI,
    MD_NODE_P, MD_NODE_HEADING, MD_NODE_CODE, MD_NODE_HR,
    MD_NODE_TABLE, MD_NODE_THEAD, MD_NODE_TBODY, MD_NODE_TR,
    MD_NODE_TH, MD_NODE_TD
} md_node_kind_t;

typedef enum {
    MD_STYLE_BOLD = 1u, MD_STYLE_ITALIC = 2u, MD_STYLE_STRIKE = 4u,
    MD_STYLE_CODE = 8u, MD_STYLE_LINK = 16u
} md_style_t;

typedef enum { MD_ALIGN_UNSET, MD_ALIGN_START, MD_ALIGN_MIDDLE, MD_ALIGN_END } md_align_t;
typedef enum { MD_RUN_TEXT, MD_RUN_SOFT_BREAK, MD_RUN_HARD_BREAK } md_run_kind_t;

typedef struct { uint32_t offset, length; } md_string_t;

typedef struct {
    md_string_t text;
    md_string_t link;             /* Decoded destination; never fetched or executed. */
    uint32_t style;              /* Bitwise md_style_t. */
    md_run_kind_t kind;
} md_run_t;

typedef struct {
    md_node_kind_t kind;
    uint32_t parent, first_child, next_sibling;
    uint32_t subtree_end;        /* Exclusive preorder end; includes this node. */
    uint32_t first_run, run_count;
    uint32_t heading_level;
    uint32_t list_start;         /* OL start; LI ordinal within OL, or 1-based UL item. */
    uint32_t table_columns;
    uint8_t list_marker, list_tight, fence_char;
    md_align_t cell_align;
    md_string_t code_info;       /* Entire decoded fenced-code info string. */
    uint64_t hash;               /* Content + metadata + ordered descendants. */
} md_node_t;

typedef struct {
    void *context;
    /* Return storage aligned for long double / pointers / uint64_t, or NULL. */
    void *(*allocate)(void *context, size_t bytes);
    void (*release)(void *context, void *memory);
} md_allocator_t;

typedef struct {
    size_t memory_bytes;         /* Peak live bytes, including parser scratch/headers. */
    uint32_t nodes, runs, text_bytes, depth;
} md_limits_t;

typedef struct {
    md_node_t *nodes;            /* Root DOC is index 0; nodes are preorder. */
    md_run_t *runs;              /* A node's runs are contiguous. */
    char *text;                  /* UTF-8 arena; strings are slices, not NUL-terminated. */
    uint32_t node_count, run_count, text_size;
    uint32_t input_consumed;     /* Excludes an incomplete final UTF-8 scalar (0..3 bytes). */
    size_t memory_bytes, peak_memory_bytes;
    void *private_state;
} md_doc_t;

/* Set all limits to the hard maxima above. Each may be lowered, never raised. */
void md_limits_default(md_limits_t *limits);

/* out must be zero-initialized or previously freed. Does not change source.
 * UTF-8 must be valid except for an incomplete final scalar, deferred for streams.
 * All dynamic memory (including MD4C) uses allocator; no default heap fallback.
 * On failure with an initially empty out, out is zero and all acquired memory
 * is released. Passing a nonempty out returns MD_INVALID_ARGUMENT unchanged.
 * Independent documents/parses may run concurrently with thread-safe allocators.
 * NULL limits selects defaults. Allocator context must outlive the document. */
md_status_t md_document_parse(const char *source, size_t length,
                              const md_allocator_t *allocator,
                              const md_limits_t *limits, md_doc_t *out);
void md_document_free(md_doc_t *doc);

/* Subtree equality: hash prefilter then exact metadata/text/link/structure check.
 * Independent of arena offsets, absolute node indices and following siblings.
 * Ancestor indentation/numbering is not part of a subtree: compare it separately
 * when reusing a flattened display block (see md_node_context_equal()). */
bool md_node_equal(const md_doc_t *a, uint32_t ai, const md_doc_t *b, uint32_t bi);

/* Ancestor layout metadata, including LI ordinal, excluding sibling content.
 * Combine with md_node_equal() for flattened P/HEADING/CODE/HR/TR reuse. */
bool md_node_context_equal(const md_doc_t *a, uint32_t ai,
                           const md_doc_t *b, uint32_t bi);

#ifdef __cplusplus
}
#endif
#endif
