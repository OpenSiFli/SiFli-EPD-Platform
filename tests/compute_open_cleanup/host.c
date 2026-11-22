#include "epd_compute.h"
#include "epd_mp3.h"
#include "epd_flac.h"
#include "epd_pcm.h"
#include "epd_anyka.h"
#include "epd_opus.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <crtdbg.h>

static unsigned allocations, buffers, close_calls, steps, resets, fallback_calls;
static int open_error, close_error;
static bool keep_session;
struct epd_compute_session { int live; };
static struct epd_compute_session remote;
static epd_compute_buffer_t saved[8];
static unsigned saved_count;
static uint32_t snapshots[8];
static uint32_t checksum(const void *p, size_t n)
{ const unsigned char *b=p; uint32_t h=1; while(n--) h=h*33u+*b++; return h; }
void *rt_calloc(size_t n, size_t size)
{ void *p=calloc(n,size); assert(p); ++allocations; return p; }
void rt_free(void *p) { if(p) { assert(allocations); --allocations; free(p); } }
bool epd_compute_buffer_alloc(epd_compute_buffer_t *b,size_t bytes)
{
    b->capacity=(uint32_t)((bytes+31u)&~31u);
    b->allocation=rt_calloc(1,b->capacity+31u);
    b->data=(void *)(((uintptr_t)b->allocation+31u)&~(uintptr_t)31u);
    ++buffers; return true;
}
void epd_compute_buffer_free(epd_compute_buffer_t *b)
{ if(b->allocation) { assert(!remote.live && buffers); --buffers; rt_free(b->allocation); } memset(b,0,sizeof(*b)); }
epd_compute_region_t epd_compute_buffer_region(const epd_compute_buffer_t *b,uint32_t n,uint32_t access)
{
    assert(saved_count<8); saved[saved_count++]=*b;
    return (epd_compute_region_t){0x20000000u+32u*saved_count,b->capacity,n,access};
}
int epd_compute_query(const char *name,uint32_t version,epd_compute_capability_t *cap)
{ (void)name; (void)version; memset(cap,0,sizeof(*cap)); cap->ready=true; return 0; }
int epd_compute_open(const epd_compute_capability_t *cap,const epd_compute_requirements_t *req,
                     const epd_compute_request_t *request,epd_compute_session_t *out)
{
    (void)cap; (void)req; (void)request; assert(!remote.live);
    for(unsigned i=0;i<saved_count;++i) snapshots[i]=checksum(saved[i].data,saved[i].capacity);
    remote.live=keep_session; *out=keep_session?&remote:NULL; return open_error;
}
int epd_compute_close(epd_compute_session_t s)
{ assert(s==&remote && remote.live); ++close_calls; if(close_error)return close_error; remote.live=0; return 0; }
int epd_compute_step(epd_compute_session_t s,const epd_compute_request_t *q)
{ (void)s; (void)q; ++steps; return EPD_COMPUTE_CANCELLED; }
int epd_compute_reset(epd_compute_session_t s,const epd_compute_request_t *q)
{ (void)s; (void)q; ++resets; return EPD_COMPUTE_CANCELLED; }
void epd_compute_cancel(epd_compute_session_t s) { assert(s==&remote); }
HMP3Decoder MP3InitDecoder(void) { ++fallback_calls; return rt_calloc(1,1); }
void MP3FreeDecoder(HMP3Decoder d) { rt_free(d); }
int MP3Decode(HMP3Decoder d,unsigned char **p,int *n,short *pcm,int size,int seeking)
{ (void)d;(void)p;(void)n;(void)pcm;(void)size;(void)seeking; assert(0); return -1; }
void MP3GetLastFrameInfo(HMP3Decoder d,MP3FrameInfo *info) { (void)d; memset(info,0,sizeof(*info)); }
int MP3GetNextFrameInfo(HMP3Decoder d,MP3FrameInfo *info,unsigned char *p)
{ (void)d;(void)info;(void)p; assert(0); return -1; }

