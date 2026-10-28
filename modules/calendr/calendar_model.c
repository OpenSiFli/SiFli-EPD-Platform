#include "calendar_model.h"
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#ifndef CAL_HOST
#include "epd_memory.h"
#endif
void *cal_alloc(size_t n) {
#ifdef CAL_HOST
    return calloc(1,n?n:1);
#else
    void *p=epd_app_alloc(n?n:1,EPD_APP_PSRAM);if(p)memset(p,0,n);return p;
#endif
}
void cal_free(void *p) {
#ifdef CAL_HOST
    free(p);
#else
    epd_app_free(p);
#endif
}
bool cal_utf8(const char *text,size_t max_chars)
{
    if(!text)return true;const unsigned char *p=(const unsigned char *)text;size_t chars=0;
    while(*p) {
        unsigned c=*p++,need,min;
        if(c<0x80){if(c<32&&c!='\n'&&c!='\t'&&c!='\r')return false;need=0;min=0;}
        else if(c>=0xc2&&c<=0xdf){c&=31;need=1;min=0x80;}
        else if(c>=0xe0&&c<=0xef){c&=15;need=2;min=0x800;}
        else if(c>=0xf0&&c<=0xf4){c&=7;need=3;min=0x10000;}
        else return false;
        for(unsigned i=0;i<need;++i){if((*p&0xc0)!=0x80)return false;c=(c<<6)|(*p++&63);}
        if(c<min||c>0x10ffff||(c>=0xd800&&c<=0xdfff)||++chars>max_chars)return false;
    }
    return true;
}
bool cal_event_valid(const cal_event_t *e)
{
    return e&&memchr(e->uid,0,sizeof(e->uid))&&cal_utf8(e->uid,127)&&!strchr(e->uid,'\n')&&!strchr(e->uid,'\r')&&e->title[0]&&memchr(e->title,0,sizeof(e->title))&&memchr(e->place,0,sizeof(e->place))&&
        cal_utf8(e->title,80)&&cal_utf8(e->place,120)&&(!e->notes||strlen(e->notes)<=CAL_TEXT_BYTES)&&cal_utf8(e->notes,2000)&&
        epd_calendar_rule_valid(&e->rule)&&e->kind<=CAL_COUNTDOWN&&e->duration_minutes>0&&e->duration_minutes<=366u*1440u&&
        (!e->all_day||(e->rule.minute==0&&e->duration_minutes%1440==0))&&e->reminder_minutes>=-1&&e->reminder_minutes<=525600;
}
void cal_event_clear(cal_event_t *e){if(e){cal_free(e->notes);memset(e,0,sizeof(*e));}}
bool cal_event_copy(cal_event_t *to,const cal_event_t *from)
{
    cal_event_t n=*from;n.notes=NULL;
    if(from->notes){n.notes=cal_alloc(strlen(from->notes)+1);if(!n.notes)return false;strcpy(n.notes,from->notes);}
    cal_event_clear(to);*to=n;return true;
}
void cal_book_init(cal_book_t *b){memset(b,0,sizeof(*b));b->next_id=1;b->default_duration=60;b->first_weekday=1;b->default_view=1;b->lunar=true;b->festivals=true;}
void cal_book_clear(cal_book_t *b){for(size_t i=0;i<b->count;++i){cal_event_clear(b->events[i]);cal_free(b->events[i]);}cal_free(b->events);cal_book_init(b);}
cal_event_t *cal_find(cal_book_t *b,uint32_t id){for(size_t i=0;i<b->count;++i)if(b->events[i]->id==id)return b->events[i];return NULL;}
bool cal_put(cal_book_t *b,const cal_event_t *e)
{
    if(!cal_event_valid(e)||e->id==UINT32_MAX)return false;cal_event_t *old=e->id?cal_find(b,e->id):NULL;
    if(old)return cal_event_copy(old,e);
    if(b->count>=CAL_LIMIT||b->next_id==UINT32_MAX)return false;
    cal_event_t *copy=cal_alloc(sizeof(*copy));cal_event_t **items=cal_alloc((b->count+1)*sizeof(*items));
    if(!copy||!items||!cal_event_copy(copy,e)){cal_free(copy);cal_free(items);return false;}
    if(!copy->id)copy->id=b->next_id++;
    else if(copy->id>=b->next_id)b->next_id=copy->id+1;
    if(b->count)memcpy(items,b->events,b->count*sizeof(*items));items[b->count++]=copy;cal_free(b->events);b->events=items;return true;
}
static bool exclude(cal_event_t *e,int32_t day)
{
    for(unsigned i=0;i<e->rule.excluded_count;++i)if(e->rule.excluded[i]==day)return true;
    if(e->rule.excluded_count==EPD_CALENDAR_EXCEPTIONS)return false;
    e->rule.excluded[e->rule.excluded_count++]=day;return true;
}
bool cal_delete(cal_book_t *b,uint32_t id,int32_t day,bool instance)
{
    cal_event_t *e=cal_find(b,id);if(!e)return false;
    if(instance&&e->rule.repeat!=EPD_REPEAT_NONE)return exclude(e,day);
    for(size_t i=0;i<b->count;) {
        if(b->events[i]->id==id||b->events[i]->parent==id){cal_event_clear(b->events[i]);cal_free(b->events[i]);--b->count;memmove(b->events+i,b->events+i+1,(b->count-i)*sizeof(*b->events));}
        else ++i;
    }
    return true;
}
bool cal_override(cal_book_t *b,uint32_t id,int32_t day,const cal_event_t *value)
{
    cal_event_t *base=cal_find(b,id);if(!base)return false;
    bool excluded=false;for(unsigned i=0;i<base->rule.excluded_count;++i)if(base->rule.excluded[i]==day)excluded=true;
    if(!excluded&&base->rule.excluded_count==EPD_CALENDAR_EXCEPTIONS)return false;
    cal_event_t e=*value;e.id=0;e.parent=id;e.original_day=day;e.rule.repeat=EPD_REPEAT_NONE;e.rule.lunar_month=0;e.rule.excluded_count=0;
    for(size_t i=0;i<b->count;++i)if(b->events[i]->parent==id&&b->events[i]->original_day==day)e.id=b->events[i]->id;
    return cal_put(b,&e)&&exclude(base,day);
}
static int compare(const void *a,const void *b)
{
    const cal_occurrence_t *x=a,*y=b;
    if(x->day!=y->day)return x->day<y->day?-1:1;
    if(x->all_day!=y->all_day)return x->all_day?-1:1;
    if(x->utc!=y->utc)return x->utc<y->utc?-1:1;
    return x->id<y->id?-1:x->id!=y->id;
}
size_t cal_query(const cal_book_t *b,int32_t first,int32_t last,int tz,const char *query,int kind,cal_occurrence_t *out,size_t cap,size_t offset)
{
    if(!out||!cap||first>last)return 0;size_t count=0;
    /* Keep only the requested sorted prefix; no unbounded recurrence expansion. */
    size_t limit=cap+offset;if(limit>4096)return 0;
    cal_occurrence_t *rows=cal_alloc(limit*sizeof(*rows));if(!rows)return 0;
    for(size_t i=0;i<b->count;++i) {
        const cal_event_t *e=b->events[i];if(kind>=0&&kind!=e->kind)continue;
        if(query&&*query&&!strstr(e->title,query)&&!strstr(e->place,query)&&(!e->notes||!strstr(e->notes,query)))continue;
        int zone=e->all_day?e->rule.timezone_minutes:tz;
        int64_t lower=(int64_t)first*86400-zone*60-e->duration_minutes*INT64_C(60)+1;
        int64_t upper=((int64_t)last+1)*86400-zone*60;
        int64_t t;int32_t d;
        while(epd_calendar_next(&e->rule,lower,&t,&d)&&t<upper) {
            int32_t shown=e->all_day?d:epd_date_utc_day(t,tz);
            int64_t end=t+e->duration_minutes*INT64_C(60);
            if(end>(int64_t)first*86400-zone*60) {
                cal_occurrence_t row={e->id,shown,d,t,e->all_day};
                size_t p;
                if(count==limit){p=limit-1;if(compare(&row,&rows[p])>=0){lower=t+1;if(shown>rows[p].day)break;continue;}}else p=count++;
                rows[p]=row;while(p&&compare(rows+p,rows+p-1)<0){cal_occurrence_t temp=rows[p];rows[p]=rows[p-1];rows[--p]=temp;}
            }
            lower=t+1;
            if(count==limit&&shown>rows[limit-1].day)break;
        }
    }
    size_t n=count>offset?count-offset:0;if(n>cap)n=cap;if(n)memcpy(out,rows+offset,n*sizeof(*out));cal_free(rows);return n;
}
uint64_t cal_busy_days(const cal_book_t *b,int32_t first,unsigned days,int tz)
{
    if(days>63)return 0;uint64_t mask=0;
    for(size_t i=0;i<b->count;++i){const cal_event_t *e=b->events[i];int zone=e->all_day?e->rule.timezone_minutes:tz;
        int64_t lower=(int64_t)first*86400-zone*60-e->duration_minutes*INT64_C(60)+1,upper=((int64_t)first+days)*86400-zone*60,t;int32_t d;
        while(epd_calendar_next(&e->rule,lower,&t,&d)&&t<upper){int32_t start=e->all_day?d:epd_date_utc_day(t,tz),end=e->all_day?d+(e->duration_minutes-1)/1440:epd_date_utc_day(t+e->duration_minutes*INT64_C(60)-1,tz);
            int a=start<first?0:start-first,b=end>=first+(int32_t)days?(int)days-1:end-first;
            for(int j=a;j<=b;++j)mask|=UINT64_C(1)<<j;lower=t+1;}}
    return mask;
}
size_t cal_search(const cal_book_t *b,int32_t today,int tz,const char *q,int kind,cal_occurrence_t *out,size_t cap,size_t offset)
{
    cal_occurrence_t *items=cal_alloc((b->count+1)*sizeof(*items));if(!items)return 0;size_t count=0;
    for(size_t i=0;i<b->count;++i){const cal_event_t *e=b->events[i];if(kind>=0&&kind!=e->kind)continue;
        if(q&&*q&&!strstr(e->title,q)&&!strstr(e->place,q)&&(!e->notes||!strstr(e->notes,q)))continue;
        int zone=e->all_day?e->rule.timezone_minutes:tz;int64_t t,at=(int64_t)today*86400-zone*60;int32_t d;
        if(!epd_calendar_next(&e->rule,at,&t,&d)&&!epd_calendar_last(&e->rule,INT64_MIN,at-1,&t,&d))continue;
        items[count++]=(cal_occurrence_t){e->id,e->all_day?d:epd_date_utc_day(t,tz),d,t,e->all_day};}
    qsort(items,count,sizeof(*items),compare);size_t n=count>offset?count-offset:0;if(n>cap)n=cap;if(n)memcpy(out,items+offset,n*sizeof(*out));cal_free(items);return n;
}

