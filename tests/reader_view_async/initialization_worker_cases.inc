enum { FIRST_COMMAND, NEW_ACTIVITY, SUSPEND_RESUME, SECOND_FAILURE, OWNER_TIMEOUT, RETURN_ORIGIN,
       ADMISSION_RESUME, ADMISSION_DENIED, ADMISSION_COLD, PAGE_MEMORY, PAGE_MEMORY_REVERSE, BACKGROUND_CLASS };
static unsigned scenario,nth_failure;
static uint32_t started_at;
static epd_memory_owner_t first_owner;
static view_state_t old_chapter;
static unsigned denied_queue_count;
static unsigned before_failure_wakes;
static void pressure_ready(void) {
    pressure_status.pending=false;pressure_status.result=APP_MEMORY_PRESSURE_SATISFIED;
}
static void queue_turn(void) {
    assert(!mailbox.activity);
    mailbox.activity=view_activity_new();++mailbox.intent;
    mailbox.command=(view_command_t){.kind=INTENT_TURN,.session=7,.serial=mailbox.intent,.value=3};
}
static void initialization_wait(unsigned timeout) {
    if(scenario==BACKGROUND_CLASS) {
        assert(ticks<=4 && steps==ticks && renders==ticks && !mailbox.error);
        if(ticks==1) {
            assert(last_step_kind==EPD_RESOURCE_INTERACTIVE && !fixture_current->background_paused);
            near_wait=true;
        } else if(ticks==2) {
            assert(last_step_kind==EPD_RESOURCE_NEARBY && !fixture_current->background_paused);
            near_wait=false;
        } else if(ticks==3) {
            assert(last_step_kind==EPD_RESOURCE_OPTIONAL);
            ui.session=fixture_current;ui.bound=&ack_frame;ack_frame.owner=fixture_current;
            ui.intent=mailbox.intent;ui.id=7;
            epd_reader_turn(-1);
        } else {
            assert(last_step_kind==EPD_RESOURCE_INTERACTIVE && fixture_current->deferred_turns==-1);
            mailbox.stop=true;
        }
        return;
    }
    if(scenario==PAGE_MEMORY || scenario==PAGE_MEMORY_REVERSE) {
        assert(ticks<=5 && !locks);
        if(ticks==1) {
            assert(renders==1 && !fixture_current->failure);
            fixture_current->displayed=true;ack_frame.owner=fixture_current;
            queue_turn();mailbox.command.origin=&ack_frame;operation_memory_denied=true;
            ui.session=fixture_current;ui.bound=&ack_frame;ui.intent=mailbox.intent;ui.id=7;ui.pending=true;
            ui.turn_delta=1;
            ui.info=(reader_view_t){.ready=true,.page=7,.pages=8,.revision=23};
            before_failure_wakes=ui_wakes;
        } else if(ticks<=4) {
            assert(timeout==(ticks==2?20:RT_WAITING_FOREVER) && renders==1 && steps==2 && !fixture_current->failure);
            assert(render_cleanups==(ticks==2?0u:1u));
            assert(mailbox.error==READER_ERROR_LAYOUT_MEMORY && mailbox.rejected && !activity_tasks);
            assert(ui_wakes==before_failure_wakes+1 && mailbox.work==READER_WORK_IDLE);
            epd_reader_step(0,false,true);
            assert(!ui.pending && !ui.turn_delta && ui.info.error_code==READER_ERROR_LAYOUT_MEMORY && ui.bound==&ack_frame);
            if(ticks==4) {
                operation_memory_denied=false;
                int direction=scenario==PAGE_MEMORY_REVERSE?-1:1;
                epd_reader_turn(direction);
                assert(mailbox.command.kind==INTENT_TURN && mailbox.command.value==direction);
                assert(ui.turn_delta==direction && ui.pending && !mailbox.rejected);
            }
        } else {
            assert(renders==2 && steps==3 && !mailbox.error && !fixture_current->failure);
            assert(fixture_current->deferred_turns==(scenario==PAGE_MEMORY_REVERSE?-1:1));
            epd_reader_step(0,false,true);assert(!ui.info.error_code);
            mailbox.stop=true;
        }
        return;
    }
    if(scenario>=ADMISSION_RESUME) {
        assert(ticks<=13 && !pressure_calls && !locks);
        if(scenario==ADMISSION_COLD) {
            assert(ticks==1 && timeout==RT_WAITING_FOREVER && !renders && !live_allocations);
            assert(mailbox.error==READER_ERROR_LAYOUT_MEMORY && !mailbox.rejected);
            mailbox.stop=true;return;
        }
        if(ticks==1) {
            assert(renders==1 && fixture_current->initialized && !mailbox.error);
            fixture_current->displayed=true;ack_frame.owner=fixture_current;
            activity_admission=scenario==ADMISSION_RESUME?1:-1;
            queue_turn();mailbox.activity->ready=-1;mailbox.command.origin=&ack_frame;
            ui.session=fixture_current;ui.bound=&ack_frame;ui.intent=mailbox.intent;ui.id=7;
            ui.pending=true;ui.turn_delta=3;ui.info=(reader_view_t){.ready=true,.pages=8,.page=7,.revision=23};
        } else if(ticks==2) {
            assert(timeout==20 && mailbox.command.kind==INTENT_TURN && mailbox.command.origin==&ack_frame);
            assert(renders==1 && !fixture_current->failure && !mailbox.error);
            assert(epd_reader_step(0,false,true)==READER_WORK_TARGET && ui.pending);
        } else if(scenario==ADMISSION_RESUME) {
            assert(ticks==3 && renders==2 && !mailbox.error && !mailbox.rejected);
            assert(mailbox.command.kind==INTENT_NONE && fixture_current->deferred_turns==3);
            mailbox.stop=true;
        } else if(ticks<=12) {
            assert(timeout==RT_WAITING_FOREVER && renders==1 && !activity_tasks);
            assert(!fixture_current->failure && mailbox.error==READER_ERROR_LAYOUT_MEMORY && mailbox.rejected);
            assert(mailbox.result_intent==ui.intent && mailbox.command.kind==INTENT_NONE && !mailbox.command.origin);
            if(ticks==3)denied_queue_count=activity_queued;
            assert(activity_queued==denied_queue_count && font_retries==4);
            assert(epd_reader_step(0,false,true)==READER_WORK_IDLE);
            assert(!ui.pending && !ui.turn_delta && ui.info.error_code==READER_ERROR_LAYOUT_MEMORY && ui.bound==&ack_frame);
            assert(ui.info.ready && ui.info.pages==8 && ui.info.page==7 && ui.info.revision==23);
            if(ticks==12) {
                activity_admission=1;queue_turn();mailbox.command.origin=&ack_frame;mailbox.rejected=false;
                ui.intent=mailbox.intent;ui.pending=true;ui.turn_delta=3;
            }
        } else {
            assert(ticks==13 && renders==2 && !mailbox.rejected && !mailbox.error && font_retries==6);
            assert(mailbox.command.kind==INTENT_NONE && fixture_current->deferred_turns==3);
            mailbox.stop=true;
        }
        return;
    }
    assert(ticks<=252 && !locks && pressure_calls==1);
    assert(fixture_current->init_recovery.started==started_at);
    assert(fixture_current->init_recovery.generation==1);
    assert(!context_rejections);
    if(ticks==1) {
        assert(timeout==20 && !fixture_current->initialized && !fixture_current->init_attempted);
        assert(!fixture_current->index && !fixture_current->pages && !fixture_current->compute_spare);
        first_owner=fixture_current->init_recovery.owner;
    }
    if(scenario==RETURN_ORIGIN) {
        if(ticks==1) {
            assert(!renders);pressure_status.pending=false;pressure_status.result=APP_MEMORY_PRESSURE_EXHAUSTED;
        } else if(ticks==2) {
            assert(timeout==RT_WAITING_FOREVER && fixture_current->init_attempted && !fixture_current->initialized);
            assert(mailbox.error==READER_ERROR_LAYOUT_MEMORY && !renders);
            mailbox.activity=view_activity_new();++mailbox.intent;
            mailbox.command=(view_command_t){.kind=INTENT_CANCEL,.origin=&ack_frame,.session=7,.serial=mailbox.intent};
        } else {
            assert(ticks==3 && state==&old_chapter && primary_state==state && cancel_calls==1);
            assert(!foreground[2] && foreground[1] && foreground_changes==2);
            assert(mailbox.command.kind==INTENT_NONE && !mailbox.command.origin && !mailbox.error && renders==1);
            mailbox.stop=true;
        }
    } else if(scenario==OWNER_TIMEOUT) {
        assert(!renders && !steps);
        if(fixture_current->init_attempted) {
            assert(timeout==RT_WAITING_FOREVER && mailbox.error==READER_ERROR_LAYOUT_MEMORY);
            assert(fixture_current->init_recovery.result==EPD_MEMORY_TIMEOUT && ticks==251);
            mailbox.stop=true;
        } else { assert(timeout==20);queue_turn(); }
    } else if(scenario==SUSPEND_RESUME) {
        if(ticks==1)mailbox.suspended=true;
        else if(ticks==2) {
            assert(timeout==RT_WAITING_FOREVER && !renders && !fixture_current->init_attempted);
            mailbox.suspended=false;pressure_ready();
        } else {
            assert(ticks==3 && renders==1 && fixture_current->initialized && !mailbox.error);
            assert(fixture_current->init_recovery.owner!=first_owner);mailbox.stop=true;
        }
    } else if(ticks<=2) {
        assert(timeout==20 && !renders && !steps && !fixture_current->failure);
        assert(mailbox.command.kind==INTENT_TURN && mailbox.command.value==3);
        if(scenario==NEW_ACTIVITY)queue_turn();
        if(ticks==2) {
            pressure_ready();fail_allocation=scenario==SECOND_FAILURE?allocation_calls+nth_failure:0;
        }
    } else {
        assert(ticks==3 && pressure_calls==1);
        if(scenario==SECOND_FAILURE) {
            assert(timeout==RT_WAITING_FOREVER && fixture_current->init_attempted && !fixture_current->initialized);
            assert(mailbox.error==READER_ERROR_LAYOUT_MEMORY && !renders && !live_allocations && !live_compute);
        } else {
            assert(renders==1 && steps==1 && fixture_current->initialized && !mailbox.error);
            assert(mailbox.command.kind==INTENT_NONE && fixture_current->deferred_turns==3);
            assert(fixture_current->direction==-1 && fixture_current->published_intent==mailbox.intent);
            if(scenario==NEW_ACTIVITY)assert(fixture_current->init_recovery.owner!=first_owner);
        }
        mailbox.stop=true;
    }
    if(timeout!=RT_WAITING_FOREVER)clock_ticks+=timeout;
}
static void run_admission(unsigned kind) {
    assert(!live_allocations && !live_compute && !activity_tasks);
    memset(&mailbox,0,sizeof(mailbox));memset(&cold_destination,0,sizeof(cold_destination));memset(&ui,0,sizeof(ui));
    scenario=kind;fixture_current=&cold_destination;state=primary_state=parked_state=NULL;
    ticks=steps=renders=pressure_calls=allocation_calls=fail_allocation=render_cleanups=0;stopped=false;work_pending=true;
    activity_admission=kind==ADMISSION_COLD?-1:1;
    font_retries=0;cold_destination.fonts[0]=cold_destination.fonts[1]=(void *)1;
    cold_destination.oblique[0]=(void *)2;
    cold_destination.session=mailbox.session=7;cold_destination.document=(void *)2;mailbox.intent=23;
    mailbox.incoming=&cold_destination;mailbox.replace=true;mailbox.activity=view_activity_new();
    view_worker(NULL);
    assert(stopped && !locks && !activity_tasks && !live_allocations && !live_compute && !layout_bytes && !index_bytes);
    activity_admission=1;
}
static void run_combined(unsigned kind,unsigned nth) {
    assert(!live_allocations && !live_compute && !index_bytes && !layout_bytes && !activity_tasks);
    memset(&mailbox,0,sizeof(mailbox));memset(&cold_destination,0,sizeof(cold_destination));
    memset(&old_chapter,0,sizeof(old_chapter));memset(&pressure_status,0,sizeof(pressure_status));
    scenario=kind;nth_failure=nth;fixture_current=&cold_destination;
    ticks=steps=renders=retired=cancel_calls=pressure_calls=context_rejections=foreground_changes=0;
    allocation_calls=fail_allocation=0;stopped=false;work_pending=true;mode=0;
    free_sentinel.next_free=&free_sentinel;
    app_image_psram_memheap.free_list=&free_sentinel;app_image_psram_memheap.available_size=1024u*1024u;
    clock_ticks=started_at=UINT32_MAX-1000u;
    cold_destination.session=mailbox.session=7;cold_destination.intent=mailbox.intent=23;
    cold_destination.direction=-1;cold_destination.document=(void *)2;
    foreground[1]=false;foreground[2]=true;
    if(kind==RETURN_ORIGIN) {
        state=&old_chapter;old_chapter.selected=true;old_chapter.session=7;old_chapter.document=(void *)1;
        assert(core_initialize());old_chapter.displayed=old_chapter.status.ready=true;
        cold_destination.previous=&old_chapter;ack_frame.owner=&old_chapter;
        allocation_calls=0;
    }
    state=primary_state=NULL;
    fail_allocation=nth;
    mailbox.incoming=&cold_destination;mailbox.replace=true;mailbox.activity=view_activity_new();
    if(kind!=RETURN_ORIGIN)mailbox.command=(view_command_t){.kind=INTENT_TURN,.session=7,.serial=23,.value=3};
    view_worker(NULL);
    assert(stopped && !state && !primary_state && !locks && !activity_tasks && !context_rejections);
    assert(!live_allocations && !live_compute && !index_bytes && !layout_bytes);
    assert(pressure_calls==1 && cold_destination.init_recovery.started==started_at);
}
int main(void) {
    for(unsigned nth=1;nth<=5;++nth) {
        run_combined(FIRST_COMMAND,nth);run_combined(NEW_ACTIVITY,nth);
        run_combined(SUSPEND_RESUME,nth);run_combined(SECOND_FAILURE,nth);
        run_combined(RETURN_ORIGIN,nth);
    }
    run_combined(OWNER_TIMEOUT,3);
    run_admission(ADMISSION_RESUME);run_admission(ADMISSION_DENIED);run_admission(ADMISSION_COLD);run_admission(PAGE_MEMORY);run_admission(PAGE_MEMORY_REVERSE);run_admission(BACKGROUND_CLASS);
    puts("PASS worker foreground phase excludes speculative work; nearby and counting use background activities, and a new turn reacquires foreground admission");
    puts("PASS resource rejection clears the accumulated UI turn: the next real forward/reverse command is exactly one page, never double-step/cancel");
    puts("PASS operation memory failure publishes once, closes activity, waits for events and resumes the next intent with the same displayed owner");
    puts("PASS admission denial retains the actual command through one retry; repeated denial reports an explicit operation error and preserves the displayed UI");
    puts("PASS rejected intent performs no automatic retry across ten wakeups, releases activities, then accepts a new turn without a permanent view failure; cold opening still reports terminal error");
    puts("PASS combined production initialization/worker/command and SDK recovery: 26 allocation/activity scenarios");
    puts("PASS incoming command survives pending initialization and executes before the new chapter first frame");
    puts("PASS activity replacement and suspend/resume preserve one pressure generation and the original deadline");
    puts("PASS repeated owner replacement reaches the original 5s timeout across tick wrap; second allocation failure is terminal");
    puts("PASS terminal cold chapter accepts displayed-origin cancellation and switches document foreground before resuming");
    return 0;
}
