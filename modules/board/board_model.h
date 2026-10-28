#ifndef BOARD_MODEL_H
#define BOARD_MODEL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* LVGL v9 uses the upper coordinate bits for special size values. */
#define BOARD_COORD_MAX ((1u << 29) - 1)

typedef struct
{
    void *(*alloc)(size_t size, void *context);
    void (*free)(void *pointer, void *context);
    void *context;
} board_allocator_t;
typedef enum { BOARD_OK, BOARD_JSON, BOARD_SCHEMA, BOARD_MEMORY, BOARD_BOUNDS, BOARD_SINK } board_result_t;
typedef struct board_card
{
    struct board_card *next;
    const char *title, *text, *summary, *empty;
    bool default_empty;
    uint32_t height, span;
} board_card_t;
typedef struct
{
    board_allocator_t allocator;
    void *document;
    const char *title;
    board_card_t *cards;
    size_t count;
    uint32_t columns, margin, gap, padding;
} board_model_t;
typedef enum { BOARD_BACK, BOARD_TITLE, BOARD_PANEL, BOARD_HEADING, BOARD_BODY } board_kind_t;
typedef struct
{
    board_kind_t kind;
    size_t card;
    int32_t x, y, width, height;
    unsigned font_size;
    const char *text, *summary, *empty;
    bool default_empty;
} board_command_t;
typedef bool (*board_emit_t)(const board_command_t *command, void *context);

/* Strings belong to the model. A failed parse leaves *out NULL. */
board_result_t board_model_parse(const char *json, size_t length,
                                  const board_allocator_t *allocator, board_model_t **out);
void board_model_free(board_model_t *model);
/* Validates the complete layout before emitting any commands. No allocations. */
board_result_t board_layout(const board_model_t *model, int32_t width, int32_t height,
                            board_emit_t emit, void *context);
const char *board_result_text(board_result_t result);
#endif
