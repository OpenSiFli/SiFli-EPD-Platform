static void combo_case(void)
{
    view_state_t *root = integration_start();
    document_t *document = root->document;
    assert(!document_toc_count(document));
    document_target_require(document, true);
    for (unsigned i=0; !document->status.complete; ++i) { assert(i<1000); integration_tick(); }
    document_target_require(document, false);
    state = root;
    root->following = state_neighbor(document_adjacent(document,1),false);
    integration_pump(root->following); state = root;
    view_state_t *neighbor = root->following;
    /* This off-screen chapter has no page in the current global near window. */
    neighbor->display->eof = false;
    reader_cached_page_t *entry = reader_page_cache_find(&neighbor->cache, window_number(neighbor), neighbor->layout);
    assert(entry); entry->eof = false;
    state=neighbor; assert(near_frames_pending()); state=root;
    root->displayed = false;
    frame_cache_clear(); reader_page_cache_clear(&root->cache);
    root->preview = root->preview_cache_active = true;
    root->wanted = root->preview_page = 103; root->cache_end = UINT_MAX;
    reader_page_cache_window(&root->cache,103,0,UINT_MAX);
    trim_page(root->display,103); root->display_layout = root->layout;
    root->status.ready = true; root->display_pending = false;
    view_frame_t *old = NULL;
    for (unsigned n=100;n<=106;++n) {
        view_frame_t *frame=trim_frame(root,n);
        trim_page(root->building,n);
        assert(reader_page_cache_put(&root->cache,n,root->layout,(epd_reader_page_t*)root->building));
        if(n==100)old=frame;
    }
    view_frame_t *backing=epd_app_alloc(sizeof(*backing),EPD_APP_PSRAM);assert(backing);
    memset(backing,0,sizeof(*backing));backing->owner=root;backing->references=1;backing->backing=old;
    ++root->references;frame_ref(old);
    trim_page(root->prefetch,107);root->prefetch_number=107;
    view_frame_t *active=frame_create((epd_reader_page_t*)root->prefetch,107,1);assert(active);
    active->pins=malloc(sizeof(unsigned));assert(active->pins);*(unsigned*)active->pins=0x1234abcd;
    active->pin_run=3;active->pin_byte=7;root->rendering=active;
    assert(reader_page_cache_put(&root->cache,107,root->layout,(epd_reader_page_t*)root->prefetch));
    unsigned pin_count=released_pins,render_count=renders;
    for(unsigned n=104;n<=106;++n) {
        root->wanted=root->preview_page=n;++root->intent;active->intent=root->intent;
        reader_page_cache_window(&root->cache,n,0,UINT_MAX);
        trim_page(root->building,n);root->display_pending=true;
        frame_prepare();layout_accept();
        check_trim(!trim_has(root,100),"CACHED_TURN_STALE_FRAME");
        assert(root->rendering==active && active->pins && active->pin_run==3 && active->pin_byte==7);
        assert(old->pixels.data && old->references==1 && released_pins==pin_count && renders==render_count);
    }
    /* Finishing transport releases pins while the old UI backing still owns
     * its pixels and document. */
    root->rendering=NULL;active->renderer=(void*)1;frame_release(active);
    assert(mailbox.cleanup==active && released_pins==pin_count);
    mailbox.cleanup=NULL;active->renderer=NULL;active->references=1;frame_release(active);
    assert(released_pins==pin_count+1 && old->pixels.data);
    for(unsigned n=107;n<=109;++n) {
        trim_page(root->building,n);trim_frame(root,n);
        assert(reader_page_cache_put(&root->cache,n,root->layout,(epd_reader_page_t*)root->building));
    }
    state=root;assert(!near_frames_pending());
    unsigned turn=0;
    for(unsigned i=0;!document_navigation_ready(document);++i) {
        if(i==599)fprintf(stderr,"FLOW current=%u owner=%u prefetch=%u:%u nav=%u cursor=%u complete=%u render=%p:%p near=%u windows=%u body=%u:%u\n",
            document->serial,flow_owner()?flow_owner()->serial:0,document->prefetch_enabled,neighbor->document->prefetch_enabled,
            document->nav_state,document->toc_context->cursor,document->toc_context->complete,
            (void*)render_frame,(void*)neighbor->rendering,near_frames_pending(),window_runnable(neighbor,root),
            document->status.complete,neighbor->document->status.complete);
        if(i==599) {
            fprintf(stderr,"NAV allowed=%u candidate=%p converter=%p note=%p restore=%u cachebad=%u owners=%u claimed=%u deferred=%u result=%d suspended=%u\n",
                navigation_allowed(document),(void*)navigation_candidate(),(void*)conversion_document,(void*)note_conversion_document,
                document->chapter_restore,document->cache_bad,document->owners,document->claimed,document->nav_deferred,document->status.result,epd_reader_suspended());
            for(document_record_request_t *r=record_requests;r;r=r->next)fprintf(stderr,"REQ doc=%u foreground=%u released=%u done=%u\n",r->document->serial,r->foreground,r->released,r->done);
            if(conversion_document)fprintf(stderr,"SLOT serial=%u nav=%u owners=%u pref=%u body_complete=%u wanted=%u shared=%u\n",
                conversion_document->serial,conversion_document->nav_state,conversion_document->owners,
                conversion_document->prefetch_enabled,conversion_document->status.complete,conversion_wanted(conversion_document),
                conversion_document->toc_context==document->toc_context);
        }
        assert(i<600 && "TRIM_NAV_BACKGROUND_LIVENESS");
        state=root;document_flow();window_service(root,&turn);integration_tick();
    }
    assert(!document->nav_requested && document_toc_count(document)==40 && old->pixels.data);
    integration_catalog(root);
    frame_release(backing);
    assert(frame_bytes<=VIEW_FRAME_BUDGET && layout_bytes<=VIEW_CACHE_BUDGET);
    integration_close();
    puts("PASS trim+NAV: three cached turns, stale frames retired, UI backing/pins retained to completion, no-menu title/TOC ready, all owners released");
}
