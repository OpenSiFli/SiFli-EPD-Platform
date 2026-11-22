/* BODY and NAV streams use the production document worker and cache ownership. */
typedef struct { uint8_t *data; size_t size; } integration_stream_t;
static integration_stream_t integration_body[20], integration_nav[20];

static void integration_emit(unsigned type,unsigned a,unsigned b,unsigned c,unsigned d,const char *one,const char *two)
{
    book_document_event_t e={0};e.type=type;e.a=a;e.b=b;e.c=c;e.d=d;
    e.length1=(uint32_t)strlen(one);e.length2=(uint32_t)strlen(two);
    e.bytes=(sizeof(e)+e.length1+e.length2+3)&~3u;
    wire_data=realloc(wire_data,wire_bytes+e.bytes);assert(wire_data);
    uint8_t *p=wire_data+wire_bytes;memset(p,0,e.bytes);
    memcpy(p,&e,sizeof(e));memcpy(p+sizeof(e),one,e.length1);memcpy(p+sizeof(e)+e.length1,two,e.length2);
    wire_bytes+=e.bytes;
}

static integration_stream_t integration_take(void)
{ integration_stream_t result={wire_data,wire_bytes};wire_data=NULL;wire_bytes=0;return result; }

static void integration_make_wires(void)
{
    char href[64],title[64],text[601];memset(text,'x',600);text[600]=0;
    for(unsigned chapter=0;chapter<20;++chapter){
        snprintf(href,sizeof(href),"p%u.xhtml",chapter);
        integration_emit(BOOK_DOCUMENT_CHAPTER,chapter,20,1,BOOK_FORMAT_EPUB,href,href);
        integration_emit(BOOK_DOCUMENT_SOURCE_ANCHOR,chapter,0,0,
            BOOK_DOCUMENT_SOURCE_BODY_UTF8|BOOK_DOCUMENT_SOURCE_BEGIN|BOOK_DOCUMENT_SOURCE_TARGET,href,"");
        integration_emit(BOOK_DOCUMENT_ANCHOR,0,0,0,0,href,"");
        snprintf(text,15,"chapter-%06u",chapter);text[14]='x';
        integration_emit(BOOK_DOCUMENT_TEXT,0,0,0,0,text,"");
        strcat(href,"#middle");
        integration_emit(BOOK_DOCUMENT_ANCHOR,2,0,0,0,href,"");
        integration_emit(BOOK_DOCUMENT_SOURCE_ANCHOR,chapter,600,0,BOOK_DOCUMENT_SOURCE_BODY_UTF8,href,"");
        integration_emit(BOOK_DOCUMENT_TEXT,0,0,0,0,text,"");
        integration_emit(BOOK_DOCUMENT_SOURCE_ANCHOR,chapter,1200,0,
            BOOK_DOCUMENT_SOURCE_BODY_UTF8|BOOK_DOCUMENT_SOURCE_END,href,"");
        integration_body[chapter]=integration_take();
        for(unsigned part=0;part<20;++part){
            if(part==chapter)continue;
            snprintf(href,sizeof(href),"p%u.xhtml",part);
            integration_emit(BOOK_DOCUMENT_CHAPTER,part,20,0,BOOK_FORMAT_EPUB,href,href);
        }
        for(unsigned entry=0;entry<40;++entry){
            snprintf(href,sizeof(href),"p%u.xhtml%s",entry/2,entry%2?"#middle":"");
            snprintf(title,sizeof(title),"Chapter %u",entry/2);
            integration_emit(BOOK_DOCUMENT_METADATA,BOOK_DOCUMENT_META_TOC,entry/2,entry%2?2:1,20,href,title);
        }
        integration_nav[chapter]=integration_take();
    }
}

static void integration_wire(unsigned chapter,bool navigation,const uint8_t **data,size_t *length)
{
    assert(chapter<20);
    integration_stream_t *s=navigation?&integration_nav[chapter]:&integration_body[chapter];
    *data=s->data;*length=s->size;assert(*length);
}

static void integration_tick(void)
{ document_worker_step(&service);document_service_process();collect(); }

