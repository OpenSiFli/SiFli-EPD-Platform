#ifndef EPD_SYSTEM_RESET_STORE_H
#define EPD_SYSTEM_RESET_STORE_H
#include "sdcard.h"
#include <stdbool.h>

typedef enum { RESET_ERASE_INTERNAL, RESET_WIRELESS, RESET_DEFAULTS, RESET_ERASE_CARD, RESET_TERMINAL } reset_step_t;
typedef enum { RESET_CARD_NONE, RESET_CARD_PENDING, RESET_CARD_CLEARED, RESET_CARD_CANCELLED } reset_card_state_t;
typedef struct
{
    uint32_t step, card_state;
    sdcard_identity_t card;
} reset_record_t;

/* RT_EOK with exists=false means no reset is pending; malformed or unreadable
 * records never authorize deletion. All paths are fixed by implementation. */
rt_err_t reset_record_read(reset_record_t *record, bool *exists);
rt_err_t reset_record_write(const reset_record_t *record);
rt_err_t reset_record_cleanup(void);
rt_err_t reset_erase_internal(void);
rt_err_t reset_erase_card(const sdcard_identity_t *identity, uint32_t session);
rt_err_t reset_default_directories(void);
#endif
