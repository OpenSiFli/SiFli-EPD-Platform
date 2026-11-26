/* Three independent views: HCPU cache, physical RAM, ACPU cache. */
#define COMPUTE_REAL_COMPONENTS
#define COMPUTE_CACHE_MODEL
#define main unused_scheduler_main
#include "../compute_scheduler/host.c"
#undef main
#include "epd_reader_render.h"
#include "render_wire.h"
#include "epd_compute_memory.h"
#include "epd_pcm.h"

extern const epd_compute_ops_t epd_pcm_remote_ops;
const epd_compute_registration_t epd_compute_capabilities[] = {
    {EPD_READER_RENDER_COMPONENT, EPD_READER_RENDER_VERSION, 7, 3, RR_ROWS_PER_STEP},
    {EPD_PCM_CAPABILITY, EPD_PCM_VERSION, 7, 3, 1024}
};
const uint32_t epd_compute_capability_count = 2;
const epd_compute_remote_registration_t epd_compute_remote_capabilities[] = {
    {{EPD_READER_RENDER_COMPONENT, EPD_READER_RENDER_VERSION, 7, 3, RR_ROWS_PER_STEP}, &epd_reader_render_remote_ops},
    {{EPD_PCM_CAPABILITY, EPD_PCM_VERSION, 7, 3, 1024}, &epd_pcm_remote_ops}
};
const uint32_t epd_compute_remote_capability_count = 2;
static unsigned remote_allocs;
void *epd_compute_malloc(size_t n) { void *p=calloc(1,n); assert(p); ++remote_allocs; return p; }
void epd_compute_free(void *p) { if(p) { assert(remote_allocs); --remote_allocs; free(p); } }
void epd_compute_memory_stats_get(epd_compute_memory_stats_t *s) { memset(s,0,sizeof(*s)); }

