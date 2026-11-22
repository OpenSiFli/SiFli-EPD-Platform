#include "board_load.h"
#include "epd_memory.h"
#include "platform/epd_app.h"
#include <rthw.h>
#include <string.h>

struct board_load {
    unsigned references;
    bool cancelled, done;
    epd_app_task_group_t group;
    epd_memory_owner_t task;
    epd_operation_scope_t business, cleanup;
    epd_operation_t operation;
    lv_timer_t *settle_timer;
    board_model_t *model;
    board_message_t message;
};

static bool cancelled(void *context)
{
    board_load_t *load = context;
    rt_base_t level = rt_hw_interrupt_disable();
    bool stop = load->cancelled;
    rt_hw_interrupt_enable(level);
    epd_operation_snapshot_t state;
    if (!stop && load->operation && epd_operation_get(load->operation, &state))
        stop = state.cancel_requested;
    return stop;
}

static void release(board_load_t *load)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool last = --load->references == 0;
    rt_hw_interrupt_enable(level);
    if (last) { board_model_free(load->model); epd_app_free(load); }
}

static void prepare(epd_operation_t operation, void *context)
{
    board_load_t *load = context;
    board_model_t *model = NULL;
    board_message_t message = BOARD_MESSAGE_LOAD_MEMORY;
    if (!cancelled(load)) {
        board_store_load_cancelable(&model, &message, cancelled, load);
    }
    load->model = model;
    load->message = message;
    epd_operation_complete(operation, cancelled(load) ? EPD_OPERATION_CANCELLED : EPD_OPERATION_OK, 0, NULL);
}

static bool received(epd_operation_t operation, const epd_operation_snapshot_t *state,
                     void *result, void *context)
{
    (void)operation; (void)state; (void)result; (void)context;
    return true;
}

static void finish(board_load_t *load)
{
    if (load->operation) epd_operation_release(load->operation);
    if (load->business) {
        epd_operation_scope_close(load->business);
        epd_operation_scope_release(load->business);
    }
    if (load->cleanup) {
        epd_operation_scope_close(load->cleanup);
        epd_operation_scope_release(load->cleanup);
    }
    if (load->task != EPD_MEMORY_OWNER_INVALID) epd_app_task_end(load->task);
    if (load->group) epd_app_task_group_release(load->group);
}

static void retired(void *context)
{
    board_load_t *load = context;
    lv_timer_resume(load->settle_timer);

}

static void settled(lv_timer_t *timer)
{
    board_load_t *load = lv_timer_get_user_data(timer);
    epd_operation_snapshot_t state;
    if (!epd_operation_get(load->operation, &state) || state.state != EPD_OPERATION_FINISHED) return;
    if (state.status != EPD_OPERATION_OK) {
        board_model_free(load->model); load->model = NULL;
        load->message = BOARD_MESSAGE_LOAD_MEMORY;
    }
    finish(load);
    /* The timer belongs to the accepted TASK, not to the disappearing page. */
    lv_timer_delete(timer);
    load->done = true;
    release(load);
}

board_load_t *board_load_start(void)
{
    board_load_t *load = epd_app_alloc(sizeof(*load), EPD_APP_PSRAM);
    if (!load) return NULL;
    memset(load, 0, sizeof(*load));
    load->references = 2;
    load->task = EPD_MEMORY_OWNER_INVALID;
    load->message = BOARD_MESSAGE_LOAD_MEMORY;
    epd_operation_spec_t spec = {0};
    if (!epd_operation_runtime_executors(&spec.execute, &spec.deliver, &spec.cleanup) ||
        epd_app_task_group_create(&load->group) != EPD_MEMORY_OK ||
        epd_app_task_begin(load->group, &load->task) != EPD_MEMORY_OK) goto failed;
    epd_app_background_policy_t policy = {EPD_APP_BG_TASKS, load->group, EPD_APP_BG_RELEASE};
    if (epd_app_background_set(&policy, NULL) != EPD_MEMORY_OK ||
        !epd_operation_scope_open(load->task, load->task, &load->business) ||
        !epd_operation_scope_open(load->task, load->task, &load->cleanup)) goto failed;
    epd_memory_owner_t previous = epd_memory_owner_enter(load->task);
    if (previous == EPD_MEMORY_OWNER_INVALID) goto failed;
    load->settle_timer = lv_timer_create(settled, 0, load);
    if (load->settle_timer) lv_timer_pause(load->settle_timer);
    epd_memory_owner_leave(previous);
    if (!load->settle_timer) goto failed;
    spec.scope = load->business; spec.cleanup_scope = load->cleanup;
    spec.run = prepare; spec.receive = received; spec.dispose_user = retired; spec.user = load;
    /* Delivery has no page pointer; it can drain while the page is paused. */
    spec.cleanup = spec.deliver;
    spec.deliver = spec.execute;
    if (epd_operation_submit(&spec, &load->operation)) return load;
    lv_timer_delete(load->settle_timer);
failed:
    finish(load);
    epd_app_free(load);
    return NULL;
}

bool board_load_take(board_load_t *load, board_model_t **model, board_message_t *message)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool ready = load->done && !load->cancelled;
    if (ready) { *model = load->model; load->model = NULL; *message = load->message; }
    rt_hw_interrupt_enable(level);
    return ready;
}

void board_load_close(board_load_t *load)
{
    if (!load) return;
    rt_base_t level = rt_hw_interrupt_disable();
    load->cancelled = true;
    rt_hw_interrupt_enable(level);
    if (!load->done) epd_operation_cancel(load->operation);
    release(load);
}
