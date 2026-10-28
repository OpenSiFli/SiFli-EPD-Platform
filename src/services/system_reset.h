#ifndef EPD_SYSTEM_RESET_H
#define EPD_SYSTEM_RESET_H
#include <stdbool.h>
#include <stdint.h>
#include <rtthread.h>
#include "sdcard.h"
#include "system_reset_guard.h"

typedef enum { SYSTEM_RESET_FACTORY, SYSTEM_RESET_NETWORK, SYSTEM_RESET_CARD } system_reset_kind_t;
typedef enum
{
    SYSTEM_RESET_IDLE, SYSTEM_RESET_PREPARING, SYSTEM_RESET_RUNNING,
    SYSTEM_RESET_WAIT_CARD, SYSTEM_RESET_FAILED, SYSTEM_RESET_DONE
} system_reset_phase_t;

/* Captured before confirmation; start never substitutes another card. */
typedef struct
{
    system_reset_kind_t kind;
    bool include_card;
    uint32_t session;
    sdcard_identity_t card;
    uint64_t card_bytes;
} system_reset_target_t;
typedef struct
{
    system_reset_kind_t kind;
    system_reset_phase_t phase;
    rt_err_t error;
    bool factory_pending, internal_done, card_pending, card_cancelled;
    uint32_t revision;
} system_reset_status_t;

/* Boot, after storage mount and before application/installer discovery. */
void system_reset_init(void);
/* UI thread: capture target, accept a confirmed request, then advance cleanup. */
rt_err_t system_reset_prepare(system_reset_kind_t kind, system_reset_target_t *target);
rt_err_t system_reset_start(const system_reset_target_t *target);
void system_reset_process(void);
void system_reset_get_status(system_reset_status_t *status);
rt_err_t system_reset_retry(void);
/* Requires a separate explicit confirmation; persists the TF cancellation. */
rt_err_t system_reset_finish_without_card(void);
void system_reset_dismiss(void);
#endif
