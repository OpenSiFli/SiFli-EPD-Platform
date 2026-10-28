#ifndef BOARD_STORE_H
#define BOARD_STORE_H
#include "board_model.h"
typedef enum
{
    BOARD_MESSAGE_NONE,
    BOARD_MESSAGE_CONFIG_UNAVAILABLE,
    BOARD_MESSAGE_LOAD_MEMORY,
    BOARD_MESSAGE_CONFIG_INVALID,
    BOARD_MESSAGE_CONFIG_BOUNDS,
    BOARD_MESSAGE_LAYOUT_BOUNDS,
    BOARD_MESSAGE_DISPLAY_MEMORY,
    BOARD_MESSAGE_TIMER_FAILED,
} board_message_t;
/* Missing user configuration falls back to the package; invalid user data is reported. */
board_result_t board_store_load(board_model_t **out, board_message_t *message);
#endif
