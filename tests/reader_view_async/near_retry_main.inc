int main(int argc,char **argv)
{
    assert(argc==2);
    const char *cases[]={"recover","sram","steady","generation","close","cancel","font","raster","early_font","early_raster","credit","credit_deadline"};
    for(scenario=0;scenario<12 && strcmp(argv[1],cases[scenario]);++scenario){}
    assert(scenario<12);retry_host_init(scenario==1);
    view_state_t current={0};view_page_t display={0};
    current.display=&display;current.complete=current.status.ready=true;
    current.session=7;mailbox.session=7;mailbox.intent=10;mailbox.wake=1;mailbox.stopped=2;
    mailbox.incoming=&current;mailbox.replace=true;fixture_current=&current;
    mailbox.activity=view_activity_new();view_worker(NULL);
    assert(stopped && !locks && !frame_bytes && !current.references && old_pin_frees==1);
    if(scenario>=10)assert(completed==1&&credit_slices==2&&!credit_payload);
    else if(scenario==0 || scenario==1 || scenario>=6)assert(completed==1 && attempts==2);
    else assert(!completed && attempts==1);
    retry_host_finish();
    printf("PASS production worker/core %s attempts=%u completed=%u old_pins=%u renderer_close=%u wakes=%u (%u ticks/s)\n",
        argv[1],attempts,completed,old_pin_frees,renderer_closes,wake_releases,RT_TICK_PER_SECOND);
    return 0;
}
