#ifndef EPD_INPUT_H
#define EPD_INPUT_H

#include "lvgl.h"
#include <rtthread.h>

typedef struct
{
    uint32_t key;
    uint16_t repeat;
} epd_input_event_t;

/* Optional page semantics. Returning true consumes the operation. */
typedef struct
{
    bool (*ready)(uint32_t key);
    bool (*move)(const epd_input_event_t *events, unsigned count);
    bool (*key)(uint32_t key);
} epd_input_ops_t;

rt_err_t epd_input_init(void);
/* UI thread: read pointer input before application navigation and rendering. */
void epd_input_read(void);
void epd_input_process(void);
void epd_input_clear(void);
/* ISR-safe: discard the complete wake press, including delayed ADC edges.
 * Call once when PA34 is accepted as a wallpaper/preparation wake source. */
void epd_input_consume_wakeup(void);
/* UI thread: cap the event-loop wait for pointer reads, holds and wake release.
 * Pointer polling is suspended during system sleep. */
uint32_t epd_input_next_delay(uint32_t default_ms);
bool epd_input_focusable(lv_obj_t *object);
void epd_input_add_object(lv_obj_t *object);
void epd_input_set_ops(lv_group_t *group, const epd_input_ops_t *ops);
/* UI thread: use the topmost visible scope's ready/back semantics. */
void epd_input_back(void);
/* UI-thread identity used to cancel navigation when the input owner changes. */
unsigned epd_input_scope_id(void);

#endif
