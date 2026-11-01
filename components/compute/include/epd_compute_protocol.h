/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EPD_COMPUTE_PROTOCOL_H
#define EPD_COMPUTE_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#define EPD_COMPUTE_PROTOCOL 2u
#define EPD_COMPUTE_CACHE_LINE 32u
#define EPD_COMPUTE_REGION_NONE UINT32_MAX

typedef enum
{
    EPD_COMPUTE_OK = 0,
    EPD_COMPUTE_MORE = 1,
    EPD_COMPUTE_CANCELLED = -1,
    EPD_COMPUTE_NO_MEMORY = -2,
    EPD_COMPUTE_UNSUPPORTED = -3,
    EPD_COMPUTE_BUSY = -4,
    EPD_COMPUTE_INVALID = -5,
    EPD_COMPUTE_COMPONENT_ERROR = -6,
    EPD_COMPUTE_UNAVAILABLE = -7
} epd_compute_status_t;

typedef enum
{
    EPD_COMPUTE_CONTINUOUS = 0,
    EPD_COMPUTE_FOREGROUND = 1,
    EPD_COMPUTE_BACKGROUND = 2,
    EPD_COMPUTE_PURPOSE_COUNT = 3
} epd_compute_purpose_t;

#define EPD_COMPUTE_PURPOSE(p) (1u << (p))
#define EPD_COMPUTE_VALIDATED 1u
#define EPD_COMPUTE_STEP_SHARED 2u
/* Explicit board opt-in while device validation is still pending. */
#define EPD_COMPUTE_ALLOW_UNVALIDATED 4u
#define EPD_COMPUTE_READ 1u
#define EPD_COMPUTE_WRITE 2u
#define EPD_COMPUTE_CANCEL 1u
#define EPD_COMPUTE_YIELD 2u

typedef enum
{
    EPD_COMPUTE_OPEN = 1,
    EPD_COMPUTE_STEP,
    EPD_COMPUTE_RESET,
    EPD_COMPUTE_CLOSE
} epd_compute_operation_t;

/* Shared data occupies independent whole cache lines. READ/WRITE are from
 * the remote component's point of view; capacity includes alignment padding. */
typedef struct
{
    uint32_t address;
    uint32_t capacity;
    uint32_t length;
    uint32_t access;
} epd_compute_region_t;

typedef struct
{
    uint32_t region;
    uint32_t offset;
    uint32_t length;
} epd_compute_ref_t;

#define EPD_COMPUTE_DIAG_VALID 1u
#define EPD_COMPUTE_DIAG_CLOSE 2u
#define EPD_COMPUTE_DIAG_CALL_SATURATED 4u
#define EPD_COMPUTE_DIAG_MEMORY_SATURATED 8u
#define EPD_COMPUTE_DIAG_PSRAM_PRESENT 16u
#define EPD_COMPUTE_DIAG_SYSTEM_PRESENT 32u
#define EPD_COMPUTE_PHASE_SHIFT 8u
#define EPD_COMPUTE_PHASE_MASK (255u << EPD_COMPUTE_PHASE_SHIFT)

typedef enum
{
    EPD_COMPUTE_PHASE_DISPATCH = 1,
    EPD_COMPUTE_PHASE_INPUT,
    EPD_COMPUTE_PHASE_EXECUTE,
    EPD_COMPUTE_PHASE_FIBER_WAIT,
    EPD_COMPUTE_PHASE_FIBER_RUN,
    EPD_COMPUTE_PHASE_FIBER_YIELD,
    EPD_COMPUTE_PHASE_OUTPUT,
    EPD_COMPUTE_PHASE_DONE
} epd_compute_phase_t;

/* Fixed-width summary with a live phase in flags. Normal batches update only
 * flags; the remaining 124 bytes are published on failure or close. */
typedef struct
{
    uint32_t flags;
    int32_t status;
    uint32_t session, tick_hz, errors, rejects;
    uint32_t calls[4], ticks[4], step_max;
    uint32_t payload_live, payload_peak, charged_live, charged_peak;
    uint32_t attempts, heap_calls, requested_bytes, failures, frees;
    uint32_t live_blocks, peak_blocks;
    uint32_t heap_size, heap_used, heap_peak;
    uint32_t system_size, system_used, system_peak;
} epd_compute_diag_wire_t;

#define EPD_COMPUTE_WIRE_PREFIX_SIZE 72u

/* This descriptor, the region table and signal word reside in uncached SRAM.
 * HCPU may change signals; ACPU publishes its current phase in diagnostics. */
typedef struct
{
    uint32_t protocol;
    uint32_t size;
    uint32_t sequence;
    uint32_t capability;
    uint32_t operation;
    uint32_t session;
    uint32_t regions;
    uint32_t region_count;
    epd_compute_ref_t params;
    epd_compute_ref_t result;
    uint32_t signals;
    uint32_t step_limit;
    int32_t status;
    uint32_t completed_sequence;
    epd_compute_diag_wire_t diagnostics;
} epd_compute_wire_t;

static inline void epd_compute_phase(const epd_compute_wire_t *request, epd_compute_phase_t phase)
{
    if (!request) return;
    volatile uint32_t *flags = (volatile uint32_t *)&request->diagnostics.flags;
    *flags = (*flags & ~EPD_COMPUTE_PHASE_MASK) | ((uint32_t)phase << EPD_COMPUTE_PHASE_SHIFT);
}

typedef struct
{
    const char *name;
    uint32_t version;
    uint32_t purposes;
    uint32_t flags;
    uint32_t step_limit;
} epd_compute_registration_t;

/* Component callbacks execute serially on ACPU. A failed open leaves *state
 * null; reset failure retains a closeable state. No HCPU callback is invoked. */
typedef struct
{
    int32_t (*open)(void **state, const epd_compute_wire_t *request);
    int32_t (*step)(void *state, const epd_compute_wire_t *request);
    int32_t (*reset)(void *state, const epd_compute_wire_t *request);
    void (*close)(void *state, const epd_compute_wire_t *request);
} epd_compute_ops_t;

typedef struct
{
    epd_compute_registration_t capability;
    const epd_compute_ops_t *ops;
} epd_compute_remote_registration_t;

extern const epd_compute_registration_t epd_compute_capabilities[];
extern const uint32_t epd_compute_capability_count;
extern const epd_compute_remote_registration_t epd_compute_remote_capabilities[];
extern const uint32_t epd_compute_remote_capability_count;

int epd_compute_validate_regions(const epd_compute_region_t *regions, uint32_t count);
void *epd_compute_resolve(const epd_compute_wire_t *request,
                          epd_compute_ref_t ref, uint32_t access);
uint32_t epd_compute_signals(const epd_compute_wire_t *request);
void epd_compute_remote_dispatch(epd_compute_wire_t *request);

typedef char epd_compute_region_layout[(sizeof(epd_compute_region_t) == 16) ? 1 : -1];
typedef char epd_compute_diag_layout[(sizeof(epd_compute_diag_wire_t) == 128) ? 1 : -1];
typedef char epd_compute_wire_prefix[(offsetof(epd_compute_wire_t, diagnostics) == EPD_COMPUTE_WIRE_PREFIX_SIZE) ? 1 : -1];
typedef char epd_compute_wire_layout[(sizeof(epd_compute_wire_t) == 200) ? 1 : -1];

#endif
