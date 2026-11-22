#include "rtthread.h"
#include "epd_reading_data.h"
#include "reading_data_wire.h"
#include <crtdbg.h>
#include <stdarg.h>
#include <string.h>

static CRITICAL_SECTION irq;
static int retained_open_error;
static unsigned heap, low, opens, closes, steps, purpose_changes, max_chunk, signal_count;
static unsigned limit = 4096, flags = EPD_COMPUTE_STEP_SHARED, mutation, reply_mutation, phase;
static int fail_alloc, query_error, open_error, close_error;
static bool ready = true, parked;
static HANDLE entered, resume;
static epd_reading_data_t target;
static void (*compute_probe)(void);
static unsigned log_count;
struct epd_compute_wait { volatile LONG cancelled; HANDLE wake; };
struct epd_compute_session { void *remote; epd_compute_wire_t wire; epd_compute_region_t regions[2]; volatile LONG signals; };
static epd_compute_session_t latest;
uint32_t host_signals(const epd_compute_wire_t *);
void host_fail(const char *f, int line, const char *message)
{ fprintf(stderr, "FAIL %s:%d: %s\n", f, line, message); fflush(stderr); ExitProcess(1); }
rt_base_t rt_hw_interrupt_disable(void) { EnterCriticalSection(&irq); return 0; }
void rt_hw_interrupt_enable(rt_base_t key) { (void)key; LeaveCriticalSection(&irq); }
void *rt_calloc(size_t n, size_t s) { if (fail_alloc && !--fail_alloc) return NULL; void *p = calloc(n,s); CHECK(p); ++heap; return p; }
void rt_free(void *p) { if (p) { CHECK(heap); --heap; free(p); } }
void *epd_compute_calloc(size_t n, size_t s) { return rt_calloc(n,s); }
void epd_compute_free(void *p) { rt_free(p); }
void rt_kprintf(const char *fmt, ...) { ++log_count; va_list a; va_start(a,fmt); vprintf(fmt,a); va_end(a); }
static void *low_alloc(size_t n)
{
    for (uintptr_t a=0x20000000; a<0x70000000; a+=0x10000) {
        void *p=VirtualAlloc((void *)a,n,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        if (p) { ++low; return p; }
    }
    CHECK(false); return NULL;
}
static void low_free(void *p) { if(p) { CHECK(low && VirtualFree(p,0,MEM_RELEASE)); --low; } }
bool epd_compute_buffer_alloc(epd_compute_buffer_t *b, size_t n)
{ if(fail_alloc && !--fail_alloc) return false; b->capacity=(uint32_t)RT_ALIGN(n,32); b->data=b->allocation=low_alloc(b->capacity); return true; }
void epd_compute_buffer_free(epd_compute_buffer_t *b) { low_free(b->allocation); memset(b,0,sizeof(*b)); }
epd_compute_region_t epd_compute_buffer_region(const epd_compute_buffer_t *b,uint32_t n,uint32_t access)
{ return (epd_compute_region_t){(uint32_t)(uintptr_t)b->data,b->capacity,n,access}; }
int epd_compute_query(const char *name,uint32_t version,epd_compute_capability_t *c)
{ CHECK(!strcmp(name,EPD_READING_DATA_COMPONENT)&&version==1); memset(c,0,sizeof(*c)); c->ready=ready; c->registration.step_limit=limit; c->registration.flags=flags; return query_error; }
int epd_compute_wait_create(epd_compute_wait_t *out)
{ *out=rt_calloc(1,sizeof(**out)); if(!*out)return EPD_COMPUTE_NO_MEMORY; (*out)->wake=CreateEvent(NULL,TRUE,FALSE,NULL); CHECK((*out)->wake); return 0; }
void epd_compute_wait_cancel(epd_compute_wait_t w) { if(w) { InterlockedExchange(&w->cancelled,1); CHECK(SetEvent(w->wake)); } }
int epd_compute_wait_destroy(epd_compute_wait_t w) { CHECK(CloseHandle(w->wake)); rt_free(w); return 0; }
static void pause_at(unsigned p)
{ if(phase==p&&!parked) { parked=true; CHECK(SetEvent(entered)); CHECK(WaitForSingleObject(resume,5000)==WAIT_OBJECT_0); } }
int epd_compute_open(const epd_compute_capability_t *c,const epd_compute_requirements_t *r,const epd_compute_request_t *q,epd_compute_session_t *out)
{
    if (compute_probe) compute_probe();
    CHECK(c->ready&&r->max_regions==2); *out=NULL;
    if(q->cancelled(q->context))return EPD_COMPUTE_CANCELLED;
    if(open_error)return open_error;
    epd_compute_session_t s=low_alloc(sizeof(*s)); s->wire.step_limit=limit;
    s->wire.signals=(uint32_t)(uintptr_t)&s->signals; s->wire.regions=(uint32_t)(uintptr_t)s->regions;
    int status=epd_reading_data_remote_ops.open(&s->remote,&s->wire);
    if(status) { low_free(s); return status; }
    ++opens; latest=s; *out=s; pause_at(2);
    if(retained_open_error) epd_compute_cancel(s);
    return retained_open_error;
}
int epd_compute_open_wait(const epd_compute_capability_t *c,const epd_compute_requirements_t *r,const epd_compute_request_t *q,epd_compute_wait_t w,epd_compute_session_t *out)
{
    if (compute_probe) CHECK(!"cache must not enter waiting admission");
    *out=NULL;
    if(phase==1) { CHECK(SetEvent(entered)); CHECK(WaitForSingleObject(w->wake,5000)==WAIT_OBJECT_0); }
    if(w->cancelled)return EPD_COMPUTE_CANCELLED;
    return epd_compute_open(c,r,q,out);
}
uint32_t epd_compute_signals(const epd_compute_wire_t *q) { ++signal_count; if(signal_count==4)pause_at(4); return host_signals(q); }
int epd_compute_step(epd_compute_session_t s,const epd_compute_request_t *request)
{
    if (compute_probe) compute_probe();
    if(request->cancelled(request->context))return EPD_COMPUTE_CANCELLED;
    pause_at(3);
    if(s->signals)return EPD_COMPUTE_CANCELLED;
    memcpy(s->regions,request->regions,sizeof(s->regions)); s->wire.region_count=2; s->wire.params=request->params; s->wire.result=request->result;
    rd_request_t *q=(void *)(uintptr_t)s->regions[0].address;
    CHECK(q->length<=limit && q->length<=4096); if(q->length>max_chunk)max_chunk=q->length;
    CHECK(s->regions[0].capacity==RT_ALIGN(q->size,32)&&s->regions[1].capacity==32); ++steps;
    switch(mutation) {
        case 1: ++q->version; break; case 2: --q->size; break; case 3: --q->sequence; break;
        case 4: q->length=4097; break; case 5: s->regions[0].access=EPD_COMPUTE_WRITE; break;
        case 6: s->regions[0].address++; break; case 7: s->wire.params.offset=1; break;
        case 8: s->wire.result.length--; break; case 9: s->regions[1].address=s->regions[0].address; break;
        case 10: q->skip_size=q->length+1; break; case 11: q->operation=99; break;
        case 12: s->regions[0].length--; break;
    }
    int status=epd_reading_data_remote_ops.step(s->remote,&s->wire);
    if(status>=0&&reply_mutation) { uint32_t *p=(void *)(uintptr_t)s->regions[1].address; ++p[reply_mutation-1]; }
    pause_at(5);
    return status;
}
void epd_compute_cancel(epd_compute_session_t s) { InterlockedExchange(&s->signals,EPD_COMPUTE_CANCEL); }
int epd_compute_set_purpose(epd_compute_session_t s,epd_compute_purpose_t p) { CHECK(s&&p<EPD_COMPUTE_PURPOSE_COUNT); ++purpose_changes; return 0; }
int epd_compute_close(epd_compute_session_t s)
{ if(close_error) { int r=close_error; close_error=0; return r; } epd_reading_data_remote_ops.close(s->remote,&s->wire); if(latest==s)latest=NULL; low_free(s); ++closes; return 0; }
static epd_reading_data_t create(void) { epd_reading_data_t c; CHECK(!epd_reading_data_create(EPD_COMPUTE_BACKGROUND,&c)&&c); return c; }
static void finish(epd_reading_data_t *c) { CHECK(!epd_reading_data_close(c)&&!*c); CHECK(!epd_reading_data_close(c)); }
static uint32_t fnv(uint32_t h,const void *data,size_t n) { const unsigned char *p=data; while(n--)h=(h^*p++)*16777619u; return h; }
static void vectors(void)
{
    epd_reading_data_t c=create(); uint32_t h=2166136261u;
    CHECK(epd_reading_data_signature_update(c,NULL,0,&h)&&h==2166136261u&&!opens);
    CHECK(epd_reading_data_signature_update(c,"hello",5,&h)&&h==0x4f9f2cabu);
    CHECK(epd_reading_data_signature_update(c," world",6,&h)&&h==0xd58b3fa7u);
    CHECK(!epd_reading_data_set_purpose(c,EPD_COMPUTE_FOREGROUND)&&purpose_changes==1&&opens==1);
    CHECK(!log_count);
    finish(&c);
}
static void chunks(void)
{
    unsigned char data[10003]; for(unsigned i=0;i<sizeof(data);++i)data[i]=(unsigned char)(i*73u);
    for(unsigned l=1;l<=4096;l=l==1?31:l==31?257:4096) {
        limit=l; epd_reading_data_t c=create(); uint32_t h=0x12345678;
        CHECK(!epd_reading_data_hash(c,data,sizeof(data),&h)&&h==fnv(0x12345678,data,sizeof(data)));
        h=1; uint32_t expected=fnv(fnv(1,data,4093),data+4102,sizeof(data)-4102);
        CHECK(!epd_reading_data_hash_skip(c,data,sizeof(data),4093,9,&h)&&h==expected);
        CHECK(!log_count); finish(&c); if(l==4096)break;
    }
}
static void failures(void)
{
    epd_reading_data_t c=(void *)1;
    ready=false; CHECK(epd_reading_data_create(EPD_COMPUTE_BACKGROUND,&c)==EPD_COMPUTE_UNAVAILABLE&&!c); ready=true;
    flags=0; CHECK(epd_reading_data_create(EPD_COMPUTE_BACKGROUND,&c)==EPD_COMPUTE_UNSUPPORTED&&!c); flags=EPD_COMPUTE_STEP_SHARED;
    for(int i=1;i<=4;++i) { fail_alloc=i; CHECK(epd_reading_data_create(EPD_COMPUTE_BACKGROUND,&c)==EPD_COMPUTE_NO_MEMORY&&!c&&!heap&&!low); }
    c=create(); uint32_t h=7; fail_alloc=1; CHECK(epd_reading_data_hash(c,"x",1,&h)==EPD_COMPUTE_NO_MEMORY&&h==7); finish(&c);
    c=create(); CHECK(!epd_reading_data_hash(c,"x",1,&h)); unsigned saved=heap+low;
    close_error=EPD_COMPUTE_BUSY; CHECK(epd_reading_data_close(&c)==EPD_COMPUTE_BUSY&&c&&heap+low==saved);
    CHECK(epd_reading_data_hash(c,"x",1,&h)==EPD_COMPUTE_INVALID); finish(&c);
}
static void protocol(void)
{
    for(mutation=1;mutation<=12;++mutation) { epd_reading_data_t c=create(); uint32_t h=7; CHECK(epd_reading_data_hash(c,"test",4,&h)==EPD_COMPUTE_INVALID&&h==7); finish(&c); }
    mutation=0;
    for(reply_mutation=1;reply_mutation<=8;++reply_mutation) {
        if(reply_mutation==7)continue; /* A valid hash value cannot be independently recomputed on HCPU. */
        epd_reading_data_t c=create(); uint32_t h=7; CHECK(epd_reading_data_hash(c,"test",4,&h)==EPD_COMPUTE_COMPONENT_ERROR&&h==7); finish(&c);
    }
    reply_mutation=0;
}
static DWORD WINAPI cancel_thread(void *p)
{ (void)p; CHECK(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0); epd_reading_data_cancel(target); CHECK(SetEvent(resume)); return 0; }
static void cancellation(void)
{
    for(phase=0;phase<=5;++phase) {
        target=create(); uint32_t h=99; unsigned char data[9000]={0}; parked=false; signal_count=0;
        ResetEvent(entered); ResetEvent(resume); HANDLE t=NULL;
        if(!phase)epd_reading_data_cancel(target); else { t=CreateThread(NULL,0,cancel_thread,NULL,0,NULL); CHECK(t); }
        CHECK(epd_reading_data_hash(target,data,sizeof(data),&h)==EPD_COMPUTE_CANCELLED&&h==99);
        if(t) { CHECK(WaitForSingleObject(t,5000)==WAIT_OBJECT_0); CloseHandle(t); }
        CHECK(epd_reading_data_status(target)==EPD_COMPUTE_CANCELLED);
        finish(&target);
    }
    phase=0;
}
static void try_admission(void)
{
    epd_reading_data_t c=create(); open_error=EPD_COMPUTE_BUSY;
    CHECK(epd_reading_data_try_open(c)==EPD_COMPUTE_BUSY&&!opens&&!epd_reading_data_status(c));
    open_error=0; CHECK(!epd_reading_data_try_open(c)&&opens==1);
    uint32_t h=4; CHECK(!epd_reading_data_hash(c,"ok",2,&h)&&opens==1); finish(&c);
}
static void interleave(void)
{
    epd_reading_data_t a=create(),b=create(); uint32_t x=1,y=2;
    for(unsigned i=0;i<20;++i) { CHECK(!epd_reading_data_hash(a,"a",1,&x)); CHECK(!epd_reading_data_hash(b,"b",1,&y)); }
    CHECK(opens==2); finish(&a); finish(&b);
}
static epd_reading_index_spec_t spec={0x58445642,2,123,10000,1000000,1024};
static unsigned char *index_blob(unsigned pages,unsigned toc,size_t *size)
{
    *size=36+((size_t)pages+1+toc)*8;
    unsigned char *p=calloc(1,*size); CHECK(p);
    epd_reading_index_header_t h={spec.magic,spec.version,spec.layout,spec.end,pages,toc,spec.end,1,0}; memcpy(p,&h,36);
    uint32_t *words=(void *)(p+36); for(unsigned i=0;i<=pages;++i) { words[i*2]=i; words[i*2+1]=0; }
    words+=(pages+1)*2; for(unsigned i=0;i<toc;++i) { words[i*2]=i; words[i*2+1]=i%4; }
    return p;
}
static void checksum(unsigned char *p,size_t n) { uint32_t h=fnv(fnv(2166136261u,p,32),p+36,n-36); memcpy(p+32,&h,4); }
static void index_valid(void)
{
    for(unsigned l=1;l<=4096;l=l==1?31:4096) {
        limit=l; size_t n; unsigned char *p=index_blob(1001,20,&n); checksum(p,n); epd_reading_data_t c=create();
        CHECK(!epd_reading_data_index_check(c,p,n,&spec)); CHECK(!epd_reading_data_index_check(c,p,n,&spec));
        finish(&c); free(p); if(l==4096)break;
    }
    spec.end=0; size_t n; unsigned char *p=index_blob(1,0,&n); memset(p+36,0,16); checksum(p,n);
    epd_reading_data_t c=create(); CHECK(!epd_reading_data_index_check(c,p,n,&spec)); finish(&c); free(p);
}
static void index_invalid(void)
{
    for(unsigned test=0;test<13;++test) {
        size_t n; unsigned char *p=index_blob(600,4,&n); epd_reading_index_header_t *h=(void *)p; uint32_t *w=(void *)(p+36);
        switch(test) {
            case 0: ++h->layout; break; case 1: ++h->version; break; case 2: ++h->end; break;
            case 3: h->count=UINT32_MAX; break; case 4: h->complete=2; break; case 5: h->toc_scanned=h->end+1; break;
            case 6: w[0]=1; break; case 7: w[900]=w[898]; break; case 8: w[901]=1025; break;
            case 9: w[900]=spec.end; w[901]=1; break; case 10: w[1204]=w[1202]; break;
            case 11: w[1202]=h->toc_scanned; break;
        }
        checksum(p,n); if(test==12)h->checksum++;
        epd_reading_data_t c=create(); CHECK(epd_reading_data_index_check(c,p,n,&spec)==EPD_COMPUTE_INVALID); finish(&c); free(p);
    }
}
static void open_cleanup(void)
{
    const int errors[]={EPD_COMPUTE_CANCELLED,EPD_COMPUTE_COMPONENT_ERROR,EPD_COMPUTE_BUSY};
    for(unsigned mode=0;mode<2;++mode) for(unsigned i=0;i<3;++i) {
        epd_reading_data_t c=create(); uint32_t h=7;
        retained_open_error=errors[i]; unsigned before=steps;
        CHECK((mode?epd_reading_data_try_open(c):epd_reading_data_hash(c,"x",1,&h))==errors[i]);
        CHECK(latest && h==7 && steps==before);
        unsigned saved=heap+low;
        CHECK(epd_reading_data_hash(c,"x",1,&h)==errors[i] && steps==before && h==7);
        for(unsigned retry=0;retry<3;++retry) {
            close_error=EPD_COMPUTE_UNAVAILABLE;
            CHECK(epd_reading_data_close(&c)==EPD_COMPUTE_UNAVAILABLE && c && latest && heap+low==saved);
        }
        retained_open_error=0; finish(&c);
    }
}
struct test { const char *name; void (*run)(void); };
static const struct test tests[]={ {"vectors",vectors},{"chunks",chunks},{"failures",failures},{"protocol",protocol},{"cancellation",cancellation},{"try_admission",try_admission},{"interleave",interleave},{"index_valid",index_valid},{"index_invalid",index_invalid},{"open_cleanup",open_cleanup} };
int main(int argc,char **argv)
{
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX); InitializeCriticalSection(&irq);
    entered=CreateEvent(NULL,TRUE,FALSE,NULL); resume=CreateEvent(NULL,TRUE,FALSE,NULL);
    if(argc==2&&!strcmp(argv[1],"--list")) { for(unsigned i=0;i<sizeof(tests)/sizeof(tests[0]);++i)puts(tests[i].name); return 0; }
    for(unsigned i=0;i<sizeof(tests)/sizeof(tests[0]);++i)if(argc==2&&!strcmp(argv[1],tests[i].name)) {
        tests[i].run(); CHECK(!heap&&!low&&opens==closes); printf("PASS %s steps=%u max_bytes=%u resources=0\n",argv[1],steps,max_chunk); CloseHandle(entered); CloseHandle(resume); DeleteCriticalSection(&irq); return 0;
    }
    return 2;
}