typedef struct {
    uint8_t *live, *h, *a, *ram;
    size_t size, begin, end;
    bool selected;
    uint64_t sync;
} cache_t;
static cache_t maps[32];
static bool acpu, reject_close;
static unsigned cancel_after, signal_count;
static LONG window_calls;
void real_rr_sync_windows(rr_frame, const rr_command *, uint32_t, rr_window *, rr_window *);
void rr_sync_windows(rr_frame f, const rr_command *c, uint32_t row, rr_window *pixels, rr_window *source)
{
    size_t bytes;
    /* The production client and remote must reject metadata before this point. */
    assert(rr_validate_command(c,&bytes)==RR_OK);
    InterlockedIncrement(&window_calls);
    real_rr_sync_windows(f,c,row,pixels,source);
}
uint32_t real_compute_signals(const epd_compute_wire_t *);
uint32_t epd_compute_signals(const epd_compute_wire_t *w)
{
    if(cancel_after && ++signal_count==cancel_after) epd_compute_cancel(running);
    return real_compute_signals(w);
}
void model_alloc(void *p,size_t n,bool shared)
{
    if(!shared)return;
    for(unsigned i=0;i<32;++i) if(!maps[i].live) {
        cache_t *m=maps+i; memset(m,0,sizeof(*m)); m->live=p; m->size=n;
        m->h=malloc(n); m->a=malloc(n); m->ram=malloc(n); assert(m->h&&m->a&&m->ram);
        memset(m->live,0xa5,n); memset(m->a,0x37,n); memset(m->ram,0x69,n); return;
    }
    assert(0);
}
void model_free(void *p)
{
    for(unsigned i=0;i<32;++i)if(maps[i].live==p) {
        cache_t *m=maps+i; assert(!acpu); free(m->h); free(m->a); free(m->ram); memset(m,0,sizeof(*m));
    }
}
static cache_t *mapping(uintptr_t p,size_t n)
{
    for(unsigned i=0;i<32;++i) {
        cache_t *m=maps+i; uintptr_t base=(uintptr_t)m->live;
        if(m->live && p>=base && p-base<m->size) { assert(n<=m->size-(p-base)); return m; }
    }
    assert(0); return NULL;
}
void model_cache(uint32_t p,uint32_t n,bool clean)
{
    cache_t *m=mapping(p,n); size_t off=p-(uintptr_t)m->live;
    assert(!(p&31) && !(n&31)); m->sync+=n;
    if(clean)memcpy(m->ram+off,m->live+off,n);
    else memcpy(m->live+off,m->ram+off,n);
}
bool model_transport_fail(epd_compute_wire_t *w)
{ return reject_close && w->operation==EPD_COMPUTE_CLOSE; }
void model_switch(epd_compute_wire_t *w,bool enter)
{
    const epd_compute_region_t *regions=(void *)(uintptr_t)w->regions;
    if(enter) {
        assert(!acpu);
        for(unsigned r=0;r<w->region_count;++r) {
            cache_t *m=mapping(regions[r].address,regions[r].capacity); assert(!m->selected);
            m->selected=true; m->begin=regions[r].address-(uintptr_t)m->live; m->end=m->begin+regions[r].capacity;
            memcpy(m->h,m->live,m->size); memcpy(m->live,m->a,m->size);
        }
        acpu=true;
    } else {
        assert(acpu);
        for(unsigned i=0;i<32;++i)if(maps[i].selected) {
            cache_t *m=maps+i;
            /* Any write beyond the advertised cache envelope is a failure. */
            assert(!memcmp(m->live,m->a,m->begin));
            assert(!memcmp(m->live+m->end,m->a+m->end,m->size-m->end));
            memcpy(m->a,m->live,m->size); memcpy(m->live,m->h,m->size); m->selected=false;
        }
        acpu=false;
    }
}
static int command(epd_reader_render_t c,rr_command *q,const epd_compute_buffer_t *src,uint32_t n)
{ return epd_reader_render_command_shared(c,q,src,n,NULL,NULL,NULL); }
static void plain(epd_reader_render_t c,rr_frame f,unsigned op)
{ rr_command q; rr_command_init(&q,op,f); assert(command(c,&q,NULL,0)==0); }
static void pixels_equal(const epd_compute_buffer_t *frame,const void *expected,size_t n)
{ assert(!memcmp(frame->data,expected,n)); }
static void cache_cases(void)
{
    rr_frame f={800,1000,1600, RR_FORMAT_RGB565}; epd_compute_buffer_t frame, source;
    assert(epd_compute_buffer_alloc(&frame,f.stride*f.height));
    assert(epd_compute_buffer_alloc(&source,f.stride*f.height));
    cache_t *fm=mapping((uintptr_t)frame.data,frame.capacity), *sm=mapping((uintptr_t)source.data,source.capacity);
    epd_reader_render_t client; assert(epd_reader_render_open(&frame,f,EPD_COMPUTE_BACKGROUND,&client)==0);
    uint8_t *reference=malloc(frame.capacity); assert(reference); memset(reference,255,frame.capacity);
    plain(client,f,RR_INIT); pixels_equal(&frame,reference,frame.capacity);
    epd_pcm_t audio; assert(epd_pcm_open((epd_pcm_format_t){16,1},EPD_COMPUTE_CONTINUOUS,&audio)==0);
    rr_command q; uint64_t initial=fm->sync; unsigned before=calls;
    memset(source.data,255,32*32);
    for(unsigned i=0;i<200;++i) {
        rr_command_init(&q,RR_BLIT_A8,f); q.width=q.src_width=32; q.height=q.src_height=32; q.src_stride=32;
        q.x=(int32_t)((i*37)%768); q.y=(int32_t)((i*29)%968);
        assert(command(client,&q,&source,1024)==0);
        for(unsigned y=0;y<32;++y)memset(reference+(q.y+y)*f.stride+q.x*2,0,64);
        if(!(i%17)) {
            int16_t in[32],out[32]; for(unsigned n=0;n<32;++n)in[n]=(int16_t)(n*17);
            assert(epd_pcm_convert(audio,in,32,out,NULL,NULL)==0 && !memcmp(in,out,sizeof(in)));
        }
    }
    pixels_equal(&frame,reference,frame.capacity);
    uint64_t old=(uint64_t)200*frame.capacity*5, now=fm->sync-initial;
    assert(now<old/20);
    printf("BYTES glyphs=200 frame=%llu legacy=%llu ratio=%.6f transport_calls=%u\n",now,old,(double)now/(double)old,calls-before);
    for(unsigned i=0;i<source.capacity;++i)((uint8_t *)source.data)[i]=(uint8_t)(i*37+11);
    initial=fm->sync; uint64_t source_before=sm->sync;
    rr_command_init(&q,RR_BLIT_RGB565,f); q.src_width=f.width; q.src_height=f.height; q.src_stride=f.stride;
    assert(command(client,&q,&source,source.capacity)==0);
    pixels_equal(&frame,source.data,frame.capacity);
    assert(fm->sync-initial==(uint64_t)frame.capacity*5 && sm->sync-source_before==(uint64_t)source.capacity*3);
    printf("BYTES rgb565 frame=%llu source=%llu legacy_frame=%llu legacy_source=%llu\n",fm->sync-initial,sm->sync-source_before,
           (uint64_t)frame.capacity*5*32,(uint64_t)source.capacity*3*32);
    rr_command_init(&q,RR_SCALE_GRAY4_TO_RGB565,f);
    q.src_width=801; q.src_height=1001; q.flags=RR_GRAY4_CONTIGUOUS;
    source_before=sm->sync;
    assert(command(client,&q,&source,(801*1001+1)/2)==0);
    for(unsigned y=0;y<f.height;++y)for(unsigned x=0;x<f.width;++x) {
        unsigned sx=x*801/f.width,sy=y*1001/f.height,bit=sy*801+sx;
        unsigned gray=(((uint8_t *)source.data)[bit/2]>>(bit&1?0:4)&15)*17;
        uint16_t color=(uint16_t)((gray/8)*2048+(gray/4)*32+gray/8);
        assert(((uint16_t *)frame.data)[y*f.width+x]==color);
    }
    assert(sm->sync-source_before<3ull*source.capacity);
    printf("BYTES gray4 scaled source=%llu legacy_source=%llu\n",sm->sync-source_before,3ull*((801*1001+1+63)/64*32)*32);
    initial=fm->sync;
    q.x=INT32_MAX; assert(command(client,&q,&source,(801*1001+1)/2)==0 && fm->sync==initial);
    epd_compute_buffer_t alias=frame; alias.data=(uint8_t *)frame.data+frame.capacity-32; alias.capacity=32;
    rr_command_init(&q,RR_BLIT_A8,f); q.width=q.src_width=q.src_stride=1; q.height=q.src_height=1;
    before=calls; assert(command(client,&q,&alias,1)==EPD_COMPUTE_INVALID && calls==before);
    plain(client,f,RR_FINISH); assert(fm->sync==initial);
    memcpy(reference,frame.data,frame.capacity);
    reject_close=true; assert(epd_reader_render_shutdown(&client)==EPD_COMPUTE_UNAVAILABLE && client);
    pixels_equal(&frame,reference,frame.capacity); reject_close=false;
    assert(epd_reader_render_shutdown(&client)==0 && !client);
    assert(epd_pcm_close(audio)==0);
    free(reference); epd_compute_buffer_free(&source); epd_compute_buffer_free(&frame);
}
static void partial_and_interleave(void)
{
    rr_frame f={13,70,28, RR_FORMAT_RGB565}; epd_compute_buffer_t frame[2]; epd_reader_render_t c[2];
    for(unsigned i=0;i<2;++i) {
        assert(epd_compute_buffer_alloc(frame+i,1960));
        assert(epd_reader_render_open(frame+i,f,EPD_COMPUTE_FOREGROUND,c+i)==0);
        plain(c[i],f,RR_INIT);
        for(unsigned j=1960;j<frame[i].capacity;++j)assert(((uint8_t *)frame[i].data)[j]==0xa5);
    }
    rr_command q; rr_command_init(&q,RR_FILLRECT,f); q.fill_rgb565=0;
    signal_count=0; cancel_after=4;
    assert(command(c[0],&q,NULL,0)==EPD_COMPUTE_CANCELLED); cancel_after=0;
    for(unsigned y=0;y<70;++y)for(unsigned x=0;x<13;++x)
        assert(((uint16_t *)frame[0].data)[y*14+x]==(y<3?0:65535));
    assert(command(c[1],&q,NULL,0)==0); plain(c[1],f,RR_FINISH);
    for(unsigned i=0;i<2;++i) { assert(epd_reader_render_shutdown(c+i)==0); epd_compute_buffer_free(frame+i); }
    puts("PASS cache ownership: partial row cancellation, padding/tail lines, two sessions and CLOSE failure");
}

