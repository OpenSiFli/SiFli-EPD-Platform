/* Deterministic platform completion for component transition tests. Actual
 * credit/backend classification is exercised with the production core. */
static uint32_t app_memory_pressure_allocation_failed(size_t minimum)
{ return app_memory_pressure_request(minimum); }
static uint32_t app_memory_pressure_allocation_failed_block(size_t minimum, size_t physical)
{ (void)physical; return app_memory_pressure_allocation_failed(minimum); }
static epd_memory_recovery_result_t fixture_recovery_poll(epd_memory_recovery_t *r)
{
    if (r->state == EPD_MEMORY_RECOVERY_DENIED) return r->state;
    if (elapsed_ms(r->started) >= VIEW_PRESSURE_WAIT_MS) return r->state = EPD_MEMORY_RECOVERY_DENIED;
    if (fixture_recovery_ready(r)) return r->state = EPD_MEMORY_RECOVERY_READY;
    app_memory_pressure_status_t pressure = app_memory_pressure_query();
    return r->state = r->generation && r->generation == pressure.generation && pressure.pending ?
        EPD_MEMORY_RECOVERY_PENDING : EPD_MEMORY_RECOVERY_DENIED;
}
epd_memory_recovery_result_t epd_memory_recovery_poll(epd_memory_recovery_t *r, bool cancelled)
{ if (cancelled) return r->state = EPD_MEMORY_RECOVERY_DENIED; return fixture_recovery_poll(r); }
epd_memory_recovery_result_t epd_memory_recovery_failed(epd_memory_recovery_t *r)
{ fixture_recovery_start(r,true); return fixture_recovery_poll(r); }
static epd_memory_recovery_result_t view_memory_recover(epd_memory_recovery_t *r,size_t bytes,bool failed)
{
    if (r->used) return fixture_recovery_poll(r);
    r->used = true; r->physical = bytes; r->started = rt_tick_get(); r->state = EPD_MEMORY_RECOVERY_PENDING;
    if (failed || !fixture_recovery_ready(r)) fixture_recovery_start(r, failed);
    return fixture_recovery_poll(r);
}
bool epd_memory_app_backend(epd_memory_region_t region,epd_memory_backend_t *out)
{ if(region!=EPD_MEMORY_REGION_HCPU_PSRAM || !out)return false;*out=1;return true; }
epd_memory_recovery_result_t epd_memory_recovery_failed_allocation(epd_memory_recovery_t *r,size_t payload)
{ (void)payload;return epd_memory_recovery_failed(r); }
epd_memory_recovery_result_t epd_memory_recovery_begin_allocation(epd_memory_recovery_t *r,
    epd_memory_backend_t backend,size_t physical,size_t payload,bool failed)
{ (void)backend;(void)payload;return view_memory_recover(r,physical,failed); }
