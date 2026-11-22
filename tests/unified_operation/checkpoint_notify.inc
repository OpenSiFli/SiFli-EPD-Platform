/* Checkpoint polling must let the lower-priority persistence worker run. */
static unsigned checkpoint_worker_wakes(void)
{
    epd_operation_executor_t execute, deliver, cleanup_executor;
    CHECK(epd_operation_runtime_executors(&execute,&deliver,&cleanup_executor));
    CHECK(execute.thread==&runtime_thread && execute.wake);
    return execute.wake->sends;
}
static void checkpoint_notify_open(hello_state_t *model)
{
    CHECK(epd_operation_runtime_init());checkpoint_bind(1,"notify-demo");
    checkpoint_open(1,model);checkpoint_pump();checkpoint_increment(1,model);
    CHECK(app_background_foreground(1,false)==EPD_MEMORY_OK);
}
static void checkpoint_notify_request(void)
{
    epd_app_background_state_t terminal;
    unsigned ui_before=power_wakes, worker_before=checkpoint_worker_wakes();
    CHECK(app_background_prepare_close(1,&terminal)==EPD_MEMORY_WAITING);
    CHECK(power_wakes>ui_before && checkpoint_worker_wakes()>worker_before);
}
static void checkpoint_notify_quiet(epd_memory_result_t expected)
{
    unsigned ui_before=power_wakes, worker_before=checkpoint_worker_wakes(), io_before=io_calls;
    epd_app_background_state_t terminal;
    for(unsigned i=0;i<32;++i) CHECK(app_background_prepare_close(1,&terminal)==expected);
    CHECK(power_wakes==ui_before && checkpoint_worker_wakes()==worker_before);
    CHECK(io_calls==io_before && !owner_closed[1]);
}
static void checkpoint_notify_capture(void)
{
    CHECK(epd_operation_runtime_ui_pump(8)>0);
    epd_cold_runtime_snapshot_t view=checkpoint_view(1);
    CHECK(view.busy && !view.restoring && !view.failed && !view.durable);
    CHECK(view.captured_dirty_revision && !commits);
}
static void checkpoint_notify_reopen(hello_state_t *model,uint32_t count)
{
    checkpoint_finish(1,model);
    hello_state_t fresh={0};checkpoint_bind(2,"notify-demo");checkpoint_open(2,&fresh);
    checkpoint_pump();CHECK(fresh.count==count);checkpoint_save(2);checkpoint_finish(2,&fresh);
}
static void checkpoint_notify_saving(void)
{
    hello_state_t model={0};checkpoint_notify_open(&model);checkpoint_notify_request();
    checkpoint_notify_capture();checkpoint_notify_quiet(EPD_MEMORY_WAITING);
    unsigned ui_before=power_wakes, worker_before=checkpoint_worker_wakes();
    settings_pump();
    CHECK(commits==1 && checkpoint_view(1).durable);
    CHECK(power_wakes>ui_before && checkpoint_worker_wakes()>worker_before);
    checkpoint_notify_reopen(&model,1);
}
static void checkpoint_notify_retry(void)
{
    hello_state_t model={0};checkpoint_notify_open(&model);checkpoint_notify_request();
    checkpoint_notify_capture();fail_write=true;
    unsigned ui_before=power_wakes, worker_before=checkpoint_worker_wakes();
    settings_pump();
    epd_cold_runtime_snapshot_t failed=checkpoint_view(1);
    CHECK(failed.failed && !failed.durable && !commits);
    CHECK(power_wakes>ui_before && checkpoint_worker_wakes()>worker_before);
    checkpoint_notify_quiet(failed.result);
    CHECK(!checkpoint_call(1,epd_checkpoint_unregister,model.checkpoint));
    fail_write=false;ui_before=power_wakes;worker_before=checkpoint_worker_wakes();
    CHECK(checkpoint_call(1,epd_checkpoint_retry,model.checkpoint));
    CHECK(power_wakes>ui_before && checkpoint_worker_wakes()>worker_before);
    checkpoint_notify_quiet(EPD_MEMORY_WAITING);
    ui_before=power_wakes;worker_before=checkpoint_worker_wakes();settings_pump();
    CHECK(commits==1 && checkpoint_view(1).durable);
    CHECK(power_wakes>ui_before && checkpoint_worker_wakes()>worker_before);
    checkpoint_notify_reopen(&model,1);
}
static void checkpoint_notify_progress(void)
{
    hello_state_t model={0};checkpoint_notify_open(&model);checkpoint_notify_request();
    unsigned background_turns=0;
    bool closed=false;
    /* Each pass consumes the previous UI event. A new self-notification keeps
     * higher-priority UI ready; only a quiet pass permits the worker to run. */
    for(unsigned turn=0;turn<8;++turn) {
        unsigned ui_before=power_wakes;
        (void)epd_operation_runtime_ui_pump(8);
        epd_app_background_state_t terminal;
        if(app_background_prepare_close(1,&terminal)==EPD_MEMORY_OK) {closed=true;break;}
        if(power_wakes!=ui_before) continue;
        ++background_turns;settings_pump();
    }
    printf("checkpoint progress: worker_turns=%u closed=%u commits=%u\n",
           background_turns,(unsigned)closed,commits);
    CHECK(background_turns && closed && commits==1 && checkpoint_view(1).durable);
    checkpoint_notify_reopen(&model,1);
}
static void checkpoint_notify_revision(void)
{
    hello_state_t model={0};checkpoint_notify_open(&model);checkpoint_notify_request();
    checkpoint_notify_quiet(EPD_MEMORY_WAITING); /* Capture is queued, not yet run. */
    checkpoint_notify_capture();
    CHECK(app_background_foreground(1,true)==EPD_MEMORY_OK);
    CHECK(app_background_foreground(1,false)==EPD_MEMORY_OK);
    checkpoint_notify_request();checkpoint_notify_quiet(EPD_MEMORY_WAITING);
    checkpoint_increment(1,&model);
    checkpoint_notify_request();checkpoint_notify_quiet(EPD_MEMORY_WAITING);
    unsigned ui_before=power_wakes;settings_pump();
    CHECK(commits==1 && !checkpoint_view(1).durable && power_wakes>ui_before);
    CHECK(epd_operation_runtime_ui_pump(8)>0);
    checkpoint_notify_quiet(EPD_MEMORY_WAITING);
    ui_before=power_wakes;settings_pump();
    CHECK(commits==2 && checkpoint_view(1).durable && power_wakes>ui_before);
    checkpoint_notify_reopen(&model,2);
}