static void sampled_rows_cases(void)
{
    rr_frame f={13,70,28, RR_FORMAT_RGB565}; epd_compute_buffer_t frame={0},src={0}; epd_reader_render_t c;
    assert(epd_compute_buffer_alloc(&frame,1960) && epd_compute_buffer_alloc(&src,8192));
    assert(epd_reader_render_open(&frame,f,EPD_COMPUTE_BACKGROUND,&c)==0);
    for(unsigned mode=0;mode<4;++mode) {
        plain(c,f,RR_INIT);
        for(unsigned i=0;i<src.capacity;++i)((uint8_t *)src.data)[i]=mode==2?(i%3?255:0):(uint8_t)(i*73+mode*19);
        rr_command q; rr_command_init(&q,mode<2?RR_SCALE_GRAY4_TO_RGB565:mode==2?RR_BLIT_A8:RR_BLIT_RGB565,f);
        q.x=-3; q.y=-19; q.width=mode<2?21:17; q.height=100;
        q.clip_x=2; q.clip_y=3; q.clip_w=9; q.clip_h=65;
        q.src_width=17; q.src_height=137;
        q.flags=mode==0?RR_GRAY4_CONTIGUOUS:0; q.src_stride=mode==0?0:mode==1?14:mode==2?19:39;
        size_t bytes; assert(rr_source_bytes(&q,&bytes)==0);
        rr_result result;
        int status=epd_reader_render_command_shared(c,&q,&src,(uint32_t)bytes,NULL,NULL,&result);
        if(status)fprintf(stderr,"sampled mode=%u status=%d kernel=%d rows=%u/%u\n",mode,status,result.status,result.rows_done,result.rows_total);
        assert(status==0);
        for(unsigned y=0;y<70;++y)for(unsigned x=0;x<14;++x) {
            uint16_t color=65535;
            if(y>=3 && y<68 && x>=2 && x<11) {
                unsigned sx=x+3,sy=y+19;
                if(mode<2) {
                    sx=sx*17/21; sy=sy*137/100;
                    unsigned bit=mode==0?sy*17+sx:sy*14*2+sx;
                    unsigned gray=(((uint8_t *)src.data)[bit/2]>>(bit&1?0:4)&15)*17;
                    color=(uint16_t)((gray/8)*2048+(gray/4)*32+gray/8);
                } else if(mode==2)color=((uint8_t *)src.data)[sy*19+sx]?0:65535;
                else { uint8_t *p=(uint8_t *)src.data+sy*39+sx*2; color=(uint16_t)(p[0]+256u*p[1]); }
            }
            assert(((uint16_t *)frame.data)[y*14+x]==color);
        }
        for(unsigned i=1960;i<frame.capacity;++i)assert(((uint8_t *)frame.data)[i]==0xa5);
    }
    assert(epd_reader_render_shutdown(&c)==0);
    epd_compute_buffer_free(&src); epd_compute_buffer_free(&frame);
    puts("PASS sampled source rows: negative destination, clipped batches, contiguous odd gray4, padded gray4/A8/RGB565");
}

