#include "dfu_system_font.h"
#include "dfu_board.h"
#include "../../font/system_font_format.h"
#include "../../font/font_estimate.h"
#include "ff.h"
#include "ptab.h"
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ZLIB_APIS
#define MINIZ_NO_STDIO
#define MINIZ_NO_MALLOC
#include "../../src/thirdparty/miniz/miniz.h"
#include <string.h>
#include <stdio.h>

#define FONT_STAGE "0:/EPDDFU/SYSFONT.BIN"
#define BLOCK 32768u
#define MAX_RAW (66u*1024u*1024u)
static tdefl_compressor compressor;
static uint8_t raw[BLOCK], zipped[BLOCK+256], old_block[BLOCK];
static uint32_t offsets[MAX_RAW/BLOCK+1];
static size_t zipped_size;
static FIL input, stage;

static uint32_t u32(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static bool put(const void *data, UINT size)
{ UINT n; return f_write(&stage,data,size,&n)==FR_OK && n==size; }
static bool get(FIL *f, void *data, UINT size)
{ UINT n; return f_read(f,data,size,&n)==FR_OK && n==size; }
static mz_bool compressed(const void *data, int size, void *user)
{
    (void)user;
    if (size<0 || (size_t)size>sizeof(zipped)-zipped_size) return MZ_FALSE;
    memcpy(zipped+zipped_size,data,(size_t)size);zipped_size+=(size_t)size;return MZ_TRUE;
}
static bool old_read(const system_font_header_t *h, uint32_t position, void *out, uint32_t count)
{
    const uint8_t *p=(const uint8_t *)(BUILTIN_RESOURCE_START_ADDR+SYSTEM_FONT_HEADER_BYTES);
    if (h->packed_bytes<24 || memcmp(p,"FTZ1",4)) return false;
    uint32_t block=u32(p+8), blocks=u32(p+12), total=h->font_bytes+h->emoji_bytes;
    if ((block!=16384 && block!=BLOCK) || u32(p+4)!=total ||
        blocks!=(total+block-1)/block || blocks>(h->packed_bytes-20)/4 ||
        position>total || count>total-position) return false;
    while(count) {
        uint32_t i=position/block, begin=u32(p+16+i*4), end=u32(p+20+i*4);
        uint32_t expected=total-i*block;if(expected>block)expected=block;
        if(begin<20+blocks*4 || end<=begin || end>h->packed_bytes ||
            tinfl_decompress_mem_to_mem(old_block,sizeof(old_block),p+begin,end-begin,
                TINFL_FLAG_PARSE_ZLIB_HEADER)!=expected) return false;
        uint32_t n=block-position%block;if(n>count)n=count;
        memcpy(out,old_block+position%block,n);out=(uint8_t *)out+n;count-=n;position+=n;
    }
    return true;
}
static bool staged(const dfu_request_t *r, system_font_header_t *h, uint32_t *image_crc)
{
    if(f_open(&stage,FONT_STAGE,FA_READ)!=FR_OK)return false;
    bool ok=get(&stage,h,sizeof(*h)) && system_font_header_valid(h) &&
        h->signature==r->signature && h->font_bytes==r->zip_size &&
        !strcmp(h->name,r->font_name) && f_size(&stage)==sizeof(*h)+h->packed_bytes;
    uint32_t crc=0,all=ok?dfu_crc32(0,h,sizeof(*h)):0;
    for(uint32_t left=ok?h->packed_bytes:0;ok && left;) {
        uint32_t n=left>sizeof(raw)?sizeof(raw):left;
        ok=get(&stage,raw,n);if(ok){crc=dfu_crc32(crc,raw,n);all=dfu_crc32(all,raw,n);left-=n;}
    }
    f_close(&stage);*image_crc=all;return ok && crc==h->packed_crc;
}
static unsigned pack(const dfu_request_t *r, dfu_package_progress_t progress)
{
    system_font_header_t previous;
    if(!dfu_flash_read(BUILTIN_RESOURCE_START_ADDR,&previous,sizeof(previous)) ||
        !system_font_header_valid(&previous))return DFU_FONT_RESOURCE;
    const uint8_t *old=(const uint8_t *)(BUILTIN_RESOURCE_START_ADDR+SYSTEM_FONT_HEADER_BYTES);
    if(dfu_crc32(0,old,previous.packed_bytes)!=previous.packed_crc)return DFU_FONT_RESOURCE;
    if(r->zip_size>64u*1024u*1024u || r->zip_size<12)return DFU_FONT_SOURCE;
    if(f_open(&input,r->path,FA_READ)!=FR_OK)return DFU_FONT_SOURCE;
    if(f_size(&input)!=r->zip_size){f_close(&input);return DFU_FONT_SOURCE;}
    FRESULT directory=f_mkdir("0:/EPDDFU");
    if(directory!=FR_OK && directory!=FR_EXIST){f_close(&input);return DFU_FONT_IO;}
    if(f_open(&stage,FONT_STAGE,FA_CREATE_ALWAYS|FA_WRITE|FA_READ)!=FR_OK){f_close(&input);return DFU_FONT_IO;}
    system_font_header_t h={0};
    uint32_t total=r->zip_size+previous.emoji_bytes, blocks=(total+BLOCK-1)/BLOCK;
    uint32_t position=20+blocks*4, signature=2166136261u;
    uint32_t ftz[4]={0x315a5446u,total,BLOCK,blocks};
    memset(offsets,0,(blocks+1)*sizeof(*offsets));
    unsigned error=DFU_FONT_OK;
    if(!put(&h,sizeof(h)) || !put(ftz,sizeof(ftz)) || !put(offsets,(blocks+1)*4))error=DFU_FONT_IO;
    for(uint32_t i=0;i<blocks && !error;++i) {
        uint32_t begin=i*BLOCK,n=total-begin;if(n>BLOCK)n=BLOCK;
        uint32_t from_ttf=begin<r->zip_size?r->zip_size-begin:0;if(from_ttf>n)from_ttf=n;
        if(from_ttf && !get(&input,raw,from_ttf)){error=DFU_FONT_SOURCE;break;}
        for(uint32_t j=0;j<from_ttf;++j)signature=(signature^raw[j])*16777619u;
        if(!i && (raw[0]!=0 || raw[1]!=1 || raw[2]!=0 || raw[3]!=0)){error=DFU_FONT_SOURCE;break;}
        if(n>from_ttf && !old_read(&previous,previous.font_bytes+begin+from_ttf-r->zip_size,
                                  raw+from_ttf,n-from_ttf)){error=DFU_FONT_RESOURCE;break;}
        zipped_size=0;
        if(tdefl_init(&compressor,compressed,NULL,SYSTEM_FONT_COMPRESS_FLAGS)!=TDEFL_STATUS_OKAY ||
            tdefl_compress_buffer(&compressor,raw,n,TDEFL_FINISH)!=TDEFL_STATUS_DONE){error=DFU_FONT_IO;break;}
        if(zipped_size>SYSTEM_FONT_SLOT_BYTES-SYSTEM_FONT_HEADER_BYTES-position){error=DFU_FONT_TOO_LARGE;break;}
        offsets[i]=position;position+=(uint32_t)zipped_size;
        if(!put(zipped,(UINT)zipped_size)){error=DFU_FONT_IO;break;}
        if (!(i % 16)) {
            char message[96];
            snprintf(message,sizeof(message),"DFU font: block=%lu/%lu compressed=%lu\r\n",
                (unsigned long)(i+1),(unsigned long)blocks,(unsigned long)position);
            dfu_log(message);
        }
        if(progress)progress(DFU_TARGET_RESOURCES,begin+n,total);
    }
    f_close(&input);
    if(!error && signature!=r->signature)error=DFU_FONT_SOURCE;
    offsets[blocks]=position;
    if(!error && (f_lseek(&stage,sizeof(h)+16)!=FR_OK || !put(offsets,(blocks+1)*4) ||
        f_sync(&stage)!=FR_OK || f_lseek(&stage,sizeof(h))!=FR_OK))error=DFU_FONT_IO;
    uint32_t crc=0;
    for(uint32_t left=position;!error && left;) {
        uint32_t n=left>sizeof(raw)?sizeof(raw):left;
        if(!get(&stage,raw,n))error=DFU_FONT_IO;
        else{crc=dfu_crc32(crc,raw,n);left-=n;}
    }
    if(!error) {
        h.magic=SYSTEM_FONT_MAGIC;h.version=1;h.packed_bytes=position;h.font_bytes=r->zip_size;
        h.emoji_bytes=previous.emoji_bytes;h.signature=signature;h.packed_crc=crc;
        memcpy(h.name,r->font_name,sizeof(h.name));h.header_crc=system_font_crc(0,&h,offsetof(system_font_header_t,header_crc));
        if(f_lseek(&stage,0)!=FR_OK || !put(&h,sizeof(h)) || f_sync(&stage)!=FR_OK)error=DFU_FONT_IO;
    }
    if(f_close(&stage)!=FR_OK && !error)error=DFU_FONT_IO;
    if(error)f_unlink(FONT_STAGE);
    return error;
}
unsigned dfu_system_font_prepare(const dfu_request_t *r, bool recovering,
    dfu_package_t *p, dfu_package_progress_t progress)
{
    system_font_header_t h;uint32_t crc;
    if(!staged(r,&h,&crc)) {
        if(recovering)return DFU_FONT_IO;
        unsigned error=pack(r,progress);if(error)return error;
        if(!staged(r,&h,&crc))return DFU_FONT_IO;
    }
    memset(p,0,sizeof(*p));p->count=1;p->total_size=sizeof(h)+h.packed_bytes;
    p->images[0]=(dfu_package_image_t){DFU_TARGET_RESOURCES,BUILTIN_RESOURCE_START_ADDR,p->total_size,crc,FONT_STAGE};
    return DFU_FONT_OK;
}
void dfu_system_font_cleanup(void)
{
    (void)f_unlink(FONT_STAGE);
}
