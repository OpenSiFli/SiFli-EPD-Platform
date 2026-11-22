#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
typedef int rt_err_t, rt_bool_t, rt_base_t, rt_tick_t, sdcard_error_stage_t;
enum { RT_FALSE, RT_TRUE, SDCARD_DRAINING, SDCARD_READY };
enum { RT_EOK = 0, RT_ETIMEOUT = 5, RT_WAITING_FOREVER = -1,
       SDCARD_ERROR_NONE = 0, SDCARD_ERROR_RELEASE_WAIT = 1 };
#define SDCARD_RELEASE_MS 35000
static struct {
    struct { bool export_requested, present; unsigned release_id; int state, error_stage, error; } view;
    bool export_draining, acknowledged, release_posted, invalidated;
    unsigned next_release, edges, removals;
    int released;
} s_card;
static bool pin, acknowledge_during_sample;
static unsigned irq, calls, publications, release_posts, worker_posts, first_id;
static const char *scenario;
static rt_base_t rt_hw_interrupt_disable(void) { return irq++; }
static void rt_hw_interrupt_enable(rt_base_t level) { assert(irq == (unsigned)level + 1); --irq; }
static rt_tick_t rt_tick_from_millisecond(int ms) { return ms; }
static bool _pin_present(void) { return pin; }
static void _publish_changed(void) { ++publications; }
static void _invalidate_locked(void) { assert(irq); s_card.invalidated = true; }
static void _wake_worker(void) { ++worker_posts; }
static void _set_state(int state, int stage, int error)
{ s_card.view.state = state; s_card.view.error_stage = stage; s_card.view.error = error; }
static void _fault(int stage, int error, bool fatal)
{ (void)stage; (void)error; (void)fatal; assert(!"unexpected drain failure"); }
static int rt_sem_release(int *sem) { assert(sem == &s_card.released); ++release_posts; return 0; }
static int rt_sem_take(int *, int);
static unsigned _sample(rt_bool_t *present)
{
    assert(!irq); *present = pin; s_card.view.present = pin;
    if (acknowledge_during_sample) s_card.acknowledged = true;
    return s_card.edges;
}
#include "production.inc"
static void edge(bool present) { pin = present; _pin_irq(NULL); }
static int rt_sem_take(int *sem, int timeout)
{
    assert(!irq && sem == &s_card.released && ++calls < 8);
    if (calls == 1) {
        first_id = s_card.view.release_id;
        assert(first_id && timeout == SDCARD_RELEASE_MS);
        if (!strcmp(scenario, "success")) { s_card.acknowledged = true; return 0; }
        if (!strcmp(scenario, "cancel")) { s_card.view.export_requested = false; return 0; }
        return -RT_ETIMEOUT;
    }
    if (calls == 2) {
        assert(timeout == RT_WAITING_FOREVER && s_card.view.error == -RT_ETIMEOUT);
        if (!strcmp(scenario, "quiet")) { s_card.acknowledged = true; return 0; }
        edge(true);
        assert(release_posts == 1);
        if (!strcmp(scenario, "bounce")) { edge(false); assert(release_posts == 1); }
        if (!strcmp(scenario, "ack-race")) s_card.acknowledged = true;
        if (!strcmp(scenario, "sample-race")) acknowledge_during_sample = true;
        return 0;
    }
    if (!strcmp(scenario, "bounce") && calls == 3) {
        assert(timeout == RT_WAITING_FOREVER && s_card.view.release_id == first_id);
        edge(true); assert(release_posts == 2); return 0;
    }
    assert(timeout == SDCARD_RELEASE_MS && s_card.view.release_id != first_id);
    assert(!s_card.view.error && s_card.view.error_stage == SDCARD_ERROR_NONE);
    s_card.acknowledged = true;
    return 0;
}
int main(int argc, char **argv)
{
#ifdef _MSC_VER
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    assert(argc == 2); scenario = argv[1];
    bool exporting = !strcmp(scenario, "cancel");
    pin = s_card.view.present = exporting;
    s_card.view.export_requested = exporting;
    bool result = _drain(exporting);
    assert(!irq && !s_card.export_draining);
    if (exporting) assert(!result && s_card.view.state == SDCARD_READY && !s_card.view.release_id);
    else {
        assert(result);
        bool renewed = !strcmp(scenario, "insert") || !strcmp(scenario, "bounce");
        assert(s_card.next_release == (renewed ? 2u : 1u));
    }
    printf("PASS %s waits=%u release_attempts=%u\n", scenario, calls, s_card.next_release);
    return 0;
}