static void integration_pump(view_state_t *owner)
{
    for(unsigned i=0;i<5000;++i){
        state=owner;neighbor_step();integration_tick();
        if(owner->status.ready && owner->frames && !owner->target_pending && !owner->display_pending)break;
        if(i==4999)fprintf(stderr,"PUMP chapter=%u ready=%u target=%u display=%u frame=%p result=%d complete=%u failure=%u\n",
            owner->progress.chapter,owner->status.ready,owner->target_pending,owner->display_pending,
            (void*)owner->frames,owner->document->status.result,owner->document->status.complete,owner->failure);
        assert(i<4999 && !owner->failure);
    }
    document_progress(owner->document,&owner->progress);
    assert(owner->frames->complete && owner->display->bytes>=600 &&
        (owner->display->eof || (owner->preview_clipped && owner->progress.complete &&
         owner->display->cursor.record==owner->progress.end)));
}

static void integration_catalog(view_state_t *owner)
{
    document_t *d=owner->document;unsigned reads=io_reads,opens=io_opens;
    assert(d->toc_context && document_toc_count(d)==40);
    char title[64],href[64],wanted[64];unsigned index=UINT_MAX,level=0;
    for(unsigned i=0;i<40;++i){
        assert(document_toc_entry(d,i,title,sizeof(title),href,sizeof(href),&level));
        snprintf(wanted,sizeof(wanted),"p%u.xhtml%s",i/2,i%2?"#middle":"");
        assert(!strcmp(href,wanted) && level==(i%2?2u:1u));
    }
    snprintf(href,sizeof(href),"p%u.xhtml",d->chapter);uint32_t root,middle;
    assert(document_anchor_find(d,href,&root));strcat(href,"#middle");
    assert(document_anchor_find(d,href,&middle) && middle>root);
    assert(document_current_title(d,root,title,sizeof(title),&index) && index==2*d->chapter);
    assert(document_current_title(d,middle,title,sizeof(title),&index) && index==2*d->chapter+1);
    assert(reads==io_reads && opens==io_opens);
}

static view_state_t *integration_start(void)
{
    view_state_t seed={0};seed.session=seed.intent=seed.layout=1;state=&seed;
    bookshelf_item_t file={0};strcpy(file.path,"/flash/toc-window.epub");
    file.size=100000;file.modified=1;file.format=BOOK_FORMAT_EPUB;
    document_anchor_t anchor={BOOK_DOCUMENT_SOURCE_BODY_UTF8,8,0,0};char error[96];
    primary_state=state_neighbor(document_open_at(&file,&anchor,error,sizeof(error)),false);
    integration_pump(primary_state);state=primary_state;
    primary_state->selected=primary_state->displayed=true;primary_state->direction=1;
    return primary_state;
}

static void integration_drain(void)
{
    unsigned idle=0;
    for(unsigned i=0;i<1000;++i){
        bool work=document_worker_step(&service);collect();
        idle=!work && !mailbox.retired?idle+1:0;if(idle==2)break;
        assert(i<999);
    }
}

static void integration_close(void)
{
    if(primary_state){primary_state->displayed=primary_state->selected=false;cluster_retire(primary_state);}
    state=primary_state=NULL;integration_drain();
    if(documents || allocated_bytes || context_caches)fprintf(stderr,
        "CLOSE docs=%p retired=%p cleanup=%p parked=%p jobs=%u frame=%zu layout=%zu index=%zu bytes=%lld fd=%u requests=%p caches=%u created=%u disposed=%u\n",
        (void*)documents,(void*)mailbox.retired,(void*)mailbox.cleanup,(void*)parked_state,flow_jobs,
        frame_bytes,layout_bytes,index_bytes,allocated_bytes,fd_live,(void*)record_requests,context_caches,created,disposed);
    assert(!documents && !mailbox.retired && !mailbox.cleanup && !parked_state && !flow_jobs &&
        !frame_bytes && !layout_bytes && !index_bytes && !allocated_bytes && !fd_live && !record_requests && !context_caches);
    assert(created==disposed && pipe_opens==pipe_closes && !observers);
}
