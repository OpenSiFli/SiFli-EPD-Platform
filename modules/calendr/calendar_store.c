#include "calendar_store.h"
#include <string.h>
static void put(uint8_t *p,uint32_t n){for(int i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static uint32_t get(const uint8_t *p){uint32_t n=0;for(int i=0;i<4;++i)n|=(uint32_t)p[i]<<(8*i);return n;}
bool cal_document_encode(const cal_book_t *b,const cal_event_t *draft,int32_t day,bool instance,void **out,size_t *size)
{
    void *main=NULL,*editing=NULL;size_t n=0,k=0;bool ok=cal_encode(b,&main,&n);uint32_t flags=instance?1:0,id=0;
    if(ok&&draft){cal_book_t db;cal_book_init(&db);cal_event_t e=*draft;id=e.id;e.id=1;e.parent=0;
        if(!e.title[0]){strcpy(e.title," ");flags|=2;}ok=cal_put(&db,&e)&&cal_encode(&db,&editing,&k);cal_book_clear(&db);}
    uint8_t *p=ok?cal_alloc(32+n+k):NULL;if(p){put(p,0x43414432);put(p+4,(uint32_t)n);put(p+8,(uint32_t)k);put(p+12,flags);put(p+16,(uint32_t)day);put(p+20,id);put(p+24,draft?draft->parent:0);put(p+28,draft?(uint32_t)draft->original_day:0);memcpy(p+32,main,n);if(k)memcpy(p+32+n,editing,k);*out=p;*size=32+n+k;}
    cal_free(main);cal_free(editing);return p!=NULL;
}
bool cal_document_decode(cal_book_t *book,cal_event_t *draft,int32_t *day,bool *instance,const void *data,size_t size)
{
    const uint8_t *p=data;if(!p||size<24||(get(p)!=0x43414431&&get(p)!=0x43414432))return false;size_t header=get(p)==0x43414432?32:24;if(size<header)return false;size_t n=get(p+4),k=get(p+8);uint32_t flags=get(p+12);
    if(n>size-header||k!=size-header-n||flags>3)return false;cal_book_t b,d;cal_book_init(&b);cal_book_init(&d);
    bool ok=cal_decode(&b,p+header,n)&&(!k||(cal_decode(&d,p+header+n,k)&&d.count==1));cal_event_t copy={0};
    if(ok&&k){ok=cal_event_copy(&copy,d.events[0]);copy.id=get(p+20);if(header==32){copy.parent=get(p+24);copy.original_day=(int32_t)get(p+28);}if(flags&2)copy.title[0]=0;}
    if(ok){cal_book_clear(book);*book=b;cal_book_init(&b);cal_event_clear(draft);*draft=copy;*day=(int32_t)get(p+16);*instance=(flags&1)!=0;}
    else cal_event_clear(&copy);cal_book_clear(&b);cal_book_clear(&d);return ok;
}