static void monochrome_glyph_cases(void)
{
    const rr_frame format = {6, 4, 8, RR_FORMAT_L8};
    const uint8_t coverage[8] = {255, 0, 127, 128, 255, 255, 255, 255};
    const uint32_t flags[] = {RR_A8_MONO, RR_A8_MONO | RR_A8_WHITE_INK, 0,
                              RR_A8_MONO | RR_A8_WHITE_BG};
    epd_compute_buffer_t frame;
    epd_reader_render_t client;
    assert(epd_compute_buffer_alloc(&frame, format.stride * format.height));
    assert(epd_reader_render_open(&frame, format, EPD_COMPUTE_FOREGROUND, &client) == 0);
    plain(client, format, RR_INIT);
    rr_command command;
    rr_command_init(&command, RR_FILLRECT, format);
    command.fill_rgb565 = 0x8410;
    assert(epd_reader_render_command(client, &command, NULL, 0, NULL, NULL, NULL) == 0);
    const uint8_t *pixels = frame.data;
    const uint8_t gray = pixels[0];
    assert(gray > 0 && gray < 255);

    for (uint32_t y = 0; y < format.height; ++y) {
        rr_command_init(&command, RR_BLIT_A8, format);
        command.x = -1;
        command.y = (int32_t)y;
        command.width = command.src_width = 5;
        command.height = command.src_height = 1;
        command.src_stride = sizeof(coverage);
        command.flags = flags[y];
        assert(epd_reader_render_a8_enqueue(client, &command, coverage, sizeof(coverage)) == 0);
    }
    assert(epd_reader_render_a8_flush(client, NULL, NULL) == 0);
    assert(pixels[0] == gray && pixels[1] == gray && pixels[2] == 0 && pixels[3] == 0);
    assert(pixels[8] == gray && pixels[9] == gray && pixels[10] == 255 && pixels[11] == 255);
    assert(pixels[17] > 0 && pixels[17] < gray && pixels[18] > 0 && pixels[18] < gray);
    assert(pixels[24] == 255 && pixels[25] == 255 && pixels[26] == 0 && pixels[27] == 0);
    for (uint32_t y = 0; y < format.height; ++y) {
        const uint8_t *row = pixels + y * format.stride;
        assert(row[4] == gray && row[5] == gray && row[6] == 255 && row[7] == 255);
    }
    assert(epd_reader_render_shutdown(&client) == 0);
    epd_compute_buffer_free(&frame);
    puts("PASS monochrome glyph batch: threshold, clipping, background and inverse ink");
}

