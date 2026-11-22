#ifndef BOARD_LOAD_H
#define BOARD_LOAD_H
#include "board_store.h"
typedef struct board_load board_load_t;
/* UI owns one reference. Closing cancels without waiting; accepted operation
 * and TASK cleanup continue independently until the executor has finished. */
board_load_t *board_load_start(void);
bool board_load_take(board_load_t *load, board_model_t **model, board_message_t *message);
void board_load_close(board_load_t *load);
#endif