enum { MP3, FLAC, PCM, ANYKA, OPUS, CODECS };
static const char *names[]={"mp3","flac","pcm","anyka","opus"};
static int open_codec(unsigned codec,void **out)
{
    epd_flac_config_t flac={0}; epd_anyka_config_t anyka={0};
    epd_opus_config_t opus={EPD_OPUS_ENCODER,16000,1,EPD_OPUS_VOIP};
    switch(codec) {
    case MP3:return epd_mp3_open((epd_mp3_t *)out);
    case FLAC:return epd_flac_open(&flac,EPD_COMPUTE_BACKGROUND,(epd_flac_t *)out);
    case PCM:return epd_pcm_open((epd_pcm_format_t){16,1},EPD_COMPUTE_BACKGROUND,(epd_pcm_t *)out);
    case ANYKA:return epd_anyka_open(&anyka,"near","far",EPD_COMPUTE_BACKGROUND,(epd_anyka_t *)out);
    default:return epd_opus_open(&opus,EPD_COMPUTE_BACKGROUND,(epd_opus_t *)out);
    }
}
static int close_codec(unsigned codec,void *p)
{
    switch(codec) {
    case MP3:return epd_mp3_close(p); case FLAC:return epd_flac_close(p);
    case PCM:return epd_pcm_close(p); case ANYKA:return epd_anyka_close(p);
    default:return epd_opus_close(p);
    }
}
static void terminal(unsigned codec,void *p)
{
    unsigned char input[512]={0},*cursor=input; short output[2304]={0}; int left=sizeof(input);
    MP3FrameInfo info; epd_flac_result_t f; epd_anyka_result_t a; epd_opus_result_t o;
    switch(codec) {
    case MP3:
        assert(epd_mp3_decode(p,&cursor,&left,output,0,0)<0);
        assert(epd_mp3_next_info(p,&info,input)<0 && epd_mp3_reset(p)<0); break;
    case FLAC:
        assert(epd_flac_decode(p,input,sizeof(input),false,output,2304,&f,NULL,NULL)<0);
        assert(epd_flac_reset(p)<0); break;
    case PCM:assert(epd_pcm_convert(p,input,32,output,NULL,NULL)<0); break;
    case ANYKA:
        assert(epd_anyka_process(p,EPD_ANYKA_UPLINK,0,input,sizeof(input),NULL,0,output,sizeof(output),&a)<0);
        assert(epd_anyka_reset(p)<0); break;
    default:
        assert(epd_opus_process(p,EPD_OPUS_ENCODE,input,sizeof(input),output,sizeof(output),160,0,&o)<0);
        assert(epd_opus_set(p,EPD_OPUS_BITRATE,16000,&o)<0 && epd_opus_reset(p)<0);
    }
    assert(!steps && !resets);
    for(unsigned i=0;i<saved_count;++i) assert(checksum(saved[i].data,saved[i].capacity)==snapshots[i]);
}
int main(void)
{
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
    const int errors[]={EPD_COMPUTE_CANCELLED,EPD_COMPUTE_COMPONENT_ERROR,EPD_COMPUTE_UNAVAILABLE,EPD_COMPUTE_BUSY};
    for(unsigned codec=0;codec<CODECS;++codec) {
        for(unsigned e=0;e<sizeof(errors)/sizeof(*errors);++e) {
            void *client=NULL; open_error=errors[e]; keep_session=true; saved_count=0;
            unsigned calls=close_calls, local=fallback_calls;
            assert(open_codec(codec,&client)<0 && client && remote.live);
            assert(close_calls==calls && fallback_calls==local);
            unsigned owned=allocations, shared=buffers;
            terminal(codec,client);
            for(unsigned retry=0;retry<3;++retry) {
                close_error=errors[retry]; assert(close_codec(codec,client)<0);
                assert(remote.live && allocations==owned && buffers==shared && close_calls==calls+retry+1);
                terminal(codec,client);
            }
            close_error=0; assert(close_codec(codec,client)==0);
            assert(!remote.live && !allocations && !buffers);
        }
        /* No retained remote state: preserve existing MP3/PCM local fallback. */
        keep_session=false; open_error=EPD_COMPUTE_UNAVAILABLE; saved_count=0;
        void *client=NULL; int status=open_codec(codec,&client);
        if(codec==MP3 || codec==PCM) { assert(status==0 && client); assert(close_codec(codec,client)==0); }
        else assert(status<0 && !client);
        assert(!allocations && !buffers);
        printf("PASS %s: 4 retained OPEN failures, repeated CLOSE errors, terminal buffer protection, recovery/fallback\n",names[codec]);
    }
    return 0;
}
