#include "font_estimate.h"
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "../src/thirdparty/miniz/miniz.h"
#include <string.h>
#define BLOCK 32768u
typedef struct { tdefl_compressor compressor; uint8_t bytes[BLOCK]; uint32_t count; } workspace_t;
size_t font_estimate_workspace_bytes(void) { return sizeof(workspace_t); }
static uint32_t be32(const uint8_t *p)
{ return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; }
static mz_bool counted(const void *data,int count,void *user)
{ (void)data; if(count<0)return MZ_FALSE;((workspace_t *)user)->count+=(uint32_t)count;return MZ_TRUE; }
static bool segment(workspace_t *w,uint32_t size,uint32_t first,uint32_t end,unsigned limit,
                    font_estimate_read_t read,void *user,uint64_t *estimate,uint32_t *sampled)
{
    if(first>=end)return true;
    unsigned n=end-first;if(n>limit)n=limit;
    /* One midpoint sample from each equal-width stratum, weighted by that
     * stratum's block count. Metadata and outlines get separate strata. */
    for(unsigned i=0;i<n;++i) {
        uint32_t a=first+(uint64_t)(end-first)*i/n,b=first+(uint64_t)(end-first)*(i+1)/n;
        uint32_t block=a+(b-a-1)/2,bytes=size-block*BLOCK;if(bytes>BLOCK)bytes=BLOCK;
        if(!read(user,block*BLOCK,w->bytes,bytes))return false;
        w->count=0;
        if(tdefl_init(&w->compressor,counted,w,SYSTEM_FONT_COMPRESS_FLAGS)!=TDEFL_STATUS_OKAY ||
            tdefl_compress_buffer(&w->compressor,w->bytes,bytes,TDEFL_FINISH)!=TDEFL_STATUS_DONE)return false;
        *estimate+=(uint64_t)w->count*(b-a);*sampled+=bytes;
    }
    return true;
}
bool font_estimate(void *workspace,uint32_t size,font_estimate_read_t read,void *user,
                   uint32_t *packed,uint32_t *sampled)
{
    if(!workspace || size<12 || !read || !packed || !sampled)return false;
    workspace_t *w=workspace;uint8_t directory[16];
    if(!read(user,0,directory,12) || be32(directory)!=0x10000)return false;
    uint32_t tables=(uint32_t)directory[4]<<8|directory[5];
    if(!tables || tables>(size-12)/16 || tables>512)return false;
    uint32_t begin=0,end=0,blocks=(size+BLOCK-1)/BLOCK;
    for(uint32_t i=0;i<tables;++i) {
        if(!read(user,12+i*16,directory,16))return false;
        if(!memcmp(directory,"glyf",4)) {
            uint32_t offset=be32(directory+8),length=be32(directory+12);
            if(offset>size || length>size-offset)return false;
            begin=offset/BLOCK;end=(offset+length+BLOCK-1)/BLOCK;break;
        }
    }
    if(!end)return false;
    uint64_t estimate=0;*sampled=0;
    if(!segment(w,size,0,begin,4,read,user,&estimate,sampled) ||
        !segment(w,size,begin,end,12,read,user,&estimate,sampled) ||
        !segment(w,size,end,blocks,4,read,user,&estimate,sampled) || estimate>UINT32_MAX)return false;
    *packed=(uint32_t)estimate;return true;
}
