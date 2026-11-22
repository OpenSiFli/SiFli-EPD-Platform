static epd_activity_work_t *credit_work;
static epd_memory_credit_t credit_before;
static uint32_t credit_started;
static void *credit_payload;
static unsigned credit_slices;
static void credit_frame_prepare(void)
{
    if(fixture_activity_kind(allocation_activity)==EPD_RESOURCE_INTERACTIVE) {
        if(!view.frames){view.frames=owned_frame(state);view.frames->complete=true;}
        view.frame_published=true;view.published_intent=view.intent;return;
    }
    assert(fixture_activity_kind(allocation_activity)==EPD_RESOURCE_NEARBY);
    if(!credit_slices++) {
        injected=true;credit_work=allocation_activity;failed_activity=epd_activity_work_activity(credit_work);
        view.rendering=owned_frame(state);old_pins=view.rendering->pins;
        credit_before=retry_host_owner(view.rendering);retry_host_recovery(&view.rendering->admission_recovery);
        credit_started=view.rendering->admission_recovery.started;
        if(scenario==11)retry_host_poll(4800);
        view.frames->admission_recovery=view.rendering->admission_recovery;
        view_frame_t *cold=owned_frame(state);cold->complete=true;
        frame_bytes-=cold->pixels.capacity;epd_compute_buffer_free(&cold->pixels);
        cold->admission_recovery=view.rendering->admission_recovery;
        cold->memory_reclaimed=true;view.frames->next=cold;
        view_frame_t *cancelled=owned_frame(state);cancelled->complete=true;
        frame_bytes-=cancelled->pixels.capacity;epd_compute_buffer_free(&cancelled->pixels);
        cancelled->admission_recovery=view.rendering->admission_recovery;
        assert(epd_memory_recovery_poll(&cancelled->admission_recovery,true)==EPD_MEMORY_RECOVERY_DENIED);
        cold->next=cancelled;
        view.init_recovery=view.image_recovery=view.rendering->admission_recovery;
        credit_payload=epd_app_alloc(40000,EPD_APP_PSRAM);assert(credit_payload);memset(credit_payload,0x63,40000);
        retry_host_pressure();return;
    }
    epd_activity_demand_t demand;assert(epd_activity_work_demand(allocation_activity,&demand));
    assert(allocation_activity==credit_work&&epd_activity_work_activity(credit_work)==failed_activity);
    assert(pressure_released&&demand.credit!=credit_before&&retry_host_ended(credit_before));
    epd_memory_recovery_t *r=&view.rendering->admission_recovery;
    if(r->owner!=demand.credit)fprintf(stderr,"CREDIT_REPLACEMENT_STRANDED\n");
    assert(r->owner==demand.credit&&r->started==credit_started);
    assert(view.init_recovery.owner==demand.credit&&view.image_recovery.owner==demand.credit);
    assert(view.frames->admission_recovery.owner==credit_before&&view.frames->complete);
    assert(view.frames->next->admission_recovery.owner==demand.credit&&view.frames->next->memory_reclaimed);
    assert(view.frames->next->next->admission_recovery.owner==credit_before&&
        view.frames->next->next->admission_recovery.state==EPD_MEMORY_RECOVERY_DENIED);
    assert(retry_host_owner(credit_payload)==credit_before&&((unsigned char *)credit_payload)[39999]==0x63);
    if(scenario==11)assert(r->state==EPD_MEMORY_RECOVERY_DENIED&&r->result==EPD_MEMORY_TIMEOUT);
    else {
        assert(epd_memory_recovery_poll(r,false)==EPD_MEMORY_RECOVERY_READY);
        void *fresh=epd_app_alloc(1024,EPD_APP_PSRAM);assert(fresh&&retry_host_owner(fresh)==demand.credit);epd_app_free(fresh);
    }
    epd_app_free(credit_payload);credit_payload=NULL;++completed;near_wait=false;mailbox.stop=true;
}
static void credit_sem_take(void)
{
    ++ticks;retry_host_poll(10);
    if(!injected){near_wait=true;return;}
    if(!pressure_released&&retry_host_waiting(credit_work)) {
        /* Expire the allocation round while the activity replacement still
         * has its own original admission window. */
        if(scenario==11)retry_host_poll(300);
        retry_host_release();pressure_released=true;
    }
    view_activity_wake(NULL);assert(ticks<100);
}
