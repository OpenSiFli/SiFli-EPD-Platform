static void check_trim(bool value,const char *message)
{
    if(!value){fprintf(stderr,"FAIL %s\n",message);exit(1);}
}
static void trim_page(view_page_t *p,unsigned number)
{
    memset(p,0,sizeof(*p));p->start=(document_cursor_t){number*1000u,17};
    p->cursor=(document_cursor_t){(number+1u)*1000u,17};p->done=p->collect=1;
    p->count=19;p->bytes=870;memset(p->text,'x',p->bytes);
}
static view_frame_t *trim_frame(view_state_t *owner,unsigned number)
{
    view_page_t *p=calloc(1,sizeof(*p));assert(p);trim_page(p,number);
    view_frame_t *f=frame_create((epd_reader_page_t*)p,number,1);assert(f);free(p);
    f->complete=true;f->image.data_size=690680;
    assert(epd_compute_buffer_alloc(&f->pixels,690680));frame_bytes+=f->pixels.capacity;
    memset(f->pixels.data,0x5a,f->pixels.capacity);
    f->next=owner->frames;owner->frames=f;return f;
}
static bool trim_has(view_state_t *owner,unsigned number)
{
    for(view_frame_t *f=owner->frames;f;f=f->next)if(f->number==number)return true;
    return false;
}
static void trim_case(int direction,bool preview,bool held,bool pending,bool stale)
{
    size_t capacity=psram_capacity,initial=allocated_bytes;
    view_state_t owner={0},other={0};view_page_t *workspace=calloc(3,sizeof(*workspace));assert(workspace);
    owner.building=&workspace[0];owner.display=&workspace[1];owner.prefetch=&workspace[2];
    owner.layout=7;owner.session=90;owner.intent=1;owner.direction=direction;
    owner.initialized=owner.selected=owner.status.ready=true;owner.preview=owner.preview_cache_active=preview;
    unsigned center=preview?0x40000003u:103u;owner.preview_page=owner.wanted=center;
    document_cursor_t pages[200];for(unsigned i=0;i<200;++i)pages[i]=(document_cursor_t){i*1000u,17};
    owner.pages=pages;owner.count=preview?UINT_MAX:199;owner.cache_end=UINT_MAX;
    owner.status.content_width=684;owner.status.bottom=1010;
    owner.cache=(reader_page_cache_t){.budget=VIEW_CACHE_BUDGET/3u,.reserve=VIEW_CACHE_RESERVE,
        .allocate=cache_allocate,.release=cache_free,.available=epd_app_psram_available};
    reader_page_cache_window(&owner.cache,center,0,UINT_MAX);state=primary_state=&owner;
    trim_page(owner.display,center);
    view_frame_t *held_frame=NULL;
    for(int offset=-3;offset<=3;++offset){
        unsigned n=(unsigned)(center+offset);view_frame_t *f=trim_frame(&owner,n);
        trim_page(owner.building,n);assert(reader_page_cache_put(&owner.cache,n,owner.layout,(epd_reader_page_t*)owner.building));
        if(offset==-3*direction)held_frame=f;
    }
    assert(held_frame);unsigned victim=held_frame->number;
    view_frame_t *backing=NULL;
    if(held){
        backing=epd_app_alloc(sizeof(*backing),EPD_APP_PSRAM);assert(backing);memset(backing,0,sizeof(*backing));
        backing->owner=&owner;backing->references=1;backing->backing=held_frame;
        ++owner.references;frame_ref(held_frame);
    }
    unsigned future=(unsigned)(center+4*direction);
    trim_page(owner.prefetch,future);owner.prefetch_number=future;
    if(pending){
        owner.rendering=frame_create((epd_reader_page_t*)owner.prefetch,future,1);assert(owner.rendering);
        owner.rendering->pins=malloc(sizeof(unsigned));assert(owner.rendering->pins);
        *(unsigned*)owner.rendering->pins=0x1234abcd;owner.rendering->pin_run=3;owner.rendering->pin_byte=7;
        assert(reader_page_cache_put(&owner.cache,future,owner.layout,(epd_reader_page_t*)owner.prefetch));
    }
    state=&other;other.layout=7;other.session=90;other.status.content_width=684;other.status.bottom=1010;
    trim_frame(&other,1);state=&owner;
    unsigned before_pins=released_pins,before_renders=renders;size_t original_frames=frame_bytes;
    view_frame_t *active=owner.rendering;void *pins=active?active->pins:NULL;
    if(stale)for(view_frame_t *f=owner.frames;f;f=f->next)if(f->number==center)f->layout=6;
    psram_capacity=(size_t)allocated_bytes+VIEW_CACHE_RESERVE+260;
    if(!pending)assert(!cache_store(future,owner.prefetch));
    for(unsigned step=1;step<=3;++step){
        unsigned n=(unsigned)(center+direction*(int)step);owner.preview_page=owner.wanted=n;
        reader_page_cache_window(&owner.cache,n,0,UINT_MAX);
        trim_page(owner.building,n);owner.display_pending=true;++owner.intent;
        if(pending){
            /* The final target is cached; the live frame remains within three pages. */
            active->intent=owner.intent;
        }
        frame_prepare();
        assert(renders==before_renders && owner.display_pending);
        layout_accept();
        check_trim(!trim_has(&owner,victim),"CACHED_TURN_STALE_FRAME");
        assert(owner.display->start.record==n*1000u && !owner.display_pending);
        for(view_frame_t *f=owner.frames;f;f=f->next){
            unsigned d=f->number>n?f->number-n:n-f->number;
            assert(d<=3 && f->layout==owner.layout);
        }
        for(int offset=-3;offset<=3;++offset){
            unsigned original=(unsigned)(center+offset);
            unsigned d=original>n?original-n:n-original;
            assert(trim_has(&owner,original)==(d<=3 && !(stale && original==center)));
        }
        assert(trim_has(&owner,n) && other.frames && other.references==1);
        if(held && step==1){
            assert(held_frame->references==1 && held_frame->pixels.data && *(uint8_t*)held_frame->pixels.data==0x5a);
            assert(backing->backing==held_frame);
            assert(frame_bytes==original_frames);
            frame_release(backing);backing=NULL;
        }
        if(pending)assert(owner.rendering==active && active->pins==pins && active->pin_run==3 && active->pin_byte==7 && released_pins==before_pins);
        check_trim(cache_store(future,owner.prefetch)!=NULL,"NEAR_LAYOUT_STILL_BLOCKED");
    }
    if(pending){
        owner.rendering=NULL;frame_release(active);assert(released_pins==before_pins+1);
    }
    psram_capacity=capacity;
    frame_cache_clear();reader_page_cache_clear(&owner.cache);assert(!owner.references);
    state=&other;frame_cache_clear();assert(!other.references);
    free(workspace);state=primary_state=NULL;
    assert(!frame_bytes && !layout_bytes && allocated_bytes==initial);
    printf("PASS trim direction=%d preview=%u held=%u pending=%u stale=%u\n",direction,preview,held,pending,stale);
}
static void trim_cases(void)
{
    for(int direction=-1;direction<=1;direction+=2)for(unsigned preview=0;preview<2;++preview){
        trim_case(direction,preview!=0,false,false,false);
        trim_case(direction,preview!=0,true,false,false);
        trim_case(direction,preview!=0,false,true,false);
        trim_case(direction,preview!=0,false,false,true);
    }
    for(unsigned deferred=0;deferred<2;++deferred){
        size_t initial=allocated_bytes;unsigned before_disposed=disposed,before_pins=released_pins;
        view_state_t *owner=epd_app_alloc(sizeof(*owner),EPD_APP_PSRAM);assert(owner);memset(owner,0,sizeof(*owner));
        owner->layout=1;owner->session=91;owner->wanted=owner->preview_page=104;
        owner->preview=owner->preview_cache_active=true;
        owner->status.content_width=684;owner->status.bottom=1010;
        owner->building=epd_app_alloc(sizeof(view_page_t),EPD_APP_PSRAM);
        owner->display=epd_app_alloc(sizeof(view_page_t),EPD_APP_PSRAM);assert(owner->building && owner->display);
        trim_page(owner->building,104);trim_page(owner->display,103);
        owner->cache=(reader_page_cache_t){.budget=VIEW_CACHE_BUDGET/3u,.reserve=VIEW_CACHE_RESERVE,
            .allocate=cache_allocate,.release=cache_free,.available=epd_app_psram_available};
        reader_page_cache_window(&owner->cache,104,0,UINT_MAX);state=primary_state=owner;
        view_frame_t *old=trim_frame(owner,100);frame_ref(old);
        view_frame_t *bound=epd_app_alloc(sizeof(*bound),EPD_APP_PSRAM);assert(bound);memset(bound,0,sizeof(*bound));
        bound->owner=owner;bound->references=1;bound->backing=old;++owner->references;
        trim_frame(owner,104);
        view_frame_t *active=NULL;
        if(deferred){
            active=frame_create((epd_reader_page_t*)owner->building,105,1);assert(active);
            active->renderer=(void*)1;active->pins=malloc(sizeof(unsigned));assert(active->pins);
            *(unsigned*)active->pins=0x1234abcd;owner->rendering=active;
        }
        layout_accept();assert(!trim_has(owner,100) && old->references==1);
        state_retire(owner);state=primary_state=NULL;collect();
        assert(disposed==before_disposed && mailbox.retired==owner && old->pixels.data);
        frame_release(bound);collect();
        if(deferred){
            assert(disposed==before_disposed && mailbox.cleanup==active && owner->references==1 && released_pins==before_pins);
            mailbox.cleanup=NULL;active->renderer=NULL;active->references=1;frame_release(active);collect();
            assert(released_pins==before_pins+1);
        }
        assert(disposed==before_disposed+1 && !mailbox.retired && !mailbox.cleanup && !frame_bytes && !layout_bytes);
        assert(allocated_bytes==initial);
        printf("PASS trimmed backing retains retired owner through UI/deferred-render release deferred=%u\n",deferred);
    }
    puts("PASS 18/18 cached-turn trim cases; real cache allocation/release and frame ownership; zero leaked charges");
}