typedef struct { uint8_t *p;size_t pos,size;bool ok; } codec_t;
static void put(codec_t *c,uint32_t v){if(c->p&&c->pos+4<=c->size)for(int i=0;i<4;++i)c->p[c->pos+i]=(uint8_t)(v>>(8*i));c->pos+=4;}
static uint32_t get(codec_t *c){if(c->pos+4>c->size){c->ok=false;return 0;}uint32_t v=0;for(int i=0;i<4;++i)v|=(uint32_t)c->p[c->pos+i]<<(8*i);c->pos+=4;return v;}
static uint32_t get_range(codec_t *c,uint32_t max){uint32_t n=get(c);if(n>max)c->ok=false;return n;}
static int16_t get_zone(codec_t *c){int32_t n=(int32_t)get(c);if(n< -720||n>840)c->ok=false;return (int16_t)n;}
static void put_text(codec_t *c,const char *s){size_t n=s?strlen(s):0;put(c,(uint32_t)n);if(c->p&&n)memcpy(c->p+c->pos,s,n);c->pos+=n;}
static char *get_text(codec_t *c,size_t max){size_t n=get(c);if(!c->ok||n>max||n>c->size-c->pos){c->ok=false;return NULL;}char *s=cal_alloc(n+1);if(!s){c->ok=false;return NULL;}memcpy(s,c->p+c->pos,n);c->pos+=n;if(memchr(s,0,n)){c->ok=false;cal_free(s);return NULL;}return s;}
static void write_book(codec_t *c,const cal_book_t *b)
{
    put(c,0x43414c31);put(c,1);put(c,b->count);put(c,b->next_id);put(c,b->first_weekday);put(c,b->default_view);put(c,b->default_duration);put(c,b->lunar);put(c,b->festivals);
    for(size_t i=0;i<b->count;++i) {
        const cal_event_t *e=b->events[i];const epd_calendar_rule_t *r=&e->rule;
        put(c,e->id);put(c,e->parent);put(c,e->original_day);put(c,r->start_day);put(c,r->until_day);put(c,(uint32_t)(int32_t)r->timezone_minutes);put(c,r->minute);
        put(c,r->repeat);put(c,r->weekdays);put(c,r->lunar_month);put(c,r->lunar_day);put(c,r->lunar_leap);put(c,r->month_end);put(c,r->excluded_count);
        for(unsigned j=0;j<r->excluded_count;++j)put(c,r->excluded[j]);
        put(c,e->duration_minutes);put(c,e->kind);put(c,e->all_day);put(c,e->inclusive);put(c,e->reminder_minutes);
        put_text(c,e->uid);put_text(c,e->title);put_text(c,e->place);put_text(c,e->notes);
    }
}
bool cal_encode(const cal_book_t *b,void **bytes,size_t *size)
{
    if(!b||!bytes||!size||b->count>CAL_LIMIT)return false;
    for(size_t i=0;i<b->count;++i)if(!cal_event_valid(b->events[i]))return false;
    codec_t c={0};write_book(&c,b);if(c.pos>2u*1024u*1024u)return false;
    c.size=c.pos;c.pos=0;c.p=cal_alloc(c.size);if(!c.p)return false;write_book(&c,b);*bytes=c.p;*size=c.size;return true;
}
bool cal_decode(cal_book_t *b,const void *bytes,size_t size)
{
    if(!bytes)return false;codec_t c={(uint8_t *)bytes,0,size,true};cal_book_t n;cal_book_init(&n);
    if(get(&c)!=0x43414c31||get(&c)!=1)return false;
    uint32_t count=get(&c),next=get(&c);n.first_weekday=get_range(&c,1);n.default_view=get_range(&c,3);n.default_duration=get_range(&c,1440);n.lunar=get_range(&c,1)!=0;n.festivals=get_range(&c,1)!=0;
    if(count>CAL_LIMIT||!next||n.first_weekday>1||n.default_view>3||!n.default_duration||n.default_duration>1440)return false;
    for(uint32_t i=0;i<count&&c.ok;++i) {
        cal_event_t e={0};epd_calendar_rule_t *r=&e.rule;
        e.id=get(&c);e.parent=get(&c);e.original_day=(int32_t)get(&c);r->start_day=(int32_t)get(&c);r->until_day=(int32_t)get(&c);r->timezone_minutes=get_zone(&c);r->minute=get_range(&c,1439);
        r->repeat=get_range(&c,EPD_REPEAT_YEARLY);r->weekdays=get_range(&c,127);r->lunar_month=get_range(&c,12);r->lunar_day=get_range(&c,30);r->lunar_leap=get_range(&c,1)!=0;r->month_end=get_range(&c,1)!=0;r->excluded_count=get_range(&c,EPD_CALENDAR_EXCEPTIONS);
        if(r->excluded_count>EPD_CALENDAR_EXCEPTIONS){c.ok=false;break;}
        for(unsigned j=0;j<r->excluded_count;++j)r->excluded[j]=(int32_t)get(&c);
        e.duration_minutes=get(&c);e.kind=get_range(&c,CAL_COUNTDOWN);e.all_day=get_range(&c,1)!=0;e.inclusive=get_range(&c,1)!=0;e.reminder_minutes=(int32_t)get(&c);
        char *uid=get_text(&c,127),*title=get_text(&c,240),*place=get_text(&c,360);e.notes=get_text(&c,CAL_TEXT_BYTES);
        if(uid)strcpy(e.uid,uid);cal_free(uid);if(title)strcpy(e.title,title);if(place)strcpy(e.place,place);cal_free(title);cal_free(place);
        if(!c.ok||!e.id||cal_find(&n,e.id)||!cal_put(&n,&e))c.ok=false;cal_event_clear(&e);
    }
    if(!c.ok||c.pos!=size||next<n.next_id){cal_book_clear(&n);return false;}
    n.next_id=next;cal_book_clear(b);*b=n;return true;
}
