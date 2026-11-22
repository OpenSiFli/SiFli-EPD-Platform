#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define RT_EOK 0
#define CHECK(x) do { ++checks; if(!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static unsigned checks;
static bool lock_busy;
static struct {
    void *mutex;
    bool queued, refresh, network_busy, speech_busy, speech_active;
    uint32_t read_id, settled_read;
    struct { bool busy; unsigned count; } view;
    struct { uint32_t id; unsigned bytes; } turns[3];
} storage, *chat;
static int rt_mutex_take(void *m, int t) { (void)m; (void)t; return lock_busy?1:0; }
static void unlock(void) {}
#include "actual.inc"
int main(void)
{
    CHECK(books_ai_idle()); chat=&storage; CHECK(books_ai_idle());
    bool *flags[]={&chat->queued,&chat->refresh,&chat->network_busy,&chat->speech_busy,
        &chat->speech_active,&chat->view.busy};
    for(unsigned i=0;i<sizeof(flags)/sizeof(flags[0]);++i) {
        *flags[i]=true; CHECK(!books_ai_idle()); *flags[i]=false;
    }
    lock_busy=true; CHECK(!books_ai_idle()); lock_busy=false;
    chat->view.count=1; chat->turns[0].id=7; chat->turns[0].bytes=64; chat->read_id=7;
    CHECK(!books_ai_idle()); chat->settled_read=7; CHECK(books_ai_idle());
    chat->speech_active=true; CHECK(!books_ai_idle()); chat->speech_active=false;
    chat->turns[0].bytes=0; chat->settled_read=0; CHECK(books_ai_idle());
    printf("PASS books AI pending speech: %u checks\n",checks); return 0;
}
