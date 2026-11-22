#include "raster.h"
#include "epd_jpeg_compute.h"
#include "epd_resource_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <crtdbg.h>
struct epd_compute_session { int live; };
static struct epd_compute_session session;
static epd_compute_buffer_t *retired;
static unsigned allocations, close_calls, transfers, steps;
static int open_error, close_error;
static bool retain;
static epd_jpeg_result_t *result_buffer;
size_t epd_app_psram_available(void) { return 8u * 1024u * 1024u; }
size_t epd_app_allocation_peak(size_t n) { return n > SIZE_MAX-95 ? SIZE_MAX : ((n+31)&~(size_t)31)+64; }
int epd_resource_context_reserve(epd_memory_region_t region,size_t n)
{ assert(region==EPD_MEMORY_REGION_HCPU_PSRAM && n>0); return EPD_RESOURCE_OK; }
void *epd_app_alloc(size_t n,epd_app_memory_t kind) { (void)kind; void *p=calloc(1,n); assert(p); ++allocations; return p; }
void epd_app_free(void *p) { if(p) { assert(!session.live && allocations); --allocations; free(p); } }
bool epd_compute_buffer_alloc(epd_compute_buffer_t *b,size_t n)
{ b->capacity=(uint32_t)n; b->data=b->allocation=epd_app_alloc(n,0); return true; }
void epd_compute_buffer_free(epd_compute_buffer_t *b) { epd_app_free(b->allocation); memset(b,0,sizeof(*b)); }
epd_compute_region_t epd_compute_buffer_region(const epd_compute_buffer_t *b,uint32_t n,uint32_t access)
{
    if(access==EPD_COMPUTE_WRITE) result_buffer=b->data;
    return (epd_compute_region_t){0x20000000,b->capacity,n,access};
}
int epd_compute_query(const char *n,uint32_t v,epd_compute_capability_t *c)
{ (void)n;(void)v; memset(c,0,sizeof(*c)); c->ready=true; c->registration.step_limit=16; return 0; }
int epd_compute_open(const epd_compute_capability_t *c,const epd_compute_requirements_t *r,
                     const epd_compute_request_t *q,epd_compute_session_t *out)
{
    (void)c;(void)r;(void)q; session.live=retain; *out=retain?&session:NULL;
    if(!open_error) { assert(result_buffer); result_buffer->width=16; result_buffer->height=16; result_buffer->msy=1; }
    return open_error;
}
int epd_compute_close(epd_compute_session_t s)
{ assert(s==&session && s->live); ++close_calls; if(close_error)return close_error; s->live=0; return 0; }
void epd_compute_close_later(epd_compute_session_t s,epd_compute_buffer_t *buffers,uint32_t count)
{ assert(s==&session && s->live && !retired && count==4); retired=buffers; ++transfers; }
int epd_compute_step(epd_compute_session_t s,const epd_compute_request_t *q)
{ (void)s;(void)q; ++steps; return EPD_COMPUTE_COMPONENT_ERROR; }
void epd_compute_cancel(epd_compute_session_t s) { assert(s==&session); }
bool raster_seek(raster_decoder_t *d,size_t n) { d->position=n; return true; }
bool raster_read(raster_decoder_t *d,void *p,size_t n) { d->position+=n; memset(p,0x5a,n); return true; }
bool raster_cancelled(const raster_decoder_t *d) { (void)d; return false; }
bool raster_sink_init_banded(raster_decoder_t *d,unsigned w,unsigned h,unsigned rows)
{ (void)d;(void)w;(void)h;(void)rows; return true; }
void raster_sink_row(raster_decoder_t *d,unsigned x,unsigned y,unsigned step,const void *p,unsigned n,int f)
{ (void)d;(void)x;(void)y;(void)step;(void)p;(void)n;(void)f; assert(0); }
void raster_sink_end_band(raster_decoder_t *d,unsigned y) { (void)d;(void)y; assert(0); }
int main(void)
{
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
    const int errors[]={EPD_COMPUTE_CANCELLED,EPD_COMPUTE_UNAVAILABLE,EPD_COMPUTE_COMPONENT_ERROR,EPD_COMPUTE_OK};
    for(unsigned i=0;i<4;++i) {
        raster_decoder_t decoder={0}; decoder.file_size=128;
        open_error=errors[i]; close_error=EPD_COMPUTE_UNAVAILABLE; retain=true;
        unsigned before=close_calls;
        assert(raster_jpeg_remote(&decoder,0)==-1);
        assert(retired && session.live && allocations==5 && close_calls==before+1);
        assert(((unsigned char *)retired[0].data)[127]==0x5a);
        /* The decoder/module can disappear; retained state has no such pointer. */
        memset(&decoder,0,sizeof(decoder));
        close_error=0; assert(epd_compute_close(&session)==0);
        for(unsigned b=0;b<4;++b) epd_compute_buffer_free(retired+b);
        epd_app_free(retired); retired=NULL; assert(!allocations);
    }
    raster_decoder_t decoder={0}; decoder.file_size=128;
    retain=false; open_error=EPD_COMPUTE_UNAVAILABLE;
    assert(raster_jpeg_remote(&decoder,0)==0 && !allocations && !retired);
    assert(transfers==4 && steps==1);
    puts("PASS jpeg: failed OPEN/STEP/CLOSE transfers all 4 buffers, no polling, no early free, cleanup after decoder lifetime");
    return 0;
}
