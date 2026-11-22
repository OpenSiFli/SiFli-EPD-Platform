#define epd_compute_query data_raw_query
#define main reading_data_fixture_main
#include "../compute_reading_data/test_data.c"
#undef main
#undef epd_compute_query
int epd_compute_query(const char *name, uint32_t version, epd_compute_capability_t *capability)
{
    static bool initialized;
    if (!initialized) { InitializeCriticalSection(&irq); initialized = true; }
    return data_raw_query(name, version, capability);
}
void host_data_probe(void (*probe)(void)) { compute_probe = probe; }
void host_data_resources(void) { CHECK(!heap && !low && opens == closes); }
