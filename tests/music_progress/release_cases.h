/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0
 */
static void release_finished(epd_service_t *service, uint64_t position)
{
    CHECK(!dirty && !save_failed && !session_active && !working.playing && !play_intent);
    CHECK(!work_holds && !test_cleanup_holds && !test_cleanup_references);
    CHECK(test_cleanup_context==1 && !commands && !queue_allocations && !has_pending_media);
    CHECK(retire_ready && !ui_process(service,false) && stops==1);
    restored_position(position);
}

static void release_wait(epd_service_t *service, rt_int32_t delay)
{
    unsigned phase=wait_phase++;
    if(run_case==11){
        CHECK(!phase && !backend_opens && store.queue.mode==MUSIC_MODE_REPEAT_ONE);
        CHECK(music_favorites_contains(&store.favorites,"/sd/accepted.flac"));
        music_store_state_t saved;music_store_init(&saved);OK(music_store_load(&saved));
        CHECK(music_favorites_contains(&saved.favorites,"/sd/accepted.flac"));music_store_free(&saved);
        release_finished(service,0);return;
    }
    if(run_case==8 || run_case==9){
        if(phase==(run_case==9?3u:2u)){release_finished(service,91000);return;}
        if(!phase){
            CHECK(!work.service && !work_holds);
            dirty=save_failed=true;store.queue.position_ms=91000;
            ordinary_admit=false;has_pending_media=true;pending_media=EPD_MEDIA_PLAY;
            fail_save=run_case==9;CHECK(!flush() && work.cleanup);return;
        }
        CHECK(delay==RT_WAITING_FOREVER && !work_holds && !test_cleanup_holds && !test_cleanup_references);
        CHECK(flush_requested==flush_completed && !backend_opens && has_pending_media);
        if(run_case==9 && phase==1){
            CHECK(dirty && save_failed && !flush_ok && !stops);
            restored_position(0);fail_save=false;CHECK(!flush() && work.cleanup);return;
        }
        if(phase==(run_case==9?2u:1u)){
            CHECK(!dirty && flush_ok && flush());restored_position(91000);
            background_cancelled=true;CHECK(!flush() || flush_ready);
            /* The policy wake alone must settle a previously deferred command. */
            return;
        }
        CHECK(false);
    }
    if(!phase){
        CHECK(!session_active && !backend_opens);submit_kind(MUSIC_CMD_PLAY);return;
    }
    if(phase==1){
        CHECK(session_active && working.playing && work_holds==1);
        audio.position_ms=91000;current_page=NULL;music_ui_active(false);
        if(run_case==10){
            /* A replacement policy/foreground return wins before worker observation. */
            background_cancelled=true;background_cancelled=false;return;
        }
        background_cancelled=true;ordinary_admit=false;
        if(run_case==6){
            uint32_t selected=store.queue.current_id;
            uint64_t position=store.queue.position_ms;
            advance(false,true);
            CHECK(store.queue.current_id==selected && store.queue.position_ms==position);
            CHECK(controls_waiting() && !open_current(true,true) && backend_opens==1);
        }
        fail_save=run_case==7 || run_case==13;
        return;
    }
    if(run_case==10 && phase==2){
        CHECK(session_active && working.playing && backend_opens==1 && work_holds==1);
        background_cancelled=true;ordinary_admit=false;return;
    }
    if((run_case==7 || run_case==13) && phase==2){
        CHECK(dirty && save_failed && store.queue.count==2 && !session_active);
        CHECK(!work_holds && !test_cleanup_holds && !retire_ready && !ui_process(service,false) && !stops);
        restored_position(0);music_command_t play={.kind=MUSIC_CMD_PLAY};CHECK(!music_submit(&play));
        fail_save=false;
        if(run_case==13){background_cancelled=false;ordinary_admit=true;CHECK(music_submit(&play));return;}
        test_cleanup_enter_fail=true;CHECK(!flush() && work.cleanup);return;
    }
    if(run_case==7 && phase==3){
        CHECK(dirty && save_failed && !work_holds && !test_cleanup_holds && !test_cleanup_references);
        restored_position(0);test_cleanup_enter_fail=false;CHECK(!flush() && work.cleanup);return;
    }
    if(run_case==13 && phase==3){
        CHECK(session_active && working.playing && work_holds==1 && audio.position_ms==91000);
        audio.position_ms=93000;background_cancelled=true;ordinary_admit=false;return;
    }
    release_finished(service,run_case==13?93000:91000);
}

static void release_integration(void)
{
    for(run_case=6;run_case<=13;++run_case){
        lifecycle_reset();music_store_free(&store);
        wait_phase=destroyed=cover_stops=0;
        test_cleanup_admit=true;test_cleanup_enter_fail=false;
        if(run_case==11){
            music_command_t favorite={.kind=MUSIC_CMD_FAVORITE,.value=1};
            strcpy(favorite.path,"/sd/accepted.flac");CHECK(music_submit(&favorite));
            submit_kind(MUSIC_CMD_PLAY);submit_kind(MUSIC_CMD_TIMER);
            music_command_t mode={.kind=MUSIC_CMD_MODE,.value=MUSIC_MODE_REPEAT_ONE};CHECK(music_submit(&mode));
            background_cancelled=true;ordinary_admit=false;
        }else if(run_case==12){background_cancelled=true;ordinary_admit=false;}
        epd_service_t *service=worker;run(service);
        CHECK(!worker && !accepting && !work_holds && !test_cleanup_holds && !test_cleanup_references);
        CHECK(!commands && !queue_allocations && !lock_depth && test_cleanup_context==1);
        if(run_case==12)CHECK(!wait_phase && !backend_opens && !destroyed);
        else CHECK(stops==1 && destroyed==1 && !store.queue.entries);
        static const char *names[]={"active RELEASE and late EOF", "failed save and cleanup scope retry",
            "denied media preserves save", "denied media failed save retry", "withdrawn policy keeps playback",
            "accepted edits drain before startup", "pre-start RELEASE without work", "reopen retained dirty queue"};
        printf("PASS music background drain: %s\n",names[run_case-6]);
    }
    reset();music_store_free(&store);
}
