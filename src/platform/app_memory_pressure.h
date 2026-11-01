#ifndef APP_MEMORY_PRESSURE_H
#define APP_MEMORY_PRESSURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_MEMORY_PRESSURE_TIMEOUT_MS 5000u

typedef enum {
    APP_MEMORY_PRESSURE_IDLE,
    APP_MEMORY_PRESSURE_PENDING,
    APP_MEMORY_PRESSURE_SATISFIED,
    APP_MEMORY_PRESSURE_EXHAUSTED,
    APP_MEMORY_PRESSURE_TIMED_OUT
} app_memory_pressure_result_t;

typedef struct {
    uint32_t generation;
    bool pending;
    app_memory_pressure_result_t result;
    size_t min_available;
    size_t available;
} app_memory_pressure_status_t;

/* Worker-safe, allocation-free publication. Returns zero if this demand is
 * already satisfied; otherwise returns the shared generation. Pending requests
 * merge by maximum demand without extending their deadline. Completed requests
 * are reused for 1 second to bound retries. Never wait here or in an allocator.
 * Above-capacity demands publish a nonzero, completed EXHAUSTED generation;
 * they do not merge into live work or alter background recovery. Query exposes
 * the latest requested generation; existing live work continues independently.
 * A subsequent feasible request can immediately join/start normal work.
 * The target refers to available application PSRAM, not a contiguous block. */
uint32_t app_memory_pressure_request(size_t min_available);
/* After an actual allocation failure, require safe reclamation or an exhausted
 * result even when total free bytes suffice. Uses the same bounded generation;
 * min_available remains the real demand, not a fragmentation estimate. */
uint32_t app_memory_pressure_allocation_failed(size_t min_available);
app_memory_pressure_status_t app_memory_pressure_query(void);
/* UI thread, after navigation and before deferred service cleanup. Pass the
 * current foreground ID; empty/NULL protects all apps. Permit background resume
 * only at an appropriate idle surface such as the desktop. One cooperative
 * service step per call; callbacks must themselves be bounded/nonblocking.
 * Normal app_service deferred processing must continue to retire stopped work. */
void app_memory_pressure_process(const char *foreground_id, bool allow_background_resume);
/* Automatic discovery only; explicitly opening an app remains permitted. */
bool app_memory_pressure_background_blocked(void);

#endif
