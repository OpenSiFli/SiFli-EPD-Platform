static unsigned scenario,attempts,completed,failed_at,old_pin_frees,renderer_closes,close_polls;
static void credit_frame_prepare(void);
static void credit_sem_take(void);
static bool injected,pressure_released;
static epd_resource_activity_t failed_activity;
static epd_memory_owner_t failed_credit;
static void *old_pins;
static bool frame_pixels_ready(const view_frame_t *f) { return f && f->complete && f->pixels.data; }
static void near_trace_finish(const char *reason) { (void)reason; }
static void frame_spare_clear(void) {}
static void frame_remember(view_frame_t *f) { (void)f; }
static void frame_drop_note(view_frame_t *f) { (void)f; }
static void image_release(void *image) { epd_app_free(image); }
static void frame_cold_clear(view_frame_t *f) { (void)f; }
static bool frame_spare_keep(view_frame_t *f) { (void)f;return false; }
static size_t frame_bytes;
static void window_uncharge_bytes(size_t bytes) { assert(!bytes); }
static void epd_app_font_page_release(void *pins)
{ if(pins==old_pins && pins){++old_pin_frees;assert(old_pin_frees==1);}epd_app_free(pins); }
static void epd_compute_buffer_free(epd_compute_buffer_t *p)
{ epd_app_free(p->allocation);memset(p,0,sizeof(*p)); }
static void epd_reader_render_shutdown(void **renderer)
{
    if(!*renderer)return;
    if(++close_polls<3)return;
    ++renderer_closes;epd_app_free(*renderer);*renderer=NULL;
}
static view_frame_t *owned_frame(view_state_t *owner)
{
    view_frame_t *f=epd_app_alloc(sizeof(*f),EPD_APP_PSRAM);assert(f);memset(f,0,sizeof(*f));
    f->owner=owner;f->references=1;++owner->references;
    f->pins=epd_app_alloc(1024,EPD_APP_PSRAM);assert(f->pins);
    f->pixels.data=f->pixels.allocation=epd_app_alloc(2048,EPD_APP_PSRAM);assert(f->pixels.data);
    f->pixels.capacity=2048;frame_bytes+=2048;return f;
}
static void cluster_retire(view_state_t *s)
{
    if(!s)return;++retired;
    frame_release(s->rendering);s->rendering=NULL;
    while(s->frames){view_frame_t *next=s->frames->next;frame_release(s->frames);s->frames=next;}
}
static void frame_prepare(void)
{
    if(scenario>=10){credit_frame_prepare();return;}
    assert(!locks && view.initialized);++renders;
    if(fixture_activity_kind(allocation_activity)==EPD_RESOURCE_INTERACTIVE) {
        if(!view.frames){view.frames=owned_frame(state);view.frames->complete=true;}
        view.frame_published=true;view.published_intent=view.intent;return;
    }
    assert(fixture_activity_kind(allocation_activity)==EPD_RESOURCE_NEARBY);
    if(!injected){
        ++attempts;injected=true;
        view.rendering=owned_frame(state);old_pins=view.rendering->pins;
        failed_activity=epd_activity_work_activity(allocation_activity);
        failed_credit=retry_host_owner(view.rendering);
        view.rendering->admission_recovery.used=true;
        view.rendering->admission_recovery.owner=failed_credit;
        view.rendering->admission_recovery.state=EPD_MEMORY_RECOVERY_DENIED;
        view.frames->admission_recovery=view.rendering->admission_recovery;
        view.frames->memory_reclaimed=view.frames->memory_waiting=view.frames->cold_reusing=true;
        view_frame_t *cold=owned_frame(state);cold->complete=true;
        frame_bytes-=cold->pixels.capacity;epd_compute_buffer_free(&cold->pixels);
        cold->admission_recovery=view.rendering->admission_recovery;
        cold->memory_reclaimed=cold->memory_waiting=cold->cold_reusing=true;
        if(scenario==1){cold->render_image=epd_app_alloc(1024,EPD_APP_SRAM);assert(cold->render_image);}
        view.frames->next=cold;
        if(scenario==7 || scenario==9){view.rendering->renderer=epd_app_alloc(128,EPD_APP_PSRAM);assert(view.rendering->renderer);render_frame=view.rendering;}
        /* Exhaust SRAM as well as consuming part of its original work grant. */
        if(scenario==1){view.rendering->render_image=epd_app_alloc(3000,EPD_APP_SRAM);assert(view.rendering->render_image);}
        retry_host_pressure();return;
    }
    assert(epd_activity_work_activity(allocation_activity)!=failed_activity);
    ++attempts;
    assert(pressure_released && old_pin_frees==1 && !view.rendering && !render_frame);
    assert(retry_host_ended(failed_credit));
    assert(view.frames->admission_recovery.used && view.frames->memory_reclaimed && view.frames->cold_reusing);
    assert(!view.frames->next->admission_recovery.used && !view.frames->next->memory_reclaimed && !view.frames->next->cold_reusing);
    assert((scenario!=7 && scenario!=9) || renderer_closes==1);
    ++completed;near_wait=false;mailbox.stop=true;
}
static void rt_sem_take(int sem,unsigned timeout)
{
    if(scenario>=10){(void)sem;(void)timeout;credit_sem_take();return;}
    (void)sem;(void)timeout;assert(!locks);++ticks;
    retry_host_poll(10);
    if(!injected){near_wait=true;return;}
    if(!pressure_released && ((scenario==8 && old_pin_frees) || (scenario==9 && mailbox.cleanup))){
        retry_host_release();pressure_released=true;
    }
    if(scenario==5 && !mailbox.paused){
        mailbox.paused=true;mailbox.command.kind=INTENT_CANCEL;mailbox.command.origin=fixture_current->frames;
        ++mailbox.command.origin->references;++mailbox.intent;view_activity_wake(NULL);
    }
    if(!failed_at && old_pin_frees && !mailbox.cleanup && !retry_host_tasks()){
        failed_at=ticks;assert(attempts==1 && fixture_current->frames);
        assert(retry_host_ended(failed_credit));
    }
    if(failed_at){
        unsigned idle=ticks-failed_at;
        if(!pressure_released && idle==25 && scenario!=2){
            if(scenario==3)retry_host_invalidate(1);
            if(scenario==4)retry_host_invalidate(2);
            retry_host_release();pressure_released=true;
        }
        if(scenario>=2 && scenario<=5 && idle==100){
            assert(!completed && attempts==1 && old_pin_frees==1 && !retry_host_tasks());
            if(scenario==5)assert(commands_applied==1);
            mailbox.stop=true;
        }
    }
    view_activity_wake(NULL);
    assert(ticks<400);
}