static void bad_command(rr_command *q,rr_frame f,unsigned fault)
{
    rr_command_init(q,RR_BLIT_A8,f); q->id=1;
    q->width=q->src_width=q->src_stride=8; q->height=q->src_height=1;
    switch(fault) {
    case 0: q->height=64; break; /* First batch already exceeds the source. */
    case 1: q->width=9; break;
    case 2: q->height=RR_MAX_DIM+1; break;
    case 3: q->width=UINT32_MAX; break;
    case 4: q->clip_h=UINT32_MAX; break;
    case 5: q->clip_w=RR_MAX_DIM+1; break;
    case 6: q->flags=UINT32_MAX; break;
    case 7: q->flags=RR_GRAY4_CONTIGUOUS; break;
    case 8: q->fill_rgb565=65536; break;
    case 9: q->operation=99; break;
    case 10: q->src_stride=UINT32_MAX; break;
    case 11: q->src_height=0; break;
    case 12: q->src_width=0; break;
    case 13: q->src_stride=7; break;
    case 14: q->operation=RR_SCALE_GRAY4_TO_RGB565; q->flags=RR_A8_WHITE_BG; break;
    case 15: q->src_height=RR_MAX_DIM+1; break;
    }
}
static void malformed_cases(void)
{
    rr_frame f={13,70,28, RR_FORMAT_RGB565}; epd_compute_buffer_t frame,src,ctrl;
    epd_reader_render_t c; rr_command q;
    assert(epd_compute_buffer_alloc(&frame,1960) && epd_compute_buffer_alloc(&src,64));
    assert(epd_reader_render_open(&frame,f,EPD_COMPUTE_BACKGROUND,&c)==0);
    unsigned before=calls, clean_before=cleans, invalidate_before=invalidates;
    LONG windows=window_calls;
    for(unsigned fault=0;fault<16;++fault) {
        bad_command(&q,f,fault);
        assert(command(c,&q,&src,8)==EPD_COMPUTE_INVALID);
        assert(epd_reader_render_command(c,&q,src.data,8,NULL,NULL,NULL)==EPD_COMPUTE_INVALID);
    }
    bad_command(&q,f,99);
    for(unsigned fault=0;fault<4;++fault) {
        epd_compute_buffer_t bad=src;
        if(fault==0)bad.capacity=0;
        if(fault==1)bad.capacity=16;
        if(fault==2)bad.data=(uint8_t *)src.data+1;
        if(fault==3)bad.data=(void *)(uintptr_t)(UINTPTR_MAX-31u);
        assert(command(c,&q,&bad,8)==EPD_COMPUTE_INVALID);
    }
    assert(calls==before && cleans==clean_before && invalidates==invalidate_before && window_calls==windows);
    plain(c,f,RR_INIT); assert(epd_reader_render_shutdown(&c)==0);

    /* Direct remote calls prove malformed destination metadata never reaches
     * window derivation, even when there is no client validator. */
    assert(epd_compute_buffer_alloc(&ctrl,128));
    epd_compute_region_t regions[3]={epd_compute_buffer_region(&ctrl,128,3),
        epd_compute_buffer_region(&frame,1960,3),epd_compute_buffer_region(&src,64,1)};
    epd_compute_wire_t w={0}; w.regions=(uint32_t)(uintptr_t)regions; w.region_count=3;
    w.params=(epd_compute_ref_t){0,0,sizeof(rr_open_wire)};
    w.result=(epd_compute_ref_t){0,96,sizeof(rr_result)};
    rr_open_wire open={RR_VERSION,sizeof(open),{1,0,1960},{13,70,28, RR_FORMAT_RGB565}};
    memcpy(ctrl.data,&open,sizeof(open)); void *state=NULL;
    assert(epd_reader_render_remote_ops.open(&state,&w)==0 && state);
    w.params.length=sizeof(rr_command_wire); windows=window_calls;
    memset(frame.data,0xa5,frame.capacity);
    for(unsigned fault=0;fault<19;++fault) {
        rr_command_wire bad={0}; bad_command(&bad.command,f,fault);
        if(fault==16)bad.command.version++;
        if(fault==17)bad.command.bytes--;
        if(fault==18)bad.command.id=0;
        memcpy(ctrl.data,&bad,sizeof(bad));
        assert(epd_reader_render_remote_ops.step(state,&w)==EPD_COMPUTE_INVALID);
        assert(window_calls==windows);
    }
    rr_command_wire p={0}; rr_command_init(&p.command,RR_INIT,f); p.command.id=1;
    p.source.region=EPD_COMPUTE_REGION_NONE; w.region_count=2;
    uint32_t frame_base=regions[1].address;
    for(uint32_t row=0;row<70;row+=32) {
        rr_window pixels,sampled; rr_sync_windows(f,&p.command,row,&pixels,&sampled);
        regions[1].address=frame_base+pixels.offset; regions[1].capacity=regions[1].length=pixels.bytes;
        p.frame=(epd_compute_ref_t){1,0,pixels.bytes};
        for(unsigned fault=0;fault<3;++fault) {
            rr_command_wire bad=p;
            if(fault==0)regions[1].address+=32;
            if(fault==1)bad.frame.length+=32;
            if(fault==2)bad.frame.offset=32;
            memcpy(ctrl.data,&bad,sizeof(bad));
            assert(epd_reader_render_remote_ops.step(state,&w)==EPD_COMPUTE_INVALID);
            regions[1].address=frame_base+pixels.offset;
            for(unsigned i=row*28;i<frame.capacity;++i)assert(((uint8_t *)frame.data)[i]==0xa5);
        }
        memcpy(ctrl.data,&p,sizeof(p));
        assert(epd_reader_render_remote_ops.step(state,&w)==(row<64?EPD_COMPUTE_MORE:EPD_COMPUTE_OK));
    }
    rr_command_init(&p.command,RR_BLIT_A8,f); p.command.id=2;
    p.command.width=p.command.src_width=p.command.src_stride=1;
    p.command.height=p.command.src_height=64; w.region_count=3;
    uint32_t src_base=regions[2].address;
    for(uint32_t row=0;row<64;row+=32) {
        rr_window pixels,sampled; rr_sync_windows(f,&p.command,row,&pixels,&sampled);
        regions[1].address=frame_base+pixels.offset; regions[1].capacity=regions[1].length=pixels.bytes;
        regions[2].address=src_base+sampled.offset; regions[2].capacity=regions[2].length=sampled.bytes;
        p.frame=(epd_compute_ref_t){1,0,pixels.bytes}; p.source=(epd_compute_ref_t){2,0,sampled.bytes};
        memcpy(ctrl.data,&p,sizeof(p));
        if(row) {
            regions[2].address=src_base;
            assert(epd_reader_render_remote_ops.step(state,&w)==EPD_COMPUTE_INVALID);
            regions[2].address=src_base+sampled.offset;
        }
        assert(epd_reader_render_remote_ops.step(state,&w)==(row?EPD_COMPUTE_OK:EPD_COMPUTE_MORE));
    }
    epd_reader_render_remote_ops.close(state,&w);
    epd_compute_buffer_free(&ctrl); epd_compute_buffer_free(&src); epd_compute_buffer_free(&frame);
    puts("PASS malformed: 36 client rejections with zero cache/transport/window calls; 19 remote metadata rejections before windows; continuation frame/source bounds");
}

