/* Real core/runtime retirement, including automatic removal during owner close. */
static epd_activity_work_t *retire_notifying;
static unsigned retire_notices;

static epd_memory_owner_t work_owner(epd_activity_work_t *work)
{
    epd_resource_activity_snapshot_t activity;
    CHECK(epd_resource_activity_get(epd_activity_work_activity(work), &activity) == EPD_RESOURCE_OK);
    return activity.owner;
}

static size_t retire_only(void)
{
    /* setup registers the runtime first; queue registers the helper second. */
    CHECK(clients[0].key && clients[1].key && !clients[2].key);
    rt_thread_t previous=actor; actor=&actors[1];
    size_t done=clients[1].process(32,clients[1].user);
    actor=previous; return done;
}
static void retire_expect_collected(epd_memory_owner_t owner,
                                    const epd_memory_metadata_snapshot_t *baseline)
{
    epd_memory_owner_snapshot_t state;
    CHECK(!epd_memory_owner_get(owner,&state));
    CHECK(!semaphores && !physical[0] && !physical[1]);
    epd_memory_metadata_snapshot_t now; epd_memory_metadata_snapshot(&now);
    CHECK(now.descriptors_used==baseline->descriptors_used);
    CHECK(now.transactions_used==baseline->transactions_used);
    CHECK(now.protocol_errors==baseline->protocol_errors);
}
static void retire_in_notice(void)
{
    component_wake_hook=NULL; ++retire_notices;
    epd_activity_work_t *work=retire_notifying;
    epd_memory_owner_t owner=work_owner(work);
    epd_resource_activity_t activity=epd_activity_work_activity(work);
    CHECK(epd_resource_runtime_close_owner(owner)==EPD_RESOURCE_ACCEPTED);
    epd_activity_work_close(work);
    /* The observer is executing outside resource/manager locks. The helper
     * must survive until both this callback and its queued ending work finish. */
    CHECK(epd_resource_activity_forget(activity)==EPD_RESOURCE_PENDING);
    CHECK(retire_only()==0 && semaphores==1);
    retire_notifying=NULL;
}
static void retire_activity_case(const char *name)
{
    epd_memory_metadata_snapshot_t baseline; epd_memory_metadata_snapshot(&baseline);
    if (!strcmp(name,"retire_auto")) {
        /* More closures than the entire owner table: each iteration must return
         * to the same live descriptor count instead of accumulating dead TASKs. */
        for (uint32_t i=0;i<baseline.descriptors_capacity+1;++i) {
            epd_activity_work_t *work=queued(EPD_RESOURCE_CONTINUOUS,0,4096,512); CHECK(work); drain();
            CHECK(epd_activity_work_ready(work)==1);
            epd_memory_owner_t owner=work_owner(work);
            epd_resource_activity_t activity=epd_activity_work_activity(work);
            CHECK(epd_resource_runtime_close_owner(owner)==EPD_RESOURCE_ACCEPTED); drain();
            CHECK(epd_resource_activity_forget(activity)==EPD_RESOURCE_STALE);
            CHECK(!epd_resource_runtime_owner_busy(owner));
            epd_activity_work_close(work); drain();
            retire_expect_collected(owner,&baseline);
        }
    } else if (!strcmp(name,"retire_pending")) {
        epd_activity_work_t *work=queued(EPD_RESOURCE_CONTINUOUS,0,4096,512); CHECK(work);
        epd_memory_owner_t owner=work_owner(work);
        epd_resource_activity_t activity=epd_activity_work_activity(work);
        epd_activity_work_close(work);
        CHECK(epd_resource_runtime_close_owner(owner)==EPD_RESOURCE_ACCEPTED);
        CHECK(epd_resource_activity_forget(activity)==EPD_RESOURCE_PENDING);
        CHECK(retire_only()==0 && semaphores==1);
        drain(); retire_expect_collected(owner,&baseline);
    } else if (!strcmp(name,"retire_notice")) {
        epd_activity_work_t *work=queued(EPD_RESOURCE_CONTINUOUS,0,4096,512); CHECK(work);
        epd_memory_owner_t owner=work_owner(work);
        retire_notifying=work; component_wake_hook=retire_in_notice;
        drain(); CHECK(retire_notices==1 && !retire_notifying && !component_wake_hook);
        retire_expect_collected(owner,&baseline);
    } else if (!strcmp(name,"retire_reuse")) {
        epd_activity_work_t *old=queued(EPD_RESOURCE_CONTINUOUS,0,4096,512); CHECK(old); drain();
        CHECK(epd_activity_work_ready(old)==1);
        epd_memory_owner_t old_owner=work_owner(old);
        epd_resource_activity_t old_id=epd_activity_work_activity(old);
        CHECK(epd_resource_runtime_close_owner(old_owner)==EPD_RESOURCE_ACCEPTED); drain();
        CHECK(epd_resource_activity_forget(old_id)==EPD_RESOURCE_STALE);
        epd_activity_work_t *fresh=queued(EPD_RESOURCE_CONTINUOUS,0,4096,512); CHECK(fresh); drain();
        epd_resource_activity_t fresh_id=epd_activity_work_activity(fresh);
        CHECK((uint32_t)fresh_id==(uint32_t)old_id && fresh_id!=old_id);
        CHECK(epd_activity_work_ready(fresh)==1);
        epd_memory_owner_t fresh_owner=work_owner(fresh);
        epd_activity_work_close(old); drain();
        epd_memory_owner_snapshot_t owner; CHECK(!epd_memory_owner_get(old_owner,&owner));
        epd_resource_activity_snapshot_t activity;
        CHECK(epd_resource_activity_get(fresh_id,&activity)==EPD_RESOURCE_OK);
        CHECK(activity.state==EPD_RESOURCE_ADMITTED && epd_activity_work_ready(fresh)==1 && semaphores==1);
        epd_activity_guard_t guard; CHECK(epd_activity_work_enter(fresh,&guard));
        void *payload=epd_app_alloc(100,EPD_APP_PSRAM); CHECK(payload);
        epd_activity_work_leave(&guard); epd_app_free(payload);
        epd_activity_work_close(fresh); drain(); retire_expect_collected(fresh_owner,&baseline);
    } else if (!strcmp(name,"retire_payload")) {
        epd_activity_work_t *work=queued(EPD_RESOURCE_CONTINUOUS,0,4096,512); CHECK(work); drain();
        CHECK(epd_activity_work_ready(work)==1);
        epd_memory_owner_t owner=work_owner(work);
        epd_resource_activity_t activity=epd_activity_work_activity(work);
        epd_activity_guard_t guard; CHECK(epd_activity_work_enter(work,&guard));
        unsigned char *payload=epd_app_alloc(100,EPD_APP_PSRAM); CHECK(payload); memset(payload,0xa5,100);
        epd_memory_owner_t credit=block_owner(payload);
        epd_activity_work_leave(&guard);
        CHECK(epd_resource_runtime_close_owner(owner)==EPD_RESOURCE_ACCEPTED); drain();
        CHECK(epd_resource_activity_forget(activity)==EPD_RESOURCE_STALE);
        epd_activity_work_close(work); drain();
        CHECK(!semaphores && physical[1]==132);
        epd_memory_owner_snapshot_t state;
        CHECK(epd_memory_owner_get(owner,&state) && state.closed && !state.allocations);
        CHECK(epd_memory_owner_get(credit,&state) && state.closed && state.allocations==1);
        epd_memory_account_snapshot_t usage; CHECK(epd_memory_scope_snapshot(credit,&usage));
        CHECK(usage.backend[backends[1]].physical[EPD_MEMORY_ACTIVE]==132 && !usage.backend[backends[1]].unused);
        for (unsigned i=0;i<100;++i) CHECK(payload[i]==0xa5);
        epd_app_free(payload); drain(); retire_expect_collected(owner,&baseline);
    } else CHECK(false);
}
