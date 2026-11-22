/* Scheduling boundary for font/LVGL/provider integration. Real SDK/core
 * admission is additionally tested by unified_memory/font_run.py. */
static unsigned font_pressure_calls;
static bool font_admission_denied;
size_t epd_app_allocation_peak(size_t bytes)
{ return bytes > SIZE_MAX - 128 ? SIZE_MAX : bytes + 128; }
bool epd_memory_app_backend(epd_memory_region_t region, epd_memory_backend_t *out)
{ if (region != EPD_MEMORY_REGION_HCPU_PSRAM) return false; *out = 1; return true; }
static epd_memory_recovery_result_t font_memory_step(epd_memory_recovery_t *state)
{
    if (font_admission_denied) return state->state = EPD_MEMORY_RECOVERY_DENIED;
    if (epd_app_psram_available() >= state->physical)
        return state->state = EPD_MEMORY_RECOVERY_READY;
    if (!state->pressure_requested) { state->pressure_requested = true; ++font_pressure_calls; }
    return state->state = EPD_MEMORY_RECOVERY_PENDING;
}
epd_memory_recovery_result_t epd_memory_recovery_begin(epd_memory_recovery_t *s,
    epd_memory_backend_t backend, size_t bytes, bool failed)
{
    assert(!s->used); s->used = true; s->physical = bytes; s->backend = backend;
    s->started = rt_tick_get(); (void)failed; return font_memory_step(s);
}
epd_memory_recovery_result_t epd_memory_recovery_poll(epd_memory_recovery_t *s, bool cancelled)
{
    if (cancelled || s->state == EPD_MEMORY_RECOVERY_DENIED ||
        (rt_tick_t)(rt_tick_get() - s->started) >= 5u * RT_TICK_PER_SECOND)
        return s->state = EPD_MEMORY_RECOVERY_DENIED;
    return font_memory_step(s);
}
epd_memory_recovery_result_t epd_memory_recovery_failed(epd_memory_recovery_t *s)
{
    if (!s->pressure_requested) { s->pressure_requested = true; ++font_pressure_calls; }
    return s->state = EPD_MEMORY_RECOVERY_PENDING;
}