typedef struct { epd_reader_render_t client; rr_command q; int status; } render_job;
static DWORD WINAPI run_render(LPVOID arg)
{ render_job *j=arg; j->status=command(j->client,&j->q,NULL,0); return 0; }
static void concurrent_cases(void)
{
    rr_frame f={13,70,28, RR_FORMAT_RGB565}; epd_compute_buffer_t frame[2]; render_job j[2]={0}; HANDLE threads[2];
    for(unsigned i=0;i<2;++i) {
        assert(epd_compute_buffer_alloc(frame+i,1960));
        assert(epd_reader_render_open(frame+i,f,EPD_COMPUTE_BACKGROUND,&j[i].client)==0);
        plain(j[i].client,f,RR_INIT);
        rr_command_init(&j[i].q,RR_FILLRECT,f); j[i].q.height=1;
    }
    for(unsigned cancel=0;cancel<2;++cancel) {
        gate_on();
        for(unsigned i=0;i<2;++i) {
            j[i].q.y=(int32_t)cancel;
            threads[i]=CreateThread(NULL,0,run_render,j+i,0,NULL); assert(threads[i]);
            await_counts(1,i,0);
        }
        if(cancel)epd_reader_render_cancel(j[1].client);
        permit();
        if(!cancel) { await_counts(2,0,0); permit(); }
        for(unsigned i=0;i<2;++i) {
            assert(WaitForSingleObject(threads[i],5000)==WAIT_OBJECT_0); CloseHandle(threads[i]);
            assert(j[i].status==(cancel && i?EPD_COMPUTE_CANCELLED:EPD_COMPUTE_OK));
            for(unsigned x=0;x<13;++x)assert(((uint16_t *)frame[i].data)[cancel*14+x]==(cancel && i?65535:0));
        }
        assert(calls==(cancel?1u:2u)); gate_off();
    }
    for(unsigned i=0;i<2;++i) { assert(epd_reader_render_shutdown(&j[i].client)==0); epd_compute_buffer_free(frame+i); }
    puts("PASS concurrent workers: isolated frame windows, FIFO dispatch, queued cancellation with no extra transport");
}
int main(void)
{
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
    InitializeCriticalSection(&irq); InitializeConditionVariable(&changed);
    transport_gate=CreateSemaphoreA(NULL,0,65535,NULL); assert(transport_gate && compute_init()==0);
    cache_cases(); partial_and_interleave(); sampled_rows_cases(); monochrome_glyph_cases();
    malformed_cases(); concurrent_cases();
    assert(!active && allocations == 1 && !remote_allocs && !remote_sessions); /* Resident dispatcher stack. */
    for(unsigned i=0;i<32;++i)assert(!maps[i].live);
    puts("PASS production dispatch + protocol + remote + render/PCM with separate CPU/RAM cache images");
    return 0;
}
